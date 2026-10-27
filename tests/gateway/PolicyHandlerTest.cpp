#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/gateway/PolicyHandler.hpp"
#include "support/ProxyTestSupport.hpp"
#include "support/RedisTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
namespace http = boost::beast::http;

constexpr double kTinyRefill = 0.001;

std::string headerOf(const network::HttpResponse& response, const std::string& name) {
  const auto it = response.find(name);
  return it == response.end() ? std::string{"<absent>"} : std::string{it->value()};
}

// The real server, registry API, reverse proxy and (Phase 8) the rate limit / cache layer in
// front of the proxy, over scripted real TCP backends and a real Redis.
class PolicyTest : public ProxyFixture {
 protected:
  using Response = network::HttpResponse;

  void SetUp() override {
    test_redis_ = testRedis();
    prefix_ = uniqueName("gw");
  }
  void TearDown() override {
    stopGateway();
    if (second_server_) second_server_->stop(1000ms);
    for (auto& client : redis_clients_) client->stop();
  }

  std::shared_ptr<redis::RedisClient> newRedisClient(std::uint16_t port, std::chrono::milliseconds command_timeout = 1000ms,
                                                     std::chrono::milliseconds retry = 200ms) {
    redis::RedisOptions options = testRedisOptions("127.0.0.1", port);
    options.command_timeout = command_timeout;
    options.retry_interval = retry;
    auto client = redis::RedisClient::create(options, log.logger());
    redis_clients_.push_back(client);
    return client;
  }

  config::RateLimitConfig ipLimit(unsigned capacity, double refill = kTinyRefill) {
    config::RateLimitConfig config;
    config.enabled = true;
    config.ip.enabled = true;
    config.ip.capacity = capacity;
    config.ip.refill_per_second = refill;
    return config;
  }

  config::CacheConfig cacheConfig() {
    config::CacheConfig config;
    config.enabled = true;
    config.default_ttl = 60s;
    config.max_ttl = 3600s;
    return config;
  }

  std::shared_ptr<gateway::PolicyHandler> makePolicy(const std::shared_ptr<redis::RedisClient>& client,
                                                     const std::optional<config::CacheConfig>& cache,
                                                     const std::optional<config::RateLimitConfig>& limit) {
    std::shared_ptr<ratelimit::RateLimiter> limiter;
    std::shared_ptr<cache::ResponseCache> response_cache;
    if (limit) limiter = std::make_shared<ratelimit::RateLimiter>(client, *limit, prefix_, log.logger());
    if (cache) response_cache = std::make_shared<cache::ResponseCache>(client, *cache, prefix_, log.logger());
    return std::make_shared<gateway::PolicyHandler>(proxy, std::move(limiter), std::move(response_cache),
                                                    log.logger());
  }

  // Starts the gateway: server -> PolicyHandler -> ProxyHandler -> registry API.
  void startPolicyGateway(const std::shared_ptr<redis::RedisClient>& client,
                          const std::optional<config::CacheConfig>& cache,
                          const std::optional<config::RateLimitConfig>& limit) {
    router = std::make_shared<routing::Router>(registry, routing::makeLoadBalancer(config::RoutingStrategy::RoundRobin));
    auto local = std::make_shared<network::LocalRequestHandler>();
    auto api = std::make_shared<network::RegistryRequestHandler>(registry, local, log.logger(), router);
    proxy = std::make_shared<proxy::ProxyHandler>(api, router, registry, proxy_config, 4, log.logger());
    policy = makePolicy(client, cache, limit);
    server = std::make_unique<network::HttpServer>(testServerConfig(), log.logger(), policy);
    ASSERT_TRUE(server->start());
  }

  // Like startPolicyGateway, with the Phase 7 reliability layer (retries, circuit breakers) in the proxy.
  void startReliablePolicyGateway(const std::shared_ptr<redis::RedisClient>& client,
                                  const std::optional<config::CacheConfig>& cache,
                                  const std::optional<config::RateLimitConfig>& limit) {
    reliability_config = testReliabilityConfig();
    router = std::make_shared<routing::Router>(registry, routing::makeLoadBalancer(config::RoutingStrategy::RoundRobin));
    auto local = std::make_shared<network::LocalRequestHandler>();
    auto api = std::make_shared<network::RegistryRequestHandler>(registry, local, log.logger(), router);
    reliability = std::make_shared<reliability::ReliabilityManager>(reliability_config, log.logger(), nullptr,
                                                                    [] { return 0.0; });
    proxy = std::make_shared<proxy::ProxyHandler>(api, router, registry, proxy_config, 4, log.logger(), nullptr,
                                                  nullptr, reliability);
    policy = makePolicy(client, cache, limit);
    server = std::make_unique<network::HttpServer>(testServerConfig(), log.logger(), policy);
    ASSERT_TRUE(server->start());
  }

