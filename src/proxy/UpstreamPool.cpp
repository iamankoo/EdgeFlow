#include "edgeflow/proxy/UpstreamPool.hpp"

#include <iterator>
#include <utility>

namespace edgeflow::proxy {

namespace net = boost::asio;

UpstreamPool::UpstreamPool(Settings settings, Clock clock)
    : settings_(settings), clock_(std::move(clock)) {}

UpstreamPool::~UpstreamPool() { close(); }

std::chrono::steady_clock::time_point UpstreamPool::now() const {
  return clock_ ? clock_() : std::chrono::steady_clock::now();
}

bool UpstreamPool::isUsable(net::ip::tcp::socket& socket) {
  if (!socket.is_open()) return false;
  boost::system::error_code error;
  // Non-blocking so the peek below can never wait. Asio's asynchronous operations work the
  // same with the flag set, and this socket is only ever used asynchronously afterwards.
  socket.non_blocking(true, error);
  if (error) return false;
  char byte = 0;
  socket.receive(net::buffer(&byte, 1), net::socket_base::message_peek, error);
  // Nothing to read and no error: an idle, healthy connection. End of stream (the backend
  // closed it), a reset, or bytes nobody asked for all make it unfit for a new request.
  return error == net::error::would_block || error == net::error::try_again;
}

void UpstreamPool::collectExpiredLocked(std::chrono::steady_clock::time_point at,
                                        std::deque<std::unique_ptr<UpstreamConnection>>& out) {
  for (auto it = idle_.begin(); it != idle_.end();) {
    auto& list = it->second;
    // Oldest connections are at the back (most recently used first).
    while (!list.empty() &&
           at - list.back()->idle_since >= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               settings_.idle_timeout)) {
      out.push_back(std::move(list.back()));
      list.pop_back();
      --idle_total_;
      expired_dropped_.fetch_add(1);
      discarded_.fetch_add(1);
    }
    it = list.empty() ? idle_.erase(it) : std::next(it);
  }
}

std::unique_ptr<UpstreamConnection> UpstreamPool::checkout(const UpstreamEndpoint& endpoint) {
  std::deque<std::unique_ptr<UpstreamConnection>> to_close;  // closed outside the lock
  std::unique_ptr<UpstreamConnection> found;
  for (;;) {
    std::unique_ptr<UpstreamConnection> candidate;
    {
      const std::lock_guard lock(mutex_);
      if (closed_) break;
      collectExpiredLocked(now(), to_close);
      const auto it = idle_.find(endpoint);
      if (it == idle_.end()) break;
      candidate = std::move(it->second.front());  // most recently used
      it->second.pop_front();
      --idle_total_;
      if (it->second.empty()) idle_.erase(it);
    }
    if (isUsable(candidate->socket)) {
      found = std::move(candidate);
      reused_.fetch_add(1);
      break;
    }
    stale_dropped_.fetch_add(1);
    discarded_.fetch_add(1);
    to_close.push_back(std::move(candidate));  // the next idle one (if any) is tried
  }
  to_close.clear();
  return found;
}

void UpstreamPool::checkin(std::unique_ptr<UpstreamConnection> connection) {
  if (!connection) return;
  std::deque<std::unique_ptr<UpstreamConnection>> to_close;
  bool kept = false;
  {
    const std::lock_guard lock(mutex_);
    const auto at = now();
    collectExpiredLocked(at, to_close);
    if (!closed_ && settings_.max_idle_per_endpoint > 0) {
      auto& list = idle_[connection->endpoint];
      if (list.size() < settings_.max_idle_per_endpoint) {
        connection->idle_since = at;
        list.push_front(std::move(connection));
        ++idle_total_;
        kept = true;
      } else if (list.empty()) {
        idle_.erase(connection->endpoint);
      }
    }
  }
  if (kept) {
    returned_.fetch_add(1);
  } else {
    discarded_.fetch_add(1);
    to_close.push_back(std::move(connection));
  }
  to_close.clear();
}

void UpstreamPool::discard(std::unique_ptr<UpstreamConnection> connection) {
  if (!connection) return;
  discarded_.fetch_add(1);
  connection.reset();  // closes the socket
}

void UpstreamPool::close() {
  std::deque<std::unique_ptr<UpstreamConnection>> to_close;
  {
    const std::lock_guard lock(mutex_);
    closed_ = true;
    for (auto& [endpoint, list] : idle_) {
      (void)endpoint;
      for (auto& connection : list) to_close.push_back(std::move(connection));
    }
    idle_.clear();
    idle_total_ = 0;
  }
  discarded_.fetch_add(to_close.size());
  to_close.clear();
}

bool UpstreamPool::closed() const {
  const std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t UpstreamPool::idleCount() const {
  const std::lock_guard lock(mutex_);
  return idle_total_;
}

std::size_t UpstreamPool::idleCount(const UpstreamEndpoint& endpoint) const {
  const std::lock_guard lock(mutex_);
  const auto it = idle_.find(endpoint);
  return it == idle_.end() ? 0 : it->second.size();
}

UpstreamPool::Stats UpstreamPool::stats() const {
  Stats stats;
  stats.created = created_.load();
  stats.reused = reused_.load();
  stats.returned = returned_.load();
  stats.discarded = discarded_.load();
  stats.stale_dropped = stale_dropped_.load();
  stats.expired_dropped = expired_dropped_.load();
  return stats;
}

void UpstreamPool::evictExpired() {
  std::deque<std::unique_ptr<UpstreamConnection>> to_close;
  {
    const std::lock_guard lock(mutex_);
    collectExpiredLocked(now(), to_close);
  }
  to_close.clear();
}

}  // namespace edgeflow::proxy
