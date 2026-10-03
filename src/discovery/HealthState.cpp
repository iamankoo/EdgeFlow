#include "edgeflow/discovery/HealthState.hpp"

namespace edgeflow::discovery {

std::optional<HealthStatus> HealthTracker::record(bool probe_succeeded,
                                                  const HealthPolicy& policy) noexcept {
  if (probe_succeeded) {
    failures_ = 0;
    ++successes_;
  } else {
    successes_ = 0;
    ++failures_;
  }

  switch (status_) {
    case HealthStatus::Unknown:
      status_ = probe_succeeded ? HealthStatus::Healthy : HealthStatus::Unhealthy;
      break;
    case HealthStatus::Healthy:
      if (probe_succeeded || failures_ < policy.failure_threshold) return std::nullopt;
      status_ = HealthStatus::Unhealthy;
      break;
    case HealthStatus::Unhealthy:
      if (!probe_succeeded || successes_ < policy.success_threshold) return std::nullopt;
      status_ = HealthStatus::Healthy;
      break;
  }
  // A transition starts a fresh streak in the new state.
  failures_ = 0;
  successes_ = 0;
  return status_;
}

}  // namespace edgeflow::discovery
