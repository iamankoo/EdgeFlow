#include "edgeflow/routing/Router.hpp"

#include <string>
#include <utility>

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

}  // namespace edgeflow::routing
