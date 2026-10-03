#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

struct ShutdownConfig {
  std::chrono::seconds grace_period{5};
};

struct Config {
  ApplicationConfig application;
  ServerConfig server;
  DatabaseConfig database;
  HealthCheckConfig health_check;
  RoutingConfig routing;
  ShutdownConfig shutdown;
};

[[nodiscard]] std::string_view toString(Environment environment) noexcept;
[[nodiscard]] std::string_view toString(LogLevel level) noexcept;
[[nodiscard]] std::string_view toString(HealthCheckType type) noexcept;
[[nodiscard]] std::string_view toString(RoutingStrategy strategy) noexcept;

}  // namespace edgeflow::config
