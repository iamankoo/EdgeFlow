#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/discovery/NameResolver.hpp"

namespace edgeflow::discovery {

struct ProbeTarget {
  std::string host;  // IP literal or resolvable name
  std::uint16_t port{0};
  std::string http_path{"/health"};  // used by the HTTP prober only
  // Bound for the WHOLE probe: resolve + connect + request + response header.
  std::chrono::milliseconds timeout{2000};
};

struct ProbeResult {
  bool healthy{false};
  // Short human-readable reason, e.g. "connected", "connection refused", "HTTP 503",
  // "timed out after 2000ms". Never contains credentials.
  std::string detail;
};

// Cancels a running probe. Safe to call after it has finished.
class ProbeHandle {
 public:
  virtual ~ProbeHandle() = default;
  // After cancel() the completion callback is never invoked. Must be called on the thread
  // running the io_context.
  virtual void cancel() noexcept = 0;
};

// Executes one health probe asynchronously.
//
// Classification:
//   TCP   healthy  <=> a TCP connection was established within the timeout. This proves
//                      only that something accepts connections on that port, NOT that the
//                      application behind it works.
//   HTTP  healthy  <=> a complete response status line and headers arrived within the
//                      timeout AND the status is 2xx. 1xx, 3xx (redirects are not
//                      followed), 4xx, 5xx, malformed responses, resets and timeouts are
//                      all unhealthy.
// Every probe is bounded by `target.timeout`; on expiry all pending operations are
// cancelled and the result is unhealthy.
class Prober {
 public:
  virtual ~Prober() = default;

  // Starts a probe on `io`. `done` is invoked exactly once on the io_context's thread,
  // unless the returned handle is cancelled first. The io_context must be run by a single
  // thread (or the caller must serialise): the probe holds no locks.
  [[nodiscard]] virtual std::shared_ptr<ProbeHandle> start(
      boost::asio::io_context& io, ProbeTarget target,
      std::function<void(ProbeResult)> done) = 0;
};

// `names` resolves host names; probers share one resolver so that a slow lookup of one
// host never delays another (see NameResolver). Defaults to a fresh one.
[[nodiscard]] std::shared_ptr<Prober> makeProber(config::HealthCheckType type,
                                                 std::shared_ptr<NameResolver> names = nullptr);

}  // namespace edgeflow::discovery
