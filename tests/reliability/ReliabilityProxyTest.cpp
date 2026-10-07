#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using edgeflow::config::RoutingStrategy;
using edgeflow::discovery::HealthStatus;
using edgeflow::discovery::InstanceStatus;
using edgeflow::reliability::CircuitState;
namespace http = boost::beast::http;

std::string whoAnswered(const network::HttpResponse& response) {
  return response.body().substr(0, response.body().find('|'));
}

// The real server, registry API and reverse proxy with the Phase 7 reliability layer on,
// over scripted real TCP backends.
class ReliabilityTest : public ProxyFixture {
 protected:
  void SetUp() override { reliability_config = testReliabilityConfig(); }

  static Action reply(unsigned code, std::string body = {}) {
    Action action;
    action.code = code;
    action.body = std::move(body);
    return action;
  }
  static Action ofKind(Kind kind) {
    Action action;
    action.kind = kind;
    return action;
  }
  static Action slow(unsigned code, std::chrono::milliseconds delay, std::string body = "slow") {
    Action action = reply(code, std::move(body));
    action.delay = delay;
    return action;
  }

  CircuitState circuitOf(const std::string& service, const std::string& id, std::uint16_t port) {
    const auto breaker = reliability->breakers()->find(
        reliability::backendKey(service, id, "127.0.0.1", port));
    return breaker ? breaker->state() : CircuitState::Closed;
  }

  // A service with one instance per backend (ids "a", "b", "c", ...), gateway started.
  void serve(const std::string& service, const std::vector<ScriptedBackend*>& backends,
             RoutingStrategy strategy = RoutingStrategy::RoundRobin) {
    for (std::size_t i = 0; i < backends.size(); ++i) {
      addInstance(service, std::string(1, static_cast<char>('a' + i)), backends[i]->port());
    }
    startGateway(strategy);
  }
};

// =====================================================================================
// Retry and failover
// =====================================================================================

TEST_F(ReliabilityTest, ConnectionRefusedFailsOverToTheNextBackend) {
  ScriptedBackend good(nullptr, "b");
  addInstance("orders", "a", closedPort());
  addInstance("orders", "b", good.port());
  startGateway();

  auto c = client();
  const auto response = c->get("/proxy/orders/x?q=1");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(whoAnswered(*response), "b");
  EXPECT_EQ(good.requests(), 1U);
  EXPECT_EQ(std::string{good.last().target()}, "/x?q=1");

  const auto stats = reliability->stats();
  EXPECT_EQ(stats.attempts, 2U);
  EXPECT_EQ(stats.retries, 1U);
  EXPECT_EQ(stats.failovers, 1U) << "the retry went to another backend";
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b"}));
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

TEST_F(ReliabilityTest, ARetryableStatusFailsOverAndTheFailedAnswerIsNotShown) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "a is overloaded"));
  serve("orders", {&a, &b});

  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(whoAnswered(*response), "b");
  EXPECT_EQ(a.requests(), 1U);
  EXPECT_EQ(b.requests(), 1U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b"}));
}

TEST_F(ReliabilityTest, EveryConfiguredRetryableStatusIsRetriedAndOthersAreForwarded) {
  reliability_config.retry.retryable_statuses = {500, 502, 503, 504, 429};
  reliability_config.circuit_breaker.enabled = false;  // this test is about the status list
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  serve("orders", {&a, &b});

  auto c = client();
  std::uint64_t expected_a = 0;
  for (const unsigned status : {500U, 502U, 503U, 504U, 429U}) {
    SCOPED_TRACE(status);
    a.always(reply(status, "failing"));
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok) << "retried on the healthy backend";
    EXPECT_EQ(whoAnswered(*response), "b");
    ++expected_a;  // after a failover b was the last choice, so every request starts at a
    EXPECT_EQ(a.requests(), expected_a);
  }
  EXPECT_EQ(reliability->stats().retries, 5U);

  // A status that is not configured is the application's answer: forwarded, never retried.
  a.always(reply(418, "teapot"));
  b.always(reply(418, "teapot"));
  const auto before = a.requests() + b.requests();
  const auto teapot = c->get("/proxy/orders/x");
  ASSERT_TRUE(teapot);
  EXPECT_EQ(teapot->result_int(), 418U) << "forwarded as it is";
  EXPECT_EQ(a.requests() + b.requests(), before + 1) << "exactly one attempt";
}

TEST_F(ReliabilityTest, ApplicationStatusesAreForwardedWithoutRetry) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  serve("orders", {&a, &b});
  auto c = client();
  for (const unsigned status : {400U, 401U, 403U, 404U, 409U, 500U, 501U}) {
    SCOPED_TRACE(status);
    a.always(reply(status, "mine"));
    b.always(reply(status, "mine"));
    const auto before = a.requests() + b.requests();
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), static_cast<http::status>(status));
    EXPECT_EQ(response->body(), "mine");
    EXPECT_EQ(a.requests() + b.requests(), before + 1) << "no retry";
  }
  EXPECT_EQ(reliability->stats().retries, 0U);
}

