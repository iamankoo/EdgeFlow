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

  // Every instance of every service (stable order: service, registration time, id). This is
  // what health checking reads to find out what to probe.
  [[nodiscard]] virtual Result<std::vector<ServiceInstance>> listInstances() = 0;

  // The instances routing may use right now: registration status `active` AND health
  // `healthy`. Draining and disabled instances are excluded however healthy they are,
  // and healthy-looking ones are excluded while unknown or unhealthy. No ordering or
  // selection strategy is applied (that is load balancing, Phase 5). ServiceNotFound for
  // a never-registered service; a known service with nothing routable yields an empty list.
  [[nodiscard]] virtual Result<std::vector<ServiceInstance>> lookupRoutable(
      std::string_view service) = 0;

  // Sets the health status of ONE incarnation of an instance: `registered_at` (as returned
  // by the registry) must still match, otherwise InstanceNotFound. This stops a probe
  // result that was computed for an instance which was since deregistered and registered
  // again (possibly at another address) from being written onto the new one. Idempotent.
  [[nodiscard]] virtual Result<ServiceInstance> updateHealth(std::string_view service,
                                                             std::string_view instance_id,
                                                             std::string_view registered_at,
                                                             HealthStatus health) = 0;

  // Atomically adds `delta` to the connection count (clamped at zero) and returns the
  // updated instance. This is the safe primitive for concurrent increment/decrement.
  [[nodiscard]] virtual Result<ServiceInstance> adjustConnectionCount(
      std::string_view service, std::string_view instance_id, std::int64_t delta) = 0;
};

}  // namespace edgeflow::discovery
