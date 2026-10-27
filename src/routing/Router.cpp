#include "edgeflow/routing/Router.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace edgeflow::routing {

Router::Router(std::shared_ptr<discovery::ServiceRegistry> registry, std::unique_ptr<LoadBalancer> balancer)
    : Router(std::move(registry), std::move(balancer), Options{}) {}

Router::Router(std::shared_ptr<discovery::ServiceRegistry> registry,
               std::unique_ptr<LoadBalancer> balancer, Options options)
    : registry_(std::move(registry)), balancer_(std::move(balancer)), options_(std::move(options)) {}

Router::Snapshot Router::freshSnapshot(const std::string& service) {
  const auto now = options_.clock ? options_.clock() : std::chrono::steady_clock::now();
  const std::lock_guard lock(mutex_);
  const auto it = kept_.find(service);
  if (it != kept_.end() && now - it->second.fetched < options_.snapshot_ttl) {
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second.instances;
  }
  return nullptr;
}

std::optional<discovery::Result<discovery::ServiceInstance>> Router::routeIfKept(
    std::string_view service, const RoutingContext& context, const Filter* filter) {
  if (options_.snapshot_ttl.count() <= 0) return std::nullopt;
  const auto kept = freshSnapshot(std::string{service});
  if (!kept) return std::nullopt;
  if (filter != nullptr) return chooseFiltered(*kept, service, context, *filter);
  if (const auto* chosen = balancer_->select(*kept, context)) return discovery::Result<discovery::ServiceInstance>{*chosen};
  return discovery::Result<discovery::ServiceInstance>{discovery::RegistryErrorCode::NoRoutableInstance,
                                                       "service " + std::string{service} + " has no routable instance"};
}

discovery::Result<Router::Snapshot> Router::snapshot(std::string_view service) {
  const auto now = [this] {
    return options_.clock ? options_.clock() : std::chrono::steady_clock::now();
  };
  const bool keep = options_.snapshot_ttl.count() > 0;
  const std::string key{service};
  if (keep) {
    if (auto fresh = freshSnapshot(key)) return fresh;
  }
  // Read outside the lock: the registry call blocks on the database. Several requests that find the
  // snapshot expired at the same moment each read it once (bounded by the registry's call pool).
  reads_.fetch_add(1, std::memory_order_relaxed);
  auto routable = registry_->lookupRoutable(service);
  if (!routable) return routable.error();
  auto shared = std::make_shared<const std::vector<discovery::ServiceInstance>>(std::move(routable).value());
  if (keep) {
    const std::lock_guard lock(mutex_);
    if (shared->empty()) {
      kept_.erase(key);  // not routable now: do not hide it becoming routable
    } else {
      kept_[key] = Kept{shared, now()};
    }
  }
  return Snapshot{std::move(shared)};
}

discovery::Result<discovery::ServiceInstance> Router::route(std::string_view service,
                                                            const RoutingContext& context) {
  auto routable = snapshot(service);
  if (!routable) return routable.error();

  const auto* chosen = balancer_->select(*routable.value(), context);
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
  auto routable = snapshot(service);
  if (!routable) return routable.error();
  return chooseFiltered(*routable.value(), service, context, filter);
}

discovery::Result<discovery::ServiceInstance> Router::chooseFiltered(
    const std::vector<discovery::ServiceInstance>& all, std::string_view service, const RoutingContext& context,
    const Filter& filter) {
  // Common case first (no retry yet, no circuit open): every instance is eligible and preferred, so the
  // snapshot itself is the candidate set and nothing is copied.
  const auto passes = [&](const discovery::ServiceInstance& instance) {
    return (!filter.eligible || filter.eligible(instance)) && (!filter.preferred || filter.preferred(instance));
  };
  if (std::all_of(all.begin(), all.end(), passes)) {
    if (const auto* chosen = balancer_->select(all, context)) return *chosen;
    return discovery::Result<discovery::ServiceInstance>{
        discovery::RegistryErrorCode::NoRoutableInstance,
        "service " + std::string{service} + " has no eligible instance"};
  }

  std::vector<discovery::ServiceInstance> candidates;
  candidates.reserve(all.size());
  for (const auto& instance : all) {
    if (!filter.eligible || filter.eligible(instance)) candidates.push_back(instance);
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