  // A second EdgeFlow "instance": its own Redis client, limiter, cache and listener, the same
  // backends and the same Redis state.
  void startSecondGateway(std::uint16_t redis_port, const std::optional<config::CacheConfig>& cache,
                          const std::optional<config::RateLimitConfig>& limit) {
    second_policy_ = makePolicy(newRedisClient(redis_port), cache, limit);
    second_server_ = std::make_unique<network::HttpServer>(testServerConfig(), log.logger(), second_policy_);
    ASSERT_TRUE(second_server_->start());
  }

  std::unique_ptr<TestClient> secondClient() const { return std::make_unique<TestClient>(second_server_->port()); }

  static ScriptedBackend::Action reply(unsigned code, std::string body,
                                       std::vector<ScriptedBackend::Header> headers = {}) {
    ScriptedBackend::Action action;
    action.code = code;
    action.body = std::move(body);
    action.headers = std::move(headers);
    return action;
  }

  // A backend whose every answer is a new version ("v1", "v2", ...): a cached answer is
  // recognisable because its version does not change.
  std::unique_ptr<ScriptedBackend> versionedBackend(std::vector<ScriptedBackend::Header> headers = {},
                                                    unsigned code = 200) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    return std::make_unique<ScriptedBackend>(
        [counter, headers, code](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
          return reply(code, "v" + std::to_string(counter->fetch_add(1) + 1), headers);
        },
        "a");
  }

  std::optional<Response> getVia(TestClient& client, const std::string& target,
                                 const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    return client.request(http::verb::get, target, {}, headers);
  }

  bool storedCount(std::uint64_t n) {
    return eventually([&] { return policy->cache()->stats().stores >= n; });
  }

  std::optional<TestRedisAddress> test_redis_;
  std::string prefix_;
  std::shared_ptr<gateway::PolicyHandler> policy;
  std::shared_ptr<gateway::PolicyHandler> second_policy_;
  std::unique_ptr<network::HttpServer> second_server_;
  std::vector<std::shared_ptr<redis::RedisClient>> redis_clients_;
};

#define REQUIRE_REDIS() \
  if (!test_redis_) GTEST_SKIP() << "EDGEFLOW_TEST_REDIS_HOST is not set: no Redis available"

// =================================================================== cache

TEST_F(PolicyTest, AMissIsForwardedAndTheNextRequestIsAHitThatNeverReachesTheBackend) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();

  const auto miss = getVia(*client, "/proxy/orders/items?id=1", {{"X-Request-Id", "req-1"}});
  ASSERT_TRUE(miss);
  EXPECT_EQ(miss->result_int(), 200U);
  EXPECT_EQ(miss->body(), "v1");
  EXPECT_EQ(headerOf(*miss, "X-Cache"), "MISS");
  EXPECT_EQ(headerOf(*miss, "X-Request-Id"), "req-1");
  EXPECT_EQ(backend->requests(), 1U);
  ASSERT_TRUE(storedCount(1));

  const auto hit = getVia(*client, "/proxy/orders/items?id=1", {{"X-Request-Id", "req-2"}});
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->result_int(), 200U);
  EXPECT_EQ(hit->body(), "v1") << "the stored answer, not a new one";
  EXPECT_EQ(headerOf(*hit, "X-Cache"), "HIT");
  EXPECT_EQ(headerOf(*hit, "X-Request-Id"), "req-2") << "the id of THIS request";
  EXPECT_NE(headerOf(*hit, "Age"), "<absent>");
  EXPECT_EQ(backend->requests(), 1U) << "a hit does not contact the backend";
  EXPECT_EQ(policy->stats().served_from_cache, 1U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(PolicyTest, EachUrlAndQueryStringIsItsOwnEntry) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  const std::vector<std::string> targets = {"/proxy/orders/a", "/proxy/orders/a?x=1", "/proxy/orders/a?x=2",
                                            "/proxy/orders/b"};
  std::set<std::string> bodies;
  for (const auto& target : targets) {
    const auto response = getVia(*client, target);
    ASSERT_TRUE(response);
    bodies.insert(response->body());
  }
  EXPECT_EQ(bodies.size(), targets.size()) << "four different answers";
  ASSERT_TRUE(storedCount(4));
  for (const auto& target : targets) {
    const auto again = getVia(*client, target);
    ASSERT_TRUE(again);
    EXPECT_EQ(headerOf(*again, "X-Cache"), "HIT") << target;
    EXPECT_TRUE(bodies.count(again->body())) << target;
  }
  EXPECT_EQ(backend->requests(), 4U);
}

