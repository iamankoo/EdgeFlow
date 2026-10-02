#include "edgeflow/core/ShutdownCoordinator.hpp"

#include <exception>
#include <utility>

namespace edgeflow::core {

ShutdownCoordinator::ShutdownCoordinator(std::shared_ptr<logging::Logger> logger)
    : logger_(std::move(logger)) {}

bool ShutdownCoordinator::registerComponent(std::string name, StopCallback stop) {
  const std::lock_guard lock(mutex_);
  if (started_) return false;
  components_.push_back({std::move(name), std::move(stop)});
  return true;
}

bool ShutdownCoordinator::shutdown(std::chrono::seconds grace_period) {
  // The mutex is held for the whole sequence: later callers block until it is
  // finished and then observe started_ == true.
  const std::lock_guard lock(mutex_);
  if (started_) return false;
  started_ = true;

  logger_->info("shutdown sequence started (grace period {}s)", grace_period.count());
  const auto begin = std::chrono::steady_clock::now();

  for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
    logger_->info("stopping component '{}'", it->name);
    try {
      it->stop();
    } catch (const std::exception& e) {
      logger_->error("component '{}' failed to stop cleanly: {}", it->name, e.what());
    }
  }
  components_.clear();

  const auto elapsed = std::chrono::steady_clock::now() - begin;
  if (elapsed > grace_period) {
    logger_->warn("shutdown exceeded the configured grace period of {}s",
                  grace_period.count());
  }

  completed_ = true;
  logger_->info("shutdown completed");
  logger_->flush();
  return true;
}

bool ShutdownCoordinator::completed() const {
  const std::lock_guard lock(mutex_);
  return completed_;
}

}  // namespace edgeflow::core
