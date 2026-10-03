#pragma once

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include "edgeflow/routing/LoadBalancer.hpp"

namespace edgeflow::routing::detail {

struct Candidate {
  InstanceKey key;
  const discovery::ServiceInstance* instance;
};

// The input in canonical identity order, independent of how the caller ordered it.
// Entries with identical keys (the same instance listed twice) are kept once.
inline std::vector<Candidate> inCanonicalOrder(std::span<const discovery::ServiceInstance> instances) {
  std::vector<Candidate> ordered;
  ordered.reserve(instances.size());
  for (const auto& instance : instances) ordered.push_back(Candidate{keyOf(instance), &instance});
  std::sort(ordered.begin(), ordered.end(),
            [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
  ordered.erase(std::unique(ordered.begin(), ordered.end(),
                            [](const Candidate& a, const Candidate& b) { return a.key == b.key; }),
                ordered.end());
  return ordered;
}

}  // namespace edgeflow::routing::detail
