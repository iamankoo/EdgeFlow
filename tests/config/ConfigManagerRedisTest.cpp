#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

#include "edgeflow/config/ConfigManager.hpp"

namespace {

using edgeflow::config::ConfigManager;
using edgeflow::config::RateLimitFailurePolicy;
using std::chrono::milliseconds;
using std::chrono::seconds;

const char* const kProxyOn = "database:\n  enabled: true\nproxy:\n  enabled: true\n";
const char* const kRedisOn = "redis:\n  enabled: true\n";

bool anyErrorContains(const ConfigManager& manager, const std::string& text) {
  const auto& errors = manager.errors();
  return std::any_of(errors.begin(), errors.end(),
                     [&](const std::string& e) { return e.find(text) != std::string::npos; });
}

TEST(ConfigManagerRedisTest, DefaultsKeepEverythingOff) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("server:\n  port: 8080\n"));
  const auto& c = manager.config();
  EXPECT_FALSE(c.redis.enabled);
  EXPECT_EQ(c.redis.host, "127.0.0.1");
  EXPECT_EQ(c.redis.port, 6379U);
  EXPECT_TRUE(c.redis.password_env.empty());
  EXPECT_EQ(c.redis.key_prefix, "edgeflow");
  EXPECT_EQ(c.redis.connect_timeout, milliseconds{1000});
  EXPECT_EQ(c.redis.command_timeout, milliseconds{250});
  EXPECT_EQ(c.redis.pool_size, 4U);
  EXPECT_EQ(c.redis.io_threads, 1U);
  EXPECT_EQ(c.redis.retry_interval, milliseconds{1000});

  EXPECT_FALSE(c.cache.enabled);
  EXPECT_EQ(c.cache.default_ttl, seconds{60});
  EXPECT_EQ(c.cache.max_ttl, seconds{3600});
  EXPECT_EQ(c.cache.max_entry_bytes, 1024U * 1024U);
  EXPECT_EQ(c.cache.cacheable_statuses, (std::vector<unsigned>{200}));

  EXPECT_FALSE(c.rate_limit.enabled);
  EXPECT_EQ(c.rate_limit.on_redis_failure, RateLimitFailurePolicy::Allow) << "fail open by default";
  EXPECT_TRUE(c.rate_limit.ip.enabled);
  EXPECT_EQ(c.rate_limit.ip.capacity, 100U);
  EXPECT_DOUBLE_EQ(c.rate_limit.ip.refill_per_second, 50.0);
  EXPECT_FALSE(c.rate_limit.api_key.enabled);
  EXPECT_EQ(c.rate_limit.api_key.header, "X-API-Key");
  EXPECT_FALSE(c.rate_limit.service.enabled);
  EXPECT_TRUE(c.rate_limit.service.overrides.empty());
}

