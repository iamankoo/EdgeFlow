#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/cache/ResponseCache.hpp"
#include "support/RedisTestSupport.hpp"

namespace edgeflow::cache {
namespace {

using namespace std::chrono_literals;
namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

HttpRequest get(const std::string& target = "/proxy/orders/items?id=1") {
  HttpRequest request{http::verb::get, target, 11};
  request.set(http::field::host, "gateway");
  return request;
}

// A response as the proxy hands it back for a backend answer: it carries the proxy's Via.
HttpResponse fromBackend(unsigned status = 200, const std::string& body = "payload") {
  HttpResponse response{static_cast<http::status>(status), 11};
  response.set(http::field::content_type, "text/plain");
  response.set(http::field::via, "1.1 edgeflow");
  response.set("X-Request-Id", "upstream-id");
  if (status != 204 && status != 304) response.body() = body;  // those statuses carry no body
  response.prepare_payload();
  return response;
}

class ResponseCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_redis_ = testing::testRedis();
    prefix = testing::uniqueName("cache");
    // A client that is only used by the tests that talk to Redis (it connects on first use).
    client = test_redis_ ? testing::makeRedisClient(*test_redis_, log.logger())
                         : redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", 1), log.logger());
  }
  void TearDown() override { client->stop(); }

  ResponseCache makeCache(config::CacheConfig config = config::CacheConfig{}) {
    config.enabled = true;
    return ResponseCache(client, std::move(config), prefix, log.logger());
  }

  ResponseCache::Lookup lookupSync(ResponseCache& cache, const std::string& key) {
    auto promise = std::make_shared<std::promise<ResponseCache::Lookup>>();
    auto future = promise->get_future();
    cache.lookup(key, [promise](ResponseCache::Lookup lookup) { promise->set_value(std::move(lookup)); });
    return future.get();
  }

  testing::CapturedLogger log;
  std::optional<testing::TestRedisAddress> test_redis_;
  std::string prefix;
  std::shared_ptr<redis::RedisClient> client;
};

#define REQUIRE_REDIS() \
  if (!test_redis_) GTEST_SKIP() << "EDGEFLOW_TEST_REDIS_HOST is not set: no Redis available"

// ------------------------------------------------------------------ pure rules

TEST_F(ResponseCacheTest, CacheControlDirectivesAreParsed) {
  auto c = parseCacheControl("max-age=60, public");
  EXPECT_EQ(c.max_age, 60);
  EXPECT_FALSE(c.no_store);
  c = parseCacheControl("No-Store , PRIVATE,no-cache");
  EXPECT_TRUE(c.no_store);
  EXPECT_TRUE(c.is_private);
  EXPECT_TRUE(c.no_cache);
  c = parseCacheControl("s-maxage=10, max-age=99");
  EXPECT_EQ(c.s_maxage, 10);
  EXPECT_EQ(c.max_age, 99);
  c = parseCacheControl("max-age=\"30\"");
  EXPECT_EQ(c.max_age, 30);
  for (const char* bad : {"max-age=abc", "max-age=-5", "max-age=", "max-age=99999999999999999999"}) {
    EXPECT_FALSE(parseCacheControl(bad).max_age) << bad;
  }
  c = parseCacheControl("");
  EXPECT_FALSE(c.no_store || c.no_cache || c.is_private || c.max_age || c.s_maxage);
  c = parseCacheControl("max-age=5,,  ,community=x");
  EXPECT_EQ(c.max_age, 5) << "empty and unknown directives are skipped";
}

TEST_F(ResponseCacheTest, RequestsThatMayUseTheCache) {
  EXPECT_TRUE(ResponseCache::requestUsesCache(get()));
  {
    auto request = get();
    request.set(http::field::accept, "application/json");
    request.set(http::field::cache_control, "max-age=0");  // not a no-store/no-cache directive
    EXPECT_TRUE(ResponseCache::requestUsesCache(request));
  }
  for (const auto verb : {http::verb::post, http::verb::put, http::verb::delete_, http::verb::head,
                          http::verb::patch, http::verb::options}) {
    HttpRequest request{verb, "/proxy/orders/", 11};
    EXPECT_FALSE(ResponseCache::requestUsesCache(request)) << static_cast<int>(verb);
  }
}