TEST_F(ReliabilityTest, TheRetryBudgetIsExhaustedWithTheLastBackendAnswerForwarded) {
  reliability_config.retry.max_attempts = 2;
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend c3(nullptr, "c");
  a.always(reply(503, "no from a"));
  b.always(reply(503, "no from b"));
  c3.always(reply(503, "no from c"));
  serve("orders", {&a, &b, &c3});

  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable) << "the backend's own answer";
  EXPECT_EQ(a.requests() + b.requests() + c3.requests(), 2U) << "max_attempts = 2 in total";
  EXPECT_EQ(response->body().substr(0, 6), "no fro");
  EXPECT_EQ(reliability->stats().exhausted, 1U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b", "c"}));
}

TEST_F(ReliabilityTest, ASingleBackendIsRetriedUpToTheLimit) {
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});

  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(a.requests(), 3U) << "three attempts, then the answer";
  EXPECT_EQ(reliability->stats().failovers, 0U);
}

TEST_F(ReliabilityTest, ARecoveringBackendIsRetriedSuccessfully) {
  ScriptedBackend a(nullptr, "a");
  std::atomic<int> seen{0};
  a.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    return ++seen < 3 ? reply(503, "warming up") : reply(200, "a|ok|");
  });
  serve("orders", {&a});

  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(a.requests(), 3U);
}

TEST_F(ReliabilityTest, RetriesAreSwitchedOffByConfiguration) {
  reliability_config.retry.enabled = false;
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "down"));
  serve("orders", {&a, &b});

  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable) << "one attempt, no failover";
  EXPECT_EQ(a.requests(), 1U);
  EXPECT_EQ(b.requests(), 0U);
  EXPECT_EQ(reliability->stats().retries, 0U);
}

TEST_F(ReliabilityTest, ADirectProxyFailureAfterAllAttemptsIs502) {
  const auto first_dead = closedPort();
  auto second_dead = closedPort();
  while (second_dead == first_dead) second_dead = closedPort();
  addInstance("orders", "a", first_dead);
  addInstance("orders", "b", second_dead);
  startGateway();
  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_gateway);
  EXPECT_EQ(reliability->stats().attempts, 3U) << "a, b, then a again (no untried backend is left)";
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b"}));
}

// =====================================================================================
// Request safety
// =====================================================================================

TEST_F(ReliabilityTest, APostAnsweredWithARetryableStatusIsNeverRetried) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "processed, then failed"));
  serve("orders", {&a, &b});

  auto c = client();
  const auto response = c->request(http::verb::post, "/proxy/orders/charge", "amount=10");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable) << "the backend's answer, forwarded";
  EXPECT_EQ(a.requests(), 1U);
  EXPECT_EQ(b.requests(), 0U) << "the backend may have charged already";
}

TEST_F(ReliabilityTest, APostThatNeverReachedABackendIsRetriedWithItsBody) {
  ScriptedBackend good(nullptr, "b");
  addInstance("orders", "a", closedPort());
  addInstance("orders", "b", good.port());
  startGateway();

  auto c = client();
  const auto response = c->request(http::verb::post, "/proxy/orders/charge", "amount=10");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(response->body(), "b|POST /charge|amount=10") << "the body went out again, intact";
  EXPECT_EQ(good.requests(), 1U);
}

TEST_F(ReliabilityTest, APostWhoseConnectionDiedAfterSendingIsNotRetried) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(ofKind(Kind::Close));
  serve("orders", {&a, &b});

  auto c = client();
  const auto response = c->request(http::verb::post, "/proxy/orders/charge", "amount=10");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_gateway);
  EXPECT_EQ(a.requests(), 1U);
  EXPECT_EQ(b.requests(), 0U);
}

TEST_F(ReliabilityTest, AGetWhoseConnectionDiedIsRetriedOnAnotherBackend) {
  for (const Kind kind : {Kind::Close, Kind::Reset, Kind::Truncated, Kind::Garbage}) {
    SCOPED_TRACE(static_cast<int>(kind));
    ScriptedBackend a(nullptr, "a");
    ScriptedBackend b(nullptr, "b");
    a.always(ofKind(kind));
    registry = std::make_shared<FlakyRegistry>();
    serve("orders", {&a, &b});

    auto c = client();
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    EXPECT_EQ(whoAnswered(*response), "b");
    stopGateway();
    server.reset();
    proxy.reset();
  }
}

TEST_F(ReliabilityTest, TheMethodListCanAllowPost) {
  reliability_config.retry.retryable_methods = {"GET", "POST"};
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "down"));
  serve("orders", {&a, &b});
  auto c = client();
  const auto response = c->request(http::verb::post, "/proxy/orders/x", "payload");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(whoAnswered(*response), "b");
}

