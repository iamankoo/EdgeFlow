#include <algorithm>

#include "Ordering.hpp"
#include "edgeflow/routing/Strategies.hpp"

namespace edgeflow::routing {

const discovery::ServiceInstance* RoundRobin::doSelect(
    std::span<const discovery::ServiceInstance> instances, const RoutingContext&) {
  const auto ordered = detail::inCanonicalOrder(instances);

  const std::lock_guard lock(mutex_);
  // First instance after the one chosen last; wrap to the smallest at the end.
  auto next = ordered.begin();
  if (has_last_) {
    next = std::upper_bound(ordered.begin(), ordered.end(), last_,
                            [](const InstanceKey& last, const detail::Candidate& c) {
                              return last < c.key;
                            });
    if (next == ordered.end()) next = ordered.begin();
  }
  last_ = next->key;
  has_last_ = true;
  return next->instance;
}

}  // namespace edgeflow::routing