TEST(ConfigManagerRedisTest, ParsesEveryKey) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString(std::string{kProxyOn} +
                                     "redis:\n"
                                     "  enabled: true\n  host: cache.internal\n  port: 6380\n"
                                     "  password_env: REDIS_SECRET\n  key_prefix: gw-1\n"
                                     "  connect_timeout_ms: 700\n  command_timeout_ms: 90\n"
                                     "  pool_size: 8\n  io_threads: 2\n  retry_interval_ms: 2500\n"
                                     "cache:\n"
                                     "  enabled: true\n  default_ttl_seconds: 30\n  max_ttl_seconds: 120\n"
                                     "  max_entry_bytes: 4096\n  cacheable_statuses: [200, 404]\n"
                                     "rate_limit:\n"
                                     "  enabled: true\n  on_redis_failure: reject\n"
                                     "  ip:\n    enabled: true\n    capacity: 7\n    refill_per_second: 0.5\n"
                                     "  api_key:\n    enabled: true\n    header: X-Token\n    capacity: 70\n"
                                     "    refill_per_second: 10\n"
                                     "  service:\n    enabled: true\n    capacity: 700\n    refill_per_second: 100\n"
                                     "    overrides:\n"
                                     "      billing:\n        capacity: 5\n        refill_per_second: 1.5\n"
                                     "      search:\n        capacity: 9\n"))
      << (manager.errors().empty() ? "" : manager.errors().front());
  const auto& c = manager.config();
  EXPECT_TRUE(c.redis.enabled);
  EXPECT_EQ(c.redis.host, "cache.internal");
  EXPECT_EQ(c.redis.port, 6380U);
  EXPECT_EQ(c.redis.password_env, "REDIS_SECRET");
  EXPECT_EQ(c.redis.key_prefix, "gw-1");
  EXPECT_EQ(c.redis.connect_timeout, milliseconds{700});
  EXPECT_EQ(c.redis.command_timeout, milliseconds{90});
  EXPECT_EQ(c.redis.pool_size, 8U);
  EXPECT_EQ(c.redis.io_threads, 2U);
  EXPECT_EQ(c.redis.retry_interval, milliseconds{2500});

  EXPECT_TRUE(c.cache.enabled);
  EXPECT_EQ(c.cache.default_ttl, seconds{30});
  EXPECT_EQ(c.cache.max_ttl, seconds{120});
  EXPECT_EQ(c.cache.max_entry_bytes, 4096U);
  EXPECT_EQ(c.cache.cacheable_statuses, (std::vector<unsigned>{200, 404}));

  EXPECT_TRUE(c.rate_limit.enabled);
  EXPECT_EQ(c.rate_limit.on_redis_failure, RateLimitFailurePolicy::Reject);
  EXPECT_EQ(c.rate_limit.ip.capacity, 7U);
  EXPECT_DOUBLE_EQ(c.rate_limit.ip.refill_per_second, 0.5);
  EXPECT_TRUE(c.rate_limit.api_key.enabled);
  EXPECT_EQ(c.rate_limit.api_key.header, "X-Token");
  EXPECT_EQ(c.rate_limit.api_key.capacity, 70U);
  EXPECT_TRUE(c.rate_limit.service.enabled);
  EXPECT_EQ(c.rate_limit.service.capacity, 700U);
  ASSERT_EQ(c.rate_limit.service.overrides.size(), 2U);
  EXPECT_EQ(c.rate_limit.service.overrides.at("billing").capacity, 5U);
  EXPECT_DOUBLE_EQ(c.rate_limit.service.overrides.at("billing").refill_per_second, 1.5);
  EXPECT_EQ(c.rate_limit.service.overrides.at("search").capacity, 9U);
  EXPECT_DOUBLE_EQ(c.rate_limit.service.overrides.at("search").refill_per_second, 100.0)
      << "an override that omits a value takes the service-wide one";
}

TEST(ConfigManagerRedisTest, UnknownKeysAreRejectedAtEveryLevel) {
  for (const std::string yaml : {"redis:\n  hosst: x\n", "cache:\n  ttl: 5\n", "rate_limit:\n  limit: 5\n",
                                 "rate_limit:\n  ip:\n    burst: 5\n", "rate_limit:\n  api_key:\n    name: x\n",
                                 "rate_limit:\n  service:\n    overrides:\n      a:\n        burst: 1\n"}) {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(yaml)) << yaml;
    EXPECT_TRUE(anyErrorContains(manager, "unknown key") || anyErrorContains(manager, "burst")) << yaml;
  }
}

