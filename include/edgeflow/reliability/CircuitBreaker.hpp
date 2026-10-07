#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace edgeflow::reliability {

enum class CircuitState { Closed, Open, HalfOpen };

[[nodiscard]] const char* toString(CircuitState state) noexcept;

using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

struct CircuitBreakerSettings {
  // Consecutive failures, while closed, that open the circuit.
  unsigned failure_threshold{5};
  // Time spent open before a probe is let through.
  std::chrono::milliseconds recovery_timeout{30000};
  // Probes allowed in flight while half-open; that many successes close the circuit.
  unsigned half_open_max_requests{1};
};

// The ticket tryAcquire() hands out. It must be settled exactly once with recordSuccess(),
// recordFailure() or release(). A ticket from an earlier state epoch (the circuit moved on
// while the request was in flight) is ignored when settled, so a late result can neither
// reopen nor close a circuit that already decided something newer.
struct Admission {
  bool admitted{false};
  bool probe{false};  // admitted as a half-open trial request
  std::uint64_t epoch{0};
};

// A circuit breaker for ONE backend instance. Pure state machine, no I/O, no timers: every
// transition is driven by a call (the open period is judged against an injectable clock when
// a request asks for admission), so it is deterministic and unit-testable.
//
//   CLOSED --failure_threshold consecutive failures--> OPEN
//   OPEN   --recovery_timeout elapsed, next request---> HALF-OPEN
//   HALF-OPEN --half_open_max_requests probes succeed--> CLOSED
//   HALF-OPEN --any probe fails----------------------> OPEN (a new recovery period starts)
//
// While OPEN every request is rejected at once (fail fast). While HALF-OPEN at most
// `half_open_max_requests` probes are in flight (success + in-flight never exceeds it), so
// a recovering backend is not flooded: the OPEN -> HALF-OPEN transition admits one bounded
// batch, never the whole waiting crowd. The state is guarded by one small mutex that is never
// held across I/O; transition listeners run after it is released.
//
// "Failure" and "success" are decided by the caller (the proxy). Only requests admitted while
// CLOSED count toward the threshold; a success resets the consecutive-failure count.
class CircuitBreaker {
 public:
  using Listener = std::function<void(CircuitState from, CircuitState to)>;

  CircuitBreaker(CircuitBreakerSettings settings, SteadyClock clock, Listener listener = nullptr);

  // Asks to send a request. Admitted requests must be settled.
  [[nodiscard]] Admission tryAcquire();

  // Would tryAcquire() admit right now? Takes no slot: used to leave a backend out of a
  // routing choice without spending its half-open probe on a request that may go elsewhere.
  [[nodiscard]] bool isAvailable() const;

  void recordSuccess(const Admission& admission);
  void recordFailure(const Admission& admission);
  // The request ended without telling anything about the backend (cancelled): frees a probe
  // slot, changes no counter.
  void release(const Admission& admission);

  // The stored state. An open circuit whose recovery period has passed still reads OPEN until
  // a request asks for admission.
  [[nodiscard]] CircuitState state() const;
  [[nodiscard]] unsigned consecutiveFailures() const;

 private:
  struct Transition {
    bool happened{false};
    CircuitState from{CircuitState::Closed};
    CircuitState to{CircuitState::Closed};
  };

  void moveTo(CircuitState next, Transition& transition);  // caller holds mutex_
  void notify(const Transition& transition) const;

  const CircuitBreakerSettings settings_;
  const SteadyClock clock_;
  const Listener listener_;

  mutable std::mutex mutex_;
  CircuitState state_{CircuitState::Closed};
  std::uint64_t epoch_{0};
  unsigned consecutive_failures_{0};
  std::chrono::steady_clock::time_point opened_at_{};
  unsigned probes_in_flight_{0};
  unsigned probe_successes_{0};
};

// The breakers of all backend instances, created on first use. The key identifies the backend
// ("service/instance_id@host:port": a re-registered instance on another endpoint starts with a
// clean circuit, and failures of one service or backend never touch another). The map is
// guarded by its own mutex, held only for the lookup; breakers do their own locking.
class CircuitBreakerRegistry {
 public:
  using Listener = std::function<void(const std::string& key, CircuitState from, CircuitState to)>;

  CircuitBreakerRegistry(CircuitBreakerSettings settings, SteadyClock clock,
                         Listener listener = nullptr, std::size_t max_entries = 4096);

  [[nodiscard]] std::shared_ptr<CircuitBreaker> get(const std::string& key);
  // nullptr when this backend has no breaker yet (nothing has been recorded: closed).
  [[nodiscard]] std::shared_ptr<CircuitBreaker> find(const std::string& key) const;
  [[nodiscard]] std::size_t size() const;

 private:
  const CircuitBreakerSettings settings_;
  const SteadyClock clock_;
  const Listener listener_;
  const std::size_t max_entries_;

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<CircuitBreaker>> breakers_;
};

}  // namespace edgeflow::reliability
