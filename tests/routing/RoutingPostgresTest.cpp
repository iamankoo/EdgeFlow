#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/discovery/HealthChecker.hpp"
#include "edgeflow/routing/Router.hpp"
#include "support/HealthTestSupport.hpp"
#include "support/PgTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;
using namespace edgeflow::discovery;
using edgeflow::config::RoutingStrategy;
using edgeflow::testing::makeInstance;
using edgeflow::testing::RawServer;
using edgeflow::testing::RegistryFixture;
using edgeflow::testing::waitFor;

config::HealthCheckConfig fastChecks() {
  config::HealthCheckConfig config;
  config.enabled = true;
  config.type = config::HealthCheckType::Tcp;
  config.interval = 40ms;
  config.timeout = 300ms;
  config.refresh_interval = 40ms;
  config.failure_threshold = 2;
  config.success_threshold = 2;
  return config;
}

// Real PostgreSQL registry + real health checker + real backends: the routing layer is fed
// by the set that Phase 4 really produces, not by a hand-made list.
class RoutingPostgresTest : public RegistryFixture {
 protected:
  NewInstance on(const std::string& service, const std::string& id, std::uint16_t port,
                 std::uint32_t weight = 1, std::uint64_t connections = 0) {
    auto instance = makeInstance(service, "127.0.0.1", port);
    instance.instance_id = id;
    instance.weight = weight;
    instance.connection_count = connections;
    return instance;
  }
  std::set<std::string> routableIds(const std::string& service) {
    std::set<std::string> ids;
    const auto routable = registry->lookupRoutable(service);
    if (routable.ok()) {
      for (const auto& i : routable.value()) ids.insert(i.instance_id);
    }
    return ids;
  }
  routing::Router routerFor(RoutingStrategy strategy) {
    return routing::Router(registry, routing::makeLoadBalancer(strategy));
  }
  std::string route(routing::Router& router, const std::string& service, std::string_view key = {}) {
    const auto chosen = router.route(service, routing::RoutingContext{key});
    return chosen.ok() ? chosen.value().instance_id : "<" + std::string{discovery::toString(chosen.error().code)} + ">";
  }
};

TEST_F(RoutingPostgresTest, AllFourStrategiesDistributeAmongHealthyInstancesOnly) {
  const auto service = newService("shop");
  RawServer a(RawServer::Mode::Close);
  RawServer b(RawServer::Mode::Close);
  RawServer c(RawServer::Mode::Close);
  // a=5/b=3/c=2 for weighted routing; 5, 2 and 7 connections for least connections.
  ASSERT_TRUE(registry->registerInstance(on(service, "a", a.port(), 5, 5)).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "b", b.port(), 3, 2)).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "c", c.port(), 2, 7)).ok());
  // d is registered with the best credentials of all (weight 9, 0 connections) but is down.
  ASSERT_TRUE(registry->registerInstance(on(service, "d", edgeflow::testing::closedPort(), 9, 0)).ok());

  HealthChecker checker(registry, fastChecks(), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] { return routableIds(service) == std::set<std::string>{"a", "b", "c"}; }))
      << "d must be excluded by health checking before any routing happens";

  // ---- Round Robin
  {
    auto router = routerFor(RoutingStrategy::RoundRobin);
    std::string sequence;
    for (int i = 0; i < 9; ++i) sequence += route(router, service);
    EXPECT_EQ(sequence, "abcabcabc");
  }
  // ---- Least Connections
  {
    auto router = routerFor(RoutingStrategy::LeastConnections);
    EXPECT_EQ(route(router, service), "b") << "a=5, b=2, c=7";
    ASSERT_TRUE(registry->adjustConnectionCount(service, "b", 6).ok());  // b: 8
    EXPECT_EQ(route(router, service), "a") << "a=5 is now the lowest";
    ASSERT_TRUE(registry->adjustConnectionCount(service, "b", -6).ok());  // back to 2
    EXPECT_EQ(route(router, service), "b");
  }
  // ---- Weighted
  {
    auto router = routerFor(RoutingStrategy::Weighted);
    std::map<std::string, int> counts;
    for (int i = 0; i < 1000; ++i) ++counts[route(router, service)];
    EXPECT_EQ(counts["a"], 500);
    EXPECT_EQ(counts["b"], 300);
    EXPECT_EQ(counts["c"], 200);
    EXPECT_EQ(counts.count("d"), 0U);
  }
  // ---- Consistent Hashing
  {
    auto router = routerFor(RoutingStrategy::ConsistentHashing);
    std::map<std::string, std::string> first;
    std::set<std::string> targets;
    for (int i = 0; i < 300; ++i) {
      const auto key = "client-" + std::to_string(i);
      first[key] = route(router, service, key);
      targets.insert(first[key]);
    }
    EXPECT_EQ(targets, (std::set<std::string>{"a", "b", "c"}));
    for (const auto& [key, target] : first) EXPECT_EQ(route(router, service, key), target) << key;
  }
}