TEST_F(ReliabilityTest, ATimedOutPostIsNotRetriedButATimedOutGetIs) {
  proxy_config.upstream_timeout = 250ms;
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(ofKind(Kind::Stall));
  serve("orders", {&a, &b});

  auto c = client();
  const auto post = c->request(http::verb::post, "/proxy/orders/x", "p");
  ASSERT_TRUE(post);
  EXPECT_EQ(post->result(), http::status::gateway_timeout);
  EXPECT_EQ(b.requests(), 0U);

  // The next round-robin choice is b; send a GET that starts at a: use a fresh client and
  // keep sending until a was tried (round robin alternates).
  bool a_was_tried = false;
  for (int i = 0; i < 4 && !a_was_tried; ++i) {
    const auto before = a.requests();
    const auto get = c->get("/proxy/orders/x");
    ASSERT_TRUE(get);
    EXPECT_EQ(get->result(), http::status::ok) << "a stalled, b answered";
    a_was_tried = a.requests() > before;
  }
  EXPECT_TRUE(a_was_tried);
}

TEST_F(ReliabilityTest, AllAttemptsTimingOutIs504) {
  proxy_config.upstream_timeout = 150ms;
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(ofKind(Kind::Stall));
  b.always(ofKind(Kind::Stall));
  serve("orders", {&a, &b});

  auto c = client();
  const auto start = std::chrono::steady_clock::now();
  const auto response = c->get("/proxy/orders/x");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::gateway_timeout);
  EXPECT_EQ(a.requests() + b.requests(), 3U) << "three attempts";
  EXPECT_LT(elapsed, 3s);
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b"}));
}

// =====================================================================================
// Timeouts and backoff
// =====================================================================================

TEST_F(ReliabilityTest, BackoffGrowsExponentiallyBetweenAttempts) {
  reliability_config.retry.base_delay = 60ms;
  reliability_config.retry.max_delay = 1000ms;
  addInstance("orders", "a", closedPort());
  startGateway();

  auto c = client();
  const auto start = std::chrono::steady_clock::now();
  const auto response = c->get("/proxy/orders/x");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_gateway);
  // 3 attempts = pauses of 60 ms and 120 ms.
  EXPECT_GE(elapsed, 175ms);
  EXPECT_LT(elapsed, 3s);
  EXPECT_EQ(reliability->stats().attempts, 3U);
}

TEST_F(ReliabilityTest, BackoffIsCapped) {
  reliability_config.retry.max_attempts = 5;
  reliability_config.retry.base_delay = 40ms;
  reliability_config.retry.max_delay = 50ms;
  addInstance("orders", "a", closedPort());
  startGateway();

  auto c = client();
  const auto start = std::chrono::steady_clock::now();
  const auto response = c->get("/proxy/orders/x");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(response);
  // pauses 40 + 50 + 50 + 50 = 190 ms; uncapped it would be 40+80+160+320 = 600 ms.
  EXPECT_GE(elapsed, 180ms);
  EXPECT_LT(elapsed, 550ms);
}

TEST_F(ReliabilityTest, TheTotalBudgetBoundsASlowAttempt) {
  reliability_config.timeout.total_timeout = 300ms;
  proxy_config.upstream_timeout = 5000ms;
  ScriptedBackend a(nullptr, "a");
  a.always(slow(200, 3000ms));
  serve("orders", {&a});

  auto c = client();
  const auto start = std::chrono::steady_clock::now();
  const auto response = c->get("/proxy/orders/x");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::gateway_timeout);
  EXPECT_LT(elapsed, 1500ms) << "the attempt was cut to the remaining budget, not 5 s";
  EXPECT_EQ(a.requests(), 1U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(ReliabilityTest, TheTotalBudgetStopsTheRetriesNoMatterTheAttemptLimit) {
  reliability_config.retry.max_attempts = 10;
  reliability_config.retry.base_delay = 200ms;
  reliability_config.retry.max_delay = 200ms;
  reliability_config.timeout.total_timeout = 500ms;
  addInstance("orders", "a", closedPort());
  startGateway();

  auto c = client();
  const auto start = std::chrono::steady_clock::now();
  const auto response = c->get("/proxy/orders/x");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_gateway) << "the last real failure";
  EXPECT_LT(elapsed, 1500ms);
  EXPECT_LE(reliability->stats().attempts, 4U) << "not ten attempts";
  EXPECT_GE(reliability->stats().attempts, 2U);
}

