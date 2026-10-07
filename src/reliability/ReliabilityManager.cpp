#include "edgeflow/reliability/ReliabilityManager.hpp"

#include <utility>

namespace edgeflow::reliability {

const char* toString(AttemptKind kind) noexcept {
  switch (kind) {
    case AttemptKind::Success: return "success";
    case AttemptKind::RetryableStatus: return "retryable_status";
    case AttemptKind::NotSent: return "not_sent";
    case AttemptKind::MaybeProcessed: return "maybe_processed";
    case AttemptKind::Permanent: return "permanent";
    case AttemptKind::Cancelled: return "cancelled";
  }
  return "unknown";
}

const char* toString(RetryReason reason) noexcept {
  switch (reason) {
    case RetryReason::Retry: return "retry";
    case RetryReason::NotRetryable: return "not_retryable";
    case RetryReason::RetryDisabled: return "retry_disabled";
    case RetryReason::AttemptsExhausted: return "attempts_exhausted";
    case RetryReason::UnsafeMethod: return "unsafe_method";
  }
  return "unknown";
}

ReliabilityManager::ReliabilityManager(config::ReliabilityConfig config,
                                       std::shared_ptr<logging::Logger> logger, SteadyClock clock,
                                       Random random)
    : config_(std::move(config)),
      logger_(std::move(logger)),
      clock_(clock ? std::move(clock) : SteadyClock{[] { return std::chrono::steady_clock::now(); }}),
      retry_(config_.retry),
      backoff_(config_.retry.base_delay, config_.retry.max_delay, config_.retry.jitter_percent),
      random_(std::move(random)),
      engine_(std::random_device{}()) {
  if (config_.circuit_breaker.enabled) {
    CircuitBreakerSettings settings;
    settings.failure_threshold = config_.circuit_breaker.failure_threshold;
    settings.recovery_timeout = config_.circuit_breaker.recovery_timeout;
    settings.half_open_max_requests = config_.circuit_breaker.half_open_max_requests;
    breakers_ = std::make_unique<CircuitBreakerRegistry>(
        settings, clock_,
        [this](const std::string& key, CircuitState from, CircuitState to) {
          if (to == CircuitState::Open) opened_.fetch_add(1, std::memory_order_relaxed);
          if (!logger_) return;
          if (to == CircuitState::Open) {
            logger_->warn("circuit breaker {}: {} -> {}", key, toString(from), toString(to));
          } else {
            logger_->info("circuit breaker {}: {} -> {}", key, toString(from), toString(to));
          }
        });
  }
}

std::chrono::milliseconds ReliabilityManager::backoffDelay(unsigned retry_number) {
  double unit = 0.0;
  if (random_) {
    unit = random_();
  } else {
    const std::lock_guard lock(random_mutex_);
    unit = std::uniform_real_distribution<double>(0.0, 1.0)(engine_);
  }
  return backoff_.delay(retry_number, unit);
}

ReliabilityManager::Stats ReliabilityManager::stats() const {
  Stats s;
  s.attempts = attempts_.load(std::memory_order_relaxed);
  s.retries = retries_.load(std::memory_order_relaxed);
  s.failovers = failovers_.load(std::memory_order_relaxed);
  s.circuit_rejections = rejections_.load(std::memory_order_relaxed);
  s.circuit_opened = opened_.load(std::memory_order_relaxed);
  s.exhausted = exhausted_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace edgeflow::reliability