TEST_F(PolicyTest, SameUrlOfAnotherServiceIsAnotherEntry) {
  REQUIRE_REDIS();
  auto a = versionedBackend();
  ScriptedBackend b([](const auto&, auto, auto) { return reply(200, "from-b"); }, "b");
  addInstance("orders", "a", a->port());
  addInstance("catalog", "b", b.port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  EXPECT_EQ(getVia(*client, "/proxy/orders/x")->body(), "v1");
  EXPECT_EQ(getVia(*client, "/proxy/catalog/x")->body(), "from-b");
  ASSERT_TRUE(storedCount(2));
  EXPECT_EQ(getVia(*client, "/proxy/orders/x")->body(), "v1");
  EXPECT_EQ(getVia(*client, "/proxy/catalog/x")->body(), "from-b");
}

TEST_F(PolicyTest, AnEntryExpiresWhenTheBackendsMaxAgeIsOver) {
  REQUIRE_REDIS();
  auto backend = versionedBackend({{"Cache-Control", "max-age=1"}});
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  EXPECT_EQ(getVia(*client, "/proxy/orders/ttl")->body(), "v1");
  ASSERT_TRUE(storedCount(1));
  EXPECT_EQ(headerOf(*getVia(*client, "/proxy/orders/ttl"), "X-Cache"), "HIT");
  std::this_thread::sleep_for(1300ms);
  const auto expired = getVia(*client, "/proxy/orders/ttl");
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->body(), "v2") << "an expired entry is not served: the backend is asked again";
  EXPECT_EQ(headerOf(*expired, "X-Cache"), "MISS");
  EXPECT_EQ(backend->requests(), 2U);
}

TEST_F(PolicyTest, TheConfiguredDefaultLifetimeIsUsedWithoutCacheControl) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  auto config = cacheConfig();
  config.default_ttl = 1s;
  startPolicyGateway(newRedisClient(test_redis_->port), config, std::nullopt);
  auto client = this->client();
  EXPECT_EQ(getVia(*client, "/proxy/orders/d")->body(), "v1");
  ASSERT_TRUE(storedCount(1));
  EXPECT_EQ(headerOf(*getVia(*client, "/proxy/orders/d"), "X-Cache"), "HIT");
  std::this_thread::sleep_for(1300ms);
  EXPECT_EQ(getVia(*client, "/proxy/orders/d")->body(), "v2");
}

TEST_F(PolicyTest, ResponsesThatMustNotBeSharedAreNeverStored) {
  REQUIRE_REDIS();
  struct Case {
    std::string what;
    unsigned code;
    std::vector<ScriptedBackend::Header> headers;
  };
  const std::vector<Case> cases = {
      {"Set-Cookie", 200, {{"Set-Cookie", "session=1"}}},
      {"Cache-Control: no-store", 200, {{"Cache-Control", "no-store"}}},
      {"Cache-Control: private", 200, {{"Cache-Control", "private, max-age=60"}}},
      {"Cache-Control: no-cache", 200, {{"Cache-Control", "no-cache"}}},
      {"Vary", 200, {{"Vary", "Accept-Language"}}},
      {"404", 404, {}},
      {"500", 500, {}},
      {"201", 201, {}},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.what);
    registry = std::make_shared<FlakyRegistry>();
    auto backend = versionedBackend(test_case.headers, test_case.code);
    addInstance("orders", "a", backend->port());
    startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
    {
      auto client = this->client();
      const std::string target = "/proxy/orders/" + std::to_string(&test_case - cases.data());
      for (int i = 0; i < 3; ++i) {
        const auto response = getVia(*client, target);
        ASSERT_TRUE(response);
        EXPECT_NE(headerOf(*response, "X-Cache"), "HIT");
      }
      EXPECT_EQ(backend->requests(), 3U) << "every request went to the backend";
      EXPECT_EQ(policy->cache()->stats().stores, 0U);
    }
    stopGateway();
  }
}

TEST_F(PolicyTest, RequestsThatMustNotUseTheCacheBypassItInBothDirections) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  ASSERT_TRUE(getVia(*client, "/proxy/orders/p"));  // stored
  ASSERT_TRUE(storedCount(1));
  ASSERT_EQ(headerOf(*getVia(*client, "/proxy/orders/p"), "X-Cache"), "HIT");
  const auto before = backend->requests();

  const std::vector<std::pair<std::string, std::string>> bypassing = {
      {"Authorization", "Bearer token"}, {"Cookie", "a=b"}, {"Cache-Control", "no-cache"},
      {"Cache-Control", "no-store"},     {"Pragma", "no-cache"}, {"Range", "bytes=0-1"},
      {"If-None-Match", "\"etag\""}};
  std::size_t expected = before;
  for (const auto& header : bypassing) {
    const auto response = getVia(*client, "/proxy/orders/p", {header});
    ASSERT_TRUE(response) << header.first;
    EXPECT_NE(headerOf(*response, "X-Cache"), "HIT") << header.first;
    EXPECT_EQ(headerOf(*response, "X-Cache"), "<absent>") << header.first << ": the cache was not consulted";
    ++expected;
    EXPECT_EQ(backend->requests(), expected) << header.first;
  }
  EXPECT_EQ(policy->cache()->stats().stores, 1U) << "bypassing requests store nothing either";
}

