#include "edgeflow/routing/Router.hpp"

#include <string>
#include <utility>
#include <vector>

namespace edgeflow::routing {

Router::Router(std::shared_ptr<discovery::ServiceRegistry> registry,
               std::unique_ptr<LoadBalancer> balancer)
    : registry_(std::move(registry)), balancer_(std::move(balancer)) {}

discovery::Result<discovery::ServiceInstance> Router::route(std::string_view service,
                                                            const RoutingContext& context) {
  auto routable = registry_->lookupRoutable(service);
  if (!routable) return routable.error();

  const auto* chosen = balancer_->select(routable.value(), context);
  if (chosen == nullptr) {
    return discovery::Result<discovery::ServiceInstance>{
        discovery::RegistryErrorCode::NoRoutableInstance,
        "service " + std::string{service} + " has no routable instance"};
  }
  return *chosen;
}

discovery::Result<discovery::ServiceInstance> Router::route(std::string_view service,
                                                            const RoutingContext& context,
                                                            const Filter& filter) {
  auto routable = registry_->lookupRoutable(service);
  if (!routable) return routable.error();

  std::vector<discovery::ServiceInstance> candidates;
  candidates.reserve(routable.value().size());
  for (auto& instance : routable.value()) {
    if (!filter.eligible || filter.eligible(instance)) candidates.push_back(std::move(instance));
  }
  std::vector<discovery::ServiceInstance> preferred;
  if (filter.preferred) {
    for (const auto& instance : candidates) {
      if (filter.preferred(instance)) preferred.push_back(instance);
    }
  }

  const auto& pool = preferred.empty() ? candidates : preferred;
  const auto* chosen = balancer_->select(pool, context);
  if (chosen == nullptr) {
    return discovery::Result<discovery::ServiceInstance>{
        discovery::RegistryErrorCode::NoRoutableInstance,
        "service " + std::string{service} + " has no eligible instance"};
  }
  return *chosen;
}

}  // namespace edgeflow::routing
