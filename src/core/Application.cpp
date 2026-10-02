#include "edgeflow/core/Application.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <utility>

namespace edgeflow::core {

namespace {
constexpr std::chrono::milliseconds kPollInterval{50};
}

Application::Application(config::Config config, std::shared_ptr<logging::Logger> logger,
                         ApplicationOptions options)
    : config_(std::move(config)),
      logger_(std::move(logger)),
      options_(options),
      shutdown_(logger_) {}

Application::~Application() {
  try {
    const auto state = state_.load();
    if (state == ApplicationState::Initialized || state == ApplicationState::Running) {
      shutdown();
    }
  } catch (const std::exception& e) {
    std::cerr << "error during application teardown: " << e.what() << '\n';
  }
}

bool Application::initialize() {
  if (state_.load() != ApplicationState::Created) {
    logger_->error("initialize() called in an invalid state");
    return false;
  }

  if (options_.install_signal_handlers) {
    auto handlers = std::make_unique<SignalHandler>();
    if (!handlers->installed()) {
      logger_->error("failed to install signal handlers (already installed elsewhere?)");
      return false;
    }
    signals_ = std::move(handlers);
    shutdown_.registerComponent("signal-handlers", [this] { signals_.reset(); });
  }

  logger_->debug("configured listen endpoint {}:{} (networking not enabled)",
                 config_.server.host, config_.server.port);
  logger_->info("application initialized (name={}, environment={})", config_.application.name,
                config::toString(config_.application.environment));
  state_.store(ApplicationState::Initialized);
  return true;
}

int Application::run() {
  if (state_.load() != ApplicationState::Initialized) {
    logger_->error("run() requires a successfully initialized application");
    return 1;
  }
  state_.store(ApplicationState::Running);
  logger_->info("EdgeFlow ready");

  int signal = 0;
  while (!stop_requested_.load()) {
    signal = SignalHandler::receivedSignal();
    if (signal != 0) break;
    std::unique_lock lock(wait_mutex_);
    wait_cv_.wait_for(lock, kPollInterval, [this] { return stop_requested_.load(); });
  }

  if (signal != 0) {
    logger_->info("shutdown requested ({})", SignalHandler::signalName(signal));
  } else {
    logger_->info("shutdown requested");
  }
  shutdown();
  return 0;
}

void Application::requestShutdown() noexcept {
  stop_requested_.store(true);
  wait_cv_.notify_all();
}

void Application::shutdown() {
  shutdown_.shutdown(config_.shutdown.grace_period);
  state_.store(ApplicationState::Stopped);
}

}  // namespace edgeflow::core