TEST_F(ResponseCacheTest, RequestsThatBypassTheCache) {
  const std::vector<std::pair<std::string, std::string>> headers = {
      {"Authorization", "Bearer t"}, {"Cookie", "a=b"},           {"Range", "bytes=0-9"},
      {"If-None-Match", "\"x\""},    {"If-Modified-Since", "Sat, 29 Oct 1994 19:43:31 GMT"},
      {"If-Match", "\"x\""},         {"If-Unmodified-Since", "x"}, {"If-Range", "x"},
      {"Cache-Control", "no-cache"}, {"Cache-Control", "no-store"}, {"Cache-Control", "max-age=0, no-cache"},
      {"Pragma", "no-cache"}};
  for (const auto& [name, value] : headers) {
    auto request = get();
    request.set(name, value);
    EXPECT_FALSE(ResponseCache::requestUsesCache(request)) << name << ": " << value;
  }
  auto with_body = get();
  with_body.body() = "x";
  with_body.prepare_payload();
  EXPECT_FALSE(ResponseCache::requestUsesCache(with_body));
}

TEST_F(ResponseCacheTest, OnlyUnsafeMethodsInvalidate) {
  for (const auto verb : {http::verb::post, http::verb::put, http::verb::patch, http::verb::delete_}) {
    EXPECT_TRUE(ResponseCache::isUnsafeMethod(HttpRequest{verb, "/x", 11}));
  }
  for (const auto verb : {http::verb::get, http::verb::head, http::verb::options}) {
    EXPECT_FALSE(ResponseCache::isUnsafeMethod(HttpRequest{verb, "/x", 11}));
  }
}

TEST_F(ResponseCacheTest, KeysDependOnServiceAndTargetOnly) {
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/items?id=1");
  EXPECT_EQ(key, cache.keyFor("orders", "/items?id=1"));
  EXPECT_NE(key, cache.keyFor("orders", "/items?id=2"));
  EXPECT_NE(key, cache.keyFor("orders", "/items"));
  EXPECT_NE(key, cache.keyFor("catalog", "/items?id=1"));
  EXPECT_NE(cache.keyFor("ab", "/c"), cache.keyFor("a", "b/c")) << "the fields cannot run together";
  EXPECT_EQ(key.rfind(prefix + ":cache:v1:", 0), 0U);
  EXPECT_EQ(key.size(), prefix.size() + std::string{":cache:v1:"}.size() + 64);
}

TEST_F(ResponseCacheTest, ADefaultLifetimeAppliesWithoutCacheControl) {
  config::CacheConfig config;
  config.default_ttl = 45s;
  auto cache = makeCache(config);
  const auto ttl = cache.storableTtl(fromBackend());
  ASSERT_TRUE(ttl);
  EXPECT_EQ(*ttl, 45s);
}

TEST_F(ResponseCacheTest, TheBackendsLifetimeWinsButNeverBeyondMaxTtl) {
  config::CacheConfig config;
  config.default_ttl = 60s;
  config.max_ttl = 100s;
  auto cache = makeCache(config);
  auto response = fromBackend();
  response.set(http::field::cache_control, "max-age=30");
  EXPECT_EQ(*cache.storableTtl(response), 30s);
  response.set(http::field::cache_control, "max-age=30, s-maxage=70");
  EXPECT_EQ(*cache.storableTtl(response), 70s) << "s-maxage is for shared caches like this one";
  response.set(http::field::cache_control, "max-age=86400");
  EXPECT_EQ(*cache.storableTtl(response), 100s) << "capped by cache.max_ttl_seconds";
}

TEST_F(ResponseCacheTest, ResponsesThatMustNotBeStored) {
  auto cache = makeCache();
  const auto refuse = [&](const std::string& what, const HttpResponse& response) {
    EXPECT_FALSE(cache.storableTtl(response)) << what;
  };
  for (const char* value : {"no-store", "no-cache", "private", "max-age=0", "s-maxage=0",
                            "public, no-store", "max-age=60, private"}) {
    auto response = fromBackend();
    response.set(http::field::cache_control, value);
    refuse(std::string{"Cache-Control: "} + value, response);
  }
  {
    auto response = fromBackend();
    response.set(http::field::set_cookie, "id=1");
    refuse("Set-Cookie", response);
  }
  {
    auto response = fromBackend();
    response.set(http::field::vary, "Accept-Encoding");
    refuse("Vary", response);
  }
  {
    auto response = fromBackend();
    response.set(http::field::content_range, "bytes 0-1/2");
    refuse("Content-Range", response);
  }
  for (const unsigned status : {201U, 204U, 301U, 302U, 304U, 400U, 404U, 429U, 500U, 502U, 503U, 504U}) {
    refuse("status " + std::to_string(status), fromBackend(status));
  }
  {
    HttpResponse gateway_error{http::status::ok, 11};  // 200 but not from a backend: no Via
    gateway_error.body() = "x";
    gateway_error.prepare_payload();
    refuse("no Via: not forwarded from a backend", gateway_error);
  }
  {
    auto response = fromBackend();
    response.set(http::field::via, "1.1 someproxy");
    refuse("Via of another proxy only", response);
  }
}

