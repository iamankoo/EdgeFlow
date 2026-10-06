#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

namespace edgeflow::proxy {

// A backend address as the pool sees it: the instance's host (as registered) and port.
struct UpstreamEndpoint {
  std::string host;
  std::uint16_t port{0};
  friend auto operator<=>(const UpstreamEndpoint&, const UpstreamEndpoint&) = default;
};

// One TCP connection to a backend. Owned either by a request that is using it or by the
// pool while it is idle, never both.
struct UpstreamConnection {
  UpstreamConnection(boost::asio::io_context& io, UpstreamEndpoint where)
      : socket(io), endpoint(std::move(where)) {}
  ~UpstreamConnection() { close(); }
  UpstreamConnection(const UpstreamConnection&) = delete;
  UpstreamConnection& operator=(const UpstreamConnection&) = delete;

  void close() noexcept {
    boost::system::error_code ignored;
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
  }

  boost::asio::ip::tcp::socket socket;
  UpstreamEndpoint endpoint;
  std::chrono::steady_clock::time_point idle_since{};  // set by the pool on check-in
};

// Idle keep-alive connections to backends, so a request does not need a new TCP
// connection when a reusable one exists.
//
// Safety rules (a connection that cannot be proven reusable is closed, never pooled):
//   - the caller checks a connection in only after a complete response that allowed
//     keep-alive was read with nothing left over;
//   - at check-out an idle connection is dropped when it is older than `idle_timeout` or when
//     the backend closed it (or sent unsolicited bytes) while it sat in the pool, which is
//     detected without sending anything;
//   - at most `max_idle_per_endpoint` idle connections are kept per backend (0 disables
//     pooling), the most recently used first, which lets surplus ones age out;
//   - after close() nothing is accepted any more and every idle connection has been closed.
// Idle connections are also evicted (expired ones, across all backends) whenever the pool
// is used; there is no background thread. Thread-safe.
class UpstreamPool {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  struct Settings {
    std::size_t max_idle_per_endpoint{32};
    std::chrono::milliseconds idle_timeout{30000};
  };

  struct Stats {
    std::uint64_t created{0};         // fresh connections opened (noteCreated)
    std::uint64_t reused{0};          // check-outs that returned an idle connection
    std::uint64_t returned{0};        // check-ins that were kept
    std::uint64_t discarded{0};       // connections closed instead of being pooled / reused
    std::uint64_t stale_dropped{0};   // idle connections found dead or unsolicited at check-out
    std::uint64_t expired_dropped{0}; // idle connections older than idle_timeout
  };

  // `clock` is replaceable so tests can age connections without sleeping.
  explicit UpstreamPool(Settings settings, Clock clock = nullptr);
  ~UpstreamPool();
  UpstreamPool(const UpstreamPool&) = delete;
  UpstreamPool& operator=(const UpstreamPool&) = delete;

  // An idle, still-usable connection to `endpoint`, or nullptr when there is none (the
  // caller then opens a new one).
  [[nodiscard]] std::unique_ptr<UpstreamConnection> checkout(const UpstreamEndpoint& endpoint);

  // Keeps the connection for reuse, or closes it when pooling is off, the endpoint already
  // has enough idle connections, or the pool was closed.
  void checkin(std::unique_ptr<UpstreamConnection> connection);

  // Closes a connection that must not be reused (counted in Stats::discarded).
  void discard(std::unique_ptr<UpstreamConnection> connection);

  // Records that the caller opened a fresh connection.
  void noteCreated() noexcept { created_.fetch_add(1); }

  // Closes every idle connection and refuses further check-ins. Idempotent.
  void close();

  [[nodiscard]] bool closed() const;
  [[nodiscard]] std::size_t idleCount() const;
  [[nodiscard]] std::size_t idleCount(const UpstreamEndpoint& endpoint) const;
  [[nodiscard]] Stats stats() const;

  // Drops idle connections older than the idle timeout (all endpoints).
  void evictExpired();

  // True when `socket` is open and shows neither end-of-stream, an error nor pending
  // unsolicited data. Sends and consumes nothing. Exposed for tests.
  [[nodiscard]] static bool isUsable(boost::asio::ip::tcp::socket& socket);

 private:
  using Idle = std::deque<std::unique_ptr<UpstreamConnection>>;

  [[nodiscard]] std::chrono::steady_clock::time_point now() const;
  // Removes expired connections from every list; they are returned so that they are
  // closed outside the lock.
  void collectExpiredLocked(std::chrono::steady_clock::time_point at,
                            std::deque<std::unique_ptr<UpstreamConnection>>& out);

  const Settings settings_;
  const Clock clock_;
  mutable std::mutex mutex_;
  std::map<UpstreamEndpoint, Idle> idle_;
  bool closed_{false};
  std::size_t idle_total_{0};

  std::atomic<std::uint64_t> created_{0};
  std::atomic<std::uint64_t> reused_{0};
  std::atomic<std::uint64_t> returned_{0};
  std::atomic<std::uint64_t> discarded_{0};
  std::atomic<std::uint64_t> stale_dropped_{0};
  std::atomic<std::uint64_t> expired_dropped_{0};
};

}  // namespace edgeflow::proxy
