#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/redis/Resp.hpp"

namespace edgeflow::redis {

// Why a command did not produce a reply. A reply that IS an error (-ERR, -NOSCRIPT, ...) is
// not one of these: it is delivered as a Reply of type Error.
enum class ErrorKind {
  Unavailable,    // failed at once: Redis was found unreachable a moment ago, or the queue is full
  ConnectFailed,  // could not resolve or connect, or AUTH was refused
  Timeout,        // connect or command exceeded its bound
  Closed,         // the connection broke while the command was in flight
  Protocol,       // the server sent something that is not RESP
  Cancelled       // the client was stopped
};

[[nodiscard]] std::string_view toString(ErrorKind kind) noexcept;

struct RedisError {
  ErrorKind kind{ErrorKind::Unavailable};
  std::string message;
};

// Outcome of one command: either a reply (even an error reply) or a transport error.
struct RedisResult {
  std::optional<Reply> reply;
  std::optional<RedisError> error;
  [[nodiscard]] bool ok() const noexcept { return reply.has_value(); }
};

using Callback = std::function<void(RedisResult)>;

struct RedisOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{6379};
  std::string password;  // empty: no AUTH
  std::chrono::milliseconds connect_timeout{1000};
  std::chrono::milliseconds command_timeout{250};
  unsigned pool_size{4};
  unsigned io_threads{1};
  std::chrono::milliseconds retry_interval{1000};
  std::size_t max_pending{1024};  // commands waiting for a free connection; more fail at once

  [[nodiscard]] static RedisOptions fromConfig(const config::RedisConfig& config,
                                               std::string password = {});
};

// Asynchronous Redis client (RESP2 over Boost.Asio) with a small pool of connections.
//
//  - command() never blocks: the callback runs later on a Redis I/O thread, so the HTTP I/O
//    workers are never held up by Redis.
//  - Every connection runs one command at a time, so replies cannot be mixed up; up to
//    `pool_size` commands run concurrently, further commands wait in a bounded queue.
//  - Every wait is bounded: connecting by `connect_timeout`, each command by
//    `command_timeout`, the queue by `max_pending`.
//  - After a connection failure, timeout, broken connection or protocol error the client is
//    "unavailable" for `retry_interval`: commands fail immediately (no network, no wait), and
//    the first command after that interval is the probe that finds out whether Redis is back.
//    Nothing is retried automatically: whether a command that was already written may be
//    repeated is for the caller to decide.
//  - An idle pooled connection that the server closed in the meantime (Redis restarted) is
//    detected before it is used and replaced; a command is never sent into a dead connection.
//
// All state lives on one strand; the class is thread-safe. Create it with create() (it hands
// out references to itself to its asynchronous operations).
class RedisClient : public std::enable_shared_from_this<RedisClient> {
 public:
  struct Stats {
    std::uint64_t commands{0};          // commands submitted
    std::uint64_t failures{0};          // commands that ended with a transport error
    std::uint64_t fast_failures{0};     // ... of which failed at once (unavailable / queue full)
    std::uint64_t connections_opened{0};
    bool available{true};               // false while in the fail-fast interval
  };

  [[nodiscard]] static std::shared_ptr<RedisClient> create(RedisOptions options,
                                                           std::shared_ptr<logging::Logger> logger);
  ~RedisClient();

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  // Runs `arguments` (command name first) and calls `callback` exactly once, from a Redis I/O
  // thread, never from inside this call (except after stop(): then there is no I/O thread left
  // and the callback runs at once on the caller's thread, with ErrorKind::Cancelled).
  void command(std::vector<std::string> arguments, Callback callback);

  // Blocking convenience for start-up checks and tests; never call it from an I/O thread of
  // the HTTP server.
  [[nodiscard]] RedisResult commandSync(std::vector<std::string> arguments);

  // Fails everything still waiting or in flight with ErrorKind::Cancelled (their callbacks
  // still run), closes every connection and joins the I/O threads. Idempotent. Later commands
  // fail with Cancelled.
  void stop();

  [[nodiscard]] Stats stats() const;
  [[nodiscard]] const RedisOptions& options() const noexcept { return options_; }

 private:
  RedisClient(RedisOptions options, std::shared_ptr<logging::Logger> logger);

  struct Connection;
  struct Pending {
    std::vector<std::string> arguments;
    Callback callback;
    std::chrono::steady_clock::time_point deadline;
  };

  void submit(Pending pending);
  void pump();
  void openConnection(const std::shared_ptr<Connection>& connection);
  void authenticate(const std::shared_ptr<Connection>& connection);
  void runOnConnection(const std::shared_ptr<Connection>& connection, Pending pending);
  void exchange(const std::shared_ptr<Connection>& connection, std::vector<std::string> arguments,
                std::chrono::milliseconds timeout, std::function<void(RedisResult)> done);
  void readReply(const std::shared_ptr<Connection>& connection);
  void finishExchange(const std::shared_ptr<Connection>& connection, RedisResult result);
  void dropConnection(const std::shared_ptr<Connection>& connection);
  void noteFailure(const RedisError& error);
  void noteSuccess();
  void deliver(Callback callback, RedisResult result);
  void failAll(const RedisError& error);
  static bool isDead(boost::asio::ip::tcp::socket& socket);

  const RedisOptions options_;
  const std::shared_ptr<logging::Logger> logger_;

  boost::asio::io_context io_;
  using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
  std::unique_ptr<WorkGuard> work_guard_;
  std::vector<std::thread> threads_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;

  // Everything below is touched only on strand_.
  std::deque<Pending> pending_;
  std::vector<std::shared_ptr<Connection>> connections_;
  std::chrono::steady_clock::time_point unavailable_until_{};
  bool available_{true};
  bool stopped_{false};

  std::mutex stop_mutex_;
  bool stop_done_{false};

  std::atomic<std::uint64_t> commands_{0};
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<std::uint64_t> fast_failures_{0};
  std::atomic<std::uint64_t> opened_{0};
  std::atomic<bool> available_flag_{true};
  std::atomic<bool> stop_requested_{false};
};

}  // namespace edgeflow::redis
