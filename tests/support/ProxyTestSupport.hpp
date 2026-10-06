#pragma once

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <sys/socket.h>

#include "edgeflow/discovery/NameResolver.hpp"
#include "edgeflow/network/RegistryRequestHandler.hpp"
#include "edgeflow/proxy/ProxyHandler.hpp"
#include "edgeflow/routing/Router.hpp"
#include "support/FakeRegistry.hpp"
#include "support/HealthTestSupport.hpp"
#include "support/NetTestSupport.hpp"

namespace edgeflow::testing {

// Opened by a test to let a held backend request go on.
class Gate {
 public:
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  // True when opened, false when `give_up` became true first or the timeout passed.
  bool wait(const std::atomic<bool>& keep_waiting, std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    std::unique_lock lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!open_ && keep_waiting.load() && std::chrono::steady_clock::now() < deadline) {
      cv_.wait_for(lock, std::chrono::milliseconds{10});
    }
    return open_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_{false};
};

// A real HTTP/1.1 backend on loopback that behaves as scripted, request by request. Each
// accepted connection is served by its own thread; the server records every connection and
// every request, so tests can assert what the proxy really sent and over how many sockets.
class ScriptedBackend {
 public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Header = std::pair<std::string, std::string>;

  struct Action {
    enum class Kind {
      Respond,   // a normal response with Content-Length
      Chunked,   // the body in three chunks (Transfer-Encoding: chunked)
      UntilEof,  // no framing: the body ends when the connection closes
      Truncated, // announces more body than it sends, then closes
      Garbage,   // bytes that are not HTTP
      Close,     // reads the request, closes without answering
      Reset,     // reads the request, resets the connection (RST)
      Stall,     // reads the request, then never answers (until the peer closes)
      PartialStall  // sends the head and part of the body, then stalls
    };
    Kind kind{Kind::Respond};
    unsigned code{200};
    std::string body;
    std::vector<Header> headers;
    bool keep_alive{true};    // false: answers "Connection: close" and closes
    bool close_after{false};  // closes silently right after answering (the pooled connection dies)
    std::chrono::milliseconds delay{0};  // before answering
    std::shared_ptr<Gate> gate;          // wait for this before answering
  };

  struct Received {
    std::uint64_t connection{0};
    Request request;
  };

  using Handler = std::function<Action(const Request&, std::uint64_t connection, std::size_t request_on_connection)>;

