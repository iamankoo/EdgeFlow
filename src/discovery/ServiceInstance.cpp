#include "edgeflow/discovery/ServiceInstance.hpp"

namespace edgeflow::discovery {

std::string_view toString(InstanceStatus status) noexcept {
  switch (status) {
    case InstanceStatus::Active: return "active";
    case InstanceStatus::Draining: return "draining";
    case InstanceStatus::Disabled: return "disabled";
  }
  return "active";
}

std::string_view toString(HealthStatus status) noexcept {
  switch (status) {
    case HealthStatus::Unknown: return "unknown";
    case HealthStatus::Healthy: return "healthy";
    case HealthStatus::Unhealthy: return "unhealthy";
  }
  return "unknown";
}

std::optional<InstanceStatus> parseInstanceStatus(std::string_view text) noexcept {
  if (text == "active") return InstanceStatus::Active;
  if (text == "draining") return InstanceStatus::Draining;
  if (text == "disabled") return InstanceStatus::Disabled;
  return std::nullopt;
}

std::optional<HealthStatus> parseHealthStatus(std::string_view text) noexcept {
  if (text == "unknown") return HealthStatus::Unknown;
  if (text == "healthy") return HealthStatus::Healthy;
  if (text == "unhealthy") return HealthStatus::Unhealthy;
  return std::nullopt;
}

}  // namespace edgeflow::discovery
