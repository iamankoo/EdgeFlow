#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/config/ConfigManager.hpp"
#include "edgeflow/routing/Router.hpp"
#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using config::RoutingStrategy;
using discovery::HealthStatus;
using discovery::RegistryErrorCode;
using Clock = std::chrono::steady_clock;

// A clock the test moves by hand.
struct Time {
  std::atomic<std::int64_t> ms{1000};
  std::function<Clock::time_point()> fn() {
    return [this] { return Clock::time_point{} + std::chrono::milliseconds{ms.load()}; };
  }
  void advance(std::chrono::milliseconds by) { ms += by.count(); }
};

class RouterSnapshotTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (const char* id : {"a", "b", "c"}) add(id);
  }

  void add(const std::string& id, HealthStatus health = HealthStatus::Healthy) {
    discovery::NewInstance instance;
    instance.service = "shop";
    instance.instance_id = id;
    instance.host = "10.0.0.1";
    instance.port = static_cast<std::uint16_t>(8000 + id.back());
    instance.health = health;
    ASSERT_TRUE(registry->registerInstance(instance).ok());
  }

  void setHealth(const std::string& id, HealthStatus health) {
    discovery::InstanceUpdate update;
    update.health = health;
    ASSERT_TRUE(registry->updateInstance("shop", id, update).ok());
  }

  std::unique_ptr<routing::Router> makeRouter(std::chrono::milliseconds ttl,
                                              RoutingStrategy strategy = RoutingStrategy::RoundRobin) {
    return std::make_unique<routing::Router>(registry, routing::makeLoadBalancer(strategy),
                                             routing::Router::Options{ttl, time.fn()});
  }

  std::shared_ptr<FlakyRegistry> registry = std::make_shared<FlakyRegistry>();
  Time time;
};

TEST_F(RouterSnapshotTest, WithoutATtlEveryRouteReadsTheRegistry) {
  auto router = makeRouter(0ms);
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(router->route("shop").ok());
  EXPECT_EQ(registry->routable_lookups.load(), 10U);
  EXPECT_EQ(router->stats().snapshot_hits, 0U);
  EXPECT_EQ(router->stats().snapshot_reads, 10U);
}

TEST_F(RouterSnapshotTest, WithATtlTheRegistryIsReadOncePerIntervalAndRoutingStillRotates) {
  auto router = makeRouter(500ms);
  std::map<std::string, int> seen;
  for (int i = 0; i < 30; ++i) {
    const auto chosen = router->route("shop");
    ASSERT_TRUE(chosen.ok());
    ++seen[chosen.value().instance_id];
  }
  EXPECT_EQ(registry->routable_lookups.load(), 1U);
  EXPECT_EQ(router->stats().snapshot_hits, 29U);
  EXPECT_EQ(seen, (std::map<std::string, int>{{"a", 10}, {"b", 10}, {"c", 10}})) << "round robin over the kept snapshot";

  time.advance(499ms);
  ASSERT_TRUE(router->route("shop").ok());
  EXPECT_EQ(registry->routable_lookups.load(), 1U) << "still inside the interval";
  time.advance(1ms);
  ASSERT_TRUE(router->route("shop").ok());
  EXPECT_EQ(registry->routable_lookups.load(), 2U) << "expired: read again";
}

TEST_F(RouterSnapshotTest, AHealthChangeReachesRoutingAtMostOneIntervalLate) {
  auto router = makeRouter(1000ms);
  ASSERT_TRUE(router->route("shop").ok());
  setHealth("b", HealthStatus::Unhealthy);

  std::map<std::string, int> before;
  for (int i = 0; i < 30; ++i) ++before[router->route("shop").value().instance_id];
  EXPECT_GT(before["b"], 0) << "documented: the unhealthy instance is still in the kept snapshot";

  time.advance(1000ms);
  std::map<std::string, int> after;
  for (int i = 0; i < 30; ++i) ++after[router->route("shop").value().instance_id];
  EXPECT_EQ(after["b"], 0) << "gone once the snapshot is read again";
  EXPECT_EQ(after["a"] + after["c"], 30);
}

TEST_F(RouterSnapshotTest, ANewInstanceAppearsAfterTheInterval) {
  auto router = makeRouter(1000ms);
  ASSERT_TRUE(router->route("shop").ok());
  add("d");
  std::set<std::string> ids;
  for (int i = 0; i < 20; ++i) ids.insert(router->route("shop").value().instance_id);
  EXPECT_EQ(ids.count("d"), 0U);
  time.advance(1000ms);
  ids.clear();
  for (int i = 0; i < 20; ++i) ids.insert(router->route("shop").value().instance_id);
  EXPECT_EQ(ids.count("d"), 1U);
}

TEST_F(RouterSnapshotTest, ANotRoutableServiceIsNeverKeptSoRecoveryIsSeenAtOnce) {
  discovery::NewInstance sick;
  sick.service = "sick";
  sick.instance_id = "x";
  sick.host = "10.0.0.9";
  sick.port = 9000;
  sick.health = HealthStatus::Unhealthy;
  ASSERT_TRUE(registry->registerInstance(sick).ok());

  auto router = makeRouter(60000ms);
  for (int i = 0; i < 5; ++i) {
    const auto chosen = router->route("sick");
    ASSERT_FALSE(chosen.ok());
    EXPECT_EQ(chosen.error().code, RegistryErrorCode::NoRoutableInstance);
  }
  EXPECT_EQ(registry->routable_lookups.load(), 5U) << "an empty answer is not kept";

  discovery::InstanceUpdate update;
  update.health = HealthStatus::Healthy;
  ASSERT_TRUE(registry->updateInstance("sick", "x", update).ok());
  EXPECT_TRUE(router->route("sick").ok()) << "routable the moment it is, with no waiting for an interval";
  EXPECT_EQ(router->route("nosuch").error().code, RegistryErrorCode::ServiceNotFound);
}

