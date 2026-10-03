#include "edgeflow/discovery/NameResolver.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace edgeflow::discovery {

namespace net = boost::asio;
using tcp = net::ip::tcp;

NameResolver::NameResolver(LookupFunction lookup)
    : lookup_(lookup ? std::move(lookup)
                     : LookupFunction{[](const std::string& host, const std::string& port,
                                         boost::system::error_code& error) {
                         net::io_context scratch;  // a synchronous lookup needs no running loop
                         tcp::resolver resolver(scratch);
                         return resolver.resolve(host, port, error);
                       }}) {}

NameResolver::Ticket NameResolver::lookup(net::io_context& io, const std::string& host,
                                          const std::string& port, Handler handler) {
  const std::string key = host + ":" + port;
  std::unique_lock lock(mutex_);
  const Ticket ticket = next_ticket_++;

  if (const auto existing = in_flight_.find(key); existing != in_flight_.end()) {
    existing->second.push_back(Waiter{ticket, &io, std::move(handler)});  // share the running lookup
    return ticket;
  }
  if (threads_ >= kMaxConcurrentLookups) {
    lock.unlock();
    net::post(io, [handler = std::move(handler)] {
      handler(net::error::make_error_code(net::error::try_again), Results{});
    });
    return ticket;
  }

  in_flight_[key].push_back(Waiter{ticket, &io, std::move(handler)});
  ++threads_;
  lock.unlock();
  // Detached on purpose: getaddrinfo() cannot be cancelled, so waiting for it (for example
  // in a destructor) could block shutdown for as long as the resolver takes. The thread
  // owns a shared_ptr to this object, so it stays valid however long the lookup takes.
  std::thread([self = shared_from_this(), key, host, port] { self->run(key, host, port); }).detach();
  return ticket;
}

void NameResolver::run(const std::string& key, const std::string& host, const std::string& port) {
  boost::system::error_code error;
  Results results;
  try {
    results = lookup_(host, port, error);
  } catch (...) {
    error = net::error::make_error_code(net::error::host_not_found_try_again);
  }

  const std::lock_guard lock(mutex_);
  auto entry = in_flight_.find(key);
  if (entry != in_flight_.end()) {
    // Delivering under the lock is what makes cancel() airtight: a waiter that cancelled
    // is no longer listed, so nothing is posted to an io_context that may be gone.
    for (auto& waiter : entry->second) {
      net::post(*waiter.io, [handler = std::move(waiter.handler), error, results] {
        handler(error, results);
      });
    }
    in_flight_.erase(entry);
  }
  --threads_;
}

void NameResolver::cancel(Ticket ticket) {
  const std::lock_guard lock(mutex_);
  for (auto& entry : in_flight_) {
    auto& waiters = entry.second;
    waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
                                 [ticket](const Waiter& w) { return w.ticket == ticket; }),
                  waiters.end());
  }
}

unsigned NameResolver::activeLookups() {
  const std::lock_guard lock(mutex_);
  return threads_;
}

}  // namespace edgeflow::discovery
