#include "edgeflow/routing/LoadBalancer.hpp"

#include "edgeflow/routing/Strategies.hpp"

namespace edgeflow::routing {

InstanceKey keyOf(const discovery::ServiceInstance& instance) {
  return InstanceKey{instance.service, instance.instance_id};
}

std::string canonicalName(const discovery::ServiceInstance& instance) {
  return instance.service + "/" + instance.instance_id;
}

std::uint64_t stableHash(std::string_view text) noexcept {
  // FNV-1a ...
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ULL;
  }
  // ... then the murmur3 64-bit finaliser, because FNV alone mixes the last bytes poorly
  // and ring points for "x#1", "x#2", ... would cluster.
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= hash >> 33;
  hash *= 0xc4ceb9fe1a85ec53ULL;
  hash ^= hash >> 33;
  return hash;
}

std::unique_ptr<LoadBalancer> makeLoadBalancer(config::RoutingStrategy strategy) {
  switch (strategy) {
    case config::RoutingStrategy::RoundRobin: return std::make_unique<RoundRobin>();
    case config::RoutingStrategy::LeastConnections: return std::make_unique<LeastConnections>();
    case config::RoutingStrategy::Weighted: return std::make_unique<WeightedRouting>();
    case config::RoutingStrategy::ConsistentHashing: return std::make_unique<ConsistentHashing>();
  }
  return std::make_unique<RoundRobin>();
}

}  // namespace edgeflow::routing
