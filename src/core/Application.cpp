#include "edgeflow/core/Application.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

#include "edgeflow/cache/ResponseCache.hpp"
#include "edgeflow/discovery/PostgresServiceRegistry.hpp"
#include "edgeflow/network/RegistryRequestHandler.hpp"
#include "edgeflow/ratelimit/RateLimiter.hpp"
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
  if (config_.health_check.enabled && registry_) {
    // Registered before the HTTP server so that, shutting down in reverse order, the HTTP
    // server stops first and the health checker after it.
    health_checker_ = std::make_unique<discovery::HealthChecker>(registry_, config_.health_check,
                                                                 logger_);
    shutdown_.registerComponent("health-checker", [this] { health_checker_->stop(); });
    health_checker_->start();
  } else if (config_.health_check.enabled) {
    logger_->warn("health checking is enabled but there is no service registry (custom request "
                  "handler): not starting it");
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
  registry_ = std::make_shared<discovery::PostgresServiceRegistry>(std::move(pool), logger_);
  router_ = std::make_shared<routing::Router>(registry_,
                                              routing::makeLoadBalancer(config_.routing.strategy));
  handler = std::make_shared<network::RegistryRequestHandler>(registry_, std::move(handler), logger_,
                                                               router_);
  logger_->info("service registry ready (PostgreSQL {}:{}/{}, {} migration(s) applied, "
                "pool size {}), routing strategy {}", db.host, db.port, db.name, report.applied,
                db.pool_size, router_->strategy());

  if (config_.proxy.enabled) {
    // Redis (Phase 8) is registered for shutdown FIRST, so shutting down in reverse it stops
    // LAST: the server, the health checker and the proxy have finished with every request
    // (and every Redis callback those requests were waiting for) by then.
    const bool policy_wanted = config_.cache.enabled || config_.rate_limit.enabled;
    if (config_.redis.enabled && policy_wanted) {
      std::string password;
      if (!config_.redis.password_env.empty()) {
        if (const char* value = std::getenv(config_.redis.password_env.c_str()); value != nullptr) {
          password = value;
        } else {
          logger_->warn("environment variable {} is not set; connecting to Redis without a "
                        "password", config_.redis.password_env);
        }
      }
      redis_ = redis::RedisClient::create(redis::RedisOptions::fromConfig(config_.redis, std::move(password)),
                                          logger_);
      shutdown_.registerComponent("redis", [this] { redis_->stop(); });
      // Redis is an optional helper: if it is down now the gateway still starts (the cache is
      // bypassed, the rate limiter follows rate_limit.on_redis_failure) and picks Redis up
      // as soon as it answers.
      const auto pong = redis_->commandSync({"PING"});
      if (pong.ok() && !pong.reply->isError()) {
        logger_->info("redis ready ({}:{}, {} connection(s), command timeout {}ms)", config_.redis.host,
                      config_.redis.port, config_.redis.pool_size,
                      config_.redis.command_timeout.count());
      } else {
        logger_->warn("redis {}:{} does not answer ({}); starting anyway: the cache is bypassed and "
                      "rate limiting {} until it does",
                      config_.redis.host, config_.redis.port,
                      pong.ok() ? pong.reply->text : pong.error->message,
                      config_.rate_limit.on_redis_failure == config::RateLimitFailurePolicy::Allow
                          ? "lets requests through"
                          : "refuses requests");
      }
    } else if (policy_wanted) {
      logger_->warn("cache or rate limiting is enabled but redis.enabled is false: ignoring both");
    }

    // Outermost handler: /proxy/... is forwarded, everything else reaches the registry API
    // and the local endpoints exactly as before. Registered for shutdown before the HTTP
    // server and the health checker, so (shutting down in reverse) it stops after both: the
    // server has then answered or cancelled every client, and the registry is still alive
    // while the proxy releases the connection counts of its last requests.
    std::shared_ptr<reliability::ReliabilityManager> rel;
    if (config_.reliability.enabled) {
      rel = std::make_shared<reliability::ReliabilityManager>(config_.reliability, logger_);
    }
    proxy_ = std::make_shared<proxy::ProxyHandler>(handler, router_, registry_, config_.proxy,
                                                   db.pool_size, logger_, nullptr, nullptr,
                                                   std::move(rel));
    shutdown_.registerComponent("reverse-proxy", [this] { proxy_->stop(); });
    handler = proxy_;

    if (redis_) {
      std::shared_ptr<ratelimit::RateLimiter> limiter;
      std::shared_ptr<cache::ResponseCache> response_cache;
      if (config_.rate_limit.enabled) {
        limiter = std::make_shared<ratelimit::RateLimiter>(redis_, config_.rate_limit,
                                                           config_.redis.key_prefix, logger_);
      }
      if (config_.cache.enabled) {
        response_cache = std::make_shared<cache::ResponseCache>(redis_, config_.cache,
                                                                config_.redis.key_prefix, logger_);
      }
      policy_ = std::make_shared<gateway::PolicyHandler>(handler, std::move(limiter),
                                                         std::move(response_cache), logger_);
      handler = policy_;
    }
  }
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