  explicit ScriptedBackend(Handler handler = nullptr, std::string name = "backend")
      : name_(std::move(name)) {
    handler_ = handler ? std::move(handler) : [this](const Request& r, std::uint64_t, std::size_t) {
      return describe(r);
    };
    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
        io_, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});
    port_ = acceptor_->local_endpoint().port();
    running_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });
  }
  ~ScriptedBackend() { stop(); }

  ScriptedBackend(const ScriptedBackend&) = delete;
  ScriptedBackend& operator=(const ScriptedBackend&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] const std::string& name() const { return name_; }

  void setHandler(Handler handler) {
    const std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
  }
  // Answers every request with the same action.
  void always(Action action) {
    setHandler([action](const Request&, std::uint64_t, std::size_t) { return action; });
  }

  // A 200 whose body says who answered and what was asked: "name|METHOD target|body".
  [[nodiscard]] Action describe(const Request& request) const {
    Action action;
    action.body = name_ + "|" + std::string{request.method_string()} + " " +
                  std::string{request.target()} + "|" + request.body();
    return action;
  }

  [[nodiscard]] std::uint64_t connections() const { return connections_.load(); }
  [[nodiscard]] std::size_t requests() {
    const std::lock_guard lock(mutex_);
    return received_.size();
  }
  [[nodiscard]] std::vector<Received> received() {
    const std::lock_guard lock(mutex_);
    return received_;
  }
  [[nodiscard]] Request last() {
    const std::lock_guard lock(mutex_);
    return received_.empty() ? Request{} : received_.back().request;
  }
  // Connections that ended because the proxy (the peer) closed them.
  [[nodiscard]] std::uint64_t peerClosed() const { return peer_closed_.load(); }
  [[nodiscard]] std::size_t openConnections() {
    const std::lock_guard lock(mutex_);
    return active_fds_.size();
  }

  void stop() {
    if (!running_.exchange(false)) return;
    {
      boost::asio::io_context io;
      boost::asio::ip::tcp::socket waker(io);
      boost::system::error_code ignored;
      waker.connect({boost::asio::ip::make_address("127.0.0.1"), port_}, ignored);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    boost::system::error_code ignored;
    acceptor_->close(ignored);
    {
      const std::lock_guard lock(mutex_);
      for (const int fd : active_fds_) ::shutdown(fd, SHUT_RDWR);
    }
    for (;;) {
      std::thread thread;
      {
        const std::lock_guard lock(mutex_);
        if (workers_.empty()) break;
        thread = std::move(workers_.back());
        workers_.pop_back();
      }
      if (thread.joinable()) thread.join();
    }
  }

 private:
  void acceptLoop() {
    while (running_.load()) {
      auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
      boost::system::error_code error;
      acceptor_->accept(*socket, error);
      if (error || !running_.load()) return;
      const auto id = ++connections_;
      const std::lock_guard lock(mutex_);
      active_fds_.insert(socket->native_handle());
      workers_.emplace_back([this, socket, id] { serve(socket, id); });
    }
  }

  void serve(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket, std::uint64_t id) {
    namespace http = boost::beast::http;
    boost::beast::flat_buffer buffer;
    std::size_t number = 0;
    bool peer_closed = false;
    while (running_.load()) {
      Request request;
      boost::system::error_code error;
      http::read(*socket, buffer, request, error);
      if (error) {
        peer_closed = error == http::error::end_of_stream || error == boost::asio::error::eof ||
                      error == boost::asio::error::connection_reset;
        break;
      }
      ++number;
      Handler handler;
      {
        const std::lock_guard lock(mutex_);
        received_.push_back(Received{id, request});
        handler = handler_;
      }
      const Action action = handler(request, id, number);
      if (!act(*socket, request, action)) break;
    }
    if (peer_closed) ++peer_closed_;
    const int fd = socket->native_handle();
    boost::system::error_code ignored;
    socket->close(ignored);
    const std::lock_guard lock(mutex_);
    active_fds_.erase(fd);
  }

  // Blocks until the peer closes the connection (or the server stops).
  void waitForPeerClose(boost::asio::ip::tcp::socket& socket) {
    std::array<char, 64> scratch{};
    boost::system::error_code error;
    while (running_.load()) {
      socket.read_some(boost::asio::buffer(scratch), error);
      if (error) {
        if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset) ++peer_closed_;
        return;
      }
    }
  }

  static std::string statusLine(unsigned code) { return "HTTP/1.1 " + std::to_string(code) + " Test\r\n"; }

  // Returns true when the connection stays open for another request.
  bool act(boost::asio::ip::tcp::socket& socket, const Request& request, const Action& action) {
    namespace http = boost::beast::http;
    if (action.gate) (void)action.gate->wait(running_);
    if (action.delay.count() > 0) {
      const auto until = std::chrono::steady_clock::now() + action.delay;
      while (running_.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
      }
    }
    if (!running_.load()) return false;

    boost::system::error_code error;
    const auto send = [&](const std::string& data) { boost::asio::write(socket, boost::asio::buffer(data), error); };
    std::string extra;
    for (const auto& [name, value] : action.headers) extra += name + ": " + value + "\r\n";

    switch (action.kind) {
      case Action::Kind::Close:
        return false;
      case Action::Kind::Reset:
        socket.set_option(boost::asio::socket_base::linger(true, 0), error);
        return false;  // closing with SO_LINGER 0 sends RST
      case Action::Kind::Stall: {
        waitForPeerClose(socket);
        return false;
      }
      case Action::Kind::PartialStall: {
        send(statusLine(action.code) + extra + "Content-Length: " + std::to_string(action.body.size() + 100) +
             "\r\n\r\n" + action.body);
        waitForPeerClose(socket);
        return false;
      }
      case Action::Kind::Garbage:
        send("THIS IS NOT HTTP AT ALL\r\n\r\n");
        return false;
      case Action::Kind::Truncated:
        send(statusLine(action.code) + extra + "Content-Length: " + std::to_string(action.body.size() + 100) +
             "\r\n\r\n" + action.body);
        return false;
      case Action::Kind::UntilEof:
        send(statusLine(action.code) + extra + "Connection: close\r\n\r\n" + action.body);
        return false;
      case Action::Kind::Chunked: {
        std::string out = statusLine(action.code) + extra + "Transfer-Encoding: chunked\r\n";
        if (!action.keep_alive) out += "Connection: close\r\n";
        out += "\r\n";
        const auto& body = action.body;
        const std::size_t third = body.size() / 3;
        const std::string parts[3] = {body.substr(0, third), body.substr(third, third), body.substr(2 * third)};
        for (const auto& part : parts) {
          if (part.empty()) continue;
          char size[32];
          std::snprintf(size, sizeof(size), "%zx\r\n", part.size());
          out += size + part + "\r\n";
        }
        out += "0\r\n\r\n";
        send(out);
        return action.keep_alive && !action.close_after && !error;
      }
      case Action::Kind::Respond: {
        std::string out = statusLine(action.code) + extra;
        const bool no_body_status = action.code == 204 || action.code == 304 || (action.code >= 100 && action.code < 200);
        if (!no_body_status) out += "Content-Length: " + std::to_string(action.body.size()) + "\r\n";
        if (!action.keep_alive) out += "Connection: close\r\n";
        out += "\r\n";
        if (!no_body_status && request.method() != http::verb::head) out += action.body;
        send(out);
        return action.keep_alive && !action.close_after && !error;
      }
    }
    return false;
  }

  const std::string name_;
  Handler handler_;
  boost::asio::io_context io_;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::uint16_t port_{0};
  std::atomic<bool> running_{false};
  std::thread accept_thread_;

  std::mutex mutex_;
  std::vector<Received> received_;
  std::set<int> active_fds_;
  std::vector<std::thread> workers_;
  std::atomic<std::uint64_t> connections_{0};
  std::atomic<std::uint64_t> peer_closed_{0};
};

