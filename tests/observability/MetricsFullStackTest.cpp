#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/core/Application.hpp"
#include "support/MetricsTestSupport.hpp"
#include "support/PgTestSupport.hpp"
#include "support/ProxyTestSupport.hpp"
#include "support/RedisTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using Action = ScriptedBackend::Action;
namespace http = boost::beast::http;

// The whole of EdgeFlow's Application over a real PostgreSQL and a real Redis with every Phase 9
// metric switched on, driven by real clients against scripted backends. Each test provokes one
// kind of behaviour and checks that the metric that is supposed to see it did.
class MetricsFullStackTest : public RegistryFixture {
 protected:
  config::Config appConfig(const std::string& redis_host, std::uint16_t redis_port, unsigned ip_capacity = 1000) {
    config::Config config;
    config.shutdown.grace_period = 1s;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.database.enabled = true;
    config.database.host = params_.host;
    config.database.port = params_.port;
    config.database.name = params_.database;
    config.database.user = params_.user;
    config.database.password_env = "EDGEFLOW_TEST_DB_PASSWORD";
    config.health_check.enabled = true;
    config.health_check.type = config::HealthCheckType::Http;
    config.health_check.interval = 50ms;
    config.health_check.timeout = 300ms;
    config.health_check.refresh_interval = 50ms;
    config.health_check.failure_threshold = 2;
    config.health_check.success_threshold = 1;
    config.proxy.enabled = true;
    config.proxy.upstream_timeout = 5000ms;
    config.reliability = testReliabilityConfig();
    config.redis.enabled = true;
    config.redis.host = redis_host;
    config.redis.port = redis_port;
    config.redis.key_prefix = uniqueName("m9");
    config.redis.command_timeout = 500ms;
    config.redis.connect_timeout = 300ms;
    config.redis.retry_interval = 200ms;
    config.cache.enabled = true;
    config.cache.default_ttl = 60s;
    config.rate_limit.enabled = true;
    config.rate_limit.ip.enabled = true;
    config.rate_limit.ip.capacity = ip_capacity;
    config.rate_limit.ip.refill_per_second = 0.001;
    config.metrics.enabled = true;
    config.metrics.window_seconds = 10;
    return config;
  }

  void start(const config::Config& config) {
    app = std::make_unique<core::Application>(
        config, appLog.logger(),
        core::ApplicationOptions{.install_signal_handlers = false, .request_handler = nullptr});
    ASSERT_TRUE(app->initialize()) << appLog.output();
    ASSERT_NE(app->metrics(), nullptr);
  }

  void add(const std::string& service, const std::string& id, std::uint16_t port) {
    discovery::NewInstance instance = makeInstance(service, "127.0.0.1", port);
    instance.instance_id = id;
    ASSERT_TRUE(registry->registerInstance(instance).ok());
  }

  bool routable(const std::string& service, std::size_t n) {
    return waitFor([&] {
      const auto r = registry->lookupRoutable(service);
      return r.ok() && r.value().size() == n;
    });
  }

  Exposition scrape() {
    TestClient client(app->httpPort());
    const auto response = client.get("/metrics");
    EXPECT_TRUE(response);
    return Exposition(response ? response->body() : std::string{});
  }

  static Action reply(unsigned code, std::string body = "ok", std::chrono::milliseconds delay = 0ms) {
    Action action;
    action.code = code;
    action.body = std::move(body);
    action.delay = delay;
    return action;
  }

  void TearDown() override {
    if (app) app->shutdown();
    app.reset();
    RegistryFixture::TearDown();
  }

  CapturedLogger appLog;
  std::unique_ptr<core::Application> app;
};

#define REQUIRE_REDIS_AND_BUILD(config_name)                                   \
  EDGEFLOW_REQUIRE_TEST_REDIS();                                               \
  auto config_name = appConfig(test_redis->host, test_redis->port)

