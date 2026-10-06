#include <gtest/gtest.h>

#include <future>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "edgeflow/core/Application.hpp"
#include "support/PgTestSupport.hpp"
#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using Action = ScriptedBackend::Action;
namespace http = boost::beast::http;

// Everything real except the backends' business logic: EdgeFlow's Application (HTTP server,
// registry API, health checker, router, reverse proxy) over a real PostgreSQL, with scripted
// HTTP backends registered as instances.
class ProxyPostgresTest : public RegistryFixture {
 protected:
  config::Config appConfig(config::RoutingStrategy strategy) {
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
    config.routing.strategy = strategy;
    config.proxy.enabled = true;
    config.proxy.upstream_timeout = 5000ms;
    return config;
  }

  void start(config::RoutingStrategy strategy = config::RoutingStrategy::RoundRobin) {
    app = std::make_unique<core::Application>(
        appConfig(strategy), appLog.logger(),
        core::ApplicationOptions{.install_signal_handlers = false, .request_handler = nullptr});
    ASSERT_TRUE(app->initialize()) << appLog.output();
    ASSERT_NE(app->proxyHandler(), nullptr);
  }

  void add(const std::string& service, const std::string& id, std::uint16_t port, std::uint32_t weight = 1) {
    discovery::NewInstance instance = makeInstance(service, "127.0.0.1", port);
    instance.instance_id = id;
    instance.weight = weight;
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

  // Requests a backend got for the proxied path (health probes also arrive at /health).
  static std::size_t proxied(ScriptedBackend& backend) {
    std::size_t n = 0;
    for (const auto& r : backend.received()) n += std::string{r.request.target()} != "/health" ? 1U : 0U;
    return n;
  }

  static std::string who(const network::HttpResponse& response) {
    return response.body().substr(0, response.body().find('|'));
  }

  void TearDown() override {
    if (app) app->shutdown();
    app.reset();
    RegistryFixture::TearDown();
  }

  CapturedLogger appLog;
  std::unique_ptr<core::Application> app;
};

TEST_F(ProxyPostgresTest, ClientToBackendThroughPostgresDiscoveryHealthAndRouting) {
  const auto service = newService("shop");
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend c(nullptr, "c");
  add(service, "a", a.port());
  add(service, "b", b.port());
  add(service, "c", c.port());
  add(service, "d", closedPort());  // registered, but nothing listens: the health checker excludes it
  start();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 3; })) << "d must stay out";

  TestClient client(app->httpPort());
  std::string order;
  std::set<std::string> ids;
  for (int i = 0; i < 12; ++i) {
    const auto response = client.request(http::verb::post, "/proxy/" + service + "/orders?id=" + std::to_string(i),
                                         "{\"n\":" + std::to_string(i) + "}",
                                         {{"Content-Type", "application/json"}, {"X-Request-Id", "e2e-" + std::to_string(i)}});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    order += who(*response);
    EXPECT_NE(response->body().find("|POST /orders?id=" + std::to_string(i) + "|{\"n\":" + std::to_string(i) + "}"),
              std::string::npos)
        << response->body();
    EXPECT_EQ(response->find("X-Request-Id")->value(), "e2e-" + std::to_string(i));
    EXPECT_EQ(response->find("Via")->value(), "1.1 edgeflow");
  }
  EXPECT_EQ(order, "abcabcabcabc") << "round robin over the three healthy instances";
  EXPECT_EQ(proxied(a) + proxied(b) + proxied(c), 12U);
  EXPECT_TRUE(waitFor([&] { return count(service, "a") + count(service, "b") + count(service, "c") == 0; }))
      << "no connection count leaked into PostgreSQL";

  // Real connection reuse: 4 requests per backend over one connection each.
  const auto pool_stats = app->proxyHandler()->stats().pool;
  EXPECT_GT(pool_stats.reused, 0U);

  // A backend that goes away is excluded by the real health checker, and the proxy follows.
  b.stop();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 2; }));
  std::set<std::string> after;
  for (int i = 0; i < 10; ++i) {
    const auto response = client.get("/proxy/" + service + "/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    after.insert(who(*response));
  }
  EXPECT_EQ(after, (std::set<std::string>{"a", "c"}));
}

TEST_F(ProxyPostgresTest, LeastConnectionsReadsTheCountTheProxyMaintainsInPostgres) {
  const auto service = newService("lc");
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  auto gate = std::make_shared<Gate>();
  a.setHandler([&](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    Action action;
    action.body = "a|held|";
    if (request.target() != "/health") action.gate = gate;
    return action;
  });
  add(service, "a", a.port());
  add(service, "b", b.port());
  start(config::RoutingStrategy::LeastConnections);
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 2; }));

  std::promise<std::optional<network::HttpResponse>> held;
  std::thread holder([&] { held.set_value(TestClient(app->httpPort()).get("/proxy/" + service + "/x")); });
  ASSERT_TRUE(waitFor([&] { return proxied(a) == 1; }));
  EXPECT_EQ(count(service, "a"), 1U) << "the proxy raised the counter in PostgreSQL for the request in flight";

  TestClient client(app->httpPort());
  for (int i = 0; i < 5; ++i) {
    const auto response = client.get("/proxy/" + service + "/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(who(*response), "b") << "least connections steers away from the busy instance";
  }
  gate->open();
  holder.join();
  ASSERT_TRUE(held.get_future().get());
  EXPECT_TRUE(waitFor([&] { return count(service, "a") == 0 && count(service, "b") == 0; }));
}

