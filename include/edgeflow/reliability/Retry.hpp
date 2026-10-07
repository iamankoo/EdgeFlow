#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "edgeflow/config/Config.hpp"

namespace edgeflow::reliability {

// How one upstream attempt ended, in the terms the retry policy needs. The reliability layer
// knows nothing about sockets or HTTP parsing: the proxy classifies what happened.
enum class AttemptKind {
  Success,         // a final answer: any backend status that is not configured as retryable
  RetryableStatus, // the backend answered with a status configured as retryable
  NotSent,         // the request provably never reached the backend (not resolved, connection
                   // refused/unreachable, connect timeout): no byte of it was processed
  MaybeProcessed,  // the backend may have received (part of) the request: timeout, reset or close
                   // before a complete answer, an answer that is not valid HTTP
  Permanent,       // failed in a way another attempt would not change (response too large, internal)
  Cancelled        // the client went away or the proxy is stopping
};

[[nodiscard]] const char* toString(AttemptKind kind) noexcept;

enum class RetryReason {
  Retry,             // retrying is allowed
  NotRetryable,      // the outcome is final (success, permanent failure, cancellation)
  RetryDisabled,     // retries are switched off
  AttemptsExhausted, // max_attempts attempts were made
  UnsafeMethod       // the backend may have processed the request and the method is not on the
                     // retryable list
};

[[nodiscard]] const char* toString(RetryReason reason) noexcept;

struct RetryDecision {
  bool retry{false};
  RetryReason reason{RetryReason::NotRetryable};
};

// Decides whether a failed attempt may be followed by another. Immutable after construction
// and therefore freely shared between threads.
//
// The safety rule: a request is retried after the backend MAY have processed it only if its
// method is on `retryable_methods` (default GET, HEAD, OPTIONS: no side effects by
// definition). A request that provably never reached a backend (NotSent) is retried whatever
// its method. `max_attempts` counts ALL attempts, the first included, and is bounded by the
// configuration (1..10); there is no code path that retries without consulting it.
class RetryPolicy {
 public:
  explicit RetryPolicy(const config::RetryConfig& config)
      : enabled_(config.enabled),
        max_attempts_(std::max(1U, config.max_attempts)),
        statuses_(config.retryable_statuses),
        methods_(config.retryable_methods) {}

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  // Attempts a request may make in total: 1 when retries are disabled.
  [[nodiscard]] unsigned maxAttempts() const noexcept { return enabled_ ? max_attempts_ : 1U; }

  [[nodiscard]] bool isRetryableStatus(unsigned status) const noexcept {
    return std::find(statuses_.begin(), statuses_.end(), status) != statuses_.end();
  }
  [[nodiscard]] bool isRetryableMethod(std::string_view method) const noexcept {
    return std::any_of(methods_.begin(), methods_.end(),
                       [&](const std::string& m) { return m == method; });
  }

  // Classifies a backend answer.
  [[nodiscard]] AttemptKind classifyStatus(unsigned status) const noexcept {
    return isRetryableStatus(status) ? AttemptKind::RetryableStatus : AttemptKind::Success;
  }

  // `attempts_made` counts the attempt that just ended.
  [[nodiscard]] RetryDecision decide(AttemptKind kind, std::string_view method,
                                     unsigned attempts_made) const noexcept {
    switch (kind) {
      case AttemptKind::Success:
      case AttemptKind::Permanent:
      case AttemptKind::Cancelled:
        return {false, RetryReason::NotRetryable};
      case AttemptKind::NotSent:
      case AttemptKind::MaybeProcessed:
      case AttemptKind::RetryableStatus:
        break;
    }
    if (!enabled_) return {false, RetryReason::RetryDisabled};
    if (attempts_made >= max_attempts_) return {false, RetryReason::AttemptsExhausted};
    if (kind != AttemptKind::NotSent && !isRetryableMethod(method)) {
      return {false, RetryReason::UnsafeMethod};
    }
    return {true, RetryReason::Retry};
  }

 private:
  bool enabled_;
  unsigned max_attempts_;
  std::vector<unsigned> statuses_;
  std::vector<std::string> methods_;
};

// Exponential backoff: the pause before retry n (n = 1 is the first retry) is
//   min(max_delay, base_delay * 2^(n-1))
// reduced by a random fraction of at most `jitter_percent` percent (so it never exceeds the
// nominal delay and retries of many requests do not line up). Overflow-safe for any n.
class BackoffPolicy {
 public:
  BackoffPolicy(std::chrono::milliseconds base_delay, std::chrono::milliseconds max_delay,
                unsigned jitter_percent)
      : base_(std::max<std::int64_t>(0, base_delay.count())),
        max_(std::max<std::int64_t>(base_, max_delay.count())),
        jitter_percent_(std::min(100U, jitter_percent)) {}

  [[nodiscard]] std::chrono::milliseconds nominal(unsigned retry_number) const noexcept {
    if (retry_number == 0) return std::chrono::milliseconds{0};
    const unsigned shift = retry_number - 1;
    if (base_ == 0) return std::chrono::milliseconds{0};
    // base * 2^shift > max  <=>  base > max >> shift (and shifts >= 62 always exceed).
    if (shift >= 62 || base_ > (max_ >> shift)) return std::chrono::milliseconds{max_};
    return std::chrono::milliseconds{std::min(max_, base_ << shift)};
  }

  // `unit` is a random number in [0, 1); values outside are clamped.
  [[nodiscard]] std::chrono::milliseconds delay(unsigned retry_number, double unit) const noexcept {
    const auto nominal_ms = nominal(retry_number).count();
    const double u = std::min(1.0, std::max(0.0, unit));
    const double factor = 1.0 - (static_cast<double>(jitter_percent_) / 100.0) * u;
    const auto jittered = static_cast<std::int64_t>(static_cast<double>(nominal_ms) * factor);
    return std::chrono::milliseconds{std::min(nominal_ms, std::max<std::int64_t>(0, jittered))};
  }

 private:
  std::int64_t base_;
  std::int64_t max_;
  unsigned jitter_percent_;
};

}  // namespace edgeflow::reliability