TEST_F(MetricsFullStackTest, EveryPhaseNineMetricFamilyIsExposedAndTheDocumentIsValid) {
  REQUIRE_REDIS_AND_BUILD(config);
  start(config);
  const auto exposition = scrape();
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  for (const char* family :
       {// requests/sec, request count, error rate, p50/p95/p99
        "edgeflow_http_requests_per_second", "edgeflow_http_requests_total", "edgeflow_http_responses_total",
        "edgeflow_http_errors_total", "edgeflow_http_error_ratio", "edgeflow_http_request_latency_seconds",
        "edgeflow_http_request_duration_seconds",
        // active connections
        "edgeflow_http_active_connections", "edgeflow_http_connections_accepted_total",
        // backend latency
        "edgeflow_backend_latency_seconds", "edgeflow_backend_latency_window_seconds", "edgeflow_backend_attempts_total",
        // cache hit ratio
        "edgeflow_cache_hit_ratio", "edgeflow_cache_lookups_total",
        // retry count, circuit-breaker trips
        "edgeflow_retries_total", "edgeflow_circuit_breaker_trips_total", "edgeflow_circuit_breakers",
        // rejected requests
        "edgeflow_rejected_requests_total", "edgeflow_rate_limit_decisions_total",
        // supporting
        "edgeflow_redis_up", "edgeflow_proxy_requests_total", "edgeflow_upstream_connections_created_total",
        "edgeflow_failovers_total", "edgeflow_build_info", "edgeflow_uptime_seconds"}) {
    EXPECT_TRUE(exposition.has(family)) << family;
  }
  EXPECT_EQ(exposition.must("edgeflow_redis_up"), 1);
}

TEST_F(MetricsFullStackTest, RequestCountsLatencyAndBackendLatencyFollowRealTraffic) {
  REQUIRE_REDIS_AND_BUILD(config);
  const auto service = newService("shop");
  ScriptedBackend backend(
      [](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
        if (request.target() == "/health") return reply(200);
        if (request.target().find("slow") != std::string::npos) return reply(200, "slow", 120ms);
        return reply(200);
      },
      "a");
  add(service, "a", backend.port());
  start(config);
  ASSERT_TRUE(routable(service, 1));

  TestClient client(app->httpPort());
  for (int i = 0; i < 6; ++i) ASSERT_EQ(client.get("/proxy/" + service + "/fast?i=" + std::to_string(i))->result_int(), 200U);
  for (int i = 0; i < 6; ++i) ASSERT_EQ(client.get("/proxy/" + service + "/slow?i=" + std::to_string(i))->result_int(), 200U);
  ASSERT_EQ(client.get("/proxy/nosuchservice/x")->result_int(), 404U);
  ASSERT_EQ(client.get("/health")->result_int(), 200U);
  std::this_thread::sleep_for(1200ms);  // let a second complete: the rate gauge counts completed seconds

  const auto exposition = scrape();
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "proxy"}}), 13);
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "local"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), 13);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "4xx"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_proxy_requests_total"), 13);
  EXPECT_EQ(exposition.must("edgeflow_proxy_backend_responses_total"), 12);
  EXPECT_EQ(exposition.must("edgeflow_proxy_gateway_errors_total"), 1);
  EXPECT_EQ(exposition.must("edgeflow_backend_attempts_total", {{"outcome", "success"}}), 12);
  EXPECT_EQ(exposition.must("edgeflow_backend_latency_seconds_count"), 12);
  EXPECT_GT(exposition.must("edgeflow_http_requests_per_second"), 0.0);

  // Half of the backend answers took >= 120 ms, half were fast: the quantiles must see both.
  const double b50 = exposition.must("edgeflow_backend_latency_window_seconds", {{"quantile", "0.5"}});
  const double b99 = exposition.must("edgeflow_backend_latency_window_seconds", {{"quantile", "0.99"}});
  EXPECT_LT(b50, 0.120);
  EXPECT_GE(b99, 0.100) << "the slow half is in the tail";
  EXPECT_LT(b99, 0.300);
  const double r99 = exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.99"}});
  EXPECT_GE(r99, b99 * 0.8) << "a client waits at least as long as the backend takes (bucket resolution aside)";
  EXPECT_NEAR(exposition.must("edgeflow_backend_latency_seconds_sum"), 6 * 0.120, 0.5) << "sum of the 12 answers";
}

