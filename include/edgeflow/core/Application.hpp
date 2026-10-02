#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/core/ShutdownCoordinator.hpp"
#include "edgeflow/core/SignalHandler.hpp"
#include "edgeflow/logging/Logger.hpp"

namespace edgeflow::core {

enum class ApplicationState { Created, Initialized, Running, Stopped };

struct ApplicationOptions {
  // Disabled by tests that must not touch process-wide signal handlers.
  bool install_signal_handlers{true};
};

// Application lifecycle: construct -> initialize() -> run() -> shutdown().
// shutdown() is idempotent and is also triggered automatically when run() returns.
class Application {
 public:
  Application(config::Config config, std::shared_ptr<logging::Logger> logger,
              ApplicationOptions options = {});
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  // Prepares required infrastructure. Returns false (after logging why) on failure.
  [[nodiscard]] bool initialize();

  // Blocks until a shutdown is requested (signal or requestShutdown()), then shuts
  // down. Returns the process exit code: 0 on a clean stop, 1 if not initialized.
  [[nodiscard]] int run();

  // Thread-safe; makes run() return. Not for use from signal handlers.
  void requestShutdown() noexcept;

  // Idempotent and safe to call from any state.
  void shutdown();

  [[nodiscard]] ApplicationState state() const noexcept { return state_.load(); }

  // Future components (e.g. network listeners) register their stop callbacks here.
  [[nodiscard]] ShutdownCoordinator& shutdownCoordinator() noexcept { return shutdown_; }

 private:
  config::Config config_;
  std::shared_ptr<logging::Logger> logger_;
  ApplicationOptions options_;
  ShutdownCoordinator shutdown_;
  std::unique_ptr<SignalHandler> signals_;

  std::atomic<ApplicationState> state_{ApplicationState::Created};
  std::atomic<bool> stop_requested_{false};
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
};

}  // namespace edgeflow::core
