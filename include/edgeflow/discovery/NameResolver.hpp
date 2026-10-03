#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio.hpp>

namespace edgeflow::discovery {

// Resolves host names for health probes without letting one slow lookup delay another.
//
// Why this exists: Asio's built-in resolver runs getaddrinfo() one at a time on a single
// background thread. A lookup that hangs for seconds (a deleted container's DNS name, a
// broken resolver) then stalls the lookups of every other instance queued behind it, and
// healthy instances time out and are wrongly marked unhealthy. getaddrinfo() cannot be
// cancelled, so isolation is the only remedy:
//   - every distinct host is looked up on its own thread, so a slow host delays only itself;
//   - concurrent lookups of the same host share one lookup instead of piling up threads;
//   - the number of simultaneous lookups is capped; beyond it a lookup fails at once with
//     `try_again` (the probe is reported unhealthy) instead of queueing behind slow ones.
// Results are delivered by posting to the caller's io_context. A lookup that is cancelled
// (the probe timed out or the instance was removed) is never delivered, so a result can
// never reach an io_context that has been torn down.
class NameResolver : public std::enable_shared_from_this<NameResolver> {
 public:
  using Results = boost::asio::ip::tcp::resolver::results_type;
  using Handler = std::function<void(const boost::system::error_code&, const Results&)>;
  using Ticket = std::uint64_t;
  // The blocking lookup; replaceable so tests can simulate slow or failing DNS.
  using LookupFunction = std::function<Results(const std::string& host, const std::string& port,
                                               boost::system::error_code&)>;

  static constexpr unsigned kMaxConcurrentLookups = 16;

  explicit NameResolver(LookupFunction lookup = nullptr);
  ~NameResolver() = default;
  NameResolver(const NameResolver&) = delete;
  NameResolver& operator=(const NameResolver&) = delete;

  // Starts (or joins) a lookup; `handler` runs on `io` unless cancelled first. Must be
  // called on the thread running `io`. Returns a ticket for cancel().
  [[nodiscard]] Ticket lookup(boost::asio::io_context& io, const std::string& host,
                              const std::string& port, Handler handler);

  // After cancel() the handler is never invoked. Safe for an unknown or finished ticket.
  void cancel(Ticket ticket);

  // Lookups currently running (for tests).
  [[nodiscard]] unsigned activeLookups();

 private:
  struct Waiter {
    Ticket ticket;
    boost::asio::io_context* io;
    Handler handler;
  };

  void run(const std::string& key, const std::string& host, const std::string& port);

  const LookupFunction lookup_;
  std::mutex mutex_;
  std::map<std::string, std::vector<Waiter>> in_flight_;  // keyed by "host:port"
  Ticket next_ticket_{1};
  unsigned threads_{0};
};

}  // namespace edgeflow::discovery
