#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "edgeflow/config/ConfigManager.hpp"
#include "edgeflow/network/RegistryRequestHandler.hpp"
#include "edgeflow/routing/Router.hpp"
#include "support/FakeRegistry.hpp"
#include "support/NetTestSupport.hpp"

namespace {

using namespace edgeflow;
using edgeflow::config::RoutingStrategy;
using edgeflow::discovery::HealthStatus;
using edgeflow::discovery::InstanceStatus;
using edgeflow::discovery::NewInstance;
using edgeflow::discovery::RegistryErrorCode;
using edgeflow::testing::FakeRegistry;
using nlohmann::json;
namespace http = edgeflow::testing::http;

const RoutingStrategy kAllStrategies[] = {RoutingStrategy::RoundRobin, RoutingStrategy::LeastConnections,
                                          RoutingStrategy::Weighted, RoutingStrategy::ConsistentHashing};

NewInstance backend(const std::string& id, std::uint16_t port, std::uint32_t weight = 1,
                    std::uint64_t connections = 0, HealthStatus health = HealthStatus::Healthy,
                    InstanceStatus status = InstanceStatus::Active) {
  NewInstance n;
  n.service = "shop";
  n.instance_id = id;
  n.host = "10.0.0.1";
  n.port = port;
  n.weight = weight;
  n.connection_count = connections;
  n.health = health;
  n.status = status;
  return n;
}

// The scenario from the Phase 5 acceptance criteria: A, B, C healthy and D unhealthy.
class RouterTest : public ::testing::TestWithParam<RoutingStrategy> {
 protected:
  RouterTest() : registry(std::make_shared<FakeRegistry>()) {
    EXPECT_TRUE(registry->registerInstance(backend("a", 1, 5, 5)).ok());
    EXPECT_TRUE(registry->registerInstance(backend("b", 2, 3, 2)).ok());
    EXPECT_TRUE(registry->registerInstance(backend("c", 3, 2, 7)).ok());
    // D has the lowest connection count and a big weight, so a strategy that looked at
    // anything but the routable set would love it.
    EXPECT_TRUE(registry->registerInstance(backend("d", 4, 1000, 0, HealthStatus::Unhealthy)).ok());
  }
  std::shared_ptr<routing::Router> router() {
    return std::make_shared<routing::Router>(registry, routing::makeLoadBalancer(GetParam()));
  }
  std::shared_ptr<FakeRegistry> registry;
};

TEST_P(RouterTest, AnUnhealthyInstanceIsNeverSelected) {
  auto r = router();
  std::set<std::string> seen;
  for (int i = 0; i < 600; ++i) {
    const auto chosen = r->route("shop", routing::RoutingContext{"key-" + std::to_string(i)});
    ASSERT_TRUE(chosen.ok()) << chosen.error().message;
    seen.insert(chosen.value().instance_id);
  }
  EXPECT_EQ(seen.count("d"), 0U);
  EXPECT_GE(seen.size(), 1U);
}

TEST_P(RouterTest, DrainingAndDisabledInstancesAreNeverSelectedEvenWhenHealthy) {
  ASSERT_TRUE(registry->registerInstance(backend("e", 5, 1000, 0, HealthStatus::Healthy, InstanceStatus::Draining)).ok());
  ASSERT_TRUE(registry->registerInstance(backend("f", 6, 1000, 0, HealthStatus::Healthy, InstanceStatus::Disabled)).ok());
  auto r = router();
  for (int i = 0; i < 300; ++i) {
    const auto id = r->route("shop", routing::RoutingContext{"k" + std::to_string(i)}).value().instance_id;
    EXPECT_TRUE(id == "a" || id == "b" || id == "c") << id;
  }
}

TEST_P(RouterTest, AnInstanceThatBecomesUnhealthyStopsReceivingRequestsAndRecoveredOnesReturn) {
  auto r = router();
  const auto sample = [&] {
    std::set<std::string> seen;
    for (int i = 0; i < 400; ++i) seen.insert(r->route("shop", routing::RoutingContext{"k" + std::to_string(i)}).value().instance_id);
    return seen;
  };
  const auto stored = registry->getInstance("shop", "b").value();
  ASSERT_TRUE(registry->updateHealth("shop", "b", stored.registered_at, HealthStatus::Unhealthy).ok());
  EXPECT_EQ(sample().count("b"), 0U) << "excluded as soon as discovery stops listing it";

  ASSERT_TRUE(registry->updateHealth("shop", "b", stored.registered_at, HealthStatus::Healthy).ok());
  if (GetParam() != RoutingStrategy::LeastConnections) {  // least connections would still prefer b (2)
    EXPECT_EQ(sample().count("b"), 1U) << "reintroduced";
  }
  EXPECT_EQ(r->route("shop").value().service, "shop");
}

TEST_P(RouterTest, ServiceWithNothingRoutableAndUnknownServiceAreDistinctErrors) {
  for (const char* id : {"a", "b", "c"}) {
    const auto stored = registry->getInstance("shop", id).value();
    ASSERT_TRUE(registry->updateHealth("shop", id, stored.registered_at, HealthStatus::Unhealthy).ok());
  }
  auto r = router();
  const auto none = r->route("shop");
  ASSERT_FALSE(none.ok());
  EXPECT_EQ(none.error().code, RegistryErrorCode::NoRoutableInstance);

  const auto unknown = r->route("nope");
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, RegistryErrorCode::ServiceNotFound);

