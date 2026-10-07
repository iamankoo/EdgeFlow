#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/reliability/CircuitBreaker.hpp"
#include "edgeflow/reliability/Failover.hpp"
#include "edgeflow/reliability/Retry.hpp"

namespace edgeflow::reliability {

// The Phase 7 reliability layer around the proxy's upstream attempts. It holds the policies
// and the shared circuit-breaker state; it performs no I/O and knows nothing about HTTP or
// sockets (the proxy classifies attempts and acts on the decisions).
//
//   timeouts         total request budget (per-attempt and connect limits are the proxy's)
//   retry policy     RetryPolicy: which failures, which methods, how many attempts
//   backoff          BackoffPolicy: exponential, capped, jittered; see backoffDelay()
//   circuit breaker  one CircuitBreaker per backend instance
//   failover         AttemptLedger (per request) + the Router filter built by the proxy
//
// Relationship to health checking (Phase 4): the health checker is the SLOW, ACTIVE view
// ("does the backend answer probes?") and decides the routable set in the registry; the
// circuit breaker is the FAST, PASSIVE view ("are real requests failing right now?") and only
// ever withholds traffic temporarily. They share no state: a backend is chosen only if the
// registry lists it routable AND its circuit admits a request; when its circuit closes again
// it is chosen again, when the health checker drops it, it is not, whatever the circuit says.
//
// Thread safety: every member may be used concurrently.
class ReliabilityManager {
 public:
  using Random = std::function<double()>;  // uniform in [0, 1)

  struct Stats {
    std::uint64_t attempts{0};            // upstream attempts started
    std::uint64_t retries{0};             // attempts that followed a failed one
    std::uint64_t failovers{0};           // retries that went to a different backend
    std::uint64_t circuit_rejections{0};  // backends skipped because their circuit refused
    std::uint64_t circuit_opened{0};      // closed/half-open -> open transitions
    std::uint64_t exhausted{0};           // requests that gave up with retries or budget spent
  };

  // `clock` and `random` are injectable for tests; the defaults are the steady clock and a
  // thread-safe pseudo-random generator.
  ReliabilityManager(config::ReliabilityConfig config, std::shared_ptr<logging::Logger> logger,
                     SteadyClock clock = nullptr, Random random = nullptr);

  [[nodiscard]] const config::ReliabilityConfig& config() const noexcept { return config_; }
  [[nodiscard]] const RetryPolicy& retryPolicy() const noexcept { return retry_; }
  [[nodiscard]] unsigned maxAttempts() const noexcept { return retry_.maxAttempts(); }
  [[nodiscard]] std::chrono::milliseconds totalTimeout() const noexcept {
    return config_.timeout.total_timeout;
  }
  [[nodiscard]] std::chrono::steady_clock::time_point now() const { return clock_(); }

  // Pause before retry number `retry_number` (1 = the first retry).
  [[nodiscard]] std::chrono::milliseconds backoffDelay(unsigned retry_number);

  // nullptr when the circuit breaker is disabled.
  [[nodiscard]] CircuitBreakerRegistry* breakers() noexcept { return breakers_.get(); }

  void noteAttempt() { attempts_.fetch_add(1, std::memory_order_relaxed); }
  void noteRetry(bool failover) {
    retries_.fetch_add(1, std::memory_order_relaxed);
    if (failover) failovers_.fetch_add(1, std::memory_order_relaxed);
  }
  void noteCircuitRejection() { rejections_.fetch_add(1, std::memory_order_relaxed); }
  void noteExhausted() { exhausted_.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] Stats stats() const;

 private:
  const config::ReliabilityConfig config_;
  const std::shared_ptr<logging::Logger> logger_;
  const SteadyClock clock_;
  const RetryPolicy retry_;
  const BackoffPolicy backoff_;
  std::unique_ptr<CircuitBreakerRegistry> breakers_;

  Random random_;
  std::mutex random_mutex_;
  std::mt19937_64 engine_;

  std::atomic<std::uint64_t> attempts_{0};
  std::atomic<std::uint64_t> retries_{0};
  std::atomic<std::uint64_t> failovers_{0};
  std::atomic<std::uint64_t> rejections_{0};
  std::atomic<std::uint64_t> opened_{0};
  std::atomic<std::uint64_t> exhausted_{0};
};

}  // namespace edgeflow::reliability
