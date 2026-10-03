#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "edgeflow/discovery/ServiceInstance.hpp"

namespace edgeflow::discovery {

// Each validator returns std::nullopt when the value is acceptable, otherwise a
// human-readable reason. The database repeats the structural rules as CHECK
// constraints, so a bug here cannot persist bad data.

// Lowercase DNS-label-like name: [a-z0-9]([a-z0-9._-]*[a-z0-9])?, 1-64 characters.
[[nodiscard]] std::optional<std::string> validateServiceName(std::string_view name);
// [A-Za-z0-9._-], 1-128 characters.
[[nodiscard]] std::optional<std::string> validateInstanceId(std::string_view id);
// An IPv4/IPv6 literal or an RFC 1123 host name (no scheme, port or path).
[[nodiscard]] std::optional<std::string> validateHost(std::string_view host);
[[nodiscard]] std::optional<std::string> validatePort(std::uint32_t port);
// At most 64 printable characters; empty is allowed.
[[nodiscard]] std::optional<std::string> validateVersion(std::string_view version);
[[nodiscard]] std::optional<std::string> validateWeight(std::uint64_t weight);
// Fits a signed 64-bit database column.
[[nodiscard]] std::optional<std::string> validateConnectionCount(std::uint64_t count);

[[nodiscard]] std::optional<std::string> validate(const NewInstance& instance);
[[nodiscard]] std::optional<std::string> validate(const InstanceUpdate& update);

}  // namespace edgeflow::discovery
