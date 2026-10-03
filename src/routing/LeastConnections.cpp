#include "edgeflow/routing/Strategies.hpp"

namespace edgeflow::routing {

const discovery::ServiceInstance* LeastConnections::doSelect(
    std::span<const discovery::ServiceInstance> instances, const RoutingContext&) {
  const discovery::ServiceInstance* best = nullptr;
  for (const auto& candidate : instances) {
    if (best == nullptr || candidate.connection_count < best->connection_count ||
        (candidate.connection_count == best->connection_count && keyOf(candidate) < keyOf(*best))) {
      best = &candidate;
    }
  }
  return best;
}

}  // namespace edgeflow::routing
