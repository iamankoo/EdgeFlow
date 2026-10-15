#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/config/ConfigManager.hpp"
#include "edgeflow/core/Application.hpp"
#include "support/MetricsTestSupport.hpp"
#include "support/NetTestSupport.hpp"
#include "support/TestLogger.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
namespace http = boost::beast::http;

config::Config baseConfig() {
  config::Config config;
  config.server.host = "127.0.0.1";
  config.server.port = 0;
  config.shutdown.grace_period = 1s;
  config.metrics.enabled = true;
  config.metrics.window_seconds = 5;
  return config;
}

const core::ApplicationOptions kNoSignals{.install_signal_handlers = false, .request_handler = nullptr};

Exposition scrape(TestClient& client, const std::string& path = "/metrics") {
  const auto response = client.get(path);
  EXPECT_TRUE(response);
  return Exposition(response ? response->body() : std::string{});
}

// ----------------------------------------------------------------- configuration

TEST(MetricsConfigTest, DefaultsAreOffWithAConventionalPath) {
  config::ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("server:\n  port: 8080\n"));
  EXPECT_FALSE(manager.config().metrics.enabled);
  EXPECT_EQ(manager.config().metrics.path, "/metrics");
  EXPECT_EQ(manager.config().metrics.window_seconds, 10U);
}

TEST(MetricsConfigTest, ParsesEveryKeyAndRejectsBadValues) {
  {
    config::ConfigManager manager;
    ASSERT_TRUE(manager.loadFromString("metrics:\n  enabled: true\n  path: /internal/metrics\n  window_seconds: 60\n"));
    EXPECT_TRUE(manager.config().metrics.enabled);
    EXPECT_EQ(manager.config().metrics.path, "/internal/metrics");
    EXPECT_EQ(manager.config().metrics.window_seconds, 60U);
  }
  for (const std::string bad : {"metrics:\n  path: metrics\n", "metrics:\n  path: /\n", "metrics:\n  path: /proxy\n",
                                "metrics:\n  path: /proxy/x\n", "metrics:\n  path: /services\n",
                                "metrics:\n  path: /health\n", "metrics:\n  path: \"/a b\"\n",
                                "metrics:\n  window_seconds: 0\n", "metrics:\n  window_seconds: 301\n",
                                "metrics:\n  enabled: maybe\n", "metrics:\n  port: 9100\n"}) {
    config::ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(bad)) << bad;
    EXPECT_FALSE(manager.errors().empty());
  }
}

TEST(MetricsConfigTest, TheShippedConfigurationsAreValidAndMetricsIsOnlyOnWhereDocumented) {
  config::ConfigManager defaults;
  ASSERT_TRUE(defaults.load(EDGEFLOW_DEFAULT_CONFIG)) << (defaults.errors().empty() ? "" : defaults.errors().front());
  EXPECT_FALSE(defaults.config().metrics.enabled);

  config::ConfigManager compose;
  ASSERT_TRUE(compose.load(EDGEFLOW_COMPOSE_CONFIG)) << (compose.errors().empty() ? "" : compose.errors().front());
  EXPECT_TRUE(compose.config().metrics.enabled);

  // The configuration the load tests run (bench/load): everything a production-like gateway does.
  config::ConfigManager bench;
  ASSERT_TRUE(bench.load(EDGEFLOW_BENCH_CONFIG)) << (bench.errors().empty() ? "" : bench.errors().front());
  const auto& c = bench.config();
  EXPECT_TRUE(c.database.enabled && c.health_check.enabled && c.proxy.enabled && c.reliability.enabled);
  EXPECT_TRUE(c.redis.enabled && c.cache.enabled && c.rate_limit.enabled && c.metrics.enabled);
  EXPECT_EQ(c.application.log_level, config::LogLevel::Warn) << "the benchmark must not measure the terminal";
}

// ----------------------------------------------------------------- the endpoint on a real server

