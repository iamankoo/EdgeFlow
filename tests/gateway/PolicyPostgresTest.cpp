#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/core/Application.hpp"
#include "support/PgTestSupport.hpp"
#include "support/ProxyTestSupport.hpp"
#include "support/RedisTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using Action = ScriptedBackend::Action;
namespace http = boost::beast::http;

std::string headerOf(const network::HttpResponse& response, const std::string& name) {
  const auto it = response.find(name);
  return it == response.end() ? std::string{"<absent>"} : std::string{it->value()};
}

// The whole of EdgeFlow's Application over a real PostgreSQL and a real Redis: server,
// registry API, health checker, router, reverse proxy, and the Phase 8 cache and rate limiter.
class PolicyPostgresTest : public RegistryFixture {
 protected:
  config::Config appConfig(const std::string& redis_host, std::uint16_t redis_port) {
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
    config.redis.enabled = true;
    config.redis.host = redis_host;
    config.redis.port = redis_port;
    config.redis.key_prefix = uniqueName("app");
    config.redis.command_timeout = 500ms;
    config.redis.retry_interval = 200ms;
    config.cache.enabled = true;
    config.cache.default_ttl = 30s;
    config.rate_limit.enabled = true;
    config.rate_limit.ip.enabled = true;
    config.rate_limit.ip.capacity = 6;
    config.rate_limit.ip.refill_per_second = 0.001;
    return config;
  }

  void start(const config::Config& config) {
    app = std::make_unique<core::Application>(
        config, appLog.logger(),
        core::ApplicationOptions{.install_signal_handlers = false, .request_handler = nullptr});
    ASSERT_TRUE(app->initialize()) << appLog.output();
  }

  void add(const std::string& service, const std::string& id, std::uint16_t port) {
    discovery::NewInstance instance = makeInstance(service, "127.0.0.1", port);
    instance.instance_id = id;
    ASSERT_TRUE(registry->registerInstance(instance).ok());
  }

  std::size_t routableCount(const std::string& service) {
    const auto routable = registry->lookupRoutable(service);
    return routable.ok() ? routable.value().size() : 0;
  }

  void TearDown() override {
    if (app) app->shutdown();
    app.reset();
    RegistryFixture::TearDown();
  }

  CapturedLogger appLog;
  std::unique_ptr<core::Application> app;
};

TEST_F(PolicyPostgresTest, CacheAndRateLimitWorkTogetherThroughTheWholeApplication) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  const auto service = newService("shop");
  std::atomic<int> versions{0};
  ScriptedBackend backend(
      [&](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
        Action action;
        if (request.target() == "/health") return action;  // the health checker's probes
        action.body = "v" + std::to_string(versions.fetch_add(1) + 1);
        return action;
      },
      "a");
  add(service, "a", backend.port());
  start(appConfig(test_redis->host, test_redis->port));
  ASSERT_NE(app->policyHandler(), nullptr);
  ASSERT_NE(app->redisClient(), nullptr);
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));

  TestClient client(app->httpPort());
  const auto miss = client.get("/proxy/" + service + "/item");
  ASSERT_TRUE(miss);
  EXPECT_EQ(miss->body(), "v1");
  EXPECT_EQ(headerOf(*miss, "X-Cache"), "MISS");
  ASSERT_TRUE(waitFor([&] { return app->policyHandler()->cache()->stats().stores >= 1; }));
  const auto hit = client.get("/proxy/" + service + "/item");
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->body(), "v1");
  EXPECT_EQ(headerOf(*hit, "X-Cache"), "HIT");

  // 2 of the 6 tokens are spent; the 4 left go to hits, then the limit answers.
  for (int i = 0; i < 4; ++i) {
    const auto response = client.get("/proxy/" + service + "/item");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 200U) << i;
  }
  const auto limited = client.get("/proxy/" + service + "/item");
  ASSERT_TRUE(limited);
  EXPECT_EQ(limited->result_int(), 429U);
  EXPECT_EQ(headerOf(*limited, "X-RateLimit-Scope"), "ip");
  EXPECT_EQ(client.get("/health")->result_int(), 200U) << "the gateway's own endpoints are not limited";

  std::size_t proxied = 0;
  for (const auto& r : backend.received()) proxied += std::string{r.request.target()} != "/health" ? 1U : 0U;
  EXPECT_EQ(proxied, 1U) << "one backend request served all seven client requests";
}

TEST_F(PolicyPostgresTest, StartsAndServesWhileRedisIsDownAndPicksItUpWhenItAppears) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  const auto service = newService("shop");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  RedisRelay relay(test_redis->host, test_redis->port);
  relay.refuse();  // Redis is down before EdgeFlow even starts
  start(appConfig("127.0.0.1", relay.port()));
  EXPECT_TRUE(appLog.contains("does not answer")) << "the start-up says so, and goes on";
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));

  TestClient client(app->httpPort());
  for (int i = 0; i < 10; ++i) {
    const auto response = client.get("/proxy/" + service + "/item");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 200U) << "no cache, no limit, but the gateway works (request " << i << ")";
  }

  relay.resume();
  ASSERT_TRUE(waitFor([&] {
    const auto response = client.get("/proxy/" + service + "/item");
    return response && headerOf(*response, "X-Cache") == "HIT";
  }, 10s)) << "caching starts by itself once Redis answers";
}

TEST_F(PolicyPostgresTest, ShutdownStopsRedisAfterTheProxyAndFinishesCleanly) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  const auto service = newService("shop");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  start(appConfig(test_redis->host, test_redis->port));
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));
  {
    TestClient client(app->httpPort());
    ASSERT_EQ(client.get("/proxy/" + service + "/item")->result_int(), 200U);
  }
  app->shutdown();
  const auto proxy_stop = appLog.position("stopping component 'reverse-proxy'");
  const auto redis_stop = appLog.position("stopping component 'redis'");
  ASSERT_NE(proxy_stop, std::string::npos) << appLog.output();
  ASSERT_NE(redis_stop, std::string::npos) << appLog.output();
  EXPECT_LT(appLog.position("stopping component 'http-server'"), proxy_stop);
  EXPECT_LT(proxy_stop, redis_stop) << "Redis outlives every request that may still use it";
  EXPECT_FALSE(appLog.contains("shutdown exceeded"));
}

TEST_F(PolicyPostgresTest, CacheOrRateLimitWithoutRedisAreIgnoredWithAWarning) {
  const auto service = newService("shop");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  auto config = appConfig("127.0.0.1", 1);
  config.redis.enabled = false;  // cache and rate limit are on, but there is nowhere to keep their state
  start(config);
  EXPECT_EQ(app->policyHandler(), nullptr);
  EXPECT_EQ(app->redisClient(), nullptr);
  EXPECT_TRUE(appLog.contains("redis.enabled is false"));
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));
  TestClient client(app->httpPort());
  EXPECT_EQ(client.get("/proxy/" + service + "/item")->result_int(), 200U) << "plain Phase 7 behaviour";
}

}  // namespace
