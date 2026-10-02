#include "edgeflow/logging/Logger.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace edgeflow::logging {

namespace {

spdlog::level::level_enum toSpdlog(config::LogLevel level) noexcept {
  switch (level) {
    case config::LogLevel::Trace: return spdlog::level::trace;
    case config::LogLevel::Debug: return spdlog::level::debug;
    case config::LogLevel::Info: return spdlog::level::info;
    case config::LogLevel::Warn: return spdlog::level::warn;
    case config::LogLevel::Error: return spdlog::level::err;
    case config::LogLevel::Critical: return spdlog::level::critical;
    case config::LogLevel::Off: return spdlog::level::off;
  }
  return spdlog::level::info;
}

config::LogLevel fromSpdlog(spdlog::level::level_enum level) noexcept {
  switch (level) {
    case spdlog::level::trace: return config::LogLevel::Trace;
    case spdlog::level::debug: return config::LogLevel::Debug;
    case spdlog::level::info: return config::LogLevel::Info;
    case spdlog::level::warn: return config::LogLevel::Warn;
    case spdlog::level::err: return config::LogLevel::Error;
    case spdlog::level::critical: return config::LogLevel::Critical;
    default: return config::LogLevel::Off;
  }
}

}  // namespace

Logger::Logger(LoggerOptions options) {
  if (options.sinks.empty()) {
    options.sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }
  logger_ = std::make_shared<spdlog::logger>(options.name, options.sinks.begin(),
                                             options.sinks.end());
  logger_->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] %v");
  logger_->set_level(toSpdlog(options.level));
  logger_->flush_on(spdlog::level::warn);
}

void Logger::setLevel(config::LogLevel level) { logger_->set_level(toSpdlog(level)); }

config::LogLevel Logger::level() const noexcept { return fromSpdlog(logger_->level()); }

void Logger::flush() { logger_->flush(); }

}  // namespace edgeflow::logging
