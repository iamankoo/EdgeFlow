#include <algorithm>
#include <cstdint>

#include "Ordering.hpp"
#include "edgeflow/routing/Strategies.hpp"

namespace edgeflow::routing {

const discovery::ServiceInstance* WeightedRouting::doSelect(
    std::span<const discovery::ServiceInstance> instances, const RoutingContext&) {
  const auto ordered = detail::inCanonicalOrder(instances);

  // Weight 0 is "no share" unless nobody has a share, in which case all are equal.
  const bool any_positive = std::any_of(ordered.begin(), ordered.end(), [](const detail::Candidate& c) {
    return c.instance->weight > 0;
  });
  const auto weight_of = [&](const detail::Candidate& c) -> std::int64_t {
    return any_positive ? static_cast<std::int64_t>(c.instance->weight) : 1;
  };

  const std::lock_guard lock(mutex_);

  // Forget scores of instances that left the set; new instances start at zero.
  for (auto it = score_.begin(); it != score_.end();) {
    const bool present = std::any_of(ordered.begin(), ordered.end(),
                                     [&](const detail::Candidate& c) { return c.key == it->first; });
    it = present ? std::next(it) : score_.erase(it);
  }

  // Smooth weighted round-robin: everyone gains its weight, the leader is chosen and pays
  // back the total.
  std::int64_t total = 0;
  const detail::Candidate* best = nullptr;
  std::int64_t best_score = 0;
  for (const auto& candidate : ordered) {
    const std::int64_t weight = weight_of(candidate);
    if (weight <= 0) continue;
    total += weight;
    const std::int64_t score = (score_[candidate.key] += weight);
    if (best == nullptr || score > best_score) {  // ties keep the smaller key (ordered ascending)
      best = &candidate;
      best_score = score;
    }
  }
  if (best == nullptr) return ordered.front().instance;  // unreachable: any_positive or all weigh 1
  score_[best->key] -= total;
  return best->instance;
}

}  // namespace edgeflow::routing