TEST_F(RouterSnapshotTest, ErrorsAreNotKeptAndAnExpiredSnapshotIsNotServedWhileTheRegistryIsDown) {
  auto router = makeRouter(1000ms);
  ASSERT_TRUE(router->route("shop").ok());
  registry->routable_unavailable.store(true);
  EXPECT_TRUE(router->route("shop").ok()) << "inside the interval the kept snapshot still answers";
  time.advance(1000ms);
  const auto down = router->route("shop");
  ASSERT_FALSE(down.ok());
  EXPECT_EQ(down.error().code, RegistryErrorCode::DatabaseUnavailable) << "staleness is bounded by the interval, not extended by an outage";
  registry->routable_unavailable.store(false);
  EXPECT_TRUE(router->route("shop").ok()) << "and it recovers at once";
}

TEST_F(RouterSnapshotTest, FiltersGiveTheSameAnswersWithAndWithoutTheKeptSnapshot) {
  routing::Router::Filter filter;
  filter.eligible = [](const discovery::ServiceInstance& i) { return i.instance_id != "b"; };
  filter.preferred = [](const discovery::ServiceInstance& i) { return i.instance_id == "c"; };
  for (const auto ttl : {0ms, 1000ms}) {
    SCOPED_TRACE(ttl.count());
    auto router = makeRouter(ttl);
    for (int i = 0; i < 6; ++i) {
      const auto chosen = router->route("shop", {}, filter);
      ASSERT_TRUE(chosen.ok());
      EXPECT_EQ(chosen.value().instance_id, "c") << "b is never eligible, c is preferred";
    }
    filter.preferred = [](const discovery::ServiceInstance&) { return false; };  // nothing preferred: any eligible
    std::set<std::string> ids;
    for (int i = 0; i < 6; ++i) ids.insert(router->route("shop", {}, filter).value().instance_id);
    EXPECT_EQ(ids, (std::set<std::string>{"a", "c"}));
    filter.eligible = [](const discovery::ServiceInstance&) { return false; };
    const auto none = router->route("shop", {}, filter);
    ASSERT_FALSE(none.ok());
    EXPECT_EQ(none.error().code, RegistryErrorCode::NoRoutableInstance);
    filter.eligible = [](const discovery::ServiceInstance& i) { return i.instance_id != "b"; };
    filter.preferred = [](const discovery::ServiceInstance& i) { return i.instance_id == "c"; };
  }
}

TEST_F(RouterSnapshotTest, AFilterThatAcceptsEverythingTakesTheNoCopyPathAndKeepsTheStrategysChoice) {
  auto cached = makeRouter(1000ms);
  auto fresh = makeRouter(0ms);
  routing::Router::Filter all;
  all.eligible = [](const discovery::ServiceInstance&) { return true; };
  all.preferred = [](const discovery::ServiceInstance&) { return true; };
  for (int i = 0; i < 12; ++i) {
    EXPECT_EQ(cached->route("shop", {}, all).value().instance_id, fresh->route("shop", {}).value().instance_id);
  }
}

TEST_F(RouterSnapshotTest, ConcurrentRoutingSharesOneSnapshotAndNeverReturnsAnInvalidInstance) {
  auto router = std::make_unique<routing::Router>(registry, routing::makeLoadBalancer(RoutingStrategy::RoundRobin),
                                                  routing::Router::Options{200ms, nullptr});  // the real clock
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      const auto end = Clock::now() + 700ms;
      while (Clock::now() < end) {
        const auto chosen = router->route("shop");
        if (!chosen.ok() || chosen.value().service != "shop" ||
            std::string("abc").find(chosen.value().instance_id) == std::string::npos) {
          bad.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(bad.load(), 0);
  const auto stats = router->stats();
  EXPECT_GT(stats.snapshot_hits, 1000U);
  EXPECT_LE(stats.snapshot_reads, 60U) << "about one read per 200 ms interval (a few more when threads race at expiry)";
}

TEST(RouterSnapshotConfigTest, ParsesTheIntervalAndRefusesItForLeastConnections) {
  config::ConfigManager defaults;
  ASSERT_TRUE(defaults.loadFromString("server:\n  port: 8080\n"));
  EXPECT_EQ(defaults.config().routing.snapshot_ttl, 0ms) << "off by default: Phase 5-9 behaviour";

  config::ConfigManager ok;
  ASSERT_TRUE(ok.loadFromString("routing:\n  strategy: weighted\n  snapshot_ttl_ms: 250\n"));
  EXPECT_EQ(ok.config().routing.snapshot_ttl, 250ms);

  config::ConfigManager zero_least;
  EXPECT_TRUE(zero_least.loadFromString("routing:\n  strategy: least_connections\n  snapshot_ttl_ms: 0\n"));

  config::ConfigManager refused;
  EXPECT_FALSE(refused.loadFromString("routing:\n  strategy: least_connections\n  snapshot_ttl_ms: 100\n"));
  ASSERT_FALSE(refused.errors().empty());
  EXPECT_NE(refused.errors().front().find("least_connections"), std::string::npos);

  for (const char* bad : {"routing:\n  snapshot_ttl_ms: -1\n", "routing:\n  snapshot_ttl_ms: 60001\n",
                          "routing:\n  snapshot_ttl_ms: soon\n"}) {
    config::ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(bad)) << bad;
  }
}

}  // namespace