  const auto invalid = r->route("Not A Name");
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, RegistryErrorCode::InvalidArgument);
}

TEST_P(RouterTest, NamesTheConfiguredStrategy) {
  EXPECT_EQ(router()->strategy(), routing::makeLoadBalancer(GetParam())->name());
}

INSTANTIATE_TEST_SUITE_P(AllStrategies, RouterTest, ::testing::ValuesIn(kAllStrategies));

// ---- each strategy's characteristic behaviour, through the router ------------------------------

class RouterBehaviourTest : public ::testing::Test {
 protected:
  RouterBehaviourTest() : registry(std::make_shared<FakeRegistry>()) {
    EXPECT_TRUE(registry->registerInstance(backend("a", 1, 5, 5)).ok());
    EXPECT_TRUE(registry->registerInstance(backend("b", 2, 3, 2)).ok());
    EXPECT_TRUE(registry->registerInstance(backend("c", 3, 2, 7)).ok());
    EXPECT_TRUE(registry->registerInstance(backend("d", 4, 1, 0, HealthStatus::Unhealthy)).ok());
  }
  routing::Router make(RoutingStrategy strategy) { return routing::Router(registry, routing::makeLoadBalancer(strategy)); }
  std::shared_ptr<FakeRegistry> registry;
};

TEST_F(RouterBehaviourTest, RoundRobinCyclesThroughTheHealthyInstances) {
  auto router = make(RoutingStrategy::RoundRobin);
  std::string sequence;
  for (int i = 0; i < 6; ++i) sequence += router.route("shop").value().instance_id;
  EXPECT_EQ(sequence, "abcabc");
}

TEST_F(RouterBehaviourTest, LeastConnectionsPrefersTheLeastLoadedHealthyInstance) {
  auto router = make(RoutingStrategy::LeastConnections);
  EXPECT_EQ(router.route("shop").value().instance_id, "b") << "a=5, b=2, c=7 (d=0 is unhealthy)";
  ASSERT_TRUE(registry->adjustConnectionCount("shop", "b", 10).ok());
  EXPECT_EQ(router.route("shop").value().instance_id, "a") << "b is now at 12";
}

TEST_F(RouterBehaviourTest, WeightedFollowsTheStoredWeights) {
  auto router = make(RoutingStrategy::Weighted);
  std::map<std::string, int> counts;
  for (int i = 0; i < 1000; ++i) ++counts[router.route("shop").value().instance_id];
  EXPECT_EQ(counts["a"], 500);
  EXPECT_EQ(counts["b"], 300);
  EXPECT_EQ(counts["c"], 200);
  EXPECT_EQ(counts.count("d"), 0U);
}