TEST_F(PolicyTest, OnlyGetRequestsAreCachedAndASuccessfulUnsafeRequestInvalidatesTheUrl) {
  REQUIRE_REDIS();
  auto counter = std::make_shared<std::atomic<int>>(0);
  ScriptedBackend backend(
      [counter](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
        const int n = counter->fetch_add(1) + 1;
        if (request.method() == http::verb::post) {
          const bool fail = request.target().find("fail") != std::string::npos;
          return reply(fail ? 500U : 200U, "posted");
        }
        return reply(200, "v" + std::to_string(n));
      },
      "a");
  addInstance("orders", "a", backend.port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();

  EXPECT_EQ(getVia(*client, "/proxy/orders/item")->body(), "v1");
  ASSERT_TRUE(storedCount(1));
  EXPECT_EQ(headerOf(*getVia(*client, "/proxy/orders/item"), "X-Cache"), "HIT");

  // A POST is forwarded, never cached; its success removes the stored GET of the same URL.
  const auto post = client->request(http::verb::post, "/proxy/orders/item", "{}");
  ASSERT_TRUE(post);
  EXPECT_EQ(post->body(), "posted");
  EXPECT_EQ(headerOf(*post, "X-Cache"), "<absent>");
  ASSERT_TRUE(eventually([&] { return policy->cache()->stats().invalidations >= 1; }));
  const auto after = getVia(*client, "/proxy/orders/item");
  ASSERT_TRUE(after);
  EXPECT_EQ(headerOf(*after, "X-Cache"), "MISS");
  EXPECT_EQ(after->body(), "v3") << "fetched again after the change";

  // A POST that failed changes nothing: the entry stays.
  ASSERT_TRUE(storedCount(2));
  const auto failed = client->request(http::verb::post, "/proxy/orders/item?fail=1", "{}");
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->result_int(), 500U);
  EXPECT_EQ(policy->cache()->stats().invalidations, 1U);
}

TEST_F(PolicyTest, AHitCarriesTheBackendsHeadersAndBody) {
  REQUIRE_REDIS();
  ScriptedBackend backend(
      [](const auto&, auto, auto) {
        return reply(200, std::string("binary\0body\r\n", 13),
                     {{"Content-Type", "application/octet-stream"}, {"X-Backend", "a"}, {"X-Multi", "1"},
                      {"X-Multi", "2"}});
      },
      "a");
  addInstance("orders", "a", backend.port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  const auto miss = getVia(*client, "/proxy/orders/h");
  ASSERT_TRUE(miss);
  ASSERT_TRUE(storedCount(1));
  const auto hit = getVia(*client, "/proxy/orders/h");
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->body(), miss->body());
  EXPECT_EQ(headerOf(*hit, "Content-Type"), "application/octet-stream");
  EXPECT_EQ(headerOf(*hit, "X-Backend"), "a");
  EXPECT_EQ(headerOf(*hit, "Content-Length"), "13");
  std::vector<std::string> multi;
  for (const auto& field : *hit) {
    if (field.name_string() == "X-Multi") multi.emplace_back(field.value());
  }
  EXPECT_EQ(multi, (std::vector<std::string>{"1", "2"})) << "repeated headers keep their order";
  EXPECT_EQ(headerOf(*hit, "Via"), headerOf(*miss, "Via")) << "no second Via on a hit";
}

TEST_F(PolicyTest, EveryRequestGetsAUsableRequestIdWhetherForwardedOrServedFromTheCache) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  const auto first = getVia(*client, "/proxy/orders/id");
  ASSERT_TRUE(first);
  const auto first_id = headerOf(*first, "X-Request-Id");
  EXPECT_EQ(first_id.size(), 36U) << "a generated UUID";
  EXPECT_EQ(header(backend->last(), "X-Request-Id"), first_id) << "the backend saw the id the client was given";
  ASSERT_TRUE(storedCount(1));
  const auto second = getVia(*client, "/proxy/orders/id");
  ASSERT_TRUE(second);
  EXPECT_EQ(headerOf(*second, "X-Cache"), "HIT");
  EXPECT_EQ(headerOf(*second, "X-Request-Id").size(), 36U);
  EXPECT_NE(headerOf(*second, "X-Request-Id"), first_id) << "a new id per request, even from the cache";
}

