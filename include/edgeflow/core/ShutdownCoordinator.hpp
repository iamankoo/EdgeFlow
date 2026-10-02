#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "edgeflow/logging/Logger.hpp"

namespace edgeflow::core {

// Owns the ordered, idempotent shutdown sequence. Components (today the signal
// handlers; later the network listeners, pools, ...) register a stop callback.
// Callbacks run once, in reverse registration order, so dependents stop before
// the things they depend on.
class ShutdownCoordinator {
 public:
  using StopCallback = std::function<void()>;

  explicit ShutdownCoordinator(std::shared_ptr<logging::Logger> logger);

  // Registering after shutdown has begun is rejected (returns false).
  bool registerComponent(std::string name, StopCallback stop);

  // Runs the sequence. Returns true for the call that performed it, false for
  // any later call. Concurrent callers block until the sequence has finished.
  // A throwing callback is logged and does not prevent the others from running.
  // Must not be called from inside a stop callback.
  bool shutdown(std::chrono::seconds grace_period);

  [[nodiscard]] bool completed() const;

 private:
  struct Component {
    std::string name;
    StopCallback stop;
  };

  std::shared_ptr<logging::Logger> logger_;
  mutable std::mutex mutex_;
  std::vector<Component> components_;
  bool started_{false};
  bool completed_{false};
};

}  // namespace edgeflow::core