TEST_F(ReliabilityTest, ARequestCancelledDuringBackoffStopsAtOnce) {
  reliability_config.retry.base_delay = 5000ms;
  reliability_config.retry.max_delay = 5000ms;
  reliability_config.timeout.total_timeout = 60000ms;
  addInstance("orders", "a", closedPort());
  startGateway();

  network::HttpRequest request{http::verb::get, "/proxy/orders/x", 11};
  request.set(http::field::host, "localhost");
  std::promise<network::HttpResponse> promise;
  auto future = promise.get_future();
  const auto cancel = proxy->handleAsync(request, network::RequestContext{"127.0.0.1", 1},
                                         [&](network::HttpResponse r) { promise.set_value(std::move(r)); });
  ASSERT_TRUE(waitFor([&] { return reliability->stats().attempts >= 1; }));
  std::this_thread::sleep_for(150ms);  // now backing off for 5 s
  EXPECT_EQ(future.wait_for(0ms), std::future_status::timeout);

  const auto start = std::chrono::steady_clock::now();
  cancel();
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready) << "no waiting for the 5 s timer";
  EXPECT_LT(std::chrono::steady_clock::now() - start, 800ms);
  EXPECT_EQ(future.get().result(), http::status::service_unavailable);
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
  EXPECT_EQ(reliability->stats().attempts, 1U) << "no second attempt after the cancellation";
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(ReliabilityTest, ShuttingDownDuringBackoffDoesNotWaitForTheTimer) {
  reliability_config.retry.base_delay = 5000ms;
  reliability_config.retry.max_delay = 5000ms;
  reliability_config.timeout.total_timeout = 60000ms;
  addInstance("orders", "a", closedPort());
  startGateway();

  std::thread requester([&] {
    auto c = client();
    (void)c->get("/proxy/orders/x");
  });
  ASSERT_TRUE(waitFor([&] { return reliability->stats().attempts >= 1; }));
  std::this_thread::sleep_for(150ms);

  const auto start = std::chrono::steady_clock::now();
  stopGateway(300ms);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
  requester.join();
  EXPECT_EQ(proxy->stats().active, 0U);
  EXPECT_EQ(connectionCount("orders", "a"), 0U);
}

TEST_F(ReliabilityTest, ShuttingDownWithRetriesInFlightLeaksNothing) {
  reliability_config.retry.base_delay = 100ms;
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(slow(503, 150ms));
  b.always(slow(503, 150ms));
  serve("orders", {&a, &b});

  std::vector<std::thread> clients;
  for (int i = 0; i < 6; ++i) {
    clients.emplace_back([&] {
      auto c = client();
      (void)c->get("/proxy/orders/x");
    });
  }
  std::this_thread::sleep_for(200ms);
  stopGateway(200ms);
  for (auto& t : clients) t.join();
  EXPECT_EQ(proxy->stats().active, 0U);
  EXPECT_EQ(connectionCount("orders", "a"), 0U);
  EXPECT_EQ(connectionCount("orders", "b"), 0U);
}

// =====================================================================================
// Request id and connection counts
// =====================================================================================

TEST_F(ReliabilityTest, TheSameRequestIdTravelsWithEveryAttempt) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "down"));
  serve("orders", {&a, &b});

  auto c = client();
  const auto supplied = c->request(http::verb::get, "/proxy/orders/x", {}, {{"X-Request-Id", "client-id-77"}});
  ASSERT_TRUE(supplied);
  EXPECT_EQ(supplied->result(), http::status::ok);
  EXPECT_EQ(header(a.last(), "X-Request-Id"), "client-id-77");
  EXPECT_EQ(header(b.last(), "X-Request-Id"), "client-id-77");
  EXPECT_EQ(header(*supplied, "X-Request-Id"), "client-id-77");

  // Generated: one id for the whole request, not one per attempt. Round robin starts at b
  // for this request, so let `b` fail instead.
  a.always(reply(200, "a|ok|"));
  b.always(reply(503, "down"));
  std::string id;
  for (int i = 0; i < 4; ++i) {
    const auto before = b.requests();
    const auto response = c->get("/proxy/orders/y");
    ASSERT_TRUE(response);
    if (b.requests() > before) {
      id = header(*response, "X-Request-Id");
      EXPECT_NE(id, "<absent>");
      EXPECT_EQ(header(b.last(), "X-Request-Id"), id) << "the failed attempt carried the id";
      EXPECT_EQ(header(a.last(), "X-Request-Id"), id) << "and so did the retry";
      break;
    }
  }
  EXPECT_FALSE(id.empty()) << "a request that went to the failing backend first was observed";
}

TEST_F(ReliabilityTest, ConnectionCountsFollowEachAttemptAndEndAtZero) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  auto gate = std::make_shared<Gate>();
  Action held = reply(503, "down");
  held.gate = gate;
  a.always(held);
  b.always(slow(200, 400ms, "b|ok|"));
  serve("orders", {&a, &b});

  std::thread requester([&] {
    auto c = client();
    (void)c->get("/proxy/orders/x");
  });
  // First attempt is parked at a: a counts 1, b counts 0.
  ASSERT_TRUE(waitFor([&] { return connectionCount("orders", "a") == 1; }));
  EXPECT_EQ(connectionCount("orders", "b"), 0U);
  gate->open();
  // a answered 503: its count is released before the retry to b starts.
  ASSERT_TRUE(waitFor([&] { return connectionCount("orders", "b") == 1; }));
  EXPECT_EQ(connectionCount("orders", "a"), 0U) << "released before the next attempt";
  requester.join();
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b"}));
}