TEST_F(ProxyPostgresTest, BackendFailuresAndTimeoutsReleaseTheCountInPostgres) {
  const auto service = newService("fail");
  ScriptedBackend backend(nullptr, "a");
  add(service, "a", backend.port());
  auto config = appConfig(config::RoutingStrategy::RoundRobin);
  config.proxy.upstream_timeout = 400ms;
  app = std::make_unique<core::Application>(
      config, appLog.logger(), core::ApplicationOptions{.install_signal_handlers = false, .request_handler = nullptr});
  ASSERT_TRUE(app->initialize());
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));

  backend.setHandler([](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    Action action;
    action.body = "ok";
    if (request.target() == "/health") return action;
    if (request.target() == "/stall") action.kind = ScriptedBackend::Action::Kind::Stall;
    if (request.target() == "/reset") action.kind = ScriptedBackend::Action::Kind::Reset;
    if (request.target() == "/garbage") action.kind = ScriptedBackend::Action::Kind::Garbage;
    if (request.target() == "/error") action.code = 500;
    return action;
  });
  TestClient client(app->httpPort());
  EXPECT_EQ(client.get("/proxy/" + service + "/stall")->result(), http::status::gateway_timeout);
  EXPECT_EQ(client.get("/proxy/" + service + "/reset")->result(), http::status::bad_gateway);
  EXPECT_EQ(client.get("/proxy/" + service + "/garbage")->result(), http::status::bad_gateway);
  EXPECT_EQ(client.get("/proxy/" + service + "/error")->result(), http::status::internal_server_error);
  EXPECT_EQ(client.get("/proxy/" + service + "/fine")->result(), http::status::ok);
  EXPECT_TRUE(waitFor([&] { return count(service, "a") == 0; }));
}

TEST_F(ProxyPostgresTest, UnknownServiceAndOutageAnswerCleanly) {
  const auto service = newService("none");
  start();
  TestClient client(app->httpPort());
  EXPECT_EQ(client.get("/proxy/" + service + "/x")->result(), http::status::not_found);
  ScriptedBackend backend;
  add(service, "a", backend.port());
  // Disabled: registered but nothing routable -> 503.
  discovery::InstanceUpdate disable;
  disable.status = discovery::InstanceStatus::Disabled;
  ASSERT_TRUE(registry->updateInstance(service, "a", disable).ok());
  ASSERT_TRUE(waitFor([&] { return client.get("/proxy/" + service + "/x")->result() == http::status::service_unavailable; }));
  EXPECT_EQ(proxied(backend), 0U);
}

TEST_F(ProxyPostgresTest, ShutdownLetsARequestFinishWithinTheGracePeriod) {
  const auto service = newService("grace");
  ScriptedBackend backend(nullptr, "a");
  backend.setHandler([](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    Action action;
    action.body = "finished";
    if (request.target() != "/health") action.delay = 400ms;
    return action;
  });
  add(service, "a", backend.port());
  start();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));

  std::promise<std::optional<network::HttpResponse>> result;
  std::thread requester([&] { result.set_value(TestClient(app->httpPort()).get("/proxy/" + service + "/slow")); });
  ASSERT_TRUE(waitFor([&] { return proxied(backend) == 1; }));
  app->shutdown();
  requester.join();
  const auto response = result.get_future().get();
  ASSERT_TRUE(response) << "the in-flight request was completed, not dropped";
  EXPECT_EQ(response->body(), "finished");
  EXPECT_EQ(count(service, "a"), 0U);
  const auto http_stopped = appLog.position("HTTP server stopped");
  const auto proxy_stopped = appLog.position("reverse proxy stopped");
  const auto completed = appLog.position("shutdown completed");
  ASSERT_NE(http_stopped, std::string::npos);
  ASSERT_NE(proxy_stopped, std::string::npos);
  EXPECT_LT(http_stopped, proxy_stopped) << "the HTTP server stops before the proxy";
  EXPECT_LT(proxy_stopped, completed);
}

TEST_F(ProxyPostgresTest, ShutdownCancelsARequestThatOutlivesTheGracePeriodAndReleasesItsCount) {
  const auto service = newService("cancel");
  ScriptedBackend backend(nullptr, "a");
  backend.setHandler([](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    Action action;
    if (request.target() != "/health") action.kind = ScriptedBackend::Action::Kind::Stall;
    return action;
  });
  add(service, "a", backend.port());
  start();
  ASSERT_TRUE(waitFor([&] { return routableCount(service) == 1; }));

  std::promise<std::optional<network::HttpResponse>> result;
  std::thread requester([&] { result.set_value(TestClient(app->httpPort()).get("/proxy/" + service + "/stuck")); });
  ASSERT_TRUE(waitFor([&] { return proxied(backend) == 1; }));
  ASSERT_EQ(count(service, "a"), 1U);

  const auto begin = std::chrono::steady_clock::now();
  app->shutdown();  // grace period 1 s
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  requester.join();
  EXPECT_FALSE(result.get_future().get().has_value()) << "the client is released, not left hanging";
  EXPECT_LT(elapsed, 6s);
  EXPECT_EQ(count(service, "a"), 0U) << "and the count was released in PostgreSQL before the registry went away";
}

}  // namespace
