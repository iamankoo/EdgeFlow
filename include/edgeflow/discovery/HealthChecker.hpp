#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/discovery/HealthState.hpp"
#include "edgeflow/discovery/Prober.hpp"
#include "edgeflow/discovery/ServiceRegistry.hpp"
#include "edgeflow/logging/Logger.hpp"

namespace edgeflow::discovery {

// Actively probes registered service instances and keeps their persisted health status
// up to date, so that ServiceRegistry::lookupRoutable() excludes unhealthy instances and
// reintroduces recovered ones.
//
//   registry.listInstances()  --refresh-->  one Monitor per instance to check
//   Monitor timer --> Prober (TCP or HTTP, bounded by a timeout) --> ProbeResult
//   ProbeResult --> HealthTracker (thresholds) --> on a transition:
//   registry.updateHealth()  (PostgreSQL stays the only authoritative store)
//
// What is checked: instances whose registration status is active or draining. Disabled
// instances are not probed. Whether an instance is *routable* is decided by the registry
// (active AND healthy); the checker never changes registration status.
//
// Discovery refresh: the registry is re-read every `refresh_interval`. New instances are
// probed immediately; deregistered or disabled ones stop being probed; an instance that
// was deregistered and registered again (different registered_at, host or port) starts
// over as a new instance. A failed refresh keeps the current set. Between refreshes the
// checker holds only per-instance runtime state (streak counters, timers), not a copy of
// the registry.
//
// Threads: one thread runs a private io_context (timers and probes, all state below is
// confined to it, no locks); one pool thread performs the blocking PostgreSQL calls, so
// database latency never delays probing. stop() cancels every timer and probe, waits for
// an in-flight database call (bounded by the registry's own timeouts) and joins both.
class HealthChecker {
 public:
  struct Stats {
    std::uint64_t probes_started{0};
    std::uint64_t probes_completed{0};
    std::uint64_t probes_failed{0};
    std::uint64_t transitions{0};
    std::uint64_t refreshes{0};
    std::uint64_t refresh_failures{0};
    std::uint64_t persist_failures{0};
    std::size_t tracked_instances{0};
  };

  // `prober` defaults to the TCP or HTTP prober selected by `config.type`.
  HealthChecker(std::shared_ptr<ServiceRegistry> registry, config::HealthCheckConfig config,
                std::shared_ptr<logging::Logger> logger, std::shared_ptr<Prober> prober = nullptr);
  ~HealthChecker();

  HealthChecker(const HealthChecker&) = delete;
  HealthChecker& operator=(const HealthChecker&) = delete;

  // Starts checking. Returns false if already started or stopped. Not restartable.
  bool start();
  // Idempotent and thread-safe. After it returns no probe, timer or callback is running.
  void stop();

  [[nodiscard]] Stats stats() const;

 private:
  struct Monitor;

  void refresh();
  void onRefreshed(Result<std::vector<ServiceInstance>> instances);
  void reconcile(const std::vector<ServiceInstance>& instances);
  void addMonitor(const ServiceInstance& instance);
  void removeMonitor(const std::shared_ptr<Monitor>& monitor);
  void scheduleProbe(const std::shared_ptr<Monitor>& monitor, std::chrono::milliseconds delay);
  void requestProbe(const std::shared_ptr<Monitor>& monitor);
  void startProbe(const std::shared_ptr<Monitor>& monitor);
  void onProbe(const std::shared_ptr<Monitor>& monitor, const ProbeResult& result);
  void pumpWaiting();
  void persistIfNeeded(const std::shared_ptr<Monitor>& monitor);
  void onPersisted(const std::shared_ptr<Monitor>& monitor, HealthStatus written,
                   const Result<ServiceInstance>& result);

  const std::shared_ptr<ServiceRegistry> registry_;
  const config::HealthCheckConfig config_;
  const HealthPolicy policy_;
  const std::shared_ptr<logging::Logger> logger_;
  const std::shared_ptr<Prober> prober_;

  // Lifecycle. stop_mutex_ serialises whole stop() calls, so every caller returns only
  // after shutdown has completed; lifecycle_mutex_ guards the two flags.
  std::mutex stop_mutex_;
  std::mutex lifecycle_mutex_;
  bool started_{false};
  bool stopped_{false};

  boost::asio::io_context io_{1};
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
  std::thread io_thread_;
  std::unique_ptr<boost::asio::thread_pool> db_;  // one thread: blocking registry calls

  // Confined to the io thread.
  boost::asio::steady_timer refresh_timer_{io_};
  std::map<std::string, std::shared_ptr<Monitor>> monitors_;
  std::deque<std::shared_ptr<Monitor>> waiting_;  // probes delayed by max_concurrent_checks
  unsigned active_probes_{0};
  bool refreshing_{false};
  bool refresh_failing_{false};
  bool shutting_down_{false};

  std::atomic<std::uint64_t> probes_started_{0};
  std::atomic<std::uint64_t> probes_completed_{0};
  std::atomic<std::uint64_t> probes_failed_{0};
  std::atomic<std::uint64_t> transitions_{0};
  std::atomic<std::uint64_t> refreshes_{0};
  std::atomic<std::uint64_t> refresh_failures_{0};
  std::atomic<std::uint64_t> persist_failures_{0};
  std::atomic<std::size_t> tracked_{0};
};

}  // namespace edgeflow::discovery