TEST_F(ReliabilityTest, ThePhase6StaleConnectionSafeguardIsNotARetry) {
  ScriptedBackend a(nullptr, "a");
  Action closing = reply(200, "a|ok|");
  closing.close_after = true;  // the backend closes the keep-alive connection after answering
  a.always(closing);
  serve("orders", {&a});

  auto c = client();
  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_EQ(reliability->stats().attempts, 3U);
  EXPECT_EQ(reliability->stats().retries, 0U) << "replacing a dead pooled connection is not a retry";
}

// =====================================================================================
// Circuit breaker
// =====================================================================================

TEST_F(ReliabilityTest, TheCircuitOpensFailsFastAndRecoversThroughHalfOpen) {
  reliability_config.retry.max_attempts = 1;
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});
  auto c = client();

  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->body(), "down") << "closed: the backend's own answer";
  }
  EXPECT_EQ(a.requests(), 3U);
  EXPECT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);

  // OPEN: fail fast, the backend is not contacted.
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::service_unavailable);
    EXPECT_NE(response->body().find("no eligible instance"), std::string::npos) << response->body();
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
  EXPECT_EQ(a.requests(), 3U) << "an open circuit sends nothing";

  // The backend recovers; after the recovery timeout one probe goes through and closes it.
  a.always(reply(200, "a|ok|"));
  std::this_thread::sleep_for(350ms);
  const auto probe = c->get("/proxy/orders/x");
  ASSERT_TRUE(probe);
  EXPECT_EQ(probe->result(), http::status::ok);
  EXPECT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Closed);
  EXPECT_EQ(a.requests(), 4U);
  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
  }
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(ReliabilityTest, AFailedHalfOpenProbeReopensTheCircuit) {
  reliability_config.retry.max_attempts = 1;
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});
  auto c = client();
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(c->get("/proxy/orders/x"));
  ASSERT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);

  std::this_thread::sleep_for(350ms);
  const auto probe = c->get("/proxy/orders/x");  // still failing
  ASSERT_TRUE(probe);
  EXPECT_EQ(probe->body(), "down");
  EXPECT_EQ(a.requests(), 4U) << "exactly one probe went out";
  EXPECT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);

  const auto rejected = c->get("/proxy/orders/x");
  ASSERT_TRUE(rejected);
  EXPECT_EQ(rejected->result(), http::status::service_unavailable);
  EXPECT_EQ(a.requests(), 4U) << "open again: nothing is sent";
}

TEST_F(ReliabilityTest, OnlyTheConfiguredNumberOfHalfOpenProbesReachTheBackend) {
  reliability_config.retry.max_attempts = 1;
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});
  auto c = client();
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(c->get("/proxy/orders/x"));
  ASSERT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);

  a.always(slow(200, 300ms, "a|ok|"));
  std::this_thread::sleep_for(350ms);

  constexpr int kClients = 12;
  std::atomic<int> ok{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&] {
      auto client_connection = client();
      const auto response = client_connection->get("/proxy/orders/x");
      if (response && response->result() == http::status::ok) ++ok;
      if (response && response->result() == http::status::service_unavailable) ++rejected;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(a.requests(), 4U) << "3 earlier + exactly one probe";
  EXPECT_EQ(ok.load(), 1);
  EXPECT_EQ(rejected.load(), kClients - 1) << "the rest failed fast";
}

TEST_F(ReliabilityTest, ARequestsRepeatedFailuresOnOneBackendCountAsOne) {
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});
  auto c = client();

  ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_EQ(a.requests(), 3U) << "three attempts on the only backend";
  const auto breaker = reliability->breakers()->find(
      reliability::backendKey("orders", "a", "127.0.0.1", a.port()));
  ASSERT_NE(breaker, nullptr);
  EXPECT_EQ(breaker->consecutiveFailures(), 1U) << "one request, one failure";
  EXPECT_EQ(breaker->state(), CircuitState::Closed);

  ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_EQ(breaker->consecutiveFailures(), 2U);
  EXPECT_EQ(a.requests(), 6U);

  // The third request fails once more: the circuit opens and cuts its remaining attempts.
  const auto third = c->get("/proxy/orders/x");
  ASSERT_TRUE(third);
  EXPECT_EQ(third->result(), http::status::service_unavailable);
  EXPECT_EQ(breaker->state(), CircuitState::Open);
  EXPECT_EQ(a.requests(), 7U) << "no further attempts once the circuit is open";
}

TEST_F(ReliabilityTest, ASickBackendIsIsolatedWhileHealthyOnesKeepServing) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend p(nullptr, "p");
  a.always(reply(503, "sick"));
  addInstance("orders", "a", a.port());
  addInstance("orders", "b", b.port());
  addInstance("payment", "a", p.port());  // the same instance id, another service
  startGateway();

  auto c = client();
  for (int i = 0; i < 20; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok) << i << ": the client never saw the sick backend";
    EXPECT_EQ(whoAnswered(*response), "b");
  }
  EXPECT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);
  EXPECT_EQ(a.requests(), 3U) << "only the requests that opened the circuit reached it";

  EXPECT_EQ(circuitOf("orders", "b", b.port()), CircuitState::Closed);
  EXPECT_EQ(circuitOf("payment", "a", p.port()), CircuitState::Closed) << "another service is unaffected";
  const auto payment = c->get("/proxy/payment/x");
  ASSERT_TRUE(payment);
  EXPECT_EQ(payment->result(), http::status::ok);
}

