#pragma once

#include <memory>
#include <string_view>

#include "edgeflow/discovery/ServiceRegistry.hpp"
#include "edgeflow/routing/LoadBalancer.hpp"

namespace edgeflow::routing {

// Connects discovery to a load-balancing strategy for one request:
//
//   ServiceRegistry::lookupRoutable(service)   active AND healthy instances (Phase 4)
//        -> one consistent snapshot, no registry or database access while choosing
//   LoadBalancer::select(snapshot, key)        the configured strategy picks one
//
// The router decides nothing about health: it chooses among what the registry says is
// routable. It does not forward the request. Forwarding to the selected instance, and
// maintaining its connection count, is the reverse proxy (Phase 6), which will call
// route() for every request.
class Router {
 public:
  Router(std::shared_ptr<discovery::ServiceRegistry> registry,
         std::unique_ptr<LoadBalancer> balancer);

  // The chosen instance (a copy of the snapshot entry). Errors: ServiceNotFound for a
  // service that was never registered, NoRoutableInstance when it exists but nothing is
  // routable right now, DatabaseUnavailable / InvalidArgument as reported by the registry.
  [[nodiscard]] discovery::Result<discovery::ServiceInstance> route(
      std::string_view service, const RoutingContext& context = {});

  [[nodiscard]] std::string_view strategy() const noexcept { return balancer_->name(); }

 private:
  const std::shared_ptr<discovery::ServiceRegistry> registry_;
  const std::unique_ptr<LoadBalancer> balancer_;
};

}  // namespace edgeflow::routing