// A registry that behaves like FakeRegistry, except that the routable lookup can be made to
// fail as if the database were down.
class FlakyRegistry final : public discovery::ServiceRegistry {
 public:
  using Code = discovery::RegistryErrorCode;
  std::shared_ptr<FakeRegistry> inner = std::make_shared<FakeRegistry>();
  std::atomic<bool> routable_unavailable{false};

  discovery::Result<discovery::ServiceInstance> registerInstance(const discovery::NewInstance& i) override {
    return inner->registerInstance(i);
  }
  discovery::Result<discovery::Unit> deregisterInstance(std::string_view s, std::string_view i) override {
    return inner->deregisterInstance(s, i);
  }
  discovery::Result<std::vector<discovery::ServiceInstance>> lookupService(std::string_view s) override {
    return inner->lookupService(s);
  }
  discovery::Result<discovery::ServiceInstance> getInstance(std::string_view s, std::string_view i) override {
    return inner->getInstance(s, i);
  }
  discovery::Result<std::vector<std::string>> listServices() override { return inner->listServices(); }
  discovery::Result<discovery::ServiceInstance> updateInstance(std::string_view s, std::string_view i,
                                                               const discovery::InstanceUpdate& u) override {
    return inner->updateInstance(s, i, u);
  }
  discovery::Result<std::vector<discovery::ServiceInstance>> listInstances() override {
    return inner->listInstances();
  }
  discovery::Result<std::vector<discovery::ServiceInstance>> lookupRoutable(std::string_view s) override {
    if (routable_unavailable.load()) return {Code::DatabaseUnavailable, "the database is unavailable"};
    return inner->lookupRoutable(s);
  }
  discovery::Result<discovery::ServiceInstance> updateHealth(std::string_view s, std::string_view i,
                                                             std::string_view at,
                                                             discovery::HealthStatus h) override {
    return inner->updateHealth(s, i, at, h);
  }
  discovery::Result<discovery::ServiceInstance> adjustConnectionCount(std::string_view s, std::string_view i,
                                                                      std::int64_t delta) override {
    ++adjustments;
    return inner->adjustConnectionCount(s, i, delta);
  }
  std::atomic<int> adjustments{0};
};

