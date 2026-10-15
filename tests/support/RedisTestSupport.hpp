#pragma once

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

#include <boost/asio.hpp>

#include "edgeflow/redis/RedisClient.hpp"
#include "edgeflow/redis/Resp.hpp"
#include "support/PgTestSupport.hpp"
#include "support/TestLogger.hpp"

namespace edgeflow::testing {

// Tests that need a real Redis read its address from the environment and are SKIPPED
// (visibly, with the reason) when EDGEFLOW_TEST_REDIS_HOST is not set:
//   EDGEFLOW_TEST_REDIS_HOST   required to enable the tests
//   EDGEFLOW_TEST_REDIS_PORT   default 6379
struct TestRedisAddress {
  std::string host;
  std::uint16_t port{6379};
};

inline std::optional<TestRedisAddress> testRedis() {
  const char* host = std::getenv("EDGEFLOW_TEST_REDIS_HOST");
  if (host == nullptr || *host == '\0') return std::nullopt;
  TestRedisAddress address;
  address.host = host;
  address.port = static_cast<std::uint16_t>(std::stoi(envOr("EDGEFLOW_TEST_REDIS_PORT", "6379")));
  return address;
}

#define EDGEFLOW_REQUIRE_TEST_REDIS()                                                         \
  const auto test_redis = ::edgeflow::testing::testRedis();                                  \
  if (!test_redis) GTEST_SKIP() << "EDGEFLOW_TEST_REDIS_HOST is not set: no Redis available"

inline redis::RedisOptions testRedisOptions(const std::string& host, std::uint16_t port) {
  redis::RedisOptions options;
  options.host = host;
  options.port = port;
  options.connect_timeout = std::chrono::milliseconds{1000};
  options.command_timeout = std::chrono::milliseconds{1000};
  options.pool_size = 4;
  options.io_threads = 2;
  options.retry_interval = std::chrono::milliseconds{200};
  return options;
}

inline std::shared_ptr<redis::RedisClient> makeRedisClient(const TestRedisAddress& address,
                                                           const std::shared_ptr<logging::Logger>& logger) {
  return redis::RedisClient::create(testRedisOptions(address.host, address.port), logger);
}

// Waits (polling) until `condition` holds or the time is up.
template <class Condition>
bool eventually(Condition condition, std::chrono::milliseconds limit = std::chrono::seconds{5}) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return condition();
}

// Closing an acceptor from another thread does not wake a thread blocked in accept(): a
// throwaway connection does.
inline void wakeAcceptor(std::uint16_t port) {
  boost::asio::io_context io;
  boost::asio::ip::tcp::socket waker(io);
  boost::system::error_code ignored;
  waker.connect({boost::asio::ip::make_address("127.0.0.1"), port}, ignored);
}

// A server that speaks just enough RESP to script failures: every command is handed to a
// handler that decides what the "Redis" does with it. Deterministic, no real Redis needed.
//
// Its threads are cooperative (short receive timeouts and a stop flag); no thread closes a
// socket another thread is using. Other threads only shut sockets down at the OS level, which
// wakes the owner; the owner closes them (or they close when the connection is destroyed).
class FakeRedisServer {
 public:
  struct Action {
    enum class Kind {
      Reply,    // send `raw` as it is
      Close,    // read the command, close the connection without answering
      Stall,    // read the command, never answer (until the peer closes)
      Garbage   // send bytes that are not RESP
    };
    Kind kind{Kind::Reply};
    std::string raw{"+OK\r\n"};
    std::chrono::milliseconds delay{0};
  };
  using Handler = std::function<Action(const std::vector<std::string>& command,
                                       std::uint64_t connection, std::size_t command_on_connection)>;

