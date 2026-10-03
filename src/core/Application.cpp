#include "edgeflow/core/Application.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

#include "edgeflow/discovery/PostgresServiceRegistry.hpp"
#include "edgeflow/network/RegistryRequestHandler.hpp"
#include "edgeflow/storage/Migrator.hpp"
#include "edgeflow/storage/Postgres.hpp"

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

  auto handler = options_.request_handler;
  if (!handler) {
    handler = std::make_shared<network::LocalRequestHandler>();
    if (config_.database.enabled && !initializeRegistry(handler)) {
      signals_.reset();
      return false;
    }
  }
  http_server_ =
      std::make_unique<network::HttpServer>(config_.server, logger_, std::move(handler));
  if (!http_server_->start()) {
    logger_->error("failed to start the HTTP server on {}:{}", config_.server.host,
                   config_.server.port);
    http_server_.reset();
    signals_.reset();
    return false;
  }
  shutdown_.registerComponent("http-server", [this] {
    http_server_->stop(
        std::chrono::duration_cast<std::chrono::milliseconds>(config_.shutdown.grace_period));
  });

  logger_->info("application initialized (name={}, environment={})", config_.application.name,
                config::toString(config_.application.environment));
  state_.store(ApplicationState::Initialized);
  return true;
}

bool Application::initializeRegistry(std::shared_ptr<network::RequestHandler>& handler) {
  const auto& db = config_.database;

  storage::ConnectionParams params;
  params.host = db.host;
  params.port = db.port;
  params.database = db.name;
  params.user = db.user;
  params.connect_timeout = db.connect_timeout;
  if (!db.password_env.empty()) {
    // The password lives only in the environment and in memory; it is never logged.
    if (const char* password = std::getenv(db.password_env.c_str()); password != nullptr) {
      params.password = password;
    } else {
      logger_->warn("environment variable {} is not set; connecting to the database without "
                    "a password", db.password_env);
    }
  }

  auto pool = std::make_shared<storage::PgPool>(std::move(params), db.pool_size, logger_);
  const auto report = storage::migrate(*pool, storage::builtinMigrations(), *logger_);
  if (!report.ok) {
    logger_->error("cannot initialize the service registry database ({}:{}/{}): {}", db.host,
                   db.port, db.name, report.error);
    return false;
  }
  auto registry = std::make_shared<discovery::PostgresServiceRegistry>(std::move(pool), logger_);
  handler = std::make_shared<network::RegistryRequestHandler>(std::move(registry),
                                                               std::move(handler), logger_);
  logger_->info("service registry ready (PostgreSQL {}:{}/{}, {} migration(s) applied, "
                "pool size {})", db.host, db.port, db.name, report.applied, db.pool_size);
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