inline config::ProxyConfig testProxyConfig() {
  config::ProxyConfig config;
  config.enabled = true;
  config.connect_timeout = std::chrono::milliseconds{1000};
  config.upstream_timeout = std::chrono::milliseconds{5000};
  config.io_threads = 2;
  config.max_idle_connections = 8;
  config.idle_timeout = std::chrono::milliseconds{30000};
  config.max_response_bytes = 8 * 1024 * 1024;
  return config;
}

// EdgeFlow's real server + registry API + reverse proxy over a FakeRegistry, with scripted
// backends registered as instances: everything is real except the database.
class ProxyFixture : public ::testing::Test {
 protected:
  using Action = ScriptedBackend::Action;
  using Kind = Action::Kind;

  void TearDown() override { stopGateway(); }

  void startGateway(config::RoutingStrategy strategy = config::RoutingStrategy::RoundRobin,
                    std::shared_ptr<discovery::NameResolver> names = nullptr,
                    config::ServerConfig server_config = testServerConfig()) {
    router = std::make_shared<routing::Router>(registry, routing::makeLoadBalancer(strategy));
    auto local = std::make_shared<network::LocalRequestHandler>();
    auto api = std::make_shared<network::RegistryRequestHandler>(registry, local, log.logger(), router);
    proxy = std::make_shared<proxy::ProxyHandler>(api, router, registry, proxy_config, 4, log.logger(),
                                                  std::move(names));
    server = std::make_unique<network::HttpServer>(std::move(server_config), log.logger(), proxy);
    ASSERT_TRUE(server->start());
  }

  void stopGateway(std::chrono::milliseconds grace = std::chrono::milliseconds{3000}) {
    if (server) server->stop(grace);
    if (proxy) proxy->stop();
  }

  std::unique_ptr<TestClient> client() const { return std::make_unique<TestClient>(server->port()); }

  // A healthy, active instance of `service` on a scripted backend (or any port).
  void addInstance(const std::string& service, const std::string& id, std::uint16_t port,
                   discovery::HealthStatus health = discovery::HealthStatus::Healthy,
                   discovery::InstanceStatus status = discovery::InstanceStatus::Active,
                   std::uint32_t weight = 1, std::uint64_t connections = 0,
                   const std::string& host = "127.0.0.1") {
    discovery::NewInstance instance;
    instance.service = service;
    instance.instance_id = id;
    instance.host = host;
    instance.port = port;
    instance.health = health;
    instance.status = status;
    instance.weight = weight;
    instance.connection_count = connections;
    const auto registered = registry->registerInstance(instance);
    ASSERT_TRUE(registered.ok()) << registered.error().message;
  }

  std::uint64_t connectionCount(const std::string& service, const std::string& id) {
    const auto instance = registry->getInstance(service, id);
    return instance.ok() ? instance.value().connection_count : 9999;
  }
  bool countsReturnToZero(const std::string& service, const std::vector<std::string>& ids) {
    return waitFor([&] {
      for (const auto& id : ids) {
        if (connectionCount(service, id) != 0) return false;
      }
      return true;
    });
  }

  static std::string header(const network::HttpResponse& response, const std::string& name) {
    const auto it = response.find(name);
    return it == response.end() ? "<absent>" : std::string{it->value()};
  }
  static std::string header(const ScriptedBackend::Request& request, const std::string& name) {
    const auto it = request.find(name);
    return it == request.end() ? "<absent>" : std::string{it->value()};
  }

  CapturedLogger log;
  std::shared_ptr<FlakyRegistry> registry = std::make_shared<FlakyRegistry>();
  config::ProxyConfig proxy_config = testProxyConfig();
  std::shared_ptr<routing::Router> router;
  std::shared_ptr<proxy::ProxyHandler> proxy;
  std::unique_ptr<network::HttpServer> server;
};

}  // namespace edgeflow::testing
