#include <algorithm>
#include <cstdint>
#include <utility>

#include "Ordering.hpp"
#include "edgeflow/routing/Strategies.hpp"

namespace edgeflow::routing {

// Immutable once built.
struct ConsistentHashing::Ring {
  std::vector<InstanceKey> members;  // canonical order; a point refers to an index into this
  std::vector<std::pair<std::uint64_t, std::size_t>> points;  // (position, member index), sorted
};

ConsistentHashing::ConsistentHashing(unsigned virtual_nodes)
    : virtual_nodes_(std::max(virtual_nodes, 1U)) {}

std::shared_ptr<const ConsistentHashing::Ring> ConsistentHashing::ringFor(
    const std::vector<InstanceKey>& members) {
  {
    const std::lock_guard lock(mutex_);
    if (ring_ && ring_->members == members) return ring_;
  }

  // Build outside the lock; selections of other threads keep using the previous ring.
  auto ring = std::make_shared<Ring>();
  ring->members = members;
  ring->points.reserve(members.size() * virtual_nodes_);
  for (std::size_t index = 0; index < members.size(); ++index) {
    const std::string base = members[index].service + "/" + members[index].instance_id + "#";
    for (unsigned replica = 0; replica < virtual_nodes_; ++replica) {
      ring->points.emplace_back(stableHash(base + std::to_string(replica)), index);
    }
  }
  // Equal positions (a hash collision) are ordered by member, so the ring is deterministic.
  std::sort(ring->points.begin(), ring->points.end());

  const std::lock_guard lock(mutex_);
  ring_ = ring;
  return ring;
}

const discovery::ServiceInstance* ConsistentHashing::doSelect(
    std::span<const discovery::ServiceInstance> instances, const RoutingContext& context) {
  const auto ordered = detail::inCanonicalOrder(instances);
  std::vector<InstanceKey> members;
  members.reserve(ordered.size());
  for (const auto& candidate : ordered) members.push_back(candidate.key);

  const auto ring = ringFor(members);
  const std::uint64_t position = stableHash(context.key);
  auto point = std::lower_bound(ring->points.begin(), ring->points.end(),
                                std::make_pair(position, std::size_t{0}));
  if (point == ring->points.end()) point = ring->points.begin();  // wrap around the ring
  return ordered[point->second].instance;
}

}  // namespace edgeflow::routing