TEST_F(ResponseCacheTest, CacheableStatusesAndEntrySizeAreConfigurable) {
  config::CacheConfig config;
  config.cacheable_statuses = {200, 404};
  config.max_entry_bytes = 10;
  auto cache = makeCache(config);
  EXPECT_TRUE(cache.storableTtl(fromBackend(200, "0123456789")));
  EXPECT_FALSE(cache.storableTtl(fromBackend(200, "0123456789X"))) << "one byte over the limit";
  EXPECT_TRUE(cache.storableTtl(fromBackend(404, "gone")));
  EXPECT_FALSE(cache.storableTtl(fromBackend(301, "x")));
}

TEST_F(ResponseCacheTest, EntriesSurviveSerialization) {
  CachedResponse entry;
  entry.status = 203;
  entry.stored_at_ms = 1700000000123;
  entry.headers = {{"Content-Type", "text/plain"}, {"Set-Foo", "a"}, {"Set-Foo", "b"}, {"Empty", ""}};
  entry.body = std::string("bin\0ary\r\n", 9);
  const auto bytes = serialize(entry);
  const auto back = deserialize(bytes);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->status, 203U);
  EXPECT_EQ(back->stored_at_ms, 1700000000123);
  EXPECT_EQ(back->headers, entry.headers);
  EXPECT_EQ(back->body, entry.body);
}

TEST_F(ResponseCacheTest, DamagedEntriesAreNeverReadAsResponses) {
  CachedResponse entry;
  entry.headers = {{"A", "b"}};
  entry.body = "body";
  const std::string bytes = serialize(entry);
  ASSERT_TRUE(deserialize(bytes));
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    EXPECT_FALSE(deserialize(std::string_view{bytes}.substr(0, length))) << "truncated to " << length;
  }
  EXPECT_FALSE(deserialize(bytes + "X")) << "trailing bytes";
  std::string wrong_magic = bytes;
  wrong_magic[0] = 'X';
  EXPECT_FALSE(deserialize(wrong_magic));
  std::string bad_status = bytes;
  bad_status[4] = 0;
  bad_status[5] = 7;
  EXPECT_FALSE(deserialize(bad_status));
  EXPECT_FALSE(deserialize("not a cache entry at all"));
}

TEST_F(ResponseCacheTest, AHitIsAnsweredWithTheCallersRequestIdAndHitMarkers) {
  CachedResponse entry;
  entry.status = 200;
  entry.headers = {{"Content-Type", "text/plain"}, {"Via", "1.1 edgeflow"}};
  entry.body = "stored body";
  entry.stored_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch() - 5s)
                           .count();
  auto request = get();
  const auto response = ResponseCache::toResponse(entry, request, "my-id");
  EXPECT_EQ(response.result_int(), 200U);
  EXPECT_EQ(response.body(), "stored body");
  EXPECT_EQ(response.version(), 11U);
  EXPECT_EQ(std::string{response["X-Request-Id"]}, "my-id");
  EXPECT_EQ(std::string{response["X-Cache"]}, "HIT");
  EXPECT_GE(std::stoi(std::string{response["Age"]}), 5);
  EXPECT_EQ(std::string{response["Content-Length"]}, "11");
  EXPECT_EQ(std::string{response["Content-Type"]}, "text/plain");
}

// ------------------------------------------------------------------ with Redis

TEST_F(ResponseCacheTest, AStoredResponseIsFoundAndAnUnknownKeyMisses) {
  REQUIRE_REDIS();
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/items");
  EXPECT_EQ(lookupSync(cache, key).outcome, ResponseCache::Lookup::Outcome::Miss);

  auto response = fromBackend(200, "hello");
  response.set("X-Backend", "a");
  response.set(http::field::age, "7");  // dropped when storing: recomputed on a hit
  cache.store(key, response, 30s);
  ASSERT_TRUE(testing::eventually([&] {
    return lookupSync(cache, key).outcome == ResponseCache::Lookup::Outcome::Hit;
  }));
  const auto hit = lookupSync(cache, key);
  EXPECT_EQ(hit.entry.status, 200U);
  EXPECT_EQ(hit.entry.body, "hello");
  bool has_backend_header = false;
  for (const auto& [name, value] : hit.entry.headers) {
    EXPECT_NE(name, "X-Request-Id") << "the id of the storing request is not stored";
    EXPECT_NE(name, "Age");
    EXPECT_NE(name, "Content-Length");
    if (name == "X-Backend" && value == "a") has_backend_header = true;
  }
  EXPECT_TRUE(has_backend_header);
  EXPECT_EQ(cache.stats().hits, 2U);
  EXPECT_EQ(cache.stats().misses, 1U);
  EXPECT_EQ(cache.stats().stores, 1U);
}

