#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "edgeflow/routing/LoadBalancer.hpp"

namespace edgeflow::routing {

// Cycles through the instances in canonical (service, instance_id) order: a, b, c, a, ...
//
// The state is the key of the instance chosen last; the next choice is the first instance
// with a greater key, wrapping to the smallest. That keeps the cycle correct when the set
// changes between calls: a removed instance is simply skipped, and a new one is picked up
// as soon as the cycle reaches its position. One mutex guards that single value.
class RoundRobin final : public LoadBalancer {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "round_robin"; }

 protected:
  const discovery::ServiceInstance* doSelect(std::span<const discovery::ServiceInstance> instances,
                                             const RoutingContext& context) override;

 private:
  std::mutex mutex_;
  InstanceKey last_;
  bool has_last_{false};
};

// Chooses the instance with the fewest connections.
//
// "Connections" is the instance's `connection_count` as present in the snapshot passed in,
// i.e. as registered in PostgreSQL when the routable set was read. Phase 5 only READS it:
// the proxy that opens and closes backend connections and keeps the count current is
// Phase 6, so until then the count reflects whatever was written through the registry
// (`updateInstance` or `adjustConnectionCount`). Ties go to the smallest key, so the
// choice is deterministic. Stateless.
class LeastConnections final : public LoadBalancer {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "least_connections"; }

 protected:
  const discovery::ServiceInstance* doSelect(std::span<const discovery::ServiceInstance> instances,
                                             const RoutingContext& context) override;
};

// Weighted distribution by `weight` using smooth weighted round-robin (the algorithm nginx
// uses): over any window of (sum of weights) consecutive selections of an unchanged set
// each instance is chosen exactly `weight` times, and heavy instances are interleaved with
// light ones rather than chosen in a burst. Deterministic, no randomness.
//
// Weight 0 means "no share": such an instance is skipped while any instance has a positive
// weight. If every weight is 0 the instances are treated as equal rather than refusing to
// route, so a mis-weighted service stays reachable. Weights are summed in 64 bits.
// State: a running score per instance, pruned to the current set on every call.
class WeightedRouting final : public LoadBalancer {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "weighted"; }

 protected:
  const discovery::ServiceInstance* doSelect(std::span<const discovery::ServiceInstance> instances,
                                             const RoutingContext& context) override;

 private:
  std::mutex mutex_;
  std::map<InstanceKey, std::int64_t> score_;
};

// Maps a request key to an instance on a hash ring so that adding or removing an instance
// remaps only the keys that belonged to it (about 1/N of them), not nearly all of them as
// `hash % N` would.
//
// Each instance owns `virtual_nodes` points on a 64-bit ring, at stableHash(
// "service/instance_id#<replica>"). A key is hashed with stableHash and served by the first
// point at or after it, wrapping around. The ring is immutable; it is rebuilt only when the
// set of instance identities differs from the cached one, and selection works on a shared
// snapshot, so concurrent selections do not block each other while the ring is stable.
// Weights are not used: every instance gets the same number of points.
class ConsistentHashing final : public LoadBalancer {
 public:
  static constexpr unsigned kDefaultVirtualNodes = 160;

  explicit ConsistentHashing(unsigned virtual_nodes = kDefaultVirtualNodes);

  [[nodiscard]] std::string_view name() const noexcept override { return "consistent_hashing"; }

 protected:
  const discovery::ServiceInstance* doSelect(std::span<const discovery::ServiceInstance> instances,
                                             const RoutingContext& context) override;

 private:
  struct Ring;
  [[nodiscard]] std::shared_ptr<const Ring> ringFor(const std::vector<InstanceKey>& members);

  const unsigned virtual_nodes_;
  std::mutex mutex_;
  std::shared_ptr<const Ring> ring_;
};

}  // namespace edgeflow::routing
