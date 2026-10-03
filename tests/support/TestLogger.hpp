#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <spdlog/sinks/base_sink.h>

#include "edgeflow/logging/Logger.hpp"

namespace edgeflow::testing {

// In-memory sink, safe to read while server worker threads are logging.
class MemorySink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  [[nodiscard]] std::string text() {
    const std::lock_guard lock(mutex_);
    return text_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    text_.append(formatted.data(), formatted.size());
  }
  void flush_() override {}

 private:
  std::string text_;
};

// A logger that writes to memory so tests can assert on lifecycle events.
class CapturedLogger {
 public:
  explicit CapturedLogger(config::LogLevel level = config::LogLevel::Debug)
      : sink_(std::make_shared<MemorySink>()) {
    logging::LoggerOptions options;
    options.name = "test";
    options.level = level;
    options.sinks.push_back(sink_);
    logger_ = std::make_shared<logging::Logger>(std::move(options));
  }

  [[nodiscard]] std::shared_ptr<logging::Logger> logger() const { return logger_; }

  [[nodiscard]] std::string output() { return sink_->text(); }

  [[nodiscard]] bool contains(const std::string& text) {
    return output().find(text) != std::string::npos;
  }

  // Position of `text` in the output, or npos. Used to assert ordering.
  [[nodiscard]] std::size_t position(const std::string& text) { return output().find(text); }

 private:
  std::shared_ptr<MemorySink> sink_;
  std::shared_ptr<logging::Logger> logger_;
};

}  // namespace edgeflow::testing
