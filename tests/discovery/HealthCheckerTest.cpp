#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/discovery/HealthChecker.hpp"
#include "support/FakeRegistry.hpp"
#include "support/HealthTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;
using namespace edgeflow::discovery;
using edgeflow::testing::CapturedLogger;
using edgeflow::testing::FakeRegistry;
using edgeflow::testing::ManualProber;
using edgeflow::testing::RawServer;
using edgeflow::testing::waitFor;

config::HealthCheckConfig fastConfig(config::HealthCheckType type = config::HealthCheckType::Tcp) {
  config::HealthCheckConfig config;
  config.enabled = true;
  config.type = type;
  config.interval = 30ms;
  config.timeout = 250ms;
  config.refresh_interval = 30ms;
  config.failure_threshold = 2;
  config.success_threshold = 2;
  config.max_concurrent_checks = 8;
  return config;
}

NewInstance backend(const std::string& service, const std::string& id, std::uint16_t port) {
  NewInstance instance;
  instance.service = service;
  instance.instance_id = id;
  instance.host = "127.0.0.1";
  instance.port = port;
  return instance;
}

class HealthCheckerTest : public ::testing::Test {
 protected:
  HealthCheckerTest() : registry(std::make_shared<FakeRegistry>()) {}

  void reg(const NewInstance& instance) {
    const auto stored = registry->registerInstance(instance);
    ASSERT_TRUE(stored.ok()) << stored.error().message;
  }
  HealthStatus health(const std::string& service, const std::string& id) {
    const auto found = registry->getInstance(service, id);
    return found.ok() ? found.value().health : HealthStatus::Unknown;
  }
  bool eventuallyHealth(const std::string& service, const std::string& id, HealthStatus wanted) {
    return waitFor([&] { return health(service, id) == wanted; });
  }
  std::size_t routableCount(const std::string& service) {
    const auto routable = registry->lookupRoutable(service);
    return routable.ok() ? routable.value().size() : 0;
  }
  std::unique_ptr<HealthChecker> checker(config::HealthCheckConfig config = fastConfig(),
                                         std::shared_ptr<Prober> prober = nullptr) {
    return std::make_unique<HealthChecker>(registry, std::move(config), log.logger(), std::move(prober));
  }

  CapturedLogger log;
  std::shared_ptr<FakeRegistry> registry;
};

// --- real TCP probing end to end ---------------------------------------------------------

TEST_F(HealthCheckerTest, ReachableInstanceBecomesHealthyAndRoutable) {
  RawServer backend_a(RawServer::Mode::Close);
  reg(backend("svc", "a", backend_a.port()));
  EXPECT_EQ(routableCount("svc"), 0U) << "registered but never checked: unknown is not routable";

  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  EXPECT_EQ(routableCount("svc"), 1U);
  EXPECT_TRUE(log.contains("now checking instance svc/a"));
  EXPECT_TRUE(log.contains("is now healthy (was unknown)"));
}

TEST_F(HealthCheckerTest, UnreachableInstanceBecomesUnhealthyAtOnceAndIsNeverRoutable) {
  reg(backend("svc", "dead", edgeflow::testing::closedPort()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "dead", HealthStatus::Unhealthy));
  EXPECT_EQ(routableCount("svc"), 0U);
  EXPECT_TRUE(log.contains("is now unhealthy (was unknown): connection refused"));
}

