#pragma once

#include <optional>

#include "edgeflow/discovery/ServiceInstance.hpp"

namespace edgeflow::discovery {

struct HealthPolicy {
  // Consecutive failed probes that turn a healthy instance unhealthy.
  unsigned failure_threshold{3};
  // Consecutive successful probes that turn an unhealthy instance healthy.
  unsigned success_threshold{2};
};

// Health state machine for one instance. Pure and deterministic: it knows nothing about
// sockets, timers or the database, so every transition can be unit-tested exhaustively.
//
//   Unknown   --success--> Healthy      the first probe decides at once: there is no prior
//   Unknown   --failure--> Unhealthy    state to flap away from, so thresholds do not apply
//   Healthy   --failure_threshold consecutive failures--> Unhealthy
//   Unhealthy --success_threshold consecutive successes--> Healthy
//
// A result in the opposite direction resets the streak, so a single transient failure
// (or success) never flips a settled instance unless the threshold is 1.
// The state never returns to Unknown once decided; only a new registration starts there.
class HealthTracker {
 public:
  explicit HealthTracker(HealthStatus initial = HealthStatus::Unknown) noexcept
      : status_(initial) {}

  // Feeds one probe result. Returns the new status when this result caused a transition.
  [[nodiscard]] std::optional<HealthStatus> record(bool probe_succeeded,
                                                   const HealthPolicy& policy) noexcept;

  [[nodiscard]] HealthStatus status() const noexcept { return status_; }
  [[nodiscard]] unsigned consecutiveFailures() const noexcept { return failures_; }
  [[nodiscard]] unsigned consecutiveSuccesses() const noexcept { return successes_; }

 private:
  HealthStatus status_;
  unsigned failures_{0};
  unsigned successes_{0};
};

}  // namespace edgeflow::discovery
