#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/discovery/ServiceInstance.hpp"

namespace edgeflow::routing {

// Optional per-request input to a strategy. Only consistent hashing uses it.
struct RoutingContext {
  // Stable identity of the request or its sender (client address, session or route key).
  // Equal keys map to the same instance for as long as the instance set is unchanged.
  // An empty key is hashed like any other, so keyless requests all land on one instance.
  std::string_view key;
};

// Chooses ONE instance out of a set of candidates.
//
// Responsibility boundary: the caller passes the routable set, that is instances that
// discovery already decided are eligible (registration status active AND health healthy,
// see ServiceRegistry::lookupRoutable). A strategy never looks at health or registration
// status, never talks to PostgreSQL, and never owns or duplicates the registry. It only
// chooses among what it is given, so an instance that left the set can never be selected.
//
// Identity: an instance is identified by (service, instance_id), never by its position in
// the span, its address in memory or its mutable metadata. The order of the input span is
// therefore irrelevant to every strategy.
//
// Thread safety: all strategies may be called concurrently from many threads.
class LoadBalancer {
 public:
  virtual ~LoadBalancer() = default;

  // Returns the chosen instance, or nullptr when `instances` is empty. The pointer refers
  // into `instances` and is valid as long as the caller keeps that storage alive.
  [[nodiscard]] const discovery::ServiceInstance* select(
      std::span<const discovery::ServiceInstance> instances,
      const RoutingContext& context = {}) {
    if (instances.empty()) return nullptr;
    return doSelect(instances, context);
  }

  // The configuration name of the strategy, e.g. "round_robin".
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

 protected:
  // Precondition: `instances` is not empty.
  [[nodiscard]] virtual const discovery::ServiceInstance* doSelect(
      std::span<const discovery::ServiceInstance> instances, const RoutingContext& context) = 0;
};

// Canonical identity of an instance, used to order instances and as hash input.
struct InstanceKey {
  std::string service;
  std::string instance_id;
  friend auto operator<=>(const InstanceKey&, const InstanceKey&) = default;
};

[[nodiscard]] InstanceKey keyOf(const discovery::ServiceInstance& instance);
// "service/instance_id": neither part can contain '/', so this is unambiguous.
[[nodiscard]] std::string canonicalName(const discovery::ServiceInstance& instance);

// Stable 64-bit hash (FNV-1a followed by a murmur3 finaliser). Identical on every platform
// and across runs, unlike std::hash.
[[nodiscard]] std::uint64_t stableHash(std::string_view text) noexcept;

[[nodiscard]] std::unique_ptr<LoadBalancer> makeLoadBalancer(config::RoutingStrategy strategy);

}  // namespace edgeflow::routing
