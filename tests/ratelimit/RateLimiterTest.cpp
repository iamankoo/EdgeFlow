#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/ratelimit/RateLimiter.hpp"
#include "support/RedisTestSupport.hpp"

namespace edgeflow::ratelimit {
namespace {

using namespace std::chrono_literals;
using Outcome = Decision::Outcome;

constexpr double kTinyRefill = 0.001;  // tokens per second: effectively none during a test

config::RateLimitConfig ipOnly(unsigned capacity, double refill) {
  config::RateLimitConfig config;
  config.enabled = true;
  config.ip.enabled = true;
  config.ip.capacity = capacity;
  config.ip.refill_per_second = refill;
  config.api_key.enabled = false;
  config.service.enabled = false;
  return config;
}

Decision acquireSync(RateLimiter& limiter, const Identity& identity) {
  auto promise = std::make_shared<std::promise<Decision>>();
  auto future = promise->get_future();
  limiter.acquire(identity, [promise](Decision decision) { promise->set_value(std::move(decision)); });
  return future.get();
}

Identity who(const std::string& ip, const std::string& service = "orders", const std::string& key = "") {
  Identity identity;
  identity.client_ip = ip;
  identity.service = service;
  identity.api_key = key;
  return identity;
}

class RateLimiterTest : public ::testing::Test {
 protected:
  std::shared_ptr<RateLimiter> makeLimiter(const config::RateLimitConfig& config,
                                           std::shared_ptr<redis::RedisClient> client = nullptr) {
    if (!client) client = testing::makeRedisClient(*test_redis_, log.logger());
    clients.push_back(client);
    return std::make_shared<RateLimiter>(client, config, prefix, log.logger());
  }

  void SetUp() override {
    test_redis_ = testing::testRedis();
    prefix = testing::uniqueName("rl");
  }
  void TearDown() override {
    for (auto& client : clients) client->stop();
  }