TEST_F(PolicyTest, ConcurrentHitsAllGetTheStoredAnswerAndTheBackendStaysQuiet) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  {
    auto client = this->client();
    ASSERT_EQ(getVia(*client, "/proxy/orders/hot")->body(), "v1");
    ASSERT_TRUE(storedCount(1));
  }
  std::atomic<int> hits{0};
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&] {
      auto client = this->client();
      for (int i = 0; i < 25; ++i) {
        const auto response = getVia(*client, "/proxy/orders/hot");
        if (!response || response->result_int() != 200 || response->body() != "v1") {
          wrong.fetch_add(1);
        } else if (headerOf(*response, "X-Cache") == "HIT") {
          hits.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_EQ(hits.load(), 400);
  EXPECT_EQ(backend->requests(), 1U) << "400 concurrent hits, the backend was asked once";
}

TEST_F(PolicyTest, ConcurrentColdRequestsForOneUrlAreAllAnsweredAndSettleOnOneStoredAnswer) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  std::mutex mutex;
  std::set<std::string> seen;
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&] {
      auto client = this->client();
      const auto response = getVia(*client, "/proxy/orders/cold");
      if (!response || response->result_int() != 200) {
        failed.fetch_add(1);
        return;
      }
      const std::lock_guard lock(mutex);
      seen.insert(response->body());
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(failed.load(), 0);
  EXPECT_GE(backend->requests(), 1U);
  EXPECT_LE(backend->requests(), 16U) << "no request is lost or repeated by the cache";
  ASSERT_TRUE(eventually([&] { return policy->cache()->stats().stores >= 1; }));
  std::this_thread::sleep_for(100ms);

  // Whatever the race left behind is one complete answer that every later request gets.
  auto client = this->client();
  const auto settled = getVia(*client, "/proxy/orders/cold");
  ASSERT_TRUE(settled);
  EXPECT_EQ(headerOf(*settled, "X-Cache"), "HIT");
  EXPECT_TRUE(seen.count(settled->body())) << "a stored answer is one of the answers that were given out";
  const auto requests = backend->requests();
  for (int i = 0; i < 20; ++i) EXPECT_EQ(getVia(*client, "/proxy/orders/cold")->body(), settled->body());
  EXPECT_EQ(backend->requests(), requests);
}

TEST_F(PolicyTest, AMissStillGoesThroughTheReliabilityLayer) {
  REQUIRE_REDIS();
  reliability_config = testReliabilityConfig();  // not used by startPolicyGateway: build it here
  auto flaky = std::make_unique<ScriptedBackend>([](const auto&, auto, auto) { return reply(503, "down"); }, "a");
  ScriptedBackend good([](const auto&, auto, auto) { return reply(200, "from-b"); }, "b");
  addInstance("orders", "a", flaky->port());
  addInstance("orders", "b", good.port());
  router = std::make_shared<routing::Router>(registry, routing::makeLoadBalancer(config::RoutingStrategy::RoundRobin));
  auto local = std::make_shared<network::LocalRequestHandler>();
  auto api = std::make_shared<network::RegistryRequestHandler>(registry, local, log.logger(), router);
  reliability = std::make_shared<reliability::ReliabilityManager>(reliability_config, log.logger(), nullptr,
                                                                  [] { return 0.0; });
  proxy = std::make_shared<proxy::ProxyHandler>(api, router, registry, proxy_config, 4, log.logger(), nullptr,
                                                nullptr, reliability);
  policy = makePolicy(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  server = std::make_unique<network::HttpServer>(testServerConfig(), log.logger(), policy);
  ASSERT_TRUE(server->start());
  auto client = this->client();
  for (int i = 0; i < 4; ++i) {
    const auto response = getVia(*client, "/proxy/orders/r" + std::to_string(i));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 200U) << "the failed backend was retried on the other one";
    EXPECT_EQ(response->body(), "from-b");
  }
  ASSERT_TRUE(storedCount(4));
  EXPECT_EQ(headerOf(*getVia(*client, "/proxy/orders/r0"), "X-Cache"), "HIT");
}

// =================================================================== rate limiting

TEST_F(PolicyTest, RequestsBeyondTheCapacityAreAnswered429WithoutReachingTheBackend) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), std::nullopt, ipLimit(3));
  auto client = this->client();
  for (int i = 0; i < 3; ++i) {
    const auto response = getVia(*client, "/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 200U) << i;
  }
  const auto refused = getVia(*client, "/proxy/orders/x", {{"X-Request-Id", "limited-1"}});
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->result_int(), 429U);
  EXPECT_GE(std::stoi(headerOf(*refused, "Retry-After")), 1);
  EXPECT_EQ(headerOf(*refused, "X-RateLimit-Scope"), "ip");
  EXPECT_EQ(headerOf(*refused, "X-Request-Id"), "limited-1");
  EXPECT_NE(refused->body().find("rate limit exceeded"), std::string::npos);
  EXPECT_EQ(backend->requests(), 3U) << "a refused request never reaches the backend";
  EXPECT_EQ(policy->stats().rate_limited, 1U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  // The connection stays usable after a 429.
  EXPECT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 429U);
}

TEST_F(PolicyTest, TokensComeBackAndTheRetryAfterHintIsTrue) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), std::nullopt, ipLimit(1, 2.0));  // 1 token per 500 ms
  auto client = this->client();
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  const auto refused = getVia(*client, "/proxy/orders/x");
  ASSERT_EQ(refused->result_int(), 429U);
  EXPECT_EQ(headerOf(*refused, "Retry-After"), "1") << "rounded up to whole seconds";
  std::this_thread::sleep_for(600ms);
  EXPECT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
}