TEST_F(MetricsFullStackTest, RetriesFailoversAndCircuitTripsAreCounted) {
  REQUIRE_REDIS_AND_BUILD(config);
  const auto service = newService("flaky");
  ScriptedBackend bad([](const auto& request, auto, auto) { return reply(request.target() == "/health" ? 200U : 503U); }, "bad");
  ScriptedBackend good([](const auto&, auto, auto) { return reply(200); }, "good");
  add(service, "bad", bad.port());
  add(service, "good", good.port());
  start(config);
  ASSERT_TRUE(routable(service, 2));

  TestClient client(app->httpPort());
  for (int i = 0; i < 12; ++i) {
    const auto response = client.get("/proxy/" + service + "/x?i=" + std::to_string(i));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 200U) << "failover hides the failing backend";
  }
  const auto exposition = scrape();
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_GE(exposition.must("edgeflow_retries_total"), 3) << "the failing backend's answers were retried";
  EXPECT_EQ(exposition.must("edgeflow_failovers_total"), exposition.must("edgeflow_retries_total"))
      << "every retry went to the other backend";
  EXPECT_GE(exposition.must("edgeflow_circuit_breaker_trips_total"), 1) << "the failing backend's circuit opened";
  EXPECT_EQ(exposition.must("edgeflow_circuit_breakers", {{"state", "open"}}) +
                exposition.must("edgeflow_circuit_breakers", {{"state", "half_open"}}),
            1)
      << "and is still open (or being probed): only the failing backend has one that is not closed";
  EXPECT_GE(exposition.must("edgeflow_backend_attempts_total", {{"outcome", "retryable_status"}}), 3);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "5xx"}}), 0)
      << "the client never saw an error";
  EXPECT_EQ(exposition.must("edgeflow_http_error_ratio"), 0);
}

TEST_F(MetricsFullStackTest, ErrorsShowUpInTheErrorRatioAndTheCircuitRefusesAfterwards) {
  REQUIRE_REDIS_AND_BUILD(config);
  const auto service = newService("down");
  ScriptedBackend bad([](const auto& request, auto, auto) { return reply(request.target() == "/health" ? 200U : 503U); }, "bad");
  add(service, "bad", bad.port());
  start(config);
  ASSERT_TRUE(routable(service, 1));

  TestClient client(app->httpPort());
  for (int i = 0; i < 6; ++i) {
    ASSERT_EQ(client.get("/proxy/" + service + "/x?i=" + std::to_string(i))->result_int(), 503U);
  }
  ASSERT_EQ(client.get("/health")->result_int(), 200U);
  const auto exposition = scrape();
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "5xx"}}), 6);
  EXPECT_EQ(exposition.must("edgeflow_http_errors_total"), 6);
  EXPECT_NEAR(exposition.must("edgeflow_http_error_ratio"), 6.0 / 7.0, 1e-9);
  EXPECT_GE(exposition.must("edgeflow_circuit_breaker_trips_total"), 1);
  EXPECT_GE(exposition.must("edgeflow_retry_exhausted_total"), 1);
  EXPECT_GE(exposition.must("edgeflow_proxy_gateway_errors_total"), 3) << "once the circuit is open EdgeFlow answers itself";
}

TEST_F(MetricsFullStackTest, CacheHitRatioFollowsHitsAndMisses) {
  REQUIRE_REDIS_AND_BUILD(config);
  const auto service = newService("cached");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  start(config);
  ASSERT_TRUE(routable(service, 1));

  TestClient client(app->httpPort());
  for (int i = 0; i < 4; ++i) ASSERT_EQ(client.get("/proxy/" + service + "/item" + std::to_string(i))->result_int(), 200U);  // 4 misses
  ASSERT_TRUE(waitFor([&] { return app->policyHandler()->cache()->stats().stores >= 4; }));
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 4; ++i) ASSERT_EQ(client.get("/proxy/" + service + "/item" + std::to_string(i))->result_int(), 200U);  // 12 hits
  }
  const auto exposition = scrape();
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.must("edgeflow_cache_lookups_total", {{"result", "miss"}}), 4);
  EXPECT_EQ(exposition.must("edgeflow_cache_lookups_total", {{"result", "hit"}}), 12);
  EXPECT_NEAR(exposition.must("edgeflow_cache_hit_ratio"), 12.0 / 16.0, 1e-9);
  EXPECT_EQ(exposition.must("edgeflow_cache_stores_total"), 4);
  EXPECT_EQ(exposition.must("edgeflow_policy_served_from_cache_total"), 12);
  EXPECT_EQ(exposition.must("edgeflow_proxy_requests_total"), 4) << "hits never reach the proxy";
}

