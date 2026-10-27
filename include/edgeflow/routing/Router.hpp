#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  struct Options {
    // 0 (the default): every route() reads the routable set from the registry (the Phase 5-9 behaviour).
    // > 0: the routable set of a service is kept for this long and shared by the requests in between,
    // so the registry (PostgreSQL) is read at most about once per service per interval instead of once
    // per request. The price is explicit: a health change, a registration or a removal reaches routing up
    // to this long late, and the connection_count in the snapshot is that old (so it must not be used with
    // Least Connections, which reads it; the configuration refuses that combination). Only a snapshot with
    // at least one instance is kept, errors and empty answers are never cached, so a service that was
    // not routable is noticed the moment it becomes routable.
    std::chrono::milliseconds snapshot_ttl{0};
    std::function<std::chrono::steady_clock::time_point()> clock;  // tests; default steady_clock
  };

  struct Stats {
    std::uint64_t snapshot_hits{0};   // routes served from a kept snapshot
    std::uint64_t snapshot_reads{0};  // routes that read the registry (cache on: refreshes; off: every route)
  };

  Router(std::shared_ptr<discovery::ServiceRegistry> registry, std::unique_ptr<LoadBalancer> balancer);
  Router(std::shared_ptr<discovery::ServiceRegistry> registry, std::unique_ptr<LoadBalancer> balancer,
         Options options);

  // The chosen instance (a copy of the snapshot entry). Errors: ServiceNotFound for a
  // service that was never registered, NoRoutableInstance when it exists but nothing is
  // routable right now, DatabaseUnavailable / InvalidArgument as reported by the registry.
  [[nodiscard]] discovery::Result<discovery::ServiceInstance> route(
      std::string_view service, const RoutingContext& context = {});

  // Same, but only among instances that pass the filter (failover: leave out the backends a
  // request already failed on, and the ones whose circuit is open). The filter is applied to
  // the one routable snapshot taken for this call, before the strategy chooses; the
  // strategies themselves are untouched. NoRoutableInstance also covers "routable instances
  // exist but none is eligible".
  struct Filter {
    // An instance for which this returns false is never chosen. Empty: all are eligible.
    std::function<bool(const discovery::ServiceInstance&)> eligible;
    // Among the eligible instances, those for which this returns true are chosen first;
    // when none qualifies, any eligible instance may be chosen. Empty: no preference.
    std::function<bool(const discovery::ServiceInstance&)> preferred;
  };
  [[nodiscard]] discovery::Result<discovery::ServiceInstance> route(std::string_view service,
                                                                    const RoutingContext& context,
                                                                    const Filter& filter);

  // The same choice as route(), but ONLY from a kept snapshot that is still fresh: it never reads the registry
  // and never blocks (apart from a short mutex), so it may run on an I/O thread. nullopt when there is no fresh
  // snapshot (snapshot_ttl is 0, nothing kept yet, or it expired): the caller then calls route() where blocking
  // is allowed. `filter` may be null (no filter).
  [[nodiscard]] std::optional<discovery::Result<discovery::ServiceInstance>> routeIfKept(
      std::string_view service, const RoutingContext& context, const Filter* filter);
  // True when snapshot_ttl > 0, i.e. routeIfKept() can ever answer.
  [[nodiscard]] bool keepsSnapshots() const noexcept { return options_.snapshot_ttl.count() > 0; }

  [[nodiscard]] std::string_view strategy() const noexcept { return balancer_->name(); }
  [[nodiscard]] std::chrono::milliseconds snapshotTtl() const noexcept { return options_.snapshot_ttl; }
  [[nodiscard]] Stats stats() const noexcept {
    return Stats{hits_.load(std::memory_order_relaxed), reads_.load(std::memory_order_relaxed)};
  }

 private:
  using Snapshot = std::shared_ptr<const std::vector<discovery::ServiceInstance>>;

  // The routable set of `service`: from the registry, or (snapshot_ttl > 0) the one kept for it.
  [[nodiscard]] discovery::Result<Snapshot> snapshot(std::string_view service);
  // The kept snapshot of `service` when it is still fresh, else null. Counts a hit.
  [[nodiscard]] Snapshot freshSnapshot(const std::string& service);
  // The strategy's choice among `all` after the filter (see Filter).
  [[nodiscard]] discovery::Result<discovery::ServiceInstance> chooseFiltered(
      const std::vector<discovery::ServiceInstance>& all, std::string_view service,
      const RoutingContext& context, const Filter& filter);

  const std::shared_ptr<discovery::ServiceRegistry> registry_;
  const std::unique_ptr<LoadBalancer> balancer_;
  const Options options_;

  struct Kept {
    Snapshot instances;
    std::chrono::steady_clock::time_point fetched;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, Kept> kept_;
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> reads_{0};
};

}  // namespace edgeflow::routing
