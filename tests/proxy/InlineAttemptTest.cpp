#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using config::ConnectionCounting;
using Action = ScriptedBackend::Action;
namespace http = boost::beast::http;

// With proxy.connection_counting that writes nothing and routing.snapshot_ttl_ms > 0 a proxied request makes no
// blocking registry call, so its attempts run on the calling threads instead of the registry call pool (Phase
// 10). These tests check that this path serves, retries, fails over, expires and recovers like the pool path.
class InlineAttemptTest : public ProxyFixture {
 protected:
  void SetUp() override {
    proxy_config.connection_counting = ConnectionCounting::Never;
    routing_snapshot_ttl = 10s;
  }
  static Action reply(unsigned code, std::string body = "ok") {
    Action action;
    action.code = code;
    action.body = std::move(body);
    return action;
  }
  std::shared_ptr<FlakyRegistry> flaky() { return registry; }
};

TEST_F(InlineAttemptTest, ManySequentialRequestsReadTheRegistryOnce) {
  ScriptedBackend backend;
  addInstance("orders", "a", backend.port());
  startGateway();
  auto c = client();
  for (int i = 0; i < 30; ++i) ASSERT_EQ(c->get("/proxy/orders/x" + std::to_string(i))->result(), http::status::ok);
  EXPECT_EQ(registry->routable_lookups.load(), 1U) << "29 of the 30 routes came from the kept snapshot";
  EXPECT_EQ(router->stats().snapshot_hits, 29U);
  EXPECT_EQ(connectionCount("orders", "a"), 0U) << "and no count was written";
  EXPECT_EQ(proxy->stats().responses, 30U);
}

TEST_F(InlineAttemptTest, AnExpiredSnapshotFallsBackToTheRegistryPoolAndKeepsServing) {
  routing_snapshot_ttl = 150ms;
  ScriptedBackend backend;
  addInstance("orders", "a", backend.port());
  startGateway();
  auto c = client();
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 5; ++i) ASSERT_EQ(c->get("/proxy/orders/x")->result(), http::status::ok);
    std::this_thread::sleep_for(180ms);
  }
  EXPECT_GE(registry->routable_lookups.load(), 4U) << "one read per expiry";
  EXPECT_LE(registry->routable_lookups.load(), 8U);
  EXPECT_EQ(proxy->stats().gateway_errors, 0U);
}

TEST_F(InlineAttemptTest, RoundRobinStillRotatesOverTheKeptSnapshot) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend c(nullptr, "c");
  addInstance("orders", "a", a.port());
  addInstance("orders", "b", b.port());
  addInstance("orders", "c", c.port());
  startGateway();
  auto client_ = client();
  for (int i = 0; i < 30; ++i) ASSERT_EQ(client_->get("/proxy/orders/x")->result(), http::status::ok);
  EXPECT_EQ(a.requests(), 10U);
  EXPECT_EQ(b.requests(), 10U);
  EXPECT_EQ(c.requests(), 10U);
}

TEST_F(InlineAttemptTest, RetryAndFailoverWorkOnTheInlinePath) {
  reliability_config = testReliabilityConfig();
  ScriptedBackend bad([](const auto&, auto, auto) { return reply(503, "down"); }, "bad");
  ScriptedBackend good([](const auto&, auto, auto) { return reply(200, "fine"); }, "good");
  addInstance("orders", "bad", bad.port());
  addInstance("orders", "good", good.port());
  startGateway();
  auto c = client();
  for (int i = 0; i < 12; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok) << "the failing backend's answer is retried on the other one";
    EXPECT_EQ(response->body(), "fine");
  }
  EXPECT_GE(reliability->stats().retries, 3U);
  EXPECT_EQ(reliability->stats().failovers, reliability->stats().retries);
  EXPECT_GE(reliability->stats().circuit_opened, 1U);
  EXPECT_EQ(registry->routable_lookups.load(), 1U) << "retries and circuit decisions used the same kept snapshot";
}

TEST_F(InlineAttemptTest, ASingleFailingBackendIsRetriedUpToTheLimitThenTheLastAnswerIsReturned) {
  reliability_config = testReliabilityConfig();
  ScriptedBackend bad([](const auto&, auto, auto) { return reply(503, "down"); }, "bad");
  addInstance("orders", "a", bad.port());
  startGateway();
  const auto response = client()->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(bad.requests(), 3U) << "max_attempts 3: the backoff timer re-enters the inline path twice";
}

TEST_F(InlineAttemptTest, AnOutageAfterTheSnapshotExpiredIsAnswered503AndRecoversAtOnce) {
  routing_snapshot_ttl = 150ms;
  ScriptedBackend backend;
  addInstance("orders", "a", backend.port());
  startGateway();
  auto c = client();
  ASSERT_EQ(c->get("/proxy/orders/x")->result(), http::status::ok);
  registry->routable_unavailable.store(true);
  EXPECT_EQ(c->get("/proxy/orders/x")->result(), http::status::ok) << "inside the interval the snapshot still serves";
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(c->get("/proxy/orders/x")->result(), http::status::service_unavailable)
      << "an expired snapshot is not served while the registry is down";
  registry->routable_unavailable.store(false);
  EXPECT_EQ(c->get("/proxy/orders/x")->result(), http::status::ok);
}

TEST_F(InlineAttemptTest, UnknownAndUnroutableServicesAreAnsweredCorrectly) {
  ScriptedBackend backend;
  addInstance("orders", "a", backend.port());
  addInstance("sick", "s", backend.port(), discovery::HealthStatus::Unhealthy);
  startGateway();
  auto c = client();
  EXPECT_EQ(c->get("/proxy/nosuch/x")->result(), http::status::not_found);
  EXPECT_EQ(c->get("/proxy/sick/x")->result(), http::status::service_unavailable);
  EXPECT_EQ(c->get("/proxy/orders/x")->result(), http::status::ok);
}

TEST_F(InlineAttemptTest, ConcurrentClientsAreAllAnsweredAndNothingIsCounted) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  addInstance("orders", "a", a.port());
  addInstance("orders", "b", b.port());
  startGateway();
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      auto c = client();
      for (int i = 0; i < 100; ++i) {
        const auto response = c->get("/proxy/orders/x");
        if (!response || response->result() != http::status::ok) bad.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_EQ(a.requests() + b.requests(), 800U);
  // Clients that arrive together on a cold snapshot may each read the registry once (no single-flight), so the bound
  // is the number of concurrent clients, not 1; without the snapshot it would be 800.
  EXPECT_LE(registry->routable_lookups.load(), 8U);
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

TEST_F(InlineAttemptTest, ShutdownWithRequestsInFlightFinishesCleanly) {
  auto gate = std::make_shared<Gate>();
  ScriptedBackend backend([&](const auto&, auto, auto) {
    Action action = reply(200, "held");
    action.gate = gate;
    return action;
  });
  addInstance("orders", "a", backend.port());
  startGateway();
  std::vector<std::thread> clients;
  std::atomic<int> answered{0};
  for (int i = 0; i < 4; ++i) {
    clients.emplace_back([&] {
      if (client()->get("/proxy/orders/x")) answered.fetch_add(1);
    });
  }
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 4; }));
  gate->open();
  stopGateway(2000ms);
  for (auto& thread : clients) thread.join();
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

}  // namespace
