#include "edgeflow/discovery/HealthChecker.hpp"

#include <future>
#include <set>
#include <utility>

#include <boost/asio/post.hpp>

namespace edgeflow::discovery {

namespace net = boost::asio;

// Runtime state of one monitored instance. Touched only on the io thread.
struct HealthChecker::Monitor {
  Monitor(net::io_context& io, ServiceInstance snapshot)
      : instance(std::move(snapshot)),
        tracker(instance.health),
        persisted(instance.health),
        timer(io) {}

  ServiceInstance instance;  // identity and endpoint as last read from the registry
  HealthTracker tracker;
  HealthStatus persisted;    // what the registry is believed to hold
  net::steady_timer timer;
  std::shared_ptr<ProbeHandle> probe;
  bool probing{false};
  bool persisting{false};
  bool removed{false};
};

namespace {

std::string keyOf(const ServiceInstance& instance) {
  // Neither a service name nor an instance id can contain '/'.
  return instance.service + "/" + instance.instance_id;
}

bool sameIncarnation(const ServiceInstance& a, const ServiceInstance& b) {
  return a.registered_at == b.registered_at && a.host == b.host && a.port == b.port;
}

}  // namespace

HealthChecker::HealthChecker(std::shared_ptr<ServiceRegistry> registry,
                             config::HealthCheckConfig config,
                             std::shared_ptr<logging::Logger> logger,
                             std::shared_ptr<Prober> prober)
    : registry_(std::move(registry)),
      config_(std::move(config)),
      policy_{config_.failure_threshold, config_.success_threshold},
      logger_(std::move(logger)),
      prober_(prober ? std::move(prober) : makeProber(config_.type)) {}

HealthChecker::~HealthChecker() { stop(); }

bool HealthChecker::start() {
  {
    const std::lock_guard lock(lifecycle_mutex_);
    if (started_ || stopped_) return false;
    started_ = true;
  }
  db_ = std::make_unique<net::thread_pool>(1);
  work_ = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(
      io_.get_executor());
  io_thread_ = std::thread([this] {
    try {
      io_.run();
    } catch (const std::exception& e) {
      logger_->error("health checker I/O thread terminated by an exception: {}", e.what());
    }
  });
  net::post(io_, [this] { refresh(); });
  logger_->info("health checker started (type={}, interval={}ms, timeout={}ms, failure "
                "threshold={}, success threshold={}, refresh every {}ms)",
                config::toString(config_.type), config_.interval.count(), config_.timeout.count(),
                config_.failure_threshold, config_.success_threshold,
                config_.refresh_interval.count());
  return true;
}

void HealthChecker::stop() {
  const std::lock_guard stop_lock(stop_mutex_);
  {
    const std::lock_guard lock(lifecycle_mutex_);
    if (!started_ || stopped_) {
      stopped_ = true;
      return;
    }
    stopped_ = true;
  }

  // 1. On the io thread: stop scheduling and cancel every timer and probe, so nothing new
  //    starts and no callback can run afterwards.
  std::promise<void> cancelled;
  auto cancelled_future = cancelled.get_future();
  net::post(io_, [this, &cancelled] {
    shutting_down_ = true;
    refresh_timer_.cancel();
    for (auto& entry : monitors_) removeMonitor(entry.second);
    monitors_.clear();
    waiting_.clear();
    tracked_.store(0);
    cancelled.set_value();
  });
  cancelled_future.wait();

  // 2. Stop the io thread, then wait for an in-flight database call (queued ones are dropped).
  work_.reset();
  io_.stop();
  if (io_thread_.joinable()) io_thread_.join();
  db_->stop();
  db_->join();
  logger_->info("health checker stopped ({} probe(s) run, {} state change(s))",
                probes_completed_.load(), transitions_.load());
}

HealthChecker::Stats HealthChecker::stats() const {
  Stats stats;
  stats.probes_started = probes_started_.load();
  stats.probes_completed = probes_completed_.load();
  stats.probes_failed = probes_failed_.load();
  stats.transitions = transitions_.load();
  stats.refreshes = refreshes_.load();
  stats.refresh_failures = refresh_failures_.load();
  stats.persist_failures = persist_failures_.load();
  stats.tracked_instances = tracked_.load();
  return stats;
}

// --- discovery refresh -----------------------------------------------------------------

void HealthChecker::refresh() {
  if (shutting_down_ || refreshing_) return;
  refreshing_ = true;
  net::post(*db_, [this] {
    auto instances = registry_->listInstances();  // blocking database call, off the io thread
    net::post(io_, [this, instances = std::move(instances)]() mutable {
      onRefreshed(std::move(instances));
    });
  });
}

void HealthChecker::onRefreshed(Result<std::vector<ServiceInstance>> instances) {
  refreshing_ = false;
  if (shutting_down_) return;
  ++refreshes_;
  if (instances.ok()) {
    if (refresh_failing_) logger_->info("discovery refresh recovered");
    refresh_failing_ = false;
    reconcile(instances.value());
  } else {
    ++refresh_failures_;
    // Keep checking the instances already known; only log when the condition begins.
    if (!refresh_failing_) {
      logger_->warn("discovery refresh failed, keeping the current instances: {}",
                    instances.error().message);
    }
    refresh_failing_ = true;
  }
  refresh_timer_.expires_after(config_.refresh_interval);
  refresh_timer_.async_wait([this](const boost::system::error_code& error) {
    if (!error) refresh();
  });
}

void HealthChecker::reconcile(const std::vector<ServiceInstance>& instances) {
  std::set<std::string> present;
  for (const auto& instance : instances) {
    // Disabled instances are not probed. Draining ones still are: their health is useful
    // while they finish serving, and they become routable again only if re-activated.
    if (instance.status == InstanceStatus::Disabled) continue;
    const std::string key = keyOf(instance);
    present.insert(key);

    const auto existing = monitors_.find(key);
    if (existing != monitors_.end()) {
      const auto& monitor = existing->second;
      if (sameIncarnation(monitor->instance, instance)) {
        monitor->instance = instance;  // refresh status, version, weight, ...
        // Adopt what the registry really holds, so a manual or stale value is corrected by
        // the next probe instead of being assumed correct.
        if (!monitor->persisting) monitor->persisted = instance.health;
        continue;
      }
      logger_->info("instance {} changed (re-registered or moved): checking it as a new instance",
                    key);
      removeMonitor(monitor);
      monitors_.erase(existing);
    }
    addMonitor(instance);
  }

  for (auto it = monitors_.begin(); it != monitors_.end();) {
    if (present.count(it->first) == 0) {
      logger_->info("no longer checking instance {} (deregistered or disabled)", it->first);
      removeMonitor(it->second);
      it = monitors_.erase(it);
    } else {
      ++it;
    }
  }
  tracked_.store(monitors_.size());
}

void HealthChecker::addMonitor(const ServiceInstance& instance) {
  auto monitor = std::make_shared<Monitor>(io_, instance);
  monitors_[keyOf(instance)] = monitor;
  logger_->info("now checking instance {} at {}:{} (health {})", keyOf(instance), instance.host,
                instance.port, toString(instance.health));
  scheduleProbe(monitor, std::chrono::milliseconds{0});  // a new instance is probed at once
}

void HealthChecker::removeMonitor(const std::shared_ptr<Monitor>& monitor) {
  monitor->removed = true;
  monitor->timer.cancel();
  if (monitor->probe) {
    monitor->probe->cancel();  // its completion callback will never run
    monitor->probe.reset();
  }
  if (monitor->probing) {
    monitor->probing = false;
    if (active_probes_ > 0) --active_probes_;
  }
  if (!shutting_down_) pumpWaiting();
}

// --- probing ------------------------------------------------------------------------------

void HealthChecker::scheduleProbe(const std::shared_ptr<Monitor>& monitor,
                                  std::chrono::milliseconds delay) {
  monitor->timer.expires_after(delay);
  monitor->timer.async_wait([this, monitor](const boost::system::error_code& error) {
    if (error || monitor->removed || shutting_down_) return;
    requestProbe(monitor);
  });
}

void HealthChecker::requestProbe(const std::shared_ptr<Monitor>& monitor) {
  if (active_probes_ >= config_.max_concurrent_checks) {
    waiting_.push_back(monitor);
    return;
  }
  startProbe(monitor);
}

void HealthChecker::startProbe(const std::shared_ptr<Monitor>& monitor) {
  ++active_probes_;
  monitor->probing = true;
  ++probes_started_;
  ProbeTarget target;
  target.host = monitor->instance.host;
  target.port = monitor->instance.port;
  target.http_path = config_.http_path;
  target.timeout = config_.timeout;
  monitor->probe = prober_->start(io_, std::move(target), [this, monitor](ProbeResult result) {
    onProbe(monitor, result);
  });
}

void HealthChecker::onProbe(const std::shared_ptr<Monitor>& monitor, const ProbeResult& result) {
  // The probe was cancelled (instance removed) before this result arrived: removeMonitor()
  // already released its slot, so the result must not be counted or applied.
  if (!monitor->probing) return;
  monitor->probing = false;
  monitor->probe.reset();
  if (active_probes_ > 0) --active_probes_;
  ++probes_completed_;
  if (!result.healthy) ++probes_failed_;

  if (!monitor->removed && !shutting_down_) {
    const auto before = monitor->tracker.status();
    const auto transition = monitor->tracker.record(result.healthy, policy_);
    const std::string key = keyOf(monitor->instance);
    if (!result.healthy) {
      logger_->debug("health check of {} failed: {}", key, result.detail);
    }
    if (transition) {
      ++transitions_;
      if (*transition == HealthStatus::Unhealthy) {
        logger_->warn("instance {} ({}:{}) is now unhealthy (was {}): {}", key,
                      monitor->instance.host, monitor->instance.port, toString(before),
                      result.detail);
      } else {
        logger_->info("instance {} ({}:{}) is now healthy (was {}): {}", key,
                      monitor->instance.host, monitor->instance.port, toString(before),
                      result.detail);
      }
    }
    persistIfNeeded(monitor);
    scheduleProbe(monitor, config_.interval);
  }
  pumpWaiting();
}

void HealthChecker::pumpWaiting() {
  while (active_probes_ < config_.max_concurrent_checks && !waiting_.empty()) {
    auto next = std::move(waiting_.front());
    waiting_.pop_front();
    if (next->removed || next->probing) continue;
    startProbe(next);
  }
}

// --- persistence ----------------------------------------------------------------------------

void HealthChecker::persistIfNeeded(const std::shared_ptr<Monitor>& monitor) {
  const auto desired = monitor->tracker.status();
  if (monitor->persisting || monitor->removed || shutting_down_) return;
  if (desired == HealthStatus::Unknown || desired == monitor->persisted) return;

  monitor->persisting = true;
  // Copies of what the database thread needs: it never touches the monitor.
  net::post(*db_, [this, monitor, desired, service = monitor->instance.service,
                   id = monitor->instance.instance_id,
                   registered_at = monitor->instance.registered_at] {
    auto result = registry_->updateHealth(service, id, registered_at, desired);
    net::post(io_, [this, monitor, desired, result = std::move(result)] {
      onPersisted(monitor, desired, result);
    });
  });
}

void HealthChecker::onPersisted(const std::shared_ptr<Monitor>& monitor, HealthStatus written,
                                const Result<ServiceInstance>& result) {
  monitor->persisting = false;
  if (shutting_down_) return;
  if (result.ok()) {
    monitor->persisted = written;
  } else if (result.error().code == RegistryErrorCode::InstanceNotFound) {
    // Deregistered or re-registered meanwhile; the next refresh drops or replaces it.
    logger_->info("health of {} not recorded: {}", keyOf(monitor->instance),
                  result.error().message);
    monitor->persisted = written;
  } else {
    ++persist_failures_;
    // Retried after the next probe, because persisted != the tracker's status.
    logger_->warn("could not record the health of {}: {}", keyOf(monitor->instance),
                  result.error().message);
  }
  // The status may have moved on while the write was in flight. After a failure the write
  // is retried only after the next probe, so an outage cannot cause a retry loop.
  if (result.ok() && !monitor->removed) persistIfNeeded(monitor);
}

}  // namespace edgeflow::discovery
