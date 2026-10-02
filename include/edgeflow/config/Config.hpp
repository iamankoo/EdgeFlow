#pragma once

#include <chrono>
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