TEST_F(HealthCheckerTest, FailureThenRecoveryExcludesAndReintroducesTheInstance) {
  RawServer a(RawServer::Mode::Close);
  RawServer b(RawServer::Mode::Close);
  reg(backend("svc", "a", a.port()));
  reg(backend("svc", "b", b.port()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  ASSERT_TRUE(eventuallyHealth("svc", "b", HealthStatus::Healthy));
  EXPECT_EQ(routableCount("svc"), 2U);

  b.stop();  // backend B disappears
  ASSERT_TRUE(eventuallyHealth("svc", "b", HealthStatus::Unhealthy));
  EXPECT_EQ(routableCount("svc"), 1U) << "the unhealthy instance is excluded";
  EXPECT_EQ(registry->lookupRoutable("svc").value()[0].instance_id, "a");
  EXPECT_EQ(health("svc", "a"), HealthStatus::Healthy) << "the healthy neighbour is unaffected";

  ASSERT_TRUE(b.restart());  // backend B recovers
  ASSERT_TRUE(eventuallyHealth("svc", "b", HealthStatus::Healthy));
  EXPECT_EQ(routableCount("svc"), 2U) << "the recovered instance is reintroduced";
  EXPECT_TRUE(log.contains("instance svc/b (127.0.0.1:" + std::to_string(b.port()) + ") is now unhealthy"));
}

// --- real HTTP probing end to end -------------------------------------------------------------

TEST_F(HealthCheckerTest, HttpChecksFollowTheApplicationLevelResponse) {
  RawServer app(RawServer::Mode::Http, 200);
  reg(backend("web", "w1", app.port()));
  auto config = fastConfig(config::HealthCheckType::Http);
  config.http_path = "/ready";
  auto hc = checker(config);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("web", "w1", HealthStatus::Healthy));
  EXPECT_NE(app.lastRequest().find("GET /ready HTTP/1.1"), std::string::npos);

  app.setStatus(503);  // the port still accepts connections, the application does not work
  ASSERT_TRUE(eventuallyHealth("web", "w1", HealthStatus::Unhealthy));
  EXPECT_EQ(routableCount("web"), 0U);

  app.setStatus(200);
  ASSERT_TRUE(eventuallyHealth("web", "w1", HealthStatus::Healthy));
  EXPECT_EQ(routableCount("web"), 1U);
}

TEST_F(HealthCheckerTest, StalledBackendIsMarkedUnhealthyByTheTimeout) {
  RawServer stalled(RawServer::Mode::Stall);
  reg(backend("web", "slow", stalled.port()));
  auto config = fastConfig(config::HealthCheckType::Http);
  config.timeout = 100ms;
  auto hc = checker(config);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("web", "slow", HealthStatus::Unhealthy)) << "a stall must not block checking";
  EXPECT_TRUE(log.contains("timed out after 100ms"));
}

// --- thresholds, deterministically (no network, no sleeps) ----------------------------------------

class ThresholdTest : public HealthCheckerTest {
 protected:
  ThresholdTest() : prober(std::make_shared<ManualProber>()) {}

  // Answers one probe and waits until the following one is open (result fully processed).
  void step(std::uint16_t port, bool ok) {
    const int before = prober->started();
    ASSERT_TRUE(waitFor([&] { return prober->openFor(port) > 0; }));
    ASSERT_TRUE(prober->answer(port, ok));
    ASSERT_TRUE(waitFor([&] { return prober->started() > before; })) << "result not processed";
  }

  std::shared_ptr<ManualProber> prober;
};

TEST_F(ThresholdTest, HealthyInstanceFallsOnlyAfterTheFailureThreshold) {
  auto config = fastConfig();
  config.failure_threshold = 3;
  reg(backend("svc", "a", 7001));
  auto hc = checker(config, prober);
  ASSERT_TRUE(hc->start());

  step(7001, true);  // unknown -> healthy
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  step(7001, false);
  step(7001, false);
  EXPECT_EQ(health("svc", "a"), HealthStatus::Healthy) << "two failures are below the threshold of three";
  step(7001, true);  // the streak is broken
  step(7001, false);
  step(7001, false);
  EXPECT_EQ(health("svc", "a"), HealthStatus::Healthy) << "failures were not consecutive";
  step(7001, false);  // third consecutive failure
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Unhealthy));
}

TEST_F(ThresholdTest, UnhealthyInstanceRecoversOnlyAfterTheSuccessThreshold) {
  auto config = fastConfig();
  config.success_threshold = 3;
  reg(backend("svc", "a", 7002));
  auto hc = checker(config, prober);
  ASSERT_TRUE(hc->start());

  step(7002, false);  // unknown -> unhealthy at once
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Unhealthy));
  step(7002, true);
  step(7002, true);
  EXPECT_EQ(health("svc", "a"), HealthStatus::Unhealthy) << "two successes are below the threshold of three";
  step(7002, false);  // breaks the streak
  step(7002, true);
  step(7002, true);
  EXPECT_EQ(health("svc", "a"), HealthStatus::Unhealthy);
  step(7002, true);  // third consecutive success
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  EXPECT_EQ(routableCount("svc"), 1U);
}