TEST_F(ReliabilityTest, WhenEveryCircuitIsOpenTheGatewayFailsFastWithoutTouchingBackends) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "down"));
  b.always(reply(503, "down"));
  serve("orders", {&a, &b});
  auto c = client();
  for (int i = 0; i < 6 && (circuitOf("orders", "a", a.port()) != CircuitState::Open ||
                            circuitOf("orders", "b", b.port()) != CircuitState::Open);
       ++i) {
    ASSERT_TRUE(c->get("/proxy/orders/x"));
  }
  ASSERT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);
  ASSERT_EQ(circuitOf("orders", "b", b.port()), CircuitState::Open);

  const auto before = a.requests() + b.requests();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::service_unavailable);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s) << "failing fast";
  EXPECT_EQ(a.requests() + b.requests(), before);
  EXPECT_GE(reliability->stats().circuit_opened, 2U);

  const auto health = c->get("/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->result(), http::status::ok) << "the gateway itself stays responsive";
}

TEST_F(ReliabilityTest, OrdinaryClientErrorsNeverTripACircuit) {
  reliability_config.retry.max_attempts = 1;
  ScriptedBackend a(nullptr, "a");
  a.always(reply(404, "not here"));
  serve("orders", {&a});
  auto c = client();
  for (int i = 0; i < 12; ++i) {
    const auto response = c->get("/proxy/orders/missing");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::not_found);
  }
  EXPECT_EQ(a.requests(), 12U);
  EXPECT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Closed);
}

TEST_F(ReliabilityTest, ACircuitBreakerCanBeSwitchedOff) {
  reliability_config.retry.max_attempts = 1;
  reliability_config.circuit_breaker.enabled = false;
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});
  EXPECT_EQ(reliability->breakers(), nullptr);
  auto c = client();
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_EQ(a.requests(), 10U) << "nothing ever opened";
}

TEST_F(ReliabilityTest, CircuitAccountingWorksWithRetriesDisabled) {
  reliability_config.retry.enabled = false;
  ScriptedBackend a(nullptr, "a");
  a.always(reply(503, "down"));
  serve("orders", {&a});
  auto c = client();
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_EQ(a.requests(), 3U);
  EXPECT_EQ(circuitOf("orders", "a", a.port()), CircuitState::Open);
}

TEST_F(ReliabilityTest, ARefusedConnectionCountsAsAFailureOfThatBackend) {
  reliability_config.retry.max_attempts = 1;
  addInstance("orders", "a", closedPort());
  startGateway();
  auto c = client();
  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::bad_gateway);
  }
  const auto breaker = reliability->breakers()->find(reliability::backendKey(
      "orders", "a", "127.0.0.1", registry->inner->getInstance("orders", "a").value().port));
  ASSERT_NE(breaker, nullptr);
  EXPECT_EQ(breaker->state(), CircuitState::Open);
  const auto fast = c->get("/proxy/orders/x");
  ASSERT_TRUE(fast);
  EXPECT_EQ(fast->result(), http::status::service_unavailable);
}

// =====================================================================================
// Failover, routing and discovery
// =====================================================================================

TEST_F(ReliabilityTest, FailoverWorksWithEveryRoutingStrategy) {
  for (const auto strategy : {RoutingStrategy::RoundRobin, RoutingStrategy::LeastConnections,
                              RoutingStrategy::Weighted, RoutingStrategy::ConsistentHashing}) {
    SCOPED_TRACE(static_cast<int>(strategy));
    registry = std::make_shared<FlakyRegistry>();
    ScriptedBackend b(nullptr, "b");
    ScriptedBackend c3(nullptr, "c");
    addInstance("orders", "a", closedPort());
    addInstance("orders", "b", b.port());
    addInstance("orders", "c", c3.port());
    startGateway(strategy);

    auto c = client();
    for (int i = 0; i < 12; ++i) {
      const auto response = c->get("/proxy/orders/x" + std::to_string(i));
      ASSERT_TRUE(response);
      EXPECT_EQ(response->result(), http::status::ok) << i;
      EXPECT_NE(whoAnswered(*response), "a");
    }
    EXPECT_TRUE(countsReturnToZero("orders", {"a", "b", "c"}));
    stopGateway();
    server.reset();
    proxy.reset();
  }
}