TEST_F(RouterBehaviourTest, ConsistentHashingIsStickyPerKey) {
  auto router = make(RoutingStrategy::ConsistentHashing);
  for (int i = 0; i < 100; ++i) {
    const std::string key = "client-" + std::to_string(i);
    const auto first = router.route("shop", routing::RoutingContext{key}).value().instance_id;
    for (int again = 0; again < 3; ++again) {
      EXPECT_EQ(router.route("shop", routing::RoutingContext{key}).value().instance_id, first);
    }
  }
}

// ---- configuration and factory -----------------------------------------------------------------------

TEST(RoutingFactoryTest, EachStrategyCreatesItsImplementation) {
  EXPECT_EQ(routing::makeLoadBalancer(RoutingStrategy::RoundRobin)->name(), "round_robin");
  EXPECT_EQ(routing::makeLoadBalancer(RoutingStrategy::LeastConnections)->name(), "least_connections");
  EXPECT_EQ(routing::makeLoadBalancer(RoutingStrategy::Weighted)->name(), "weighted");
  EXPECT_EQ(routing::makeLoadBalancer(RoutingStrategy::ConsistentHashing)->name(), "consistent_hashing");
}

TEST(RoutingConfigTest, StrategyNamesRoundTripThroughTheConfiguration) {
  for (const auto strategy : kAllStrategies) {
    config::ConfigManager manager;
    const std::string yaml = "routing:\n  strategy: " + std::string{config::toString(strategy)} + "\n";
    ASSERT_TRUE(manager.loadFromString(yaml)) << yaml;
    EXPECT_EQ(manager.config().routing.strategy, strategy);
    EXPECT_EQ(routing::makeLoadBalancer(manager.config().routing.strategy)->name(), config::toString(strategy));
  }
}

TEST(RoutingConfigTest, DefaultIsRoundRobin) {
  config::ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("server:\n  port: 8080\n"));
  EXPECT_EQ(manager.config().routing.strategy, RoutingStrategy::RoundRobin);
}

TEST(RoutingConfigTest, InvalidValuesAndUnknownKeysAreRejected) {
  for (const char* bad : {"strategy: random", "strategy: Round_Robin", "strategy: ''", "strategy: 3",
                          "strategy: round-robin", "strategy: weighted_round_robin"}) {
    config::ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{"routing:\n  "} + bad + "\n")) << bad;
    ASSERT_FALSE(manager.errors().empty());
    EXPECT_NE(manager.errors().front().find("routing.strategy"), std::string::npos) << bad;
  }
  config::ConfigManager unknown;
  EXPECT_FALSE(unknown.loadFromString("routing:\n  stratagy: weighted\n"));
  EXPECT_NE(unknown.errors().front().find("unknown key 'routing.stratagy'"), std::string::npos);
  config::ConfigManager not_a_map;
  EXPECT_FALSE(not_a_map.loadFromString("routing: weighted\n"));
}

// ---- the HTTP routing decision endpoint ----------------------------------------------------------------

class RouteEndpointTest : public ::testing::Test {
 protected:
  void start(RoutingStrategy strategy, bool with_router = true) {
    registry = std::make_shared<FakeRegistry>();
    ASSERT_TRUE(registry->registerInstance(backend("a", 1)).ok());
    ASSERT_TRUE(registry->registerInstance(backend("b", 2)).ok());
    ASSERT_TRUE(registry->registerInstance(backend("sick", 3, 1, 0, HealthStatus::Unhealthy)).ok());
    std::shared_ptr<routing::Router> router;
    if (with_router) router = std::make_shared<routing::Router>(registry, routing::makeLoadBalancer(strategy));
    handler = std::make_shared<network::RegistryRequestHandler>(
        registry, std::make_shared<network::LocalRequestHandler>(), log.logger(), router);
    harness = std::make_unique<edgeflow::testing::ServerHarness>(edgeflow::testing::testServerConfig(), handler);
    ASSERT_TRUE(harness->started);
    client = std::make_unique<edgeflow::testing::TestClient>(harness->port());
  }
  json body(const std::optional<network::HttpResponse>& response) {
    return json::parse(response->body(), nullptr, false);
  }

