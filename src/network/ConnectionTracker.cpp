#include "edgeflow/network/ConnectionTracker.hpp"

#include <utility>

#include "edgeflow/network/HttpConnection.hpp"

namespace edgeflow::network {

void ConnectionTracker::add(std::uint64_t id, std::weak_ptr<HttpConnection> connection) {
  const std::lock_guard lock(mutex_);
  connections_.emplace(id, std::move(connection));
}

void ConnectionTracker::remove(std::uint64_t id) noexcept {
  const std::lock_guard lock(mutex_);
  connections_.erase(id);
  if (connections_.empty()) empty_.notify_all();
}

std::size_t ConnectionTracker::size() const {
  const std::lock_guard lock(mutex_);
  return connections_.size();
}

std::vector<std::shared_ptr<HttpConnection>> ConnectionTracker::snapshot() const {
  std::vector<std::shared_ptr<HttpConnection>> alive;
  const std::lock_guard lock(mutex_);
  alive.reserve(connections_.size());
  for (const auto& entry : connections_) {
    if (auto connection = entry.second.lock()) alive.push_back(std::move(connection));
  }
  return alive;
}

bool ConnectionTracker::waitUntilEmpty(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  return empty_.wait_for(lock, timeout, [this] { return connections_.empty(); });
}

}  // namespace edgeflow::network
