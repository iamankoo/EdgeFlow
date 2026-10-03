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

struct ShutdownConfig {
  std::chrono::seconds grace_period{5};
};

struct Config {
  ApplicationConfig application;
  ServerConfig server;
  ShutdownConfig shutdown;
};

[[nodiscard]] std::string_view toString(Environment environment) noexcept;
[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

}  // namespace edgeflow::config
