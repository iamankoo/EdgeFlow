#pragma once

#include <string>
#include <utility>
#include <variant>

namespace edgeflow::discovery {

enum class RegistryErrorCode {
  InvalidArgument,      // malformed or out-of-range input; nothing was changed
  ServiceNotFound,      // the logical service has never been registered
  InstanceNotFound,     // the service exists but has no such instance
  DuplicateInstance,    // instance id or host:port already registered for the service
  DatabaseUnavailable,  // PostgreSQL cannot be reached; the operation did NOT happen
  Internal              // unexpected database or logic failure
};

struct RegistryError {
  RegistryErrorCode code{RegistryErrorCode::Internal};
  std::string message;
};

[[nodiscard]] constexpr const char* toString(RegistryErrorCode code) noexcept {
  switch (code) {
    case RegistryErrorCode::InvalidArgument: return "invalid_argument";
    case RegistryErrorCode::ServiceNotFound: return "service_not_found";
    case RegistryErrorCode::InstanceNotFound: return "instance_not_found";
    case RegistryErrorCode::DuplicateInstance: return "duplicate_instance";
    case RegistryErrorCode::DatabaseUnavailable: return "database_unavailable";
    case RegistryErrorCode::Internal: return "internal";
  }
  return "internal";
}

struct Unit {};

// Minimal value-or-error type (std::expected needs C++23).
template <typename T>
class Result {
 public:
  Result(T value) : data_(std::move(value)) {}                // NOLINT(google-explicit-constructor)
  Result(RegistryError error) : data_(std::move(error)) {}    // NOLINT(google-explicit-constructor)
  Result(RegistryErrorCode code, std::string message)
      : data_(RegistryError{code, std::move(message)}) {}

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  // Precondition: ok().
  [[nodiscard]] T& value() & { return std::get<T>(data_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(data_)); }

  // Precondition: !ok().
  [[nodiscard]] const RegistryError& error() const { return std::get<RegistryError>(data_); }

 private:
  std::variant<T, RegistryError> data_;
};

}  // namespace edgeflow::discovery