TEST(MetricsApplicationTest, TheEndpointIsOffUnlessEnabled) {
  CapturedLogger log;
  auto config = baseConfig();
  config.metrics.enabled = false;
  core::Application app(config, log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  EXPECT_EQ(app.metrics(), nullptr);
  TestClient client(app.httpPort());
  EXPECT_EQ(client.get("/metrics")->result(), http::status::not_found);
  app.shutdown();
}

TEST(MetricsApplicationTest, ServesAValidDocumentAndCountsTheRequestsMadeToTheServer) {
  CapturedLogger log;
  core::Application app(baseConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize()) << log.output();
  ASSERT_NE(app.metrics(), nullptr);
  EXPECT_TRUE(log.contains("metrics ready"));

  TestClient client(app.httpPort());
  for (int i = 0; i < 7; ++i) ASSERT_EQ(client.get("/health")->result(), http::status::ok);
  ASSERT_EQ(client.get("/nowhere")->result(), http::status::not_found);
  ASSERT_EQ(client.request(http::verb::post, "/health")->result(), http::status::method_not_allowed);

  const auto exposition = scrape(client);
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "local"}}), 9);
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "proxy"}}), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), 7);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "4xx"}}), 2);
  EXPECT_EQ(exposition.must("edgeflow_http_errors_total"), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), 9) << "the scrape itself is not counted";
  EXPECT_GE(exposition.must("edgeflow_http_active_connections"), 1);
  EXPECT_GE(exposition.must("edgeflow_http_connections_accepted_total"), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_connections_rejected_total"), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_window_seconds"), 5);

  // Components that are not enabled have no families at all (no misleading zeros).
  EXPECT_FALSE(exposition.has("edgeflow_proxy_requests_total"));
  EXPECT_FALSE(exposition.has("edgeflow_cache_lookups_total"));
  EXPECT_FALSE(exposition.has("edgeflow_redis_up"));
  EXPECT_FALSE(exposition.has("edgeflow_retries_total"));
  app.shutdown();
}

TEST(MetricsApplicationTest, ARateOverTheCompletedSecondsAndLatencyQuantilesAppear) {
  CapturedLogger log;
  core::Application app(baseConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  TestClient client(app.httpPort());
  const auto start = std::chrono::steady_clock::now();
  int sent = 0;
  while (std::chrono::steady_clock::now() - start < 2300ms) {
    ASSERT_EQ(client.get("/health")->result(), http::status::ok);
    ++sent;
  }
  const auto exposition = scrape(client);
  EXPECT_GT(exposition.must("edgeflow_http_requests_per_second"), 10.0);
  EXPECT_LE(exposition.must("edgeflow_http_requests_per_second"), sent / 2.0 + 1) << "at most what could have been sent per second";
  const double p50 = exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.5"}});
  const double p99 = exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.99"}});
  EXPECT_GT(p50, 0.0);
  EXPECT_LE(p50, p99);
  EXPECT_LT(p99, 0.5) << "a local endpoint answers in well under half a second";
  EXPECT_EQ(exposition.must("edgeflow_http_error_ratio"), 0);
  app.shutdown();
}

TEST(MetricsApplicationTest, OpenAndRejectedConnectionsAreReported) {
  CapturedLogger log;
  auto config = baseConfig();
  config.server.max_connections = 3;
  core::Application app(config, log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < 3; ++i) {
    clients.push_back(std::make_unique<TestClient>(app.httpPort()));
    ASSERT_EQ(clients.back()->get("/health")->result(), http::status::ok);
  }
  const auto exposition = scrape(*clients.front());
  EXPECT_EQ(exposition.must("edgeflow_http_active_connections"), 3);

  // The fourth connection is over the limit: it is closed at once, and counted.
  TestClient refused(app.httpPort());
  EXPECT_TRUE(refused.closedByPeer());
  EXPECT_TRUE(waitFor([&] { return scrape(*clients.front()).must("edgeflow_http_connections_rejected_total") >= 1; }));
  clients.pop_back();  // frees a slot
  clients.pop_back();
  ASSERT_TRUE(waitFor([&] { return scrape(*clients.front()).must("edgeflow_http_active_connections") == 1; }));
  app.shutdown();
}

TEST(MetricsApplicationTest, ACustomPathAndPostIsRefused) {
  CapturedLogger log;
  auto config = baseConfig();
  config.metrics.path = "/internal/stats";
  core::Application app(config, log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  TestClient client(app.httpPort());
  EXPECT_EQ(client.get("/metrics")->result(), http::status::not_found);
  EXPECT_EQ(client.get("/internal/stats")->result(), http::status::ok);
  EXPECT_EQ(client.request(http::verb::post, "/internal/stats")->result(), http::status::method_not_allowed);
  app.shutdown();
}

TEST(MetricsApplicationTest, ScrapingWhileServingConcurrentClientsIsSafe) {
  CapturedLogger log;
  core::Application app(baseConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&] {
      TestClient client(app.httpPort());
      for (int i = 0; i < 300; ++i) {
        if (!client.get("/health")) bad.fetch_add(1);
      }
    });
  }
  std::thread scraper([&] {
    TestClient client(app.httpPort());
    while (!stop.load()) {
      const auto exposition = scrape(client);
      if (!exposition.valid()) bad.fetch_add(1);
    }
  });
  for (auto& thread : threads) thread.join();
  stop.store(true);
  scraper.join();
  EXPECT_EQ(bad.load(), 0);
  TestClient client(app.httpPort());
  EXPECT_EQ(scrape(client).must("edgeflow_http_request_duration_seconds_count"), 1800);
  app.shutdown();
}

}  // namespace