  edgeflow::testing::CapturedLogger log;
  std::shared_ptr<FakeRegistry> registry;
  std::shared_ptr<network::RegistryRequestHandler> handler;
  std::unique_ptr<edgeflow::testing::ServerHarness> harness;
  std::unique_ptr<edgeflow::testing::TestClient> client;
};

TEST_F(RouteEndpointTest, ReturnsTheSelectedInstanceAndNamesTheStrategy) {
  start(RoutingStrategy::RoundRobin);
  std::string sequence;
  for (int i = 0; i < 4; ++i) {
    const auto response = client->get("/services/shop/route");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->result(), http::status::ok) << response->body();
    const auto b = body(response);
    EXPECT_EQ(b["strategy"], "round_robin");
    EXPECT_EQ(b["service"], "shop");
    sequence += b["selected"]["instance_id"].get<std::string>();
  }
  EXPECT_EQ(sequence, "abab") << "the unhealthy instance is not part of the cycle";
}

TEST_F(RouteEndpointTest, TheKeyParameterDrivesConsistentHashing) {
  start(RoutingStrategy::ConsistentHashing);
  const auto first = body(client->get("/services/shop/route?key=client-42"));
  EXPECT_EQ(first["strategy"], "consistent_hashing");
  EXPECT_EQ(first["key"], "client-42");
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(body(client->get("/services/shop/route?key=client-42"))["selected"]["instance_id"],
              first["selected"]["instance_id"]);
  }
  // percent-encoding is decoded: "a b" and "a%20b" are the same key
  EXPECT_EQ(body(client->get("/services/shop/route?key=a%20b"))["key"], "a b");
  std::set<std::string> targets;
  for (int i = 0; i < 100; ++i) {
    targets.insert(body(client->get("/services/shop/route?key=k" + std::to_string(i)))["selected"]["instance_id"].get<std::string>());
  }
  EXPECT_EQ(targets, (std::set<std::string>{"a", "b"})) << "many keys reach every healthy instance, never the sick one";
}

TEST_F(RouteEndpointTest, ErrorsMapToStatusCodes) {
  start(RoutingStrategy::RoundRobin);
  EXPECT_EQ(client->get("/services/nope/route")->result(), http::status::not_found);
  EXPECT_EQ(client->get("/services/Bad_Name/route")->result(), http::status::bad_request);
  const auto wrong = client->request(http::verb::post, "/services/shop/route", "{}");
  EXPECT_EQ(wrong->result(), http::status::method_not_allowed);
  EXPECT_EQ((*wrong)[http::field::allow], "GET");

  for (const char* id : {"a", "b"}) {
    const auto stored = registry->getInstance("shop", id).value();
    ASSERT_TRUE(registry->updateHealth("shop", id, stored.registered_at, HealthStatus::Unhealthy).ok());
  }
  const auto none = client->get("/services/shop/route");
  EXPECT_EQ(none->result(), http::status::service_unavailable);
  EXPECT_NE(body(none)["detail"].get<std::string>().find("no routable instance"), std::string::npos);
}

TEST_F(RouteEndpointTest, WithoutARouterTheEndpointDoesNotExist) {
  start(RoutingStrategy::RoundRobin, /*with_router=*/false);
  EXPECT_EQ(client->get("/services/shop/route")->result(), http::status::not_found);
  EXPECT_EQ(client->get("/services/shop/routable")->result(), http::status::ok) << "the rest is unaffected";
}

TEST_F(RouteEndpointTest, NothingIsForwardedAndNoStateInTheRegistryChanges) {
  start(RoutingStrategy::LeastConnections);
  const auto before = registry->getInstance("shop", "a").value();
  for (int i = 0; i < 20; ++i) ASSERT_EQ(client->get("/services/shop/route")->result(), http::status::ok);
  const auto after = registry->getInstance("shop", "a").value();
  EXPECT_EQ(after.connection_count, before.connection_count) << "a routing decision is not a connection";
  EXPECT_EQ(after.updated_at, before.updated_at);
}

}  // namespace