TEST_F(PolicyTest, ApiKeysAreLimitedSeparately) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  config::RateLimitConfig limit;
  limit.enabled = true;
  limit.ip.enabled = false;
  limit.api_key.enabled = true;
  limit.api_key.header = "X-Api-Key";
  limit.api_key.capacity = 2;
  limit.api_key.refill_per_second = kTinyRefill;
  startPolicyGateway(newRedisClient(test_redis_->port), std::nullopt, limit);
  auto client = this->client();
  const auto call = [&](const std::string& key) {
    return getVia(*client, "/proxy/orders/x", key.empty() ? std::vector<std::pair<std::string, std::string>>{}
                                                          : std::vector<std::pair<std::string, std::string>>{{"x-api-key", key}})
        ->result_int();
  };
  EXPECT_EQ(call("alice"), 200U);
  EXPECT_EQ(call("alice"), 200U);
  EXPECT_EQ(call("alice"), 429U);
  EXPECT_EQ(call("bob"), 200U) << "another key, another bucket (header names are case-insensitive)";
  for (int i = 0; i < 6; ++i) EXPECT_EQ(call(""), 200U) << "no key: this scope does not apply";
  const auto refused = getVia(*client, "/proxy/orders/x", {{"X-Api-Key", "alice"}});
  EXPECT_EQ(headerOf(*refused, "X-RateLimit-Scope"), "api_key");
}

TEST_F(PolicyTest, ServicesAreLimitedSeparatelyAndPerServiceLimitsApply) {
  REQUIRE_REDIS();
  auto orders = versionedBackend();
  ScriptedBackend catalog([](const auto&, auto, auto) { return reply(200, "c"); }, "c");
  ScriptedBackend billing([](const auto&, auto, auto) { return reply(200, "b"); }, "b");
  addInstance("orders", "a", orders->port());
  addInstance("catalog", "c", catalog.port());
  addInstance("billing", "b", billing.port());
  config::RateLimitConfig limit;
  limit.enabled = true;
  limit.ip.enabled = false;
  limit.service.enabled = true;
  limit.service.capacity = 3;
  limit.service.refill_per_second = kTinyRefill;
  config::TokenBucketConfig strict;
  strict.capacity = 1;
  strict.refill_per_second = kTinyRefill;
  limit.service.overrides["billing"] = strict;
  startPolicyGateway(newRedisClient(test_redis_->port), std::nullopt, limit);
  auto client = this->client();
  for (int i = 0; i < 3; ++i) EXPECT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  const auto refused = getVia(*client, "/proxy/orders/x");
  EXPECT_EQ(refused->result_int(), 429U);
  EXPECT_EQ(headerOf(*refused, "X-RateLimit-Scope"), "service");
  EXPECT_EQ(getVia(*client, "/proxy/catalog/x")->result_int(), 200U) << "another service has its own bucket";
  EXPECT_EQ(getVia(*client, "/proxy/billing/x")->result_int(), 200U);
  EXPECT_EQ(getVia(*client, "/proxy/billing/x")->result_int(), 429U) << "billing's own limit is 1";
  // The service bucket is shared by every client: another connection sees the same state.
  auto other = this->client();
  EXPECT_EQ(getVia(*other, "/proxy/orders/x")->result_int(), 429U);
}

TEST_F(PolicyTest, GatewayEndpointsAndTheRegistryApiAreNeverLimited) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), ipLimit(1));
  auto client = this->client();
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 429U);
  for (int i = 0; i < 30; ++i) {
    EXPECT_EQ(getVia(*client, "/health")->result_int(), 200U);
    EXPECT_EQ(getVia(*client, "/services")->result_int(), 200U);
  }
  EXPECT_EQ(getVia(*client, "/proxy/")->result_int(), 400U) << "a malformed proxy URL still reaches the proxy";
}

TEST_F(PolicyTest, CacheHitsCountAgainstTheRateLimit) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), ipLimit(5));
  auto client = this->client();
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  ASSERT_TRUE(storedCount(1));
  for (int i = 0; i < 4; ++i) EXPECT_EQ(headerOf(*getVia(*client, "/proxy/orders/x"), "X-Cache"), "HIT");
  EXPECT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 429U) << "hits spend tokens too";
  EXPECT_EQ(backend->requests(), 1U);
}