TEST_F(ReliabilityTest, ConsistentHashingFailsOverToTheNextBackendOnTheRingAndStaysStable) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend c3(nullptr, "c");
  serve("orders", {&a, &b, &c3}, RoutingStrategy::ConsistentHashing);
  auto c = client();
  const auto first = c->get("/proxy/orders/x");
  ASSERT_TRUE(first);
  const auto primary = whoAnswered(*first);  // this client's usual backend
  ASSERT_FALSE(primary.empty());

  // The primary starts failing: the key moves to another backend, and stays there.
  ScriptedBackend* backends[] = {&a, &b, &c3};
  backends[primary[0] - 'a']->always(reply(503, "down"));
  std::set<std::string> answered;
  for (int i = 0; i < 6; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    answered.insert(whoAnswered(*response));
  }
  EXPECT_EQ(answered.size(), 1U) << "a stable second choice";
  EXPECT_EQ(answered.count(primary), 0U);
}

TEST_F(ReliabilityTest, FailoverNeverSelectsUnhealthyDrainingOrDisabledInstances) {
  ScriptedBackend good(nullptr, "b");
  ScriptedBackend unhealthy(nullptr, "u");
  ScriptedBackend draining(nullptr, "d");
  ScriptedBackend disabled(nullptr, "x");
  addInstance("orders", "a", closedPort());
  addInstance("orders", "b", good.port());
  addInstance("orders", "u", unhealthy.port(), HealthStatus::Unhealthy);
  addInstance("orders", "d", draining.port(), HealthStatus::Healthy, InstanceStatus::Draining);
  addInstance("orders", "x", disabled.port(), HealthStatus::Healthy, InstanceStatus::Disabled);
  startGateway();

  auto c = client();
  for (int i = 0; i < 10; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    EXPECT_EQ(whoAnswered(*response), "b");
  }
  EXPECT_EQ(unhealthy.requests() + draining.requests() + disabled.requests(), 0U);
}

TEST_F(ReliabilityTest, ABackendThatTheHealthCheckerExcludesLeavesFailoverToo) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "down"));
  serve("orders", {&a, &b});
  auto c = client();
  ASSERT_TRUE(c->get("/proxy/orders/x"));
  // The health checker (Phase 4) now marks b unhealthy: the only other candidate is gone.
  discovery::InstanceUpdate update;
  update.health = HealthStatus::Unhealthy;
  ASSERT_TRUE(registry->updateInstance("orders", "b", update).ok());
  const auto before = b.requests();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(b.requests(), before) << "an unhealthy backend is never a failover target";
  EXPECT_EQ(response->result(), http::status::service_unavailable);
}

TEST_F(ReliabilityTest, NoRoutableInstanceOnTheFirstAttemptIsStill503) {
  addInstance("orders", "a", closedPort(), HealthStatus::Unhealthy);
  startGateway();
  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(reliability->stats().attempts, 0U);
}

TEST_F(ReliabilityTest, ARegistryOutageIsStill503AndIsNotRetriedBlindly) {
  ScriptedBackend a(nullptr, "a");
  serve("orders", {&a});
  registry->routable_unavailable = true;
  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(a.requests(), 0U);
  registry->routable_unavailable = false;
  const auto healthy = c->get("/proxy/orders/x");
  ASSERT_TRUE(healthy);
  EXPECT_EQ(healthy->result(), http::status::ok);
}

// =====================================================================================
// Graceful degradation
// =====================================================================================

TEST_F(ReliabilityTest, ADeadBackendFleetDegradesToFastErrorsAndRecovers) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(ofKind(Kind::Close));
  b.always(ofKind(Kind::Close));
  serve("orders", {&a, &b});
  auto c = client();

  const auto first = c->get("/proxy/orders/x");
  ASSERT_TRUE(first);
  EXPECT_EQ(first->result(), http::status::bad_gateway) << "controlled failure, not a hang";

  // Keep asking: the circuits open and the answers become immediate 503s.
  http::status last = http::status::ok;
  for (int i = 0; i < 6; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    last = response->result();
  }
  EXPECT_EQ(last, http::status::service_unavailable);
  const auto contacted = a.requests() + b.requests();
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_EQ(a.requests() + b.requests(), contacted) << "no retry storm against a dead fleet";

  // Recovery: both backends come back; after the recovery timeout traffic flows again.
  a.setHandler([&a](const ScriptedBackend::Request& r, std::uint64_t, std::size_t) { return a.describe(r); });
  b.setHandler([&b](const ScriptedBackend::Request& r, std::uint64_t, std::size_t) { return b.describe(r); });
  std::this_thread::sleep_for(350ms);
  bool recovered = false;
  for (int i = 0; i < 20 && !recovered; ++i) {
    const auto response = c->get("/proxy/orders/x");
    recovered = response && response->result() == http::status::ok;
    if (!recovered) std::this_thread::sleep_for(100ms);
  }
  EXPECT_TRUE(recovered);
  EXPECT_TRUE(countsReturnToZero("orders", {"a", "b"}));
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

TEST_F(ReliabilityTest, SuccessfulRequestsAreUnaffectedByTheLayer) {
  ScriptedBackend a(nullptr, "a");
  serve("orders", {&a});
  auto c = client();
  for (int i = 0; i < 25; ++i) {
    const auto response = c->request(http::verb::post, "/proxy/orders/echo", "body-" + std::to_string(i));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->body(), "a|POST /echo|body-" + std::to_string(i));
  }
  EXPECT_EQ(a.requests(), 25U);
  EXPECT_EQ(reliability->stats().retries, 0U);
  EXPECT_GT(proxy->stats().pool.reused, 0U) << "connections are still reused";
}

