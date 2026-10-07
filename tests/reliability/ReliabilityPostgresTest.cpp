#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/core/Application.hpp"
#include "support/PgTestSupport.hpp"
#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using Action = ScriptedBackend::Action;
using edgeflow::reliability::CircuitState;
namespace http = boost::beast::http;

// Phase 7 over the real stack: EdgeFlow's Application (HTTP server, registry API, health
// checker, router, reverse proxy, reliability layer) over a real PostgreSQL, with scripted
// HTTP backends registered as instances.
class ReliabilityPostgresTest : public RegistryFixture {
 protected:
  config::Config appConfig() {
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
    config.health_check.interval = 200ms;
    config.health_check.timeout = 300ms;
    config.health_check.refresh_interval = 100ms;
    config.health_check.failure_threshold = 2;
    config.health_check.success_threshold = 1;
    config.routing.strategy = config::RoutingStrategy::RoundRobin;
    config.proxy.enabled = true;
    config.proxy.connect_timeout = 500ms;
    config.proxy.upstream_timeout = upstream_timeout;
    config.reliability = testReliabilityConfig();
    config.reliability.circuit_breaker.recovery_timeout = 400ms;
    return config;
  }

  void start() {
    app = std::make_unique<core::Application>(
        appConfig(), appLog.logger(),
        core::ApplicationOptions{.install_signal_handlers = false, .request_handler = nullptr});
    ASSERT_TRUE(app->initialize()) << appLog.output();
    ASSERT_NE(app->proxyHandler(), nullptr);
    ASSERT_NE(app->proxyHandler()->reliability(), nullptr);
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
  std::uint64_t count(const std::string& service, const std::string& id) {
    const auto instance = registry->getInstance(service, id);
    return instance.ok() ? instance.value().connection_count : 9999;
  }
  static std::string who(const network::HttpResponse& response) {
    return response.body().substr(0, response.body().find('|'));
  }
  // Answers /health normally and everything else with `code`.
  static void failProxiedTraffic(ScriptedBackend& backend, unsigned code) {
    backend.setHandler([&backend, code](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
      Action action = backend.describe(request);
      if (std::string{request.target()} != "/health") {
        action.code = code;
        action.body = "sick";
      }
      return action;
    });
  }

  void TearDown() override {
    if (app) app->shutdown();
    app.reset();
    RegistryFixture::TearDown();
  }

  std::chrono::milliseconds upstream_timeout{3000};
  CapturedLogger appLog;
  std::unique_ptr<core::Application> app;
};

TEST_F(ReliabilityPostgresTest, ABackendThatDiesIsInvisibleToClientsEvenBeforeHealthCheckingNoticesIt) {
  const auto service = newService("shop");
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend c(nullptr, "c");
  add(service, "a", a.port());
  add(service, "b", b.port());
  add(service, "c", c.port());
  start();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 3; }));

  // b dies. The registry still lists it as healthy for a while: this is the window in which,
  // without the reliability layer, clients would see failures.
  b.stop();
  TestClient client(app->httpPort());
  int ok = 0;
  int failed = 0;
  for (int i = 0; i < 30; ++i) {
    const auto response = client.get("/proxy/" + service + "/x");
    if (response && response->result() == http::status::ok) {
      ++ok;
      EXPECT_NE(who(*response), "b");
    } else {
      ++failed;
    }
  }
  EXPECT_EQ(failed, 0) << "backend failure did not cascade into gateway failures";
  EXPECT_EQ(ok, 30);
  EXPECT_GT(app->proxyHandler()->reliability()->stats().retries, 0U) << "failover did the work";

  // Later the health checker catches up and removes b from the routable set; nothing changes
  // for the clients.
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 2; }));
  for (int i = 0; i < 10; ++i) {
    const auto response = client.get("/proxy/" + service + "/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
  }
  EXPECT_TRUE(waitFor([&] { return count(service, "a") + count(service, "b") + count(service, "c") == 0; }))
      << "no connection count leaked into PostgreSQL";
}

TEST_F(ReliabilityPostgresTest, TheCircuitProtectsABackendThatPassesHealthChecksButFailsRealRequests) {
  const auto service = newService("shop");
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  failProxiedTraffic(a, 503);
  add(service, "a", a.port());
  add(service, "b", b.port());
  start();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 2; }));

  TestClient client(app->httpPort());
  for (int i = 0; i < 20; ++i) {
    const auto response = client.get("/proxy/" + service + "/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    EXPECT_EQ(who(*response), "b");
  }
  const auto key = reliability::backendKey(service, "a", "127.0.0.1", a.port());
  const auto breaker = app->proxyHandler()->reliability()->breakers()->find(key);
  ASSERT_NE(breaker, nullptr);
  EXPECT_NE(breaker->state(), CircuitState::Closed);
  // The two systems answer different questions: health checking still calls `a` healthy ...
  EXPECT_EQ(routableCount(service), 2U);
  const auto health = registry->getInstance(service, "a");
  ASSERT_TRUE(health.ok());
  EXPECT_EQ(health.value().health, discovery::HealthStatus::Healthy);
  // ... while the circuit keeps real traffic away from it.
  std::size_t proxied_to_a = 0;
  for (const auto& r : a.received()) proxied_to_a += std::string{r.request.target()} != "/health" ? 1U : 0U;
  EXPECT_LE(proxied_to_a, 5U) << "only the requests that opened the circuit (and recovery probes) reached a";

  // The backend recovers: after the recovery timeout a probe closes the circuit again.
  a.setHandler([&a](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    return a.describe(request);
  });
  std::set<std::string> answered;
  ASSERT_TRUE(waitFor([&] {
    const auto response = client.get("/proxy/" + service + "/x");
    if (response && response->result() == http::status::ok) answered.insert(who(*response));
    return answered.count("a") == 1;
  }));
  EXPECT_EQ(breaker->state(), CircuitState::Closed);
}

TEST_F(ReliabilityPostgresTest, ASlowBackendTimesOutAndTheRequestIsServedElsewhere) {
  upstream_timeout = 300ms;
  const auto service = newService("shop");
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  add(service, "a", a.port());
  add(service, "b", b.port());
  start();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 2; }));

  // From now on `a` accepts requests and never answers them (health probes excepted).
  a.setHandler([&a](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    Action action = a.describe(request);
    if (std::string{request.target()} != "/health") action.kind = ScriptedBackend::Action::Kind::Stall;
    return action;
  });
  TestClient client(app->httpPort());
  const auto start_time = std::chrono::steady_clock::now();
  int ok = 0;
  for (int i = 0; i < 6; ++i) {
    const auto response = client.get("/proxy/" + service + "/x");
    if (response && response->result() == http::status::ok) {
      ++ok;
      EXPECT_EQ(who(*response), "b");
    }
  }
  EXPECT_EQ(ok, 6) << "a timed out (300 ms), the request was served by b";
  EXPECT_LT(std::chrono::steady_clock::now() - start_time, 8s);
  EXPECT_TRUE(waitFor([&] { return count(service, "a") + count(service, "b") == 0; }));
}

}  // namespace
