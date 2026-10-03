#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "edgeflow/discovery/Result.hpp"
#include "edgeflow/discovery/ServiceInstance.hpp"

namespace edgeflow::discovery {

// Registry of backend service instances.
//
// This is the discovery abstraction the later phases consume (health checking in Phase 4,
// load balancing in Phase 5, the proxy in Phase 6). It knows nothing about HTTP, routing
// or health probing. Implementations are thread-safe. Every operation either fully
// happens or returns an error; a database failure is never reported as success.
class ServiceRegistry {
 public:
  virtual ~ServiceRegistry() = default;

  // Adds an instance, creating its service on first use. Registering an instance whose
  // (service, instance_id) or (service, host, port) already exists is rejected with
  // DuplicateInstance; registration is deliberately NOT an idempotent upsert, so two
  // deployments can never silently overwrite each other. Use updateInstance to change
  // mutable metadata.
  [[nodiscard]] virtual Result<ServiceInstance> registerInstance(const NewInstance& instance) = 0;

  // Removes an instance. A second deregistration of the same instance returns
  // InstanceNotFound (or ServiceNotFound), so callers can tell it was already gone.
  // The service itself stays known, with zero instances.
  [[nodiscard]] virtual Result<Unit> deregisterInstance(std::string_view service,
                                                        std::string_view instance_id) = 0;

  // All instances of a service, in a stable order (registration time, then id). The
  // registry imposes no routing order or health filter. ServiceNotFound when the service
  // was never registered; a known service with no instances yields an empty list.
  [[nodiscard]] virtual Result<std::vector<ServiceInstance>> lookupService(
      std::string_view service) = 0;

  [[nodiscard]] virtual Result<ServiceInstance> getInstance(std::string_view service,
                                                            std::string_view instance_id) = 0;

  // Names of all known services, sorted.
  [[nodiscard]] virtual Result<std::vector<std::string>> listServices() = 0;

  // Applies the mutable fields that are present. Identity (service, instance id, host,
  // port) cannot change.
  [[nodiscard]] virtual Result<ServiceInstance> updateInstance(std::string_view service,
                                                               std::string_view instance_id,
                                                               const InstanceUpdate& update) = 0;

  // Atomically adds `delta` to the connection count (clamped at zero) and returns the
  // updated instance. This is the safe primitive for concurrent increment/decrement.
  [[nodiscard]] virtual Result<ServiceInstance> adjustConnectionCount(
      std::string_view service, std::string_view instance_id, std::int64_t delta) = 0;
};

}  // namespace edgeflow::discovery