TEST(ConfigManagerRedisTest, InvalidValuesAreRejectedWithAnExplanation) {
  struct Case {
    std::string yaml;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {"redis:\n  port: 0\n", "redis.port"},
      {"redis:\n  port: 70000\n", "redis.port"},
      {"redis:\n  host: \"has space\"\n", "redis.host"},
      {"redis:\n  password_env: \"1BAD\"\n", "redis.password_env"},
      {"redis:\n  password_env: hunter2!\n", "redis.password_env"},
      {"redis:\n  key_prefix: \"a b\"\n", "redis.key_prefix"},
      {"redis:\n  key_prefix: \"\"\n", "redis.key_prefix"},
      {"redis:\n  command_timeout_ms: 1\n", "redis.command_timeout_ms"},
      {"redis:\n  pool_size: 0\n", "redis.pool_size"},
      {"redis:\n  io_threads: 999\n", "redis.io_threads"},
      {"redis:\n  enabled: yes\n", "redis.enabled"},
      {"cache:\n  default_ttl_seconds: 0\n", "cache.default_ttl_seconds"},
      {"cache:\n  default_ttl_seconds: 500\n  max_ttl_seconds: 100\n", "must not exceed"},
      {"cache:\n  max_entry_bytes: 0\n", "cache.max_entry_bytes"},
      {"cache:\n  cacheable_statuses: [100]\n", "cache.cacheable_statuses"},
      {"cache:\n  cacheable_statuses: [200, 200]\n", "twice"},
      {"cache:\n  cacheable_statuses: [abc]\n", "integers"},
      {"cache:\n  cacheable_statuses: []\n", "must not be empty"},
      {"cache:\n  cacheable_statuses: 200\n", "must be a list"},
      {"rate_limit:\n  on_redis_failure: maybe\n", "on_redis_failure"},
      {"rate_limit:\n  ip:\n    capacity: 0\n", "rate_limit.ip.capacity"},
      {"rate_limit:\n  ip:\n    capacity: 99999999\n", "rate_limit.ip.capacity"},
      {"rate_limit:\n  ip:\n    refill_per_second: 0\n", "refill_per_second"},
      {"rate_limit:\n  ip:\n    refill_per_second: -3\n", "refill_per_second"},
      {"rate_limit:\n  ip:\n    refill_per_second: fast\n", "must be a number"},
      {"rate_limit:\n  ip:\n    refill_per_second: .inf\n", "must be a number"},
      {"rate_limit:\n  api_key:\n    header: \"bad header\"\n", "rate_limit.api_key.header"},
      {"rate_limit:\n  service:\n    overrides:\n      \"Bad Name\":\n        capacity: 1\n", "service name"},
      {"rate_limit:\n  service:\n    overrides:\n      a: 5\n", "must be a mapping"},
      {"rate_limit:\n  service:\n    overrides: [a]\n", "must be a mapping"},
  };
  for (const auto& test_case : cases) {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(test_case.yaml)) << test_case.yaml;
    EXPECT_TRUE(anyErrorContains(manager, test_case.expected))
        << test_case.yaml << " -> " << (manager.errors().empty() ? "" : manager.errors().front());
  }
}

TEST(ConfigManagerRedisTest, CacheAndRateLimitNeedRedisAndTheProxy) {
  {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{kProxyOn} + "cache:\n  enabled: true\n"));
    EXPECT_TRUE(anyErrorContains(manager, "'cache.enabled' requires 'redis.enabled'"));
  }
  {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{kProxyOn} + "rate_limit:\n  enabled: true\n"));
    EXPECT_TRUE(anyErrorContains(manager, "'rate_limit.enabled' requires 'redis.enabled'"));
  }
  {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{kRedisOn} + "cache:\n  enabled: true\n"));
    EXPECT_TRUE(anyErrorContains(manager, "'cache.enabled' requires 'proxy.enabled'"));
  }
  {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{kRedisOn} + "rate_limit:\n  enabled: true\n"));
    EXPECT_TRUE(anyErrorContains(manager, "'rate_limit.enabled' requires 'proxy.enabled'"));
  }
  {
    ConfigManager manager;
    EXPECT_TRUE(manager.loadFromString(std::string{kProxyOn} + kRedisOn +
                                       "cache:\n  enabled: true\nrate_limit:\n  enabled: true\n"));
  }
}

TEST(ConfigManagerRedisTest, ShippedConfigurationsStillLoadAndComposeEnablesPhase8) {
  ConfigManager defaults;
  ASSERT_TRUE(defaults.load(EDGEFLOW_DEFAULT_CONFIG))
      << (defaults.errors().empty() ? "" : defaults.errors().front());
  EXPECT_FALSE(defaults.config().redis.enabled);
  EXPECT_FALSE(defaults.config().cache.enabled);
  EXPECT_FALSE(defaults.config().rate_limit.enabled);

  ConfigManager compose;
  ASSERT_TRUE(compose.load(std::string{EDGEFLOW_COMPOSE_CONFIG}))
      << (compose.errors().empty() ? "" : compose.errors().front());
  EXPECT_TRUE(compose.config().redis.enabled);
  EXPECT_EQ(compose.config().redis.host, "redis");
  EXPECT_TRUE(compose.config().cache.enabled);
  EXPECT_TRUE(compose.config().rate_limit.enabled);
}

}  // namespace