TEST_F(PolicyTest, ManyConcurrentClientsAreGrantedExactlyTheCapacity) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), std::nullopt, ipLimit(40));
  std::atomic<int> ok{0};
  std::atomic<int> limited{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&] {
      auto client = this->client();
      for (int i = 0; i < 10; ++i) {
        const auto response = getVia(*client, "/proxy/orders/x");
        if (!response) {
          other.fetch_add(1);
        } else if (response->result_int() == 200) {
          ok.fetch_add(1);
        } else if (response->result_int() == 429) {
          limited.fetch_add(1);
        } else {
          other.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(other.load(), 0);
  EXPECT_EQ(ok.load(), 40) << "160 concurrent requests, the bucket holds 40";
  EXPECT_EQ(limited.load(), 120);
  EXPECT_EQ(backend->requests(), 40U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(PolicyTest, TwoGatewayInstancesShareOneBucketAndOneCache) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), ipLimit(30));
  startSecondGateway(test_redis_->port, cacheConfig(), ipLimit(30));

  // The cache: stored by one instance, hit by the other.
  {
    auto first = this->client();
    auto second = secondClient();
    ASSERT_EQ(getVia(*first, "/proxy/orders/shared")->body(), "v1");
    ASSERT_TRUE(storedCount(1));
    const auto hit = getVia(*second, "/proxy/orders/shared");
    ASSERT_TRUE(hit);
    EXPECT_EQ(headerOf(*hit, "X-Cache"), "HIT");
    EXPECT_EQ(hit->body(), "v1");
    EXPECT_EQ(backend->requests(), 1U);
  }

  // The bucket: 2 requests used so far; 8 threads on EACH instance fight for the other 28.
  std::atomic<int> ok{0};
  std::atomic<int> limited{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&, t] {
      auto client = (t % 2 == 0) ? this->client() : secondClient();
      for (int i = 0; i < 10; ++i) {
        const auto response = getVia(*client, "/proxy/orders/shared");
        if (response && response->result_int() == 200) {
          ok.fetch_add(1);
        } else if (response && response->result_int() == 429) {
          limited.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(ok.load(), 28) << "30 tokens in total across both instances, 2 already spent";
  EXPECT_EQ(limited.load(), 132);
}

// =================================================================== Redis trouble

TEST_F(PolicyTest, WithRedisDownRequestsStillReachTheBackendAndNothingIsCached) {
  REQUIRE_REDIS();
  RedisRelay relay(test_redis_->host, test_redis_->port);
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(relay.port(), 300ms, 200ms), cacheConfig(), std::nullopt);
  auto client = this->client();
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->body(), "v1");
  ASSERT_TRUE(storedCount(1));
  ASSERT_EQ(headerOf(*getVia(*client, "/proxy/orders/x"), "X-Cache"), "HIT");

  relay.refuse();
  for (int i = 0; i < 5; ++i) {
    const auto response = getVia(*client, "/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 200U) << "Redis being down never fails a request";
    EXPECT_NE(headerOf(*response, "X-Cache"), "HIT");
  }
  EXPECT_EQ(backend->requests(), 6U) << "the cache is bypassed: every request goes to the backend";
  EXPECT_TRUE(log.contains("is unavailable"));
  EXPECT_EQ(getVia(*client, "/health")->result_int(), 200U);

  relay.resume();  // recovery: the next lookups find the entry that is still in Redis
  ASSERT_TRUE(eventually(
      [&] { return headerOf(*getVia(*client, "/proxy/orders/x"), "X-Cache") == "HIT"; }, 4s));
}

TEST_F(PolicyTest, AHungRedisCostsOneCommandTimeoutNotAHungRequest) {
  REQUIRE_REDIS();
  RedisRelay relay(test_redis_->host, test_redis_->port);
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(relay.port(), 250ms, 1000ms), cacheConfig(), ipLimit(100));
  auto client = this->client();
  relay.hang();
  const auto start = std::chrono::steady_clock::now();
  const auto first = getVia(*client, "/proxy/orders/x");
  const auto first_took = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(first);
  EXPECT_EQ(first->result_int(), 200U) << "rate limiting fails open, the cache is bypassed";
  EXPECT_LT(first_took, 1500ms);
  const auto second_start = std::chrono::steady_clock::now();
  const auto second = getVia(*client, "/proxy/orders/x");
  ASSERT_TRUE(second);
  EXPECT_EQ(second->result_int(), 200U);
  EXPECT_LT(std::chrono::steady_clock::now() - second_start, 300ms)
      << "while Redis is known to be down, requests do not wait for it at all";
}

TEST_F(PolicyTest, WithRedisDownTheRateLimitFailsOpenByDefault) {
  REQUIRE_REDIS();
  RedisRelay relay(test_redis_->host, test_redis_->port);
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startPolicyGateway(newRedisClient(relay.port(), 300ms, 200ms), std::nullopt, ipLimit(1));
  auto client = this->client();
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 429U);
  relay.refuse();
  for (int i = 0; i < 5; ++i) EXPECT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  relay.resume();
  ASSERT_TRUE(eventually([&] { return getVia(*client, "/proxy/orders/x")->result_int() == 429U; }, 4s))
      << "limits apply again as soon as Redis is back";
}

TEST_F(PolicyTest, WithRedisDownAndTheRejectPolicyRequestsAreRefusedWith503) {
  REQUIRE_REDIS();
  RedisRelay relay(test_redis_->host, test_redis_->port);
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  auto limit = ipLimit(100);
  limit.on_redis_failure = config::RateLimitFailurePolicy::Reject;
  startPolicyGateway(newRedisClient(relay.port(), 300ms, 200ms), std::nullopt, limit);
  auto client = this->client();
  ASSERT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 200U);
  relay.refuse();
  for (int i = 0; i < 3; ++i) {
    const auto response = getVia(*client, "/proxy/orders/x", {{"X-Request-Id", "rej-" + std::to_string(i)}});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 503U);
    EXPECT_EQ(headerOf(*response, "X-Request-Id"), "rej-" + std::to_string(i));
    EXPECT_NE(response->body().find("rate limiting is unavailable"), std::string::npos);
  }
  EXPECT_EQ(backend->requests(), 1U);
  EXPECT_EQ(getVia(*client, "/health")->result_int(), 200U) << "the gateway itself stays up";
  EXPECT_EQ(policy->stats().refused_degraded, 3U);
  relay.resume();
  ASSERT_TRUE(eventually([&] { return getVia(*client, "/proxy/orders/x")->result_int() == 200U; }, 4s));
}