TEST_F(ResponseCacheTest, AnExpiredEntryIsGone) {
  REQUIRE_REDIS();
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/short");
  cache.store(key, fromBackend(), 1s);
  ASSERT_TRUE(testing::eventually([&] {
    return lookupSync(cache, key).outcome == ResponseCache::Lookup::Outcome::Hit;
  }));
  std::this_thread::sleep_for(1300ms);
  EXPECT_EQ(lookupSync(cache, key).outcome, ResponseCache::Lookup::Outcome::Miss);
  const auto ttl = client->commandSync({"EXISTS", key});
  EXPECT_EQ(ttl.reply->integer, 0);
}

TEST_F(ResponseCacheTest, TheLifetimeIsSetInRedis) {
  REQUIRE_REDIS();
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/ttl");
  cache.store(key, fromBackend(), 20s);
  ASSERT_TRUE(testing::eventually([&] {
    return client->commandSync({"EXISTS", key}).reply->integer == 1;
  }));
  const auto pttl = client->commandSync({"PTTL", key}).reply->integer;
  EXPECT_GT(pttl, 18000);
  EXPECT_LE(pttl, 20000);
}

TEST_F(ResponseCacheTest, InvalidateRemovesTheEntry) {
  REQUIRE_REDIS();
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/inv");
  cache.store(key, fromBackend(), 30s);
  ASSERT_TRUE(testing::eventually([&] {
    return lookupSync(cache, key).outcome == ResponseCache::Lookup::Outcome::Hit;
  }));
  cache.invalidate(key);
  ASSERT_TRUE(testing::eventually([&] {
    return lookupSync(cache, key).outcome == ResponseCache::Lookup::Outcome::Miss;
  }));
  EXPECT_EQ(cache.stats().invalidations, 1U);
}

TEST_F(ResponseCacheTest, ACorruptEntryIsReportedRemovedAndNeverServed) {
  REQUIRE_REDIS();
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/corrupt");
  ASSERT_TRUE(client->commandSync({"SET", key, "garbage that is not an entry", "PX", "30000"}).reply->isOk());
  const auto first = lookupSync(cache, key);
  EXPECT_EQ(first.outcome, ResponseCache::Lookup::Outcome::Error);
  ASSERT_TRUE(testing::eventually([&] { return client->commandSync({"EXISTS", key}).reply->integer == 0; }));
  EXPECT_EQ(lookupSync(cache, key).outcome, ResponseCache::Lookup::Outcome::Miss);
  EXPECT_GE(cache.stats().errors, 1U);
}

TEST_F(ResponseCacheTest, ConcurrentStoresAndLookupsNeverReturnAMixedEntry) {
  REQUIRE_REDIS();
  auto cache = makeCache();
  const auto key = cache.keyFor("orders", "/race");
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w] {
      const std::string body(2000, static_cast<char>('a' + w));
      while (!stop.load()) {
        cache.store(key, fromBackend(200, body), 30s);
        std::this_thread::sleep_for(2ms);  // a steady writer, not a flood
      }
    });
  }
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        const auto lookup = lookupSync(cache, key);
        if (lookup.outcome == ResponseCache::Lookup::Outcome::Hit) {
          const auto& body = lookup.entry.body;
          if (body.size() != 2000 || body.find_first_not_of(body.front()) != std::string::npos) bad.fetch_add(1);
        } else if (lookup.outcome == ResponseCache::Lookup::Outcome::Error) {
          bad.fetch_add(1);
        }
      }
    });
  }
  for (auto& reader : readers) reader.join();
  stop.store(true);
  for (auto& writer : writers) writer.join();
  EXPECT_EQ(bad.load(), 0) << "every entry read back is exactly one writer's entry";
}

TEST_F(ResponseCacheTest, RedisFailureIsAnErrorOutcomeNotACrash) {
  testing::FakeRedisServer server([](const auto&, auto, auto) {
    testing::FakeRedisServer::Action action;
    action.kind = testing::FakeRedisServer::Action::Kind::Close;
    return action;
  });
  client->stop();
  client = redis::RedisClient::create(testing::testRedisOptions("127.0.0.1", server.port()), log.logger());
  auto cache = makeCache();
  const auto lookup = lookupSync(cache, cache.keyFor("orders", "/x"));
  EXPECT_EQ(lookup.outcome, ResponseCache::Lookup::Outcome::Error);
  EXPECT_FALSE(lookup.detail.empty());
  cache.store(cache.keyFor("orders", "/x"), fromBackend(), 10s);  // must not throw or block
  EXPECT_TRUE(testing::eventually([&] { return cache.stats().store_failures >= 1; }));
}

}  // namespace
}  // namespace edgeflow::cache
