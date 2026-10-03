#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace edgeflow::discovery {

// Registration/administrative state of an instance, set by whoever manages the
// deployment. It says whether the instance is meant to receive traffic; it says nothing
// about whether the instance is actually working (that is HealthStatus).
enum class InstanceStatus {
  Active,    // registered and intended to receive traffic (default)
  Draining,  // still registered, but should receive no new traffic
  Disabled   // registered but administratively switched off
};

// Last known health of an instance, stored as metadata. Phase 3 never probes anything:
// the value is whatever was registered or last written. Phase 4 (health checking) will
// own the transitions.
enum class HealthStatus { Unknown, Healthy, Unhealthy };

[[nodiscard]] std::string_view toString(InstanceStatus status) noexcept;
[[nodiscard]] std::string_view toString(HealthStatus status) noexcept;
[[nodiscard]] std::optional<InstanceStatus> parseInstanceStatus(std::string_view text) noexcept;
[[nodiscard]] std::optional<HealthStatus> parseHealthStatus(std::string_view text) noexcept;

inline constexpr std::uint32_t kMaxWeight = 1000;
inline constexpr std::uint32_t kDefaultWeight = 1;
inline constexpr std::size_t kMaxVersionLength = 64;

// A concrete backend instance of a logical service, e.g. "user-service @ 10.0.0.11:9001".
// Identity is (service, instance_id) and never changes; host/port are fixed at
// registration too (an instance that moves is deregistered and registered again).
struct ServiceInstance {
  // --- identity (immutable) ---
  std::string service;
  std::string instance_id;
  std::string host;
  std::uint16_t port{0};

  // --- mutable metadata ---
  InstanceStatus status{InstanceStatus::Active};
  HealthStatus health{HealthStatus::Unknown};
  std::string version;  // free-form release label, e.g. "1.4.2"; may be empty
  // Relative share intended for weighted routing (Phase 5). Metadata only here.
  std::uint32_t weight{kDefaultWeight};
  // Number of requests currently in flight to this instance. Phase 3 only stores and
  // updates it (set / atomic adjust); the proxy (Phase 6) will maintain it and the
  // Least Connections strategy (Phase 5) will read it.
  std::uint64_t connection_count{0};

  // --- bookkeeping, assigned by the database (ISO-8601 UTC) ---
  std::string registered_at;
  std::string updated_at;
};

// Input for registration. `instance_id` is assigned by the database when absent.
struct NewInstance {
  std::string service;
  std::optional<std::string> instance_id;
  std::string host;
  std::uint16_t port{0};
  InstanceStatus status{InstanceStatus::Active};
  HealthStatus health{HealthStatus::Unknown};
  std::string version;
  std::uint32_t weight{kDefaultWeight};
  std::uint64_t connection_count{0};
};

// Mutable fields only; an absent field is left unchanged.
struct InstanceUpdate {
  std::optional<InstanceStatus> status;
  std::optional<HealthStatus> health;
  std::optional<std::string> version;
  std::optional<std::uint32_t> weight;
  std::optional<std::uint64_t> connection_count;

  [[nodiscard]] bool empty() const noexcept {
    return !status && !health && !version && !weight && !connection_count;
  }
};

}  // namespace edgeflow::discovery