TEST_F(ThresholdTest, OnlyTransitionsAreWrittenToTheRegistry) {
  reg(backend("svc", "a", 7003));
  auto hc = checker(fastConfig(), prober);
  ASSERT_TRUE(hc->start());
  step(7003, true);
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  const int writes = registry->healthUpdates();
  for (int i = 0; i < 6; ++i) step(7003, true);
  EXPECT_EQ(registry->healthUpdates(), writes) << "steady health causes no database writes";
}

TEST_F(ThresholdTest, ProbesUseTheConfiguredEndpointPathAndTimeout) {
  reg(backend("svc", "a", 7004));
  auto config = fastConfig(config::HealthCheckType::Http);
  config.http_path = "/status/ready";
  config.timeout = 123ms;
  auto hc = checker(config, prober);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(waitFor([&] { return prober->started() > 0; }));
  const auto probe_target = prober->lastTarget();
  EXPECT_EQ(probe_target.host, "127.0.0.1");
  EXPECT_EQ(probe_target.port, 7004);
  EXPECT_EQ(probe_target.http_path, "/status/ready");
  EXPECT_EQ(probe_target.timeout, 123ms);
}

TEST_F(ThresholdTest, ConcurrentProbesAreLimitedAndAllInstancesAreEventuallyChecked) {
  auto config = fastConfig();
  config.max_concurrent_checks = 2;
  for (std::uint16_t n = 0; n < 6; ++n) reg(backend("svc", "i" + std::to_string(n), static_cast<std::uint16_t>(7100 + n)));
  auto hc = checker(config, prober);
  ASSERT_TRUE(hc->start());

  std::vector<bool> seen(6, false);
  ASSERT_TRUE(waitFor([&] {
    for (std::uint16_t n = 0; n < 6; ++n) {
      const auto port = static_cast<std::uint16_t>(7100 + n);
      if (prober->openFor(port) > 0) {
        EXPECT_LE(prober->open(), 2) << "never more than max_concurrent_checks probes at once";
        (void)prober->answer(port, true);
        seen[n] = true;
      }
    }
    for (bool s : seen) {
      if (!s) return false;
    }
    return true;
  }));
  EXPECT_LE(prober->maxOpen(), 2);
}

// --- discovery refresh ------------------------------------------------------------------------------

TEST_F(HealthCheckerTest, InstanceRegisteredWhileRunningIsPickedUp) {
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(waitFor([&] { return hc->stats().refreshes >= 2; }));
  EXPECT_EQ(hc->stats().tracked_instances, 0U);

  RawServer late(RawServer::Mode::Close);
  reg(backend("svc", "late", late.port()));  // no EdgeFlow restart involved
  ASSERT_TRUE(eventuallyHealth("svc", "late", HealthStatus::Healthy));
  EXPECT_EQ(hc->stats().tracked_instances, 1U);
}

TEST_F(HealthCheckerTest, DeregisteredInstanceStopsBeingChecked) {
  RawServer server(RawServer::Mode::Close);
  reg(backend("svc", "a", server.port()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));

  ASSERT_TRUE(registry->deregisterInstance("svc", "a").ok());
  ASSERT_TRUE(waitFor([&] { return hc->stats().tracked_instances == 0; }));
  const int connections = server.connections();
  std::this_thread::sleep_for(200ms);  // several probe intervals: nothing must arrive
  EXPECT_EQ(server.connections(), connections) << "a deregistered instance is no longer probed";
  EXPECT_TRUE(log.contains("no longer checking instance svc/a"));
}