TEST_F(PolicyTest, ShuttingDownWhileRedisIsHungAnswersEveryWaitingRequest) {
  REQUIRE_REDIS();
  RedisRelay relay(test_redis_->host, test_redis_->port);
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  auto limit = ipLimit(100);
  limit.on_redis_failure = config::RateLimitFailurePolicy::Allow;
  auto client_handle = newRedisClient(relay.port(), 8000ms, 1000ms);  // a long command timeout
  startPolicyGateway(client_handle, cacheConfig(), limit);
  relay.hang();
  std::vector<std::future<std::optional<Response>>> answers;
  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < 6; ++i) clients.push_back(this->client());  // all of them before any task starts
  for (std::size_t i = 0; i < clients.size(); ++i) {
    answers.push_back(std::async(std::launch::async, [this, &clients, i] {
      return getVia(*clients[i], "/proxy/orders/x");
    }));
  }
  std::this_thread::sleep_for(300ms);  // all six are waiting for Redis
  const auto start = std::chrono::steady_clock::now();
  server->stop(500ms);                  // the grace period ends: connections are cancelled
  proxy->stop();
  client_handle->stop();                // Redis client last, as the application does
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s) << "no request keeps the shutdown waiting";
  for (auto& answer : answers) {
    answer.wait();  // every client call returns (a response or a closed connection), none hangs
  }
}

// =================================================================== how the layers interact

TEST_F(PolicyTest, CachedAnswersKeepBeingServedWhileTheBackendIsDownAndItsCircuitIsOpen) {
  REQUIRE_REDIS();
  auto backend = versionedBackend();
  addInstance("orders", "a", backend->port());
  startReliablePolicyGateway(newRedisClient(test_redis_->port), cacheConfig(), std::nullopt);
  auto client = this->client();
  const auto warm = getVia(*client, "/proxy/orders/cached");
  ASSERT_TRUE(warm);
  ASSERT_EQ(warm->result_int(), 200U);
  ASSERT_TRUE(storedCount(1));

  backend.reset();  // the only backend dies
  // Uncached URLs fail (502: refused, retried, then answered) until the circuit opens and they are refused fast.
  for (int i = 0; i < 6; ++i) {
    const auto response = getVia(*client, "/proxy/orders/other" + std::to_string(i));
    ASSERT_TRUE(response);
    EXPECT_GE(response->result_int(), 500U) << i;
  }
  EXPECT_GE(reliability->stats().circuit_opened, 1U);

  const auto hit = getVia(*client, "/proxy/orders/cached");
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->result_int(), 200U) << "a stored answer needs neither the backend nor its circuit";
  EXPECT_EQ(hit->body(), warm->body());
  EXPECT_EQ(headerOf(*hit, "X-Cache"), "HIT");
}

TEST_F(PolicyTest, RetriesInsideOneRequestSpendNoRateLimitTokens) {
  REQUIRE_REDIS();
  auto bad = std::make_unique<ScriptedBackend>([](const auto&, auto, auto) { return reply(503, "down"); }, "a");
  addInstance("orders", "a", bad->port());
  startReliablePolicyGateway(newRedisClient(test_redis_->port), std::nullopt, ipLimit(2));
  auto client = this->client();
  for (int i = 0; i < 2; ++i) {
    const auto response = getVia(*client, "/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), 503U) << "the backend's last answer, after the retries";
  }
  const auto attempts = bad->requests();
  EXPECT_GE(attempts, 4U) << "each client request was attempted more than once";
  const auto refused = getVia(*client, "/proxy/orders/x");
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->result_int(), 429U) << "two client requests used the two tokens, however many attempts they took";
  EXPECT_EQ(bad->requests(), attempts) << "and a limited request makes no attempt at all";
}

TEST_F(PolicyTest, ARefusedRequestNeverTouchesTheCircuitBreakerOrTheRetryBudget) {
  REQUIRE_REDIS();
  auto bad = std::make_unique<ScriptedBackend>([](const auto&, auto, auto) { return reply(503, "down"); }, "a");
  addInstance("orders", "a", bad->port());
  startReliablePolicyGateway(newRedisClient(test_redis_->port), std::nullopt, ipLimit(1));
  auto client = this->client();
  ASSERT_TRUE(getVia(*client, "/proxy/orders/x"));
  const auto before = reliability->stats();
  for (int i = 0; i < 20; ++i) EXPECT_EQ(getVia(*client, "/proxy/orders/x")->result_int(), 429U);
  const auto after = reliability->stats();
  EXPECT_EQ(after.retries, before.retries);
  EXPECT_EQ(after.circuit_opened, before.circuit_opened);
  EXPECT_EQ(after.circuit_rejections, before.circuit_rejections);
}

}  // namespace
