#include "edgeflow/core/Application.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <utility>

#include "edgeflow/cache/ResponseCache.hpp"
#include "edgeflow/discovery/PostgresServiceRegistry.hpp"
#include "edgeflow/network/RegistryRequestHandler.hpp"
#include "edgeflow/observability/MetricsHandler.hpp"
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
    if (config_.metrics.enabled) metrics_ = std::make_shared<observability::Metrics>(config_.metrics);
    handler = std::make_shared<network::LocalRequestHandler>();
    if (config_.database.enabled && !initializeRegistry(handler)) {
      signals_.reset();
      return false;
    }
    if (metrics_) {
      // The outermost handler: the metrics endpoint is answered above the rate limiter and the
      // cache, and every other request is timed from here to its response.
      registerMetricsCollectors();
      handler = std::make_shared<observability::MetricsHandler>(handler, metrics_);
      logger_->info("metrics ready: GET {} (Prometheus text format), {} s window", config_.metrics.path,
                    config_.metrics.window_seconds);
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
    if (metrics_) proxy_->attachMetrics(metrics_);
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

void Application::registerMetricsCollectors() {
  using observability::Labels;
  using observability::MetricsWriter;
  auto u64 = [](std::uint64_t value) { return value; };
  auto ratio = [](std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? std::numeric_limits<double>::quiet_NaN()
                      : static_cast<double>(part) / static_cast<double>(whole);
  };

  // The HTTP server: connections.
  metrics_->addCollector([this, u64](MetricsWriter& w) {
    if (!http_server_) return;
    w.gauge("edgeflow_http_active_connections", "Client connections open right now.",
            static_cast<double>(http_server_->activeConnections()));
    w.counter("edgeflow_http_connections_accepted_total", "Client connections accepted.",
              u64(http_server_->acceptedConnections()));
    w.counter("edgeflow_http_connections_rejected_total",
              "Client connections closed at once because server.max_connections was reached.",
              u64(http_server_->rejectedConnections()));
  });

  // The reverse proxy, its upstream connections and the Phase 7 reliability layer.
  if (proxy_) {
    metrics_->addCollector([this](MetricsWriter& w) {
      const auto stats = proxy_->stats();
      w.counter("edgeflow_proxy_requests_total", "Proxied requests started.", stats.requests);
      w.gauge("edgeflow_proxy_active_requests", "Proxied requests in flight.", static_cast<double>(stats.active));
      w.counter("edgeflow_proxy_backend_responses_total", "Backend responses forwarded to clients (any status).",
                stats.responses);
      w.counter("edgeflow_proxy_gateway_errors_total", "Responses EdgeFlow generated itself (400/404/502/503/504).",
                stats.gateway_errors);
      std::vector<std::pair<Labels, std::uint64_t>> outcomes;
      for (std::size_t i = 0; i < stats.attempt_outcomes.size(); ++i) {
        outcomes.push_back({Labels{{"outcome", reliability::toString(static_cast<reliability::AttemptKind>(i))}},
                            stats.attempt_outcomes[i]});
      }
      w.counter("edgeflow_backend_attempts_total", "Attempts to reach a backend, by outcome.", outcomes);
      w.counter("edgeflow_upstream_connections_created_total", "New connections opened to backends.", stats.pool.created);
      w.counter("edgeflow_upstream_connections_reused_total", "Requests that reused an idle pooled backend connection.",
                stats.pool.reused);
      w.counter("edgeflow_upstream_connections_stale_total", "Idle pooled connections found dead at check-out.",
                stats.pool.stale_dropped);
    });
    if (proxy_->reliability() != nullptr) {
      metrics_->addCollector([this](MetricsWriter& w) {
        auto* reliability = proxy_->reliability();
        const auto stats = reliability->stats();
        w.counter("edgeflow_retries_total", "Attempts that followed a failed attempt of the same request.", stats.retries);
        w.counter("edgeflow_failovers_total", "Retries that went to a different backend.", stats.failovers);
        w.counter("edgeflow_retry_exhausted_total", "Requests that gave up with retries or time budget spent.",
                  stats.exhausted);
        w.counter("edgeflow_circuit_breaker_trips_total", "Times a backend's circuit opened (closed or half-open to open).",
                  stats.circuit_opened);
        w.counter("edgeflow_circuit_breaker_rejections_total", "Backends skipped because their circuit refused.",
                  stats.circuit_rejections);
        if (auto* breakers = reliability->breakers()) {
          const auto counts = breakers->stateCounts();
          w.gauge("edgeflow_circuit_breakers", "Backend circuits that exist, by state.",
                  {{Labels{{"state", "closed"}}, static_cast<double>(counts.closed)},
                   {Labels{{"state", "open"}}, static_cast<double>(counts.open)},
                   {Labels{{"state", "half_open"}}, static_cast<double>(counts.half_open)}});
        }
      });
    }
  }

  // Rate limiting, the response cache and Redis.
  if (policy_) {
    metrics_->addCollector([this, ratio](MetricsWriter& w) {
      const auto stats = policy_->stats();
      w.counter("edgeflow_policy_requests_total", "Proxied requests that entered rate limiting / caching.", stats.seen);
      w.counter("edgeflow_policy_served_from_cache_total", "Requests answered from the cache.", stats.served_from_cache);
      w.counter("edgeflow_policy_forwarded_total", "Requests handed on to the proxy.", stats.forwarded);
      if (auto* limiter = policy_->limiter()) {
        const auto l = limiter->stats();
        w.counter("edgeflow_rate_limit_decisions_total", "Rate limit decisions.",
                  {{Labels{{"result", "allowed"}}, l.allowed},
                   {Labels{{"result", "limited"}}, l.limited},
                   {Labels{{"result", "degraded_allowed"}}, l.degraded_allowed},
                   {Labels{{"result", "degraded_rejected"}}, l.degraded_rejected}});
      }
      if (auto* cache = policy_->cache()) {
        const auto c = cache->stats();
        w.counter("edgeflow_cache_lookups_total", "Cache lookups by result (error = Redis could not answer: the request bypassed the cache).",
                  {{Labels{{"result", "hit"}}, c.hits},
                   {Labels{{"result", "miss"}}, c.misses},
                   {Labels{{"result", "error"}}, c.errors}});
        w.gauge("edgeflow_cache_hit_ratio", "Cache hits / (hits + misses) since start (NaN before the first lookup).",
                ratio(c.hits, c.hits + c.misses));
        w.counter("edgeflow_cache_stores_total", "Responses stored in the cache.", c.stores);
        w.counter("edgeflow_cache_store_failures_total", "Stores that failed.", c.store_failures);
        w.counter("edgeflow_cache_invalidations_total", "Entries removed because a write succeeded.", c.invalidations);
      }
    });
  }
  // Requests the gateway refused instead of serving (one family, whatever the mechanism).
  metrics_->addCollector([this](MetricsWriter& w) {
    std::vector<std::pair<Labels, std::uint64_t>> reasons;
    if (policy_) {
      const auto stats = policy_->stats();
      reasons.push_back({Labels{{"reason", "rate_limited"}}, stats.rate_limited});
      reasons.push_back({Labels{{"reason", "rate_limit_unavailable"}}, stats.refused_degraded});
    }
    if (http_server_) {
      reasons.push_back({Labels{{"reason", "connection_limit"}}, http_server_->rejectedConnections()});
    }
    if (!reasons.empty()) {
      w.counter("edgeflow_rejected_requests_total",
                "Requests refused by the gateway: 429 (rate_limited), 503 because Redis was down and the limiter fails closed "
                "(rate_limit_unavailable), connections closed at the connection limit (connection_limit).",
                reasons);
    }
  });
  if (redis_) {
    metrics_->addCollector([this](MetricsWriter& w) {
      const auto stats = redis_->stats();
      w.gauge("edgeflow_redis_up", "1 while Redis answers, 0 while commands fail at once after a failure.",
              stats.available ? 1.0 : 0.0);
      w.counter("edgeflow_redis_commands_total", "Redis commands submitted.", stats.commands);
      w.counter("edgeflow_redis_failures_total", "Redis commands that ended with a transport error.", stats.failures);
      w.counter("edgeflow_redis_fast_failures_total", "... of which failed at once without touching the network.",
                stats.fast_failures);
      w.counter("edgeflow_redis_connections_opened_total", "Connections opened to Redis.", stats.connections_opened);
    });
  }
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
