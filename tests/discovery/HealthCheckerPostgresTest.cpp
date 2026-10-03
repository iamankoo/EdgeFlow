#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/discovery/HealthChecker.hpp"
#include "support/HealthTestSupport.hpp"
#include "support/PgTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;
using namespace edgeflow::discovery;
using edgeflow::testing::makeInstance;
using edgeflow::testing::RawServer;
using edgeflow::testing::RegistryFixture;
using edgeflow::testing::waitFor;

config::HealthCheckConfig fastConfig(config::HealthCheckType type) {
  config::HealthCheckConfig config;
  config.enabled = true;
  config.type = type;
  config.interval = 40ms;
  config.timeout = 300ms;
  config.refresh_interval = 40ms;
  config.failure_threshold = 2;
  config.success_threshold = 2;
  return config;
}

// Real PostgreSQL registry + real health checker + real backends on loopback.
class HealthCheckerPostgresTest : public RegistryFixture {
 protected:
  HealthStatus health(const std::string& service, const std::string& id) {
    const auto found = registry->getInstance(service, id);
    return found.ok() ? found.value().health : HealthStatus::Unknown;
  }
  bool eventually(const std::string& service, const std::string& id, HealthStatus wanted) {
    return waitFor([&] { return health(service, id) == wanted; });
  }
  std::set<std::string> routableIds(const std::string& service) {
    std::set<std::string> ids;
    const auto routable = registry->lookupRoutable(service);
    if (routable.ok()) {
      for (const auto& i : routable.value()) ids.insert(i.instance_id);
    }
    return ids;
  }
  // The checker watches EVERY instance in the registry, not just one test's service, so
  // expectations about what it tracks are registry-wide: all instances except disabled ones.
  std::size_t checkableInRegistry() {
    const auto all = registry->listInstances();
    std::size_t count = 0;
    if (all.ok()) {
      for (const auto& i : all.value()) count += (i.status != InstanceStatus::Disabled);
    }
    return count;
  }
  NewInstance instanceOn(const std::string& service, const std::string& id, std::uint16_t port) {
    auto instance = makeInstance(service, "127.0.0.1", port);
    instance.instance_id = id;
    return instance;
  }
};

TEST_F(HealthCheckerPostgresTest, TheExitConditionWithRealTcpBackends) {
  const auto service = newService("exit");
  RawServer a(RawServer::Mode::Close);
  RawServer b(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "a", a.port())).ok());
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "b", b.port())).ok());
  EXPECT_TRUE(routableIds(service).empty()) << "registered but unchecked: not routable yet";

  HealthChecker checker(registry, fastConfig(config::HealthCheckType::Tcp), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(eventually(service, "a", HealthStatus::Healthy));
  ASSERT_TRUE(eventually(service, "b", HealthStatus::Healthy));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"a", "b"}));

  b.stop();
  ASSERT_TRUE(eventually(service, "b", HealthStatus::Unhealthy));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"a"})) << "B is excluded";

  ASSERT_TRUE(b.restart());
  ASSERT_TRUE(eventually(service, "b", HealthStatus::Healthy));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"a", "b"})) << "B is reintroduced";
}

TEST_F(HealthCheckerPostgresTest, TheExitConditionWithRealHttpBackends) {
  const auto service = newService("exithttp");
  RawServer a(RawServer::Mode::Http, 200);
  RawServer b(RawServer::Mode::Http, 200);
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "a", a.port())).ok());
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "b", b.port())).ok());

  HealthChecker checker(registry, fastConfig(config::HealthCheckType::Http), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(eventually(service, "a", HealthStatus::Healthy));
  ASSERT_TRUE(eventually(service, "b", HealthStatus::Healthy));
  EXPECT_EQ(routableIds(service).size(), 2U);

  b.setStatus(500);  // the port is open, the application is failing
  ASSERT_TRUE(eventually(service, "b", HealthStatus::Unhealthy));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"a"}));

  b.setMode(RawServer::Mode::Stall);  // now it does not even answer
  b.setStatus(200);
  ASSERT_TRUE(waitFor([&] { return b.requests() > 0; }));
  EXPECT_EQ(health(service, "b"), HealthStatus::Unhealthy);

  b.setMode(RawServer::Mode::Http);  // recovered
  ASSERT_TRUE(eventually(service, "b", HealthStatus::Healthy));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"a", "b"}));
}

TEST_F(HealthCheckerPostgresTest, HealthIsPersistedAndSurvivesARestartOfTheChecker) {
  const auto service = newService("persist");
  RawServer up(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "up", up.port())).ok());
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "down", edgeflow::testing::closedPort())).ok());
  {
    HealthChecker checker(registry, fastConfig(config::HealthCheckType::Tcp), log.logger());
    ASSERT_TRUE(checker.start());
    ASSERT_TRUE(eventually(service, "up", HealthStatus::Healthy));
    ASSERT_TRUE(eventually(service, "down", HealthStatus::Unhealthy));
  }  // checker stopped and destroyed

  EXPECT_EQ(health(service, "up"), HealthStatus::Healthy) << "persisted in PostgreSQL, not in the checker";
  EXPECT_EQ(health(service, "down"), HealthStatus::Unhealthy);
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"up"}));

  // A fresh checker (as after an EdgeFlow restart) starts from the persisted state.
  HealthChecker again(registry, fastConfig(config::HealthCheckType::Tcp), log.logger());
  ASSERT_TRUE(again.start());
  ASSERT_TRUE(waitFor([&] { return again.stats().tracked_instances == checkableInRegistry(); }));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"up"})) << "no flapping across the restart";
  EXPECT_EQ(health(service, "up"), HealthStatus::Healthy);
}

