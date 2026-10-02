#pragma once

#include <memory>
#include <sstream>
#include <string>

#include <spdlog/sinks/ostream_sink.h>

#include "edgeflow/logging/Logger.hpp"

namespace edgeflow::testing {

// A logger that writes to an in-memory buffer so tests can assert on lifecycle events.
class CapturedLogger {
 public:
  explicit CapturedLogger(config::LogLevel level = config::LogLevel::Debug) {
    logging::LoggerOptions options;
    options.name = "test";
    options.level = level;
    options.sinks.push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(buffer_));
    logger_ = std::make_shared<logging::Logger>(std::move(options));
  }

  [[nodiscard]] std::shared_ptr<logging::Logger> logger() const { return logger_; }

  [[nodiscard]] std::string output() {
    logger_->flush();
    return buffer_.str();
  }

  [[nodiscard]] bool contains(const std::string& text) { return output().find(text) != std::string::npos; }

  // Position of `text` in the output, or npos. Used to assert ordering.
  [[nodiscard]] std::size_t position(const std::string& text) { return output().find(text); }

 private:
  std::ostringstream buffer_;
  std::shared_ptr<logging::Logger> logger_;
};

}  // namespace edgeflow::testing