  testing::CapturedLogger log;
  std::optional<testing::TestRedisAddress> test_redis_;
  std::string prefix;
  std::vector<std::shared_ptr<redis::RedisClient>> clients;
};

#define REQUIRE_REDIS() \
  if (!test_redis_) GTEST_SKIP() << "EDGEFLOW_TEST_REDIS_HOST is not set: no Redis available"

TEST_F(RateLimiterTest, BucketsFollowTheConfiguration) {
  // No Redis needed: the plan of which buckets a request draws from.
  config::RateLimitConfig config = ipOnly(10, 5);
  config.api_key.enabled = true;
  config.api_key.capacity = 20;
  config.api_key.refill_per_second = 4;
  config.service.enabled = true;
  config.service.capacity = 30;
  config.service.refill_per_second = 3;
  config::TokenBucketConfig vip;
  vip.capacity = 2;
  vip.refill_per_second = 1;
  config.service.overrides["vip"] = vip;
  auto client = redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", 1), log.logger());
  clients.push_back(client);
  RateLimiter limiter(client, config, "p", log.logger());

  auto buckets = limiter.bucketsFor(who("1.2.3.4", "orders", "k1"));
  ASSERT_EQ(buckets.size(), 3U);
  EXPECT_EQ(buckets[0].scope, Scope::Ip);
  EXPECT_EQ(buckets[0].key, "p:rl:ip:1.2.3.4");
  EXPECT_EQ(buckets[0].capacity, 10.0);
  EXPECT_EQ(buckets[1].scope, Scope::ApiKey);
  EXPECT_EQ(buckets[1].key.rfind("p:rl:key:", 0), 0U);
  EXPECT_EQ(buckets[1].key.find("k1"), std::string::npos) << "the API key is hashed";
  EXPECT_EQ(buckets[1].key.size(), std::string{"p:rl:key:"}.size() + 64);
  EXPECT_EQ(buckets[2].scope, Scope::Service);
  EXPECT_EQ(buckets[2].key, "p:rl:svc:orders");
  EXPECT_EQ(buckets[2].capacity, 30.0);

  buckets = limiter.bucketsFor(who("1.2.3.4", "vip"));
  ASSERT_EQ(buckets.size(), 2U) << "no API key, no api_key bucket";
  EXPECT_EQ(buckets[1].capacity, 2.0) << "the override replaces the service limits";
  EXPECT_EQ(buckets[1].refill_per_second, 1.0);

  EXPECT_EQ(limiter.bucketsFor(who("", "orders")).front().key, "p:rl:ip:unknown");
}

TEST_F(RateLimiterTest, NoApplicableBucketMeansNoRedisCall) {
  testing::FakeRedisServer server;
  auto client = redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", server.port()),
                                           log.logger());
  clients.push_back(client);
  config::RateLimitConfig config = ipOnly(5, 1);
  config.ip.enabled = false;
  RateLimiter limiter(client, config, "p", log.logger());
  const auto decision = acquireSync(limiter, who("1.1.1.1"));
  EXPECT_TRUE(decision.allowed);
  EXPECT_EQ(decision.outcome, Outcome::Allowed);
  EXPECT_TRUE(server.commands().empty());
}

TEST_F(RateLimiterTest, CapacityTokensAreGrantedThenTheNextRequestIsRefused) {
  REQUIRE_REDIS();
  auto limiter = makeLimiter(ipOnly(3, kTinyRefill));
  for (int i = 0; i < 3; ++i) {
    const auto decision = acquireSync(*limiter, who("10.0.0.1"));
    EXPECT_TRUE(decision.allowed) << i;
    EXPECT_EQ(decision.outcome, Outcome::Allowed);
  }
  const auto refused = acquireSync(*limiter, who("10.0.0.1"));
  EXPECT_FALSE(refused.allowed);
  EXPECT_EQ(refused.outcome, Outcome::Limited);
  ASSERT_TRUE(refused.scope);
  EXPECT_EQ(*refused.scope, Scope::Ip);
  EXPECT_GT(refused.retry_after, 1000ms) << "at 0.001 token/s the wait is long";
  EXPECT_EQ(limiter->stats().allowed, 3U);
  EXPECT_EQ(limiter->stats().limited, 1U);
}

TEST_F(RateLimiterTest, TokensRefillContinuouslyAndRetryAfterIsHonest) {
  REQUIRE_REDIS();
  auto limiter = makeLimiter(ipOnly(2, 10.0));  // one token per 100 ms
  ASSERT_TRUE(acquireSync(*limiter, who("10.0.0.2")).allowed);
  ASSERT_TRUE(acquireSync(*limiter, who("10.0.0.2")).allowed);
  const auto refused = acquireSync(*limiter, who("10.0.0.2"));
  ASSERT_FALSE(refused.allowed);
  EXPECT_GE(refused.retry_after, 1ms);
  EXPECT_LE(refused.retry_after, 100ms);

  std::this_thread::sleep_for(std::chrono::milliseconds{refused.retry_after.count() + 60});
  EXPECT_TRUE(acquireSync(*limiter, who("10.0.0.2")).allowed) << "after Retry-After a token exists";
  EXPECT_FALSE(acquireSync(*limiter, who("10.0.0.2")).allowed) << "and only one";
}

TEST_F(RateLimiterTest, RefillNeverExceedsTheCapacity) {
  REQUIRE_REDIS();
  auto limiter = makeLimiter(ipOnly(2, 5.0));
  ASSERT_TRUE(acquireSync(*limiter, who("10.0.0.3")).allowed);
  ASSERT_TRUE(acquireSync(*limiter, who("10.0.0.3")).allowed);
  std::this_thread::sleep_for(1200ms);  // 6 tokens' worth of time, but the bucket holds 2
  EXPECT_TRUE(acquireSync(*limiter, who("10.0.0.3")).allowed);
  EXPECT_TRUE(acquireSync(*limiter, who("10.0.0.3")).allowed);
  EXPECT_FALSE(acquireSync(*limiter, who("10.0.0.3")).allowed);
}

TEST_F(RateLimiterTest, EachClientAddressHasItsOwnBucket) {
  REQUIRE_REDIS();
  auto limiter = makeLimiter(ipOnly(2, kTinyRefill));
  EXPECT_TRUE(acquireSync(*limiter, who("10.1.0.1")).allowed);
  EXPECT_TRUE(acquireSync(*limiter, who("10.1.0.1")).allowed);
  EXPECT_FALSE(acquireSync(*limiter, who("10.1.0.1")).allowed);
  EXPECT_TRUE(acquireSync(*limiter, who("10.1.0.2")).allowed) << "another address is unaffected";
  EXPECT_TRUE(acquireSync(*limiter, who("2001:db8::1")).allowed) << "IPv6 text works as a key";
}

TEST_F(RateLimiterTest, EachApiKeyHasItsOwnBucketAndIsNeverStoredInTheClear) {
  REQUIRE_REDIS();
  config::RateLimitConfig config;
  config.enabled = true;
  config.ip.enabled = false;
  config.api_key.enabled = true;
  config.api_key.capacity = 2;
  config.api_key.refill_per_second = kTinyRefill;
  config.service.enabled = false;
  auto limiter = makeLimiter(config);

  EXPECT_TRUE(acquireSync(*limiter, who("1.1.1.1", "orders", "secret-key-123")).allowed);
  EXPECT_TRUE(acquireSync(*limiter, who("2.2.2.2", "orders", "secret-key-123")).allowed)
      << "the key, not the address, identifies the client";
  const auto refused = acquireSync(*limiter, who("3.3.3.3", "orders", "secret-key-123"));
  EXPECT_FALSE(refused.allowed);
  ASSERT_TRUE(refused.scope);
  EXPECT_EQ(*refused.scope, Scope::ApiKey);
  EXPECT_TRUE(acquireSync(*limiter, who("1.1.1.1", "orders", "another-key")).allowed);
  EXPECT_TRUE(acquireSync(*limiter, who("1.1.1.1", "orders", "")).allowed) << "no key: scope does not apply";

  const auto keys = clients.front()->commandSync({"KEYS", prefix + ":rl:key:*"});
  ASSERT_TRUE(keys.ok());
  EXPECT_EQ(keys.reply->elements.size(), 2U);
  for (const auto& key : keys.reply->elements) {
    EXPECT_EQ(key.text.find("secret-key-123"), std::string::npos);
    EXPECT_EQ(key.text.find("another-key"), std::string::npos);
  }
}

TEST_F(RateLimiterTest, EachServiceHasItsOwnBucketAndOverridesApply) {
  REQUIRE_REDIS();
  config::RateLimitConfig config;
  config.enabled = true;
  config.ip.enabled = false;
  config.service.enabled = true;
  config.service.capacity = 4;
  config.service.refill_per_second = kTinyRefill;
  config::TokenBucketConfig strict;
  strict.capacity = 1;
  strict.refill_per_second = kTinyRefill;
  config.service.overrides["billing"] = strict;
  auto limiter = makeLimiter(config);

  for (int i = 0; i < 4; ++i) EXPECT_TRUE(acquireSync(*limiter, who("1.1.1.1", "orders")).allowed);
  const auto refused = acquireSync(*limiter, who("9.9.9.9", "orders"));
  EXPECT_FALSE(refused.allowed) << "the service bucket is shared by all its clients";
  EXPECT_EQ(*refused.scope, Scope::Service);
  EXPECT_TRUE(acquireSync(*limiter, who("1.1.1.1", "catalog")).allowed) << "another service";

  EXPECT_TRUE(acquireSync(*limiter, who("1.1.1.1", "billing")).allowed);
  EXPECT_FALSE(acquireSync(*limiter, who("1.1.1.1", "billing")).allowed) << "billing's own limit is 1";
}

TEST_F(RateLimiterTest, ARefusedRequestTakesNoTokenFromTheOtherScopes) {
  REQUIRE_REDIS();
  config::RateLimitConfig config = ipOnly(5, kTinyRefill);
  config.service.enabled = true;
  config.service.capacity = 1;
  config.service.refill_per_second = kTinyRefill;
  auto limiter = makeLimiter(config);

  EXPECT_TRUE(acquireSync(*limiter, who("7.7.7.7", "orders")).allowed);  // ip 5 -> 4, service 1 -> 0
  for (int i = 0; i < 3; ++i) {
    const auto refused = acquireSync(*limiter, who("7.7.7.7", "orders"));
    ASSERT_FALSE(refused.allowed);
    EXPECT_EQ(*refused.scope, Scope::Service);
  }
  const auto tokens = clients.front()->commandSync({"HGET", prefix + ":rl:ip:7.7.7.7", "tokens"});
  ASSERT_TRUE(tokens.ok());
  ASSERT_EQ(tokens.reply->type, redis::Reply::Type::Bulk);
  EXPECT_NEAR(std::stod(tokens.reply->text), 4.0, 0.01)
      << "three refusals by the service bucket left the ip bucket untouched";
}

TEST_F(RateLimiterTest, TwoLimitersWithTheirOwnConnectionsShareOneBucket) {
  REQUIRE_REDIS();
  const config::RateLimitConfig config = ipOnly(10, kTinyRefill);
  auto first = makeLimiter(config);
  auto second = makeLimiter(config);  // a second "gateway": its own client, its own connections
  int allowed = 0;
  for (int i = 0; i < 25; ++i) {
    RateLimiter& limiter = (i % 2 == 0) ? *first : *second;
    if (acquireSync(limiter, who("20.0.0.1")).allowed) ++allowed;
  }
  EXPECT_EQ(allowed, 10);
}

TEST_F(RateLimiterTest, ConcurrentGatewaysNeverGrantMoreThanTheCapacity) {
  REQUIRE_REDIS();
  const config::RateLimitConfig config = ipOnly(100, kTinyRefill);
  std::vector<std::shared_ptr<RateLimiter>> gateways;
  for (int i = 0; i < 3; ++i) gateways.push_back(makeLimiter(config));

  std::atomic<int> allowed{0};
  std::atomic<int> limited{0};
  std::atomic<int> degraded{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 12; ++t) {
    threads.emplace_back([&, t] {
      RateLimiter& limiter = *gateways[static_cast<std::size_t>(t) % gateways.size()];
      for (int i = 0; i < 50; ++i) {
        const auto decision = acquireSync(limiter, who("30.0.0.1"));
        if (decision.outcome == Outcome::Degraded) {
          degraded.fetch_add(1);
        } else if (decision.allowed) {
          allowed.fetch_add(1);
        } else {
          limited.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(degraded.load(), 0);
  EXPECT_EQ(allowed.load(), 100) << "600 concurrent requests, exactly the capacity is granted";
  EXPECT_EQ(limited.load(), 500);
}

TEST_F(RateLimiterTest, ConcurrentRequestsForManyClientsEachGetExactlyTheirOwnCapacity) {
  REQUIRE_REDIS();
  const config::RateLimitConfig config = ipOnly(5, kTinyRefill);
  auto gateway_a = makeLimiter(config);
  auto gateway_b = makeLimiter(config);
  std::map<int, std::atomic<int>> allowed;
  for (int c = 0; c < 20; ++c) allowed[c].store(0);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      RateLimiter& limiter = (t % 2 == 0) ? *gateway_a : *gateway_b;
      for (int round = 0; round < 3; ++round) {
        for (int c = 0; c < 20; ++c) {
          if (acquireSync(limiter, who("40.0.0." + std::to_string(c))).allowed) allowed[c].fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  for (int c = 0; c < 20; ++c) EXPECT_EQ(allowed[c].load(), 5) << "client " << c;
}

TEST_F(RateLimiterTest, IdleBucketsExpireFromRedis) {
  REQUIRE_REDIS();
  auto limiter = makeLimiter(ipOnly(1, 20.0));  // full again after 50 ms; key lives about 1.05 s
  ASSERT_TRUE(acquireSync(*limiter, who("50.0.0.1")).allowed);
  const std::string key = prefix + ":rl:ip:50.0.0.1";
  const auto ttl = clients.front()->commandSync({"PTTL", key});
  ASSERT_TRUE(ttl.ok());
  EXPECT_GT(ttl.reply->integer, 0);
  EXPECT_LE(ttl.reply->integer, 1100);
  std::this_thread::sleep_for(1300ms);
  EXPECT_EQ(clients.front()->commandSync({"EXISTS", key}).reply->integer, 0);
  EXPECT_TRUE(acquireSync(*limiter, who("50.0.0.1")).allowed) << "a fresh bucket starts full";
}

TEST_F(RateLimiterTest, ALostScriptCacheIsReloadedTransparently) {
  REQUIRE_REDIS();
  auto limiter = makeLimiter(ipOnly(1000, kTinyRefill));
  ASSERT_TRUE(acquireSync(*limiter, who("60.0.0.1")).allowed);   // runs the script in full, loads it
  std::this_thread::sleep_for(100ms);
  ASSERT_TRUE(acquireSync(*limiter, who("60.0.0.1")).allowed);   // now by hash
  ASSERT_TRUE(clients.front()->commandSync({"SCRIPT", "FLUSH"}).reply->isOk());
  for (int i = 0; i < 5; ++i) {
    const auto decision = acquireSync(*limiter, who("60.0.0.1"));
    EXPECT_EQ(decision.outcome, Outcome::Allowed) << "request " << i << " after SCRIPT FLUSH";
  }
}

// ----------------------------------------------------------------- Redis failure

TEST_F(RateLimiterTest, RedisDownLetsRequestsThroughWhenTheDefaultPolicyIsAllow) {
  testing::FakeRedisServer server([](const auto&, auto, auto) {
    testing::FakeRedisServer::Action action;
    action.kind = testing::FakeRedisServer::Action::Kind::Close;
    return action;
  });
  config::RateLimitConfig config = ipOnly(1, kTinyRefill);
  config.on_redis_failure = config::RateLimitFailurePolicy::Allow;
  auto client = redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", server.port()),
                                           log.logger());
  clients.push_back(client);
  RateLimiter limiter(client, config, "p", log.logger());
  for (int i = 0; i < 5; ++i) {
    const auto decision = acquireSync(limiter, who("1.1.1.1"));
    EXPECT_EQ(decision.outcome, Outcome::Degraded);
    EXPECT_TRUE(decision.allowed) << "fail open";
    EXPECT_FALSE(decision.detail.empty());
  }
  EXPECT_EQ(limiter.stats().degraded_allowed, 5U);
  EXPECT_EQ(limiter.stats().degraded_rejected, 0U);
}

TEST_F(RateLimiterTest, RedisDownRefusesRequestsWhenThePolicyIsReject) {
  testing::FakeRedisServer server([](const auto&, auto, auto) {
    testing::FakeRedisServer::Action action;
    action.kind = testing::FakeRedisServer::Action::Kind::Close;
    return action;
  });
  config::RateLimitConfig config = ipOnly(1, kTinyRefill);
  config.on_redis_failure = config::RateLimitFailurePolicy::Reject;
  auto client = redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", server.port()),
                                           log.logger());
  clients.push_back(client);
  RateLimiter limiter(client, config, "p", log.logger());
  const auto decision = acquireSync(limiter, who("1.1.1.1"));
  EXPECT_EQ(decision.outcome, Outcome::Degraded);
  EXPECT_FALSE(decision.allowed) << "fail closed";
  EXPECT_EQ(limiter.stats().degraded_rejected, 1U);
}

TEST_F(RateLimiterTest, AnUnexpectedScriptReplyIsADegradedAnswerNotACrash) {
  testing::FakeRedisServer server([](const auto&, auto, auto) {
    testing::FakeRedisServer::Action action;
    action.raw = "*2\r\n:1\r\n:0\r\n";  // not the three integers the script returns
    return action;
  });
  auto client = redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", server.port()),
                                           log.logger());
  clients.push_back(client);
  RateLimiter limiter(client, ipOnly(1, 1), "p", log.logger());
  EXPECT_EQ(acquireSync(limiter, who("1.1.1.1")).outcome, Outcome::Degraded);

  server.setHandler([](const auto&, auto, auto) {
    testing::FakeRedisServer::Action action;
    action.raw = "-ERR something broke\r\n";
    return action;
  });
  EXPECT_EQ(acquireSync(limiter, who("1.1.1.1")).outcome, Outcome::Degraded);
}

TEST_F(RateLimiterTest, LimitsAreEnforcedAgainAfterRedisOutageAndRecovery) {
  REQUIRE_REDIS();
  testing::RedisRelay relay(test_redis_->host, test_redis_->port);
  redis::RedisOptions options = testing::testRedisOptions("127.0.0.1", relay.port());
  options.command_timeout = 300ms;
  options.retry_interval = 200ms;
  auto client = redis::RedisClient::create(options, log.logger());
  clients.push_back(client);
  config::RateLimitConfig config = ipOnly(2, kTinyRefill);
  config.on_redis_failure = config::RateLimitFailurePolicy::Allow;
  RateLimiter limiter(client, config, prefix, log.logger());

  EXPECT_TRUE(acquireSync(limiter, who("70.0.0.1")).allowed);
  EXPECT_TRUE(acquireSync(limiter, who("70.0.0.1")).allowed);
  EXPECT_FALSE(acquireSync(limiter, who("70.0.0.1")).allowed);

  relay.refuse();  // Redis dies: fail open
  for (int i = 0; i < 3; ++i) {
    const auto decision = acquireSync(limiter, who("70.0.0.1"));
    EXPECT_EQ(decision.outcome, Outcome::Degraded);
    EXPECT_TRUE(decision.allowed);
  }

  relay.resume();  // Redis returns with its state intact: the exhausted bucket is exhausted again
  ASSERT_TRUE(testing::eventually(
      [&] { return acquireSync(limiter, who("70.0.0.1")).outcome == Outcome::Limited; }, 3s));
}

}  // namespace
}  // namespace edgeflow::ratelimit