TEST_F(MetricsFullStackTest, RejectedRequestsAreCountedAndTheEndpointIsNeverRejected) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  auto config = appConfig(test_redis->host, test_redis->port, /*ip_capacity=*/10);
  const auto service = newService("limited");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  start(config);
  ASSERT_TRUE(routable(service, 1));

  TestClient client(app->httpPort());
  int ok = 0;
  int limited = 0;
  for (int i = 0; i < 25; ++i) {
    const auto response = client.get("/proxy/" + service + "/x?i=" + std::to_string(i));
    ASSERT_TRUE(response);
    (response->result_int() == 429U ? limited : ok) += 1;
  }
  EXPECT_EQ(ok, 10);
  EXPECT_EQ(limited, 15);
  const auto exposition = scrape();  // the bucket is empty: the scrape still works
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.must("edgeflow_rejected_requests_total", {{"reason", "rate_limited"}}), 15);
  EXPECT_EQ(exposition.must("edgeflow_rate_limit_decisions_total", {{"result", "limited"}}), 15);
  EXPECT_EQ(exposition.must("edgeflow_rate_limit_decisions_total", {{"result", "allowed"}}), 10);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "4xx"}}), 15);
  EXPECT_EQ(exposition.must("edgeflow_http_errors_total"), 0) << "a 429 is the limiter working, not an error";
  EXPECT_EQ(exposition.must("edgeflow_rejected_requests_total", {{"reason", "rate_limit_unavailable"}}), 0);
}

TEST_F(MetricsFullStackTest, RedisTroubleIsVisibleInTheMetrics) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  RedisRelay relay(test_redis->host, test_redis->port);
  auto config = appConfig("127.0.0.1", relay.port(), /*ip_capacity=*/100);
  const auto service = newService("redisy");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  start(config);
  ASSERT_TRUE(routable(service, 1));

  TestClient client(app->httpPort());
  ASSERT_EQ(client.get("/proxy/" + service + "/x")->result_int(), 200U);
  EXPECT_EQ(scrape().must("edgeflow_redis_up"), 1);

  relay.refuse();
  for (int i = 0; i < 5; ++i) ASSERT_EQ(client.get("/proxy/" + service + "/y" + std::to_string(i))->result_int(), 200U);
  const auto down = scrape();
  EXPECT_EQ(down.must("edgeflow_redis_up"), 0);
  EXPECT_GE(down.must("edgeflow_redis_failures_total"), 1);
  EXPECT_GE(down.must("edgeflow_redis_fast_failures_total"), 1);
  EXPECT_GE(down.must("edgeflow_cache_lookups_total", {{"result", "error"}}), 1);
  EXPECT_GE(down.must("edgeflow_rate_limit_decisions_total", {{"result", "degraded_allowed"}}), 1);
  EXPECT_NEAR(down.must("edgeflow_http_error_ratio"), 0.0, 1e-12) << "Redis down costs no client errors";

  relay.resume();
  ASSERT_TRUE(waitFor([&] {
    (void)client.get("/proxy/" + service + "/z");
    return scrape().must("edgeflow_redis_up") == 1;
  }, 10s));
}

TEST_F(MetricsFullStackTest, ConcurrentTrafficIsCountedExactly) {
  REQUIRE_REDIS_AND_BUILD(config);
  const auto service = newService("busy");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  start(config);
  ASSERT_TRUE(routable(service, 1));

  constexpr int kThreads = 8;
  constexpr int kPerThread = 60;
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      TestClient client(app->httpPort());
      for (int i = 0; i < kPerThread; ++i) {
        const auto response = client.get("/proxy/" + service + "/c" + std::to_string(t) + "-" + std::to_string(i % 10));
        if (!response || response->result_int() != 200) failed.fetch_add(1);
      }
    });
  }
  std::thread scraper([&] {
    for (int i = 0; i < 20; ++i) {
      const auto exposition = scrape();
      EXPECT_TRUE(exposition.valid()) << exposition.error();
      std::this_thread::sleep_for(10ms);
    }
  });
  for (auto& thread : threads) thread.join();
  scraper.join();
  EXPECT_EQ(failed.load(), 0);
  const auto exposition = scrape();
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "proxy"}}), kThreads * kPerThread);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), kThreads * kPerThread);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), kThreads * kPerThread);
  EXPECT_EQ(exposition.must("edgeflow_http_in_flight_requests"), 0);
  const double hits = exposition.must("edgeflow_cache_lookups_total", {{"result", "hit"}});
  const double misses = exposition.must("edgeflow_cache_lookups_total", {{"result", "miss"}});
  EXPECT_EQ(hits + misses, kThreads * kPerThread) << "every cacheable request was looked up exactly once";
  EXPECT_EQ(exposition.must("edgeflow_proxy_requests_total"), misses) << "and exactly the misses reached the proxy";
}

}  // namespace