TEST_F(ReliabilityTest, WithoutTheReliabilityLayerBehaviourIsPhase6) {
  reliability_config.enabled = false;
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  a.always(reply(503, "down"));
  serve("orders", {&a, &b});
  EXPECT_EQ(proxy->reliability(), nullptr);
  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(a.requests() + b.requests(), 1U) << "one attempt, no failover";
}

// =====================================================================================
// Concurrency
// =====================================================================================

TEST_F(ReliabilityTest, ConcurrentTrafficAroundSickBackendsStaysCorrect) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  ScriptedBackend c3(nullptr, "c");
  a.always(reply(503, "sick"));
  addInstance("shop", "a", a.port());
  addInstance("shop", "b", b.port());
  addInstance("shop", "c", c3.port());
  addInstance("shop", "d", closedPort());
  startGateway(RoutingStrategy::RoundRobin);

  constexpr int kThreads = 8;
  constexpr int kRequests = 25;
  std::atomic<int> good{0};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      auto c = client();
      for (int i = 0; i < kRequests; ++i) {
        const std::string payload = "p" + std::to_string(t) + "-" + std::to_string(i);
        const auto response = c->request(http::verb::get, "/proxy/shop/q?v=" + payload);
        if (response && response->result() == http::status::ok &&
            response->body().find("/q?v=" + payload) != std::string::npos) {
          ++good;
        } else {
          ++bad;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(good.load(), kThreads * kRequests) << "every request ended on a healthy backend";
  EXPECT_EQ(bad.load(), 0);
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
  EXPECT_TRUE(countsReturnToZero("shop", {"a", "b", "c", "d"}));
  EXPECT_EQ(circuitOf("shop", "a", a.port()), CircuitState::Open);
}

TEST_F(ReliabilityTest, RandomlyFailingBackendsNeverLeaveInconsistentState) {
  std::vector<std::unique_ptr<ScriptedBackend>> backends;
  std::vector<std::string> ids;
  std::atomic<unsigned> seed{1};
  for (int i = 0; i < 3; ++i) {
    const std::string id(1, static_cast<char>('a' + i));
    backends.push_back(std::make_unique<ScriptedBackend>(nullptr, id));
    ids.push_back(id);
    addInstance("shop", id, backends.back()->port());
    auto engine = std::make_shared<std::mt19937>(seed++);
    auto mutex = std::make_shared<std::mutex>();
    const std::string name = id;
    backends.back()->setHandler([engine, mutex, name](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
      const std::lock_guard lock(*mutex);
      const auto roll = (*engine)() % 10;
      Action action;
      if (roll < 3) {
        action.code = 503;
        action.body = "x";
      } else if (roll == 3) {
        action.kind = Kind::Close;
      } else {
        action.body = name + "|ok|";
      }
      return action;
    });
  }
  reliability_config.circuit_breaker.recovery_timeout = 50ms;
  startGateway();

  std::atomic<int> ok{0};
  std::atomic<int> degraded{0};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      auto c = client();
      for (int i = 0; i < 40; ++i) {
        const auto response = c->get("/proxy/shop/x");
        if (!response) {
          ++unexpected;
          c = client();
          continue;
        }
        switch (response->result()) {
          case http::status::ok: ++ok; break;
          case http::status::service_unavailable:
          case http::status::bad_gateway: ++degraded; break;
          default: ++unexpected; break;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(unexpected.load(), 0) << "only a real answer or a controlled gateway error";
  EXPECT_GT(ok.load(), 0) << "traffic keeps flowing while circuits open and close";
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
  EXPECT_TRUE(countsReturnToZero("shop", ids)) << "no connection count leaked";
  const auto stats = reliability->stats();
  EXPECT_LE(stats.attempts, static_cast<std::uint64_t>(8 * 40 * 3)) << "never more than max_attempts per request";
  EXPECT_GE(stats.attempts, stats.retries);
  (void)degraded;
}

TEST_F(ReliabilityTest, ConcurrentBackoffsDoNotBlockTheGateway) {
  reliability_config.retry.base_delay = 300ms;
  reliability_config.retry.max_delay = 300ms;
  reliability_config.circuit_breaker.enabled = false;
  addInstance("orders", "a", closedPort());
  startGateway();

  std::vector<std::thread> threads;
  for (int i = 0; i < 12; ++i) {
    threads.emplace_back([&] {
      auto c = client();
      (void)c->get("/proxy/orders/x");
    });
  }
  std::this_thread::sleep_for(150ms);
  // Twelve requests are sleeping in backoff; none of them holds a thread.
  const auto start = std::chrono::steady_clock::now();
  auto c = client();
  const auto health = c->get("/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->result(), http::status::ok);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
  for (auto& t : threads) t.join();
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

}  // namespace
