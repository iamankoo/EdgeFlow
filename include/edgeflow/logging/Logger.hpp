#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "edgeflow/config/Config.hpp"

namespace edgeflow::logging {

struct LoggerOptions {
  std::string name{"edgeflow"};
  config::LogLevel level{config::LogLevel::Info};
  // Empty means "colored console on stdout".
  std::vector<spdlog::sink_ptr> sinks;
};

// Thin wrapper over a private (non-registered) spdlog logger, so application
// code depends on this type rather than on logger setup details or global state.
class Logger {
 public:
  explicit Logger(LoggerOptions options = {});

  template <typename... Args>
  void debug(spdlog::format_string_t<Args...> format, Args&&... args) {
    logger_->debug(format, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void info(spdlog::format_string_t<Args...> format, Args&&... args) {
    logger_->info(format, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void warn(spdlog::format_string_t<Args...> format, Args&&... args) {
    logger_->warn(format, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void error(spdlog::format_string_t<Args...> format, Args&&... args) {
    logger_->error(format, std::forward<Args>(args)...);
  }

  void setLevel(config::LogLevel level);
  [[nodiscard]] config::LogLevel level() const noexcept;
  void flush();

 private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace edgeflow::logging
