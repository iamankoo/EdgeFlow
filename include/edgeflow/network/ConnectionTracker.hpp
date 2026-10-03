#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace edgeflow::network {

class HttpConnection;

// Thread-safe registry of live connections. It holds weak references only, so it
// never extends a connection's lifetime; connections deregister themselves when
// destroyed. Used for the connection limit and for draining on shutdown.
class ConnectionTracker {
 public:
  void add(std::uint64_t id, std::weak_ptr<HttpConnection> connection);
  void remove(std::uint64_t id) noexcept;

  [[nodiscard]] std::size_t size() const;
  // Strong references to every connection still alive at the time of the call.
  [[nodiscard]] std::vector<std::shared_ptr<HttpConnection>> snapshot() const;
  // Returns true if the registry became empty within `timeout`.
  [[nodiscard]] bool waitUntilEmpty(std::chrono::milliseconds timeout);

 private:
  mutable std::mutex mutex_;
  std::condition_variable empty_;
  std::unordered_map<std::uint64_t, std::weak_ptr<HttpConnection>> connections_;
};

}  // namespace edgeflow::network