TEST_F(HealthCheckerTest, DisabledInstanceIsNotCheckedAndNeverRoutable) {
  RawServer server(RawServer::Mode::Close);
  reg(backend("svc", "a", server.port()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  ASSERT_EQ(routableCount("svc"), 1U);

  InstanceUpdate disable;
  disable.status = InstanceStatus::Disabled;
  ASSERT_TRUE(registry->updateInstance("svc", "a", disable).ok());
  ASSERT_TRUE(waitFor([&] { return hc->stats().tracked_instances == 0; }));
  EXPECT_EQ(routableCount("svc"), 0U) << "healthy but disabled: not routable";
  const int connections = server.connections();
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(server.connections(), connections) << "a disabled instance is not probed";

  InstanceUpdate enable;
  enable.status = InstanceStatus::Active;
  ASSERT_TRUE(registry->updateInstance("svc", "a", enable).ok());
  ASSERT_TRUE(waitFor([&] { return server.connections() > connections; })) << "checking resumes";
  EXPECT_EQ(routableCount("svc"), 1U);
}

TEST_F(HealthCheckerTest, DrainingInstanceIsCheckedButNotRoutable) {
  RawServer server(RawServer::Mode::Close);
  auto draining = backend("svc", "d", server.port());
  draining.status = InstanceStatus::Draining;
  reg(draining);
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "d", HealthStatus::Healthy)) << "draining instances are still checked";
  EXPECT_EQ(routableCount("svc"), 0U) << "but a healthy probe does not make a draining instance routable";
}

TEST_F(ThresholdTest, ReRegisteredInstanceStartsOverAndOldResultsAreDiscarded) {
  reg(backend("svc", "a", 7201));
  auto hc = checker(fastConfig(), prober);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(waitFor([&] { return prober->openFor(7201) > 0; }));  // probe of the old incarnation is in flight

  ASSERT_TRUE(registry->deregisterInstance("svc", "a").ok());
  reg(backend("svc", "a", 7202));  // same id, new address and incarnation
  ASSERT_TRUE(waitFor([&] { return prober->openFor(7202) > 0; })) << "the new incarnation is probed";
  EXPECT_EQ(prober->openFor(7201), 0) << "the old incarnation's probe was cancelled";
  EXPECT_FALSE(prober->answer(7201, true)) << "its result can no longer be delivered";
  EXPECT_EQ(health("svc", "a"), HealthStatus::Unknown) << "nothing was written for the new incarnation yet";

  ASSERT_TRUE(prober->answer(7202, true));
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
}

TEST_F(ThresholdTest, ADeregisteredInstanceIsNeverResurrectedByALateResult) {
  reg(backend("svc", "a", 7301));
  auto hc = checker(fastConfig(), prober);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(waitFor([&] { return prober->openFor(7301) > 0; }));
  ASSERT_TRUE(registry->deregisterInstance("svc", "a").ok());
  (void)prober->answer(7301, true);  // may or may not still be deliverable; must be harmless
  ASSERT_TRUE(waitFor([&] { return hc->stats().tracked_instances == 0; }));
  EXPECT_FALSE(registry->getInstance("svc", "a").ok());
  EXPECT_EQ(routableCount("svc"), 0U);
}

TEST_F(HealthCheckerTest, ADatabaseOutageKeepsCheckingAndCatchesUpAfterwards) {
  RawServer server(RawServer::Mode::Close);
  reg(backend("svc", "a", server.port()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));

  registry->setUnavailable(true);
  server.stop();  // the instance fails while the registry cannot be reached
  ASSERT_TRUE(waitFor([&] { return hc->stats().persist_failures > 0; }));
  EXPECT_EQ(health("svc", "a"), HealthStatus::Healthy) << "nothing could be written, nothing was faked";
  EXPECT_EQ(hc->stats().tracked_instances, 1U) << "a failed refresh keeps the known instances";
  EXPECT_GT(hc->stats().refresh_failures, 0U);

  registry->setUnavailable(false);
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Unhealthy)) << "the pending change is written once the registry is back";
  EXPECT_TRUE(log.contains("discovery refresh recovered"));
}

TEST_F(HealthCheckerTest, ManualHealthValuesAreCorrectedByTheChecker) {
  RawServer server(RawServer::Mode::Close);
  reg(backend("svc", "a", server.port()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  const int writes = registry->healthUpdates();

  InstanceUpdate lie;
  lie.health = HealthStatus::Unhealthy;  // someone writes a wrong value by hand
  ASSERT_TRUE(registry->updateInstance("svc", "a", lie).ok());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy)) << "the probe result wins";
  EXPECT_GT(registry->healthUpdates(), writes);
}

// --- concurrency ---------------------------------------------------------------------------------