TEST_F(HealthCheckerPostgresTest, NewDeregisteredAndDisabledInstancesAreHandledWhileRunning) {
  const auto service = newService("dyn");
  RawServer s1(RawServer::Mode::Close);
  RawServer s2(RawServer::Mode::Close);
  HealthChecker checker(registry, fastConfig(config::HealthCheckType::Tcp), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(waitFor([&] { return checker.stats().refreshes >= 2; }));

  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "one", s1.port())).ok());
  ASSERT_TRUE(eventually(service, "one", HealthStatus::Healthy)) << "new instance detected without a restart";
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "two", s2.port())).ok());
  ASSERT_TRUE(eventually(service, "two", HealthStatus::Healthy));
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"one", "two"}));

  ASSERT_TRUE(registry->deregisterInstance(service, "one").ok());
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"two"}));
  InstanceUpdate disable;
  disable.status = InstanceStatus::Disabled;
  ASSERT_TRUE(registry->updateInstance(service, "two", disable).ok());
  EXPECT_TRUE(routableIds(service).empty()) << "a disabled instance is not routable although healthy";
  ASSERT_TRUE(waitFor([&] {
    const auto before = s2.connections();
    std::this_thread::sleep_for(150ms);
    return s2.connections() == before;
  })) << "a disabled instance stops being probed";
  EXPECT_EQ(health(service, "two"), HealthStatus::Healthy) << "its last known health is kept";

  InstanceUpdate enable;
  enable.status = InstanceStatus::Active;
  ASSERT_TRUE(registry->updateInstance(service, "two", enable).ok());
  EXPECT_EQ(routableIds(service), (std::set<std::string>{"two"}));
}

TEST_F(HealthCheckerPostgresTest, AProbeResultForAReplacedInstanceIsNotWrittenToTheNewOne) {
  const auto service = newService("incarnation");
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "x", 41001)).ok());
  const auto old_incarnation = registry->getInstance(service, "x").value();

  ASSERT_TRUE(registry->deregisterInstance(service, "x").ok());
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "x", 41002)).ok());

  const auto stale = registry->updateHealth(service, "x", old_incarnation.registered_at, HealthStatus::Healthy);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, RegistryErrorCode::InstanceNotFound);
  EXPECT_EQ(health(service, "x"), HealthStatus::Unknown) << "the new instance was not touched";

  const auto current = registry->getInstance(service, "x").value();
  const auto fresh = registry->updateHealth(service, "x", current.registered_at, HealthStatus::Healthy);
  ASSERT_TRUE(fresh.ok()) << fresh.error().message;
  EXPECT_EQ(fresh.value().health, HealthStatus::Healthy);
}

TEST_F(HealthCheckerPostgresTest, ConcurrentRegistrationChurnWhileCheckingStaysConsistent) {
  const auto service = newService("churn");
  RawServer backend_server(RawServer::Mode::Close);
  HealthChecker checker(registry, fastConfig(config::HealthCheckType::Tcp), log.logger());
  ASSERT_TRUE(checker.start());

  constexpr int kWorkers = 4;
  constexpr int kRounds = 12;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWorkers; ++w) {
    threads.emplace_back([&, w] {
      for (int round = 0; round < kRounds; ++round) {
        const std::string id = "w" + std::to_string(w) + "-" + std::to_string(round);
        auto instance = makeInstance(service, "127.0." + std::to_string(w + 1) + "." + std::to_string(round + 1),
                                     backend_server.port());
        instance.instance_id = id;
        if (!registry->registerInstance(instance).ok()) ++errors;
        if (round % 2 == 0 && !registry->deregisterInstance(service, id).ok()) ++errors;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(errors.load(), 0);

  // The listener is on 127.0.0.1 only, so just those instances are reachable; all others
  // are refused. Every remaining instance must end up with a verdict and the checker must
  // track exactly the instances that still exist.
  ASSERT_TRUE(waitFor([&] {
    const auto all = registry->lookupService(service);
    if (!all.ok()) return false;
    for (const auto& i : all.value()) {
      if (i.health == HealthStatus::Unknown) return false;
    }
    return !all.value().empty() && checker.stats().tracked_instances == checkableInRegistry();
  }, 20s));
  EXPECT_EQ(registry->lookupService(service).value().size(), static_cast<std::size_t>(kWorkers * kRounds / 2));
  EXPECT_EQ(checker.stats().persist_failures, 0U);
}

TEST_F(HealthCheckerPostgresTest, ADatabaseOutageIsSurvivedByTheChecker) {
  const auto service = newService("outage");
  RawServer up(RawServer::Mode::Close);
  ASSERT_TRUE(registry->registerInstance(instanceOn(service, "a", up.port())).ok());
  HealthChecker checker(registry, fastConfig(config::HealthCheckType::Tcp), log.logger());
  ASSERT_TRUE(checker.start());
  ASSERT_TRUE(eventually(service, "a", HealthStatus::Healthy));

  // Kill every pooled backend connection from outside: the checker's next database calls
  // must recover by reconnecting instead of failing for good.
  std::string error;
  auto admin = storage::PgConnection::connect(params_, error);
  ASSERT_TRUE(admin) << error;
  ASSERT_TRUE(admin->exec("SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                          "WHERE pid <> pg_backend_pid() AND datname = current_database() "
                          "AND usename = current_user AND application_name = 'edgeflow'").ok());

  up.stop();
  ASSERT_TRUE(eventually(service, "a", HealthStatus::Unhealthy)) << "health is still recorded after the connections were killed";
}

}  // namespace
