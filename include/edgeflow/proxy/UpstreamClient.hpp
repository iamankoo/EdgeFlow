#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>

#include "edgeflow/discovery/NameResolver.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/Http.hpp"
#include "edgeflow/proxy/UpstreamPool.hpp"

namespace edgeflow::proxy {

enum class UpstreamError {
  Resolve,         // the backend's host name could not be resolved
  Connect,         // the TCP connection was refused, reset or unreachable
  ConnectTimeout,  // connecting took longer than the connect timeout
  Timeout,         // the exchange took longer than the upstream timeout
  Closed,          // the connection closed or failed before a complete response arrived
  Malformed,       // the backend did not answer with valid HTTP
  TooLarge,        // the response body exceeds the configured maximum
  Cancelled,       // the caller cancelled the exchange
  Internal         // unexpected local failure
};

[[nodiscard]] const char* toString(UpstreamError error) noexcept;

struct UpstreamFailure {
  UpstreamError error{UpstreamError::Internal};
  std::string detail;
};

struct UpstreamResult {
  std::optional<UpstreamFailure> failure;  // empty on success
  network::HttpResponse response;          // valid on success
  bool reused_connection{false};           // the request went out on a pooled connection
  // A pooled connection turned out dead before any request byte was sent; it was discarded
  // and the request went out on a fresh connection instead (connection-pool correctness,
  // not a retry policy).
  bool stale_connection_replaced{false};
  bool connection_pooled{false};  // the connection was kept for reuse after the response

  [[nodiscard]] bool ok() const noexcept { return !failure.has_value(); }
};

struct UpstreamSettings {
  std::chrono::milliseconds connect_timeout{2000};
  std::chrono::milliseconds upstream_timeout{30000};
  std::size_t max_response_bytes{16 * 1024 * 1024};
};

// Handle to a running exchange.
class UpstreamCall {
 public:
  virtual ~UpstreamCall() = default;
  // Aborts the exchange; its callback then reports UpstreamError::Cancelled (unless it had
  // already finished). Thread-safe, idempotent.
  virtual void cancel() noexcept = 0;
};

// Sends ONE request to ONE backend and returns the complete response (buffered, bounded by
// `max_response_bytes`). It never retries and never picks a backend: choosing is routing's
// job, retrying is not part of this phase.
//
// The exchange:
//   1. take an idle pooled connection to the backend, or open a new one (resolve + connect,
//      bounded by connect_timeout);
//   2. write the request; read the response;
//   3. keep the connection for reuse only if the response was complete, allowed keep-alive
//      and left nothing unread; close it otherwise.
// One deadline (upstream_timeout) covers all of it. Every exchange calls its callback exactly
// once. The callback runs on one of the client's I/O threads.
//
// The only repeat attempt, by design: a POOLED connection that is found dead before the first
// request byte was written (detected at check-out, or a write that failed having written
// nothing) is discarded and the request goes out on a freshly opened connection. After the
// first byte was written, or on a fresh connection, a failure is final.
class UpstreamClient {
 public:
  using Callback = std::function<void(UpstreamResult)>;

  UpstreamClient(boost::asio::io_context& io, std::shared_ptr<UpstreamPool> pool,
                 std::shared_ptr<discovery::NameResolver> names, UpstreamSettings settings,
                 std::shared_ptr<logging::Logger> logger);

  // Starts the exchange. `request` is sent as given (the caller prepared the headers).
  [[nodiscard]] std::shared_ptr<UpstreamCall> send(UpstreamEndpoint endpoint,
                                                   network::HttpRequest request, Callback done);

 private:
  boost::asio::io_context& io_;
  const std::shared_ptr<UpstreamPool> pool_;
  const std::shared_ptr<discovery::NameResolver> names_;
  const UpstreamSettings settings_;
  const std::shared_ptr<logging::Logger> logger_;
};

}  // namespace edgeflow::proxy