TEST_F(HealthCheckerTest, RegistrationChurnWhileCheckingStaysConsistent) {
  auto hc = checker();
  ASSERT_TRUE(hc->start());

  constexpr int kWorkers = 4;
  constexpr int kRounds = 25;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWorkers; ++w) {
    threads.emplace_back([&, w] {
      for (int round = 0; round < kRounds; ++round) {
        const std::string id = "w" + std::to_string(w) + "-" + std::to_string(round);
        // Nothing listens on these ports, so every instance is verdict "unhealthy". Every
        // loopback address is local, which keeps the host:port pairs distinct.
        auto instance = backend("churn", id, static_cast<std::uint16_t>(8000 + w * 100 + round));
        instance.host = "127.0." + std::to_string(w + 1) + "." + std::to_string(round + 1);
        if (!registry->registerInstance(instance).ok()) ++errors;
        if (round % 2 == 0 && !registry->deregisterInstance("churn", id).ok()) ++errors;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(errors.load(), 0);

  // Every remaining instance gets a verdict; deregistered ones are gone from tracking.
  ASSERT_TRUE(waitFor([&] {
    const auto all = registry->listInstances();
    if (!all.ok()) return false;
    for (const auto& i : all.value()) {
      if (i.health == HealthStatus::Unknown) return false;
    }
    return hc->stats().tracked_instances == all.value().size();
  })) << "every registered instance was checked and none is tracked after deregistration";
}

// --- shutdown ---------------------------------------------------------------------------------------

TEST_F(HealthCheckerTest, StopIsFastEvenWhileAnHttpProbeIsStalled) {
  RawServer stalled(RawServer::Mode::Stall);
  reg(backend("web", "slow", stalled.port()));
  auto config = fastConfig(config::HealthCheckType::Http);
  config.timeout = 30s;  // far longer than the test: only cancellation can end the probe
  config.interval = 30s;
  auto hc = checker(config);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(waitFor([&] { return stalled.requests() > 0; })) << "the probe is in flight";

  const auto begin = std::chrono::steady_clock::now();
  hc->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
  EXPECT_TRUE(log.contains("health checker stopped"));
}

TEST_F(HealthCheckerTest, StopDuringATcpProbeAndDuringTimerWaits) {
  RawServer a(RawServer::Mode::Close);
  reg(backend("svc", "a", a.port()));
  auto config = fastConfig();
  config.interval = 60s;  // the checker spends its time waiting on timers
  config.timeout = 5s;
  config.refresh_interval = 60s;
  auto hc = checker(config);
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  const auto begin = std::chrono::steady_clock::now();
  hc->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
}

TEST_F(HealthCheckerTest, NothingRunsAfterStop) {
  RawServer a(RawServer::Mode::Close);
  reg(backend("svc", "a", a.port()));
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  hc->stop();
  const int connections = a.connections();
  const int writes = registry->healthUpdates();
  const auto probes = hc->stats().probes_started;
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(a.connections(), connections);
  EXPECT_EQ(registry->healthUpdates(), writes);
  EXPECT_EQ(hc->stats().probes_started, probes);
}

TEST_F(HealthCheckerTest, LifecycleRulesAreEnforced) {
  auto never_started = checker();
  never_started->stop();  // harmless
  EXPECT_FALSE(never_started->start()) << "a stopped checker cannot be restarted";

  auto hc = checker();
  ASSERT_TRUE(hc->start());
  EXPECT_FALSE(hc->start()) << "not started twice";
  hc->stop();
  hc->stop();  // idempotent
}

TEST_F(HealthCheckerTest, DestructorStopsARunningChecker) {
  RawServer a(RawServer::Mode::Close);
  reg(backend("svc", "a", a.port()));
  {
    auto hc = checker();
    ASSERT_TRUE(hc->start());
    ASSERT_TRUE(eventuallyHealth("svc", "a", HealthStatus::Healthy));
  }  // destroyed while running
  SUCCEED();
}

TEST_F(HealthCheckerTest, ConcurrentStopCallsAreSafe) {
  auto hc = checker();
  ASSERT_TRUE(hc->start());
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { hc->stop(); });
  for (auto& t : threads) t.join();
  EXPECT_TRUE(log.contains("health checker stopped"));
}

}  // namespace