  // `port` 0 picks a free port; a fixed one lets a test bring "Redis" back where it was.
  explicit FakeRedisServer(Handler handler = nullptr, std::uint16_t port = 0)
      : handler_(std::move(handler)) {
    if (!handler_) handler_ = [](const std::vector<std::string>&, std::uint64_t, std::size_t) { return Action{}; };
    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_);
    const boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address("127.0.0.1"), port};
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen();
    port_ = acceptor_->local_endpoint().port();
    running_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });
  }
  ~FakeRedisServer() { stop(); }
  FakeRedisServer(const FakeRedisServer&) = delete;
  FakeRedisServer& operator=(const FakeRedisServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::uint64_t connections() const { return connections_.load(); }
  [[nodiscard]] std::vector<std::vector<std::string>> commands() {
    const std::lock_guard lock(mutex_);
    return commands_;
  }
  void setHandler(Handler handler) {
    const std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
  }
  // Ends every open connection (the listener keeps accepting): what a Redis restart looks
  // like to a client that holds idle connections.
  void dropConnections() {
    const std::lock_guard lock(mutex_);
    for (const auto& connection : open_) shutdownSocket(*connection);
  }

  void stop() {
    if (!running_.exchange(false)) return;
    wakeAcceptor(port_);  // the accept thread sees running_ == false and leaves
    if (accept_thread_.joinable()) accept_thread_.join();
    boost::system::error_code ignored;
    acceptor_->close(ignored);
    {
      const std::lock_guard lock(mutex_);
      for (const auto& connection : open_) shutdownSocket(*connection);
    }
    for (auto& thread : connection_threads_) {
      if (thread.joinable()) thread.join();
    }
  }

 private:
  struct Connection {
    explicit Connection(boost::asio::io_context& io) : socket(io) {}
    boost::asio::ip::tcp::socket socket;
  };

  static void shutdownSocket(Connection& connection) {
    ::shutdown(connection.socket.native_handle(), SHUT_RDWR);  // wakes the thread that reads it
  }

  void acceptLoop() {
    while (running_.load()) {
      auto connection = std::make_shared<Connection>(io_);
      boost::system::error_code error;
      acceptor_->accept(connection->socket, error);
      if (error || !running_.load()) return;
      timeval timeout{};
      timeout.tv_usec = 100 * 1000;
      ::setsockopt(connection->socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      const auto id = connections_.fetch_add(1) + 1;
      const std::lock_guard lock(mutex_);
      open_.push_back(connection);
      connection_threads_.emplace_back([this, connection, id] { serve(connection, id); });
    }
  }

  static bool timedOut(const boost::system::error_code& error) {
    return error == boost::asio::error::would_block || error == boost::asio::error::try_again ||
           error == boost::asio::error::timed_out;
  }

  void serve(const std::shared_ptr<Connection>& connection, std::uint64_t id) {
    boost::asio::ip::tcp::socket& socket = connection->socket;
    redis::RespParser parser;
    std::array<char, 4096> buffer{};
    std::size_t index = 0;
    while (running_.load()) {
      boost::system::error_code error;
      const std::size_t n = socket.read_some(boost::asio::buffer(buffer), error);
      if (timedOut(error)) continue;
      if (error) return;
      parser.feed(std::string_view{buffer.data(), n});
      while (auto request = parser.next()) {
        std::vector<std::string> command;
        for (const auto& element : request->elements) command.push_back(element.text);
        Handler handler;
        {
          const std::lock_guard lock(mutex_);
          commands_.push_back(command);
          handler = handler_;
        }
        const Action action = handler(command, id, index++);
        if (action.delay.count() > 0) std::this_thread::sleep_for(action.delay);
        switch (action.kind) {
          case Action::Kind::Reply:
            boost::asio::write(socket, boost::asio::buffer(action.raw), error);
            if (error) return;
            break;
          case Action::Kind::Garbage: {
            const std::string junk = "this is not RESP\r\n";
            boost::asio::write(socket, boost::asio::buffer(junk), error);
            break;
          }
          case Action::Kind::Close:
            ::shutdown(socket.native_handle(), SHUT_RDWR);  // the peer sees the end of the stream
            return;
          case Action::Kind::Stall:
            while (running_.load()) {
              char byte = 0;
              socket.read_some(boost::asio::buffer(&byte, 1), error);
              if (error && !timedOut(error)) return;
            }
            return;
        }
      }
      if (parser.invalid()) return;
    }
  }

  boost::asio::io_context io_;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::uint16_t port_{0};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> connections_{0};
  std::thread accept_thread_;
  std::mutex mutex_;
  Handler handler_;
  std::vector<std::vector<std::string>> commands_;
  std::vector<std::shared_ptr<Connection>> open_;
  std::vector<std::thread> connection_threads_;
};

// A TCP relay in front of the real Redis that can be told to misbehave: refuse connections
// (Redis down), accept and swallow everything (Redis hung), drop the connections it holds
// (Redis restarted), and later work again on the same port (Redis back). It lets the
// outage-and-recovery tests run against the real Redis server and its real Lua scripts.
//
// Its threads are cooperative: they read with a short receive timeout and leave when their
// connection was marked dead, so no thread ever depends on another thread closing a socket
// under it.
class RedisRelay {
 public:
  RedisRelay(std::string target_host, std::uint16_t target_port)
      : target_host_(std::move(target_host)), target_port_(target_port) {
    listen(0);
    port_ = acceptor_->local_endpoint().port();
  }
  ~RedisRelay() { shutdown(); }
  RedisRelay(const RedisRelay&) = delete;
  RedisRelay& operator=(const RedisRelay&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

  // Redis is gone: new connections are refused and the open ones are closed.
  void refuse() {
    stopListening();
    dropConnections();
  }
  // Redis is back.
  void resume() {
    const bool was_hanging = hang_.exchange(false);
    if (was_hanging) dropConnections();  // connections made while it hung must not survive
    if (!acceptor_) listen(port_);
  }
  // Redis is up but unresponsive: connections are accepted, nothing is ever answered.
  void hang() {
    hang_.store(true);
    dropConnections();  // existing connections must not keep working
    if (!acceptor_) listen(port_);
  }
  void dropConnections() {
    const std::lock_guard lock(mutex_);
    for (const auto& link : links_) {
      link->dead.store(true);
      boost::system::error_code ignored;
      link->client.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
      if (link->upstream.is_open()) {
        link->upstream.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
      }
    }
  }
  [[nodiscard]] std::uint64_t accepted() const { return accepted_.load(); }

  void shutdown() {
    stopListening();
    dropConnections();
    std::vector<std::thread> threads;
    {
      const std::lock_guard lock(mutex_);
      threads.swap(threads_);
    }
    for (auto& thread : threads) {
      if (thread.joinable()) thread.join();
    }
  }

 private:
  using Socket = boost::asio::ip::tcp::socket;

  struct Link {
    explicit Link(boost::asio::io_context& io) : client(io), upstream(io) {}
    Socket client;
    Socket upstream;  // unopened while the relay hangs
    std::atomic<bool> dead{false};
  };

  static void setReceiveTimeout(Socket& socket) {
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100 * 1000;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }

  void listen(std::uint16_t port) {
    auto acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(io_);
    const boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address("127.0.0.1"), port};
    acceptor->open(endpoint.protocol());
    acceptor->set_option(boost::asio::socket_base::reuse_address(true));
    acceptor->bind(endpoint);
    acceptor->listen();
    acceptor_ = std::move(acceptor);
    auto* raw = acceptor_.get();
    accept_thread_ = std::thread([this, raw] { acceptLoop(raw); });
  }

  void stopListening() {
    if (!acceptor_) return;
    stopping_.store(true);
    wakeAcceptor(port_);  // the accept thread sees `stopping_` and leaves ...
    if (accept_thread_.joinable()) accept_thread_.join();
    boost::system::error_code ignored;
    acceptor_->close(ignored);  // ... so nobody else uses the acceptor while it is closed
    acceptor_.reset();
    stopping_.store(false);
  }

  void acceptLoop(boost::asio::ip::tcp::acceptor* acceptor) {
    while (true) {
      auto link = std::make_shared<Link>(io_);
      boost::system::error_code error;
      acceptor->accept(link->client, error);
      if (error || stopping_.load()) return;
      accepted_.fetch_add(1);
      const bool hanging = hang_.load();
      if (!hanging) {
        boost::asio::ip::tcp::resolver resolver(io_);
        boost::asio::connect(link->upstream, resolver.resolve(target_host_, std::to_string(target_port_)), error);
        if (error) continue;  // the client socket closes with the link
      }
      setReceiveTimeout(link->client);
      if (!hanging) setReceiveTimeout(link->upstream);
      const std::lock_guard lock(mutex_);
      links_.push_back(link);
      threads_.emplace_back([this, link] { pump(link, true); });
      if (!hanging) threads_.emplace_back([this, link] { pump(link, false); });
    }
  }

  // Copies bytes from one side of the link to the other until the link dies.
  void pump(const std::shared_ptr<Link>& link, bool client_to_upstream) {
    Socket& from = client_to_upstream ? link->client : link->upstream;
    Socket& to = client_to_upstream ? link->upstream : link->client;
    std::array<char, 8192> buffer{};
    while (!link->dead.load()) {
      boost::system::error_code error;
      const std::size_t n = from.read_some(boost::asio::buffer(buffer), error);
      if (error == boost::asio::error::would_block || error == boost::asio::error::try_again ||
          error == boost::asio::error::timed_out) {
        continue;  // the receive timeout: look at the dead flag again
      }
      if (error) break;
      if (hang_.load() || !to.is_open()) continue;  // swallowed
      boost::asio::write(to, boost::asio::buffer(buffer.data(), n), error);
      if (error) break;
    }
    link->dead.store(true);
    boost::system::error_code ignored;
    link->client.shutdown(Socket::shutdown_both, ignored);
    if (link->upstream.is_open()) link->upstream.shutdown(Socket::shutdown_both, ignored);
  }

  const std::string target_host_;
  const std::uint16_t target_port_;
  std::uint16_t port_{0};
  boost::asio::io_context io_;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::thread accept_thread_;
  std::atomic<bool> hang_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> accepted_{0};
  std::mutex mutex_;
  std::vector<std::shared_ptr<Link>> links_;
  std::vector<std::thread> threads_;
};

}  // namespace edgeflow::testing