TEST_F(RoutingPostgresTest, AFailingInstanceLeavesTheRotationAndRecoveredOnesReturn) {
  const auto service = newService("failover");
  RawServer a(RawServer::Mode::Close);
  RawServer b(RawServer::Mode::Close);
  RawServer c(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(on(service, "a", a.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "b", b.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "c", c.port())).ok());
  HealthChecker checker(registry, fastChecks(), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] { return routableIds(service).size() == 3; }));

  auto router = routerFor(RoutingStrategy::RoundRobin);
  b.stop();
  ASSERT_TRUE(waitFor([&] { return routableIds(service) == std::set<std::string>{"a", "c"}; }));
  std::set<std::string> while_down;
  for (int i = 0; i < 20; ++i) while_down.insert(route(router, service));
  EXPECT_EQ(while_down, (std::set<std::string>{"a", "c"})) << "the failed instance receives nothing";

  ASSERT_TRUE(b.restart());
  ASSERT_TRUE(waitFor([&] { return routableIds(service).size() == 3; }));
  std::set<std::string> after;
  for (int i = 0; i < 20; ++i) after.insert(route(router, service));
  EXPECT_EQ(after, (std::set<std::string>{"a", "b", "c"})) << "and it is back in the rotation";
}

TEST_F(RoutingPostgresTest, ConsistentHashingRemapsOnlyTheAffectedKeysWhenTheLiveSetChanges) {
  const auto service = newService("hash");
  RawServer a(RawServer::Mode::Close);
  RawServer b(RawServer::Mode::Close);
  RawServer c(RawServer::Mode::Close);
  RawServer d(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(on(service, "a", a.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "b", b.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "c", c.port())).ok());
  HealthChecker checker(registry, fastChecks(), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] { return routableIds(service).size() == 3; }));

  auto router = routerFor(RoutingStrategy::ConsistentHashing);
  const auto snapshot = [&] {
    std::map<std::string, std::string> result;
    for (int i = 0; i < 2000; ++i) {
      const auto key = "client-" + std::to_string(i);
      result[key] = route(router, service, key);
    }
    return result;
  };
  const auto three = snapshot();

  // A fourth instance is registered while running and becomes routable after its checks.
  ASSERT_TRUE(registry->registerInstance(on(service, "d", d.port())).ok());
  ASSERT_TRUE(waitFor([&] { return routableIds(service).size() == 4; }));
  const auto four = snapshot();
  int moved_to_d = 0;
  for (const auto& [key, target] : three) {
    if (four.at(key) == target) continue;
    EXPECT_EQ(four.at(key), "d") << key << " moved between old instances";
    ++moved_to_d;
  }
  EXPECT_GT(moved_to_d, 0);
  EXPECT_LT(moved_to_d, 2000 * 40 / 100);

  // Instance b dies: only b's keys move.
  b.stop();
  ASSERT_TRUE(waitFor([&] { return routableIds(service).count("b") == 0; }));
  const auto without_b = snapshot();
  for (const auto& [key, target] : four) {
    EXPECT_NE(without_b.at(key), "b");
    if (target != "b") {
      EXPECT_EQ(without_b.at(key), target) << key << " did not belong to b but moved";
    }
  }
}

TEST_F(RoutingPostgresTest, ASingleHealthyInstanceReceivesEverythingWhateverTheStrategy) {
  const auto service = newService("single");
  RawServer only(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(on(service, "only", only.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "dead", edgeflow::testing::closedPort())).ok());
  HealthChecker checker(registry, fastChecks(), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] { return routableIds(service) == std::set<std::string>{"only"}; }));
  for (const auto strategy : {RoutingStrategy::RoundRobin, RoutingStrategy::LeastConnections,
                              RoutingStrategy::Weighted, RoutingStrategy::ConsistentHashing}) {
    auto router = routerFor(strategy);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(route(router, service, "k" + std::to_string(i)), "only");
  }
}

TEST_F(RoutingPostgresTest, NothingRoutableIsReportedAsSuch) {
  const auto service = newService("none");
  ASSERT_TRUE(registry->registerInstance(on(service, "dead", edgeflow::testing::closedPort())).ok());
  HealthChecker checker(registry, fastChecks(), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] {
    const auto found = registry->getInstance(service, "dead");
    return found.ok() && found.value().health == HealthStatus::Unhealthy;
  }));
  auto router = routerFor(RoutingStrategy::RoundRobin);
  EXPECT_EQ(route(router, service), "<no_routable_instance>");
  EXPECT_EQ(route(router, newService("unknown-" + std::string{"x"})), "<service_not_found>");
}

TEST_F(RoutingPostgresTest, ConcurrentRoutingWhileHealthChangesNeverSelectsAnExcludedInstance) {
  const auto service = newService("concurrent");
  RawServer a(RawServer::Mode::Close);
  RawServer b(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(on(service, "a", a.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "b", b.port())).ok());
  ASSERT_TRUE(registry->registerInstance(on(service, "z", edgeflow::testing::closedPort())).ok());
  HealthChecker checker(registry, fastChecks(), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] { return routableIds(service) == std::set<std::string>{"a", "b"}; }));

  auto router = routerFor(RoutingStrategy::RoundRobin);
  std::atomic<int> bad{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      while (!stop.load()) {
        const auto chosen = router.route(service);
        if (chosen.ok() && chosen.value().instance_id == "z") ++bad;
        if (!chosen.ok() && chosen.error().code != RegistryErrorCode::NoRoutableInstance) ++bad;
      }
    });
  }
  for (int i = 0; i < 3; ++i) {  // b flaps while requests are being routed
    b.stop();
    std::this_thread::sleep_for(150ms);
    (void)b.restart();
    std::this_thread::sleep_for(150ms);
  }
  stop = true;
  for (auto& t : threads) t.join();
  EXPECT_EQ(bad.load(), 0);
}

}  // namespace
