#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace edgeflow::config {

enum class Environment { Development, Staging, Production };

enum class LogLevel { Trace, Debug, Info, Warn, Error, Critical, Off };

struct ApplicationConfig {
  std::string name{"edgeflow"};
  Environment environment{Environment::Development};
  LogLevel log_level{LogLevel::Info};
};

struct ServerConfig {
  std::string host{"0.0.0.0"};
  std::uint16_t port{8080};
  // Max time to wait for a request to finish arriving once its first byte was read
  // (and to write a response). Exceeding it yields 408 for requests.
  std::chrono::milliseconds request_timeout{5000};
  // Max time an idle connection (no request in progress) is kept open.
  std::chrono::milliseconds keep_alive_timeout{10000};
  std::size_t max_request_body_bytes{1024 * 1024};
  std::size_t max_header_bytes{8192};
  std::size_t max_connections{1024};
  unsigned worker_threads{2};  // threads running the io_context
};

// PostgreSQL connection settings for the service registry (Phase 3). The password is
// never stored in configuration: `password_env` names the environment variable that
// holds it (empty means no password).
struct DatabaseConfig {
  bool enabled{false};
  std::string host{"127.0.0.1"};
  std::uint16_t port{5432};
  std::string name{"edgeflow"};
  std::string user{"edgeflow"};
  std::string password_env{"EDGEFLOW_DB_PASSWORD"};
  unsigned pool_size{4};
  std::chrono::seconds connect_timeout{5};
};

enum class HealthCheckType { Tcp, Http };

enum class RoutingStrategy { RoundRobin, LeastConnections, Weighted, ConsistentHashing };

// Active health checking of registered service instances (Phase 4). Requires the service
// registry (`database.enabled`): it reads the instances from it and writes health back.
struct HealthCheckConfig {
  bool enabled{false};
  HealthCheckType type{HealthCheckType::Tcp};
  // Pause between the end of one probe of an instance and the start of the next.
  std::chrono::milliseconds interval{5000};
  // Upper bound for one whole probe (resolve + connect + request + response). Must not
  // exceed `interval`.
  std::chrono::milliseconds timeout{2000};
  std::string http_path{"/health"};  // used by the http type
  // Consecutive failed probes before a healthy instance becomes unhealthy.
  unsigned failure_threshold{3};
  // Consecutive successful probes before an unhealthy instance becomes healthy again.
  unsigned success_threshold{2};
  // How often the registry is re-read to notice added, removed, changed and disabled
  // instances.
  std::chrono::milliseconds refresh_interval{5000};
  // Upper bound for probes running at the same time.
  unsigned max_concurrent_checks{32};
};

// How an instance is chosen among the routable ones (Phase 5).
struct RoutingConfig {
  RoutingStrategy strategy{RoutingStrategy::RoundRobin};
};

// Reverse proxy (Phase 6): forwards `/proxy/{service}/...` to a healthy instance chosen by
// the router. Requires the service registry (`database.enabled`): the proxy only ever
// forwards to what routing selects from the routable (active AND healthy) instances.
struct ProxyConfig {
  bool enabled{false};
  // Bound for establishing the TCP connection to a backend (name resolution + connect).
  // Exceeding it answers 504.
  std::chrono::milliseconds connect_timeout{2000};
  // Bound for the WHOLE upstream exchange (connect + send request + receive the complete
  // response). Exceeding it answers 504. Must not be smaller than `connect_timeout`.
  std::chrono::milliseconds upstream_timeout{30000};
  unsigned io_threads{2};  // threads running upstream I/O (separate from server.worker_threads)
  // Idle keep-alive connections kept per backend (host:port); 0 disables connection reuse.
  unsigned max_idle_connections{32};
  // An idle pooled connection older than this is closed instead of being reused.
  std::chrono::milliseconds idle_timeout{30000};
  // A backend response larger than this (body bytes) is refused with 502.
  std::size_t max_response_bytes{16 * 1024 * 1024};
};

// Phase 7 reliability engineering around the reverse proxy (see edgeflow/reliability/).
// Off by default: with it off the proxy makes exactly one attempt per request (Phase 6).
// Requires `proxy.enabled`.
struct RetryConfig {
  bool enabled{true};
  // TOTAL number of attempts per request, the first one included: 1 means "never retry",
  // 3 means "one try and at most two retries".
  unsigned max_attempts{3};
  // Pause before retry n (n = 1 is the first retry): min(max_delay, base_delay * 2^(n-1)),
  // reduced by up to `jitter_percent` percent (so never above the nominal delay).
  std::chrono::milliseconds base_delay{100};
  std::chrono::milliseconds max_delay{2000};
  unsigned jitter_percent{20};  // 0..100; 0 = no jitter
  // A backend answer with one of these statuses counts as a failed attempt (and may be
  // retried); every other status is the application's answer and is forwarded as it is.
  std::vector<unsigned> retryable_statuses{502, 503, 504};
  // Methods whose requests may be retried after the backend may already have received them.
  // A request that provably never reached a backend (connection refused, name not resolved,
  // connect timeout) is retried whatever the method.
  std::vector<std::string> retryable_methods{"GET", "HEAD", "OPTIONS"};
};

struct CircuitBreakerConfig {
  bool enabled{true};
  // Consecutive failed requests (per backend instance) that open the circuit.
  unsigned failure_threshold{5};
  // How long a circuit stays open before one bounded probe is allowed (half-open).
  std::chrono::milliseconds recovery_timeout{30000};
  // Probe requests allowed in flight while half-open; that many successes close the circuit,
  // one failure reopens it.
  unsigned half_open_max_requests{1};
};

struct ReliabilityTimeoutConfig {
  // Bound for the WHOLE proxied request: every attempt, every backoff pause. An attempt gets at
  // most min(proxy.upstream_timeout_ms, what is left of this budget).
  std::chrono::milliseconds total_timeout{30000};
};

struct ReliabilityConfig {
  bool enabled{false};
  ReliabilityTimeoutConfig timeout;
  RetryConfig retry;
  CircuitBreakerConfig circuit_breaker;
};

struct ShutdownConfig {
  std::chrono::seconds grace_period{5};
};

struct Config {
  ApplicationConfig application;
  ServerConfig server;
  DatabaseConfig database;
  HealthCheckConfig health_check;
  RoutingConfig routing;
  ProxyConfig proxy;
  ReliabilityConfig reliability;
  ShutdownConfig shutdown;
};

[[nodiscard]] std::string_view toString(Environment environment) noexcept;
[[nodiscard]] std::string_view toString(LogLevel level) noexcept;
[[nodiscard]] std::string_view toString(HealthCheckType type) noexcept;
[[nodiscard]] std::string_view toString(RoutingStrategy strategy) noexcept;

}  // namespace edgeflow::config
