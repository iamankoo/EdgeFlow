// Micro-benchmarks of the paths every proxied request takes. They measure CPU cost of a
// function on one machine; they say nothing about end-to-end throughput (see bench/load).
#include <benchmark/benchmark.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "edgeflow/cache/ResponseCache.hpp"
#include "edgeflow/config/Config.hpp"
#include "edgeflow/discovery/ServiceInstance.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/observability/Metrics.hpp"
#include "edgeflow/proxy/ProxyHeaders.hpp"
#include "edgeflow/ratelimit/RateLimiter.hpp"
#include "edgeflow/redis/RedisClient.hpp"
#include "edgeflow/redis/Resp.hpp"
#include "edgeflow/redis/Sha256.hpp"
#include "edgeflow/reliability/CircuitBreaker.hpp"
#include "edgeflow/reliability/Retry.hpp"
#include "edgeflow/routing/LoadBalancer.hpp"

namespace {

using namespace edgeflow;  // NOLINT: benchmark convenience
namespace http = boost::beast::http;

std::shared_ptr<logging::Logger> quietLogger() {
  logging::LoggerOptions options;
  options.name = "bench";
  options.level = config::LogLevel::Off;
  return std::make_shared<logging::Logger>(std::move(options));
}

std::vector<discovery::ServiceInstance> instances(std::size_t n) {
  std::vector<discovery::ServiceInstance> out;
  for (std::size_t i = 0; i < n; ++i) {
    discovery::ServiceInstance instance;
    instance.service = "orders";
    instance.instance_id = "instance-" + std::to_string(1000 + i);
    instance.host = "10.0.0." + std::to_string(i % 250);
    instance.port = 8080;
    instance.health = discovery::HealthStatus::Healthy;
    instance.weight = static_cast<std::uint32_t>(1 + i % 5);
    instance.connection_count = i % 7;
    out.push_back(std::move(instance));
  }
  return out;
}

// ----------------------------------------------------------------------------- routing

void routing(benchmark::State& state, config::RoutingStrategy strategy) {
  const auto set = instances(static_cast<std::size_t>(state.range(0)));
  auto balancer = routing::makeLoadBalancer(strategy);
  routing::RoutingContext context{"203.0.113.9"};
  for (auto _ : state) {
    benchmark::DoNotOptimize(balancer->select(set, context));
  }
  state.SetItemsProcessed(state.iterations());
}
void BM_RoundRobin(benchmark::State& s) { routing(s, config::RoutingStrategy::RoundRobin); }
void BM_LeastConnections(benchmark::State& s) { routing(s, config::RoutingStrategy::LeastConnections); }
void BM_Weighted(benchmark::State& s) { routing(s, config::RoutingStrategy::Weighted); }
void BM_ConsistentHashing(benchmark::State& s) { routing(s, config::RoutingStrategy::ConsistentHashing); }
BENCHMARK(BM_RoundRobin)->Arg(3)->Arg(10)->Arg(100);
BENCHMARK(BM_LeastConnections)->Arg(3)->Arg(10)->Arg(100);
BENCHMARK(BM_Weighted)->Arg(3)->Arg(10)->Arg(100);
BENCHMARK(BM_ConsistentHashing)->Arg(3)->Arg(10)->Arg(100);
BENCHMARK(BM_RoundRobin)->Arg(10)->Threads(4)->Name("BM_RoundRobin_4threads");

// ----------------------------------------------------------------------------- proxy translation

network::HttpRequest clientRequest() {
  network::HttpRequest request{http::verb::get, "/proxy/orders/api/items?id=42&sort=asc", 11};
  request.set(http::field::host, "gateway.example.com");
  request.set(http::field::user_agent, "bench/1.0");
  request.set(http::field::accept, "application/json");
  request.set(http::field::accept_encoding, "gzip");
  request.set(http::field::connection, "keep-alive");
  request.set("X-Request-Id", "bench-request-0001");
  return request;
}

void BM_MapProxyTarget(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(proxy::mapProxyTarget("/proxy/orders/api/items?id=42&sort=asc"));
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MapProxyTarget);

void BM_MakeUpstreamRequest(benchmark::State& state) {
  const auto request = clientRequest();
  proxy::ForwardInfo info{"203.0.113.9", "10.0.0.5:8080", "bench-request-0001"};
  for (auto _ : state) benchmark::DoNotOptimize(proxy::makeUpstreamRequest(request, "/api/items?id=42&sort=asc", info));
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MakeUpstreamRequest);

void BM_MakeClientResponse(benchmark::State& state) {
  const auto request = clientRequest();
  network::HttpResponse upstream{http::status::ok, 11};
  upstream.set(http::field::content_type, "application/json");
  upstream.set(http::field::connection, "keep-alive");
  upstream.body() = std::string(static_cast<std::size_t>(state.range(0)), 'x');
  upstream.prepare_payload();
  for (auto _ : state) benchmark::DoNotOptimize(proxy::makeClientResponse(upstream, request, "bench-request-0001"));
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_MakeClientResponse)->Arg(256)->Arg(16 * 1024);

// ----------------------------------------------------------------------------- reliability

void BM_CircuitBreakerClosedPath(benchmark::State& state) {
  reliability::CircuitBreakerSettings settings;
  reliability::CircuitBreaker breaker(settings, [] { return std::chrono::steady_clock::now(); });
  for (auto _ : state) {
    const auto admission = breaker.tryAcquire();
    breaker.recordSuccess(admission);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CircuitBreakerClosedPath)->Threads(1)->Threads(4);

void BM_RetryDecision(benchmark::State& state) {
  config::RetryConfig config;
  reliability::RetryPolicy policy(config);
  for (auto _ : state) {
    benchmark::DoNotOptimize(policy.decide(reliability::AttemptKind::RetryableStatus, "GET", 1));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RetryDecision);

// ----------------------------------------------------------------------------- redis, cache, rate limit

void BM_Sha256(benchmark::State& state) {
  const std::string input(static_cast<std::size_t>(state.range(0)), 'k');
  for (auto _ : state) benchmark::DoNotOptimize(redis::sha256Hex(input));
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Sha256)->Arg(32)->Arg(512)->Arg(16 * 1024);

void BM_EncodeCommand(benchmark::State& state) {
  const std::vector<std::string> command{"SET", "edgeflow:cache:v1:0123456789abcdef", std::string(1024, 'v'), "PX", "60000"};
  for (auto _ : state) benchmark::DoNotOptimize(redis::encodeCommand(command));
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EncodeCommand);

void BM_ParseBulkReply(benchmark::State& state) {
  const std::string payload(static_cast<std::size_t>(state.range(0)), 'v');
  const std::string wire = "$" + std::to_string(payload.size()) + "\r\n" + payload + "\r\n";
  for (auto _ : state) {
    redis::RespParser parser;
    parser.feed(wire);
    benchmark::DoNotOptimize(parser.next());
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ParseBulkReply)->Arg(64)->Arg(4096)->Arg(256 * 1024);

std::shared_ptr<redis::RedisClient> idleRedis() {
  redis::RedisOptions options;
  options.port = 1;  // nothing listens: these benchmarks never send a command
  return redis::RedisClient::create(options, quietLogger());
}

void BM_RateLimiterBucketPlan(benchmark::State& state) {
  config::RateLimitConfig config;
  config.enabled = true;
  config.api_key.enabled = true;
  config.service.enabled = true;
  auto client = idleRedis();
  ratelimit::RateLimiter limiter(client, config, "edgeflow", quietLogger());
  ratelimit::Identity identity{"203.0.113.9", "api-key-0123456789abcdef", "orders"};
  for (auto _ : state) benchmark::DoNotOptimize(limiter.bucketsFor(identity));
  state.SetItemsProcessed(state.iterations());
  client->stop();
}
BENCHMARK(BM_RateLimiterBucketPlan);

cache::CachedResponse sampleEntry(std::size_t body) {
  cache::CachedResponse entry;
  entry.status = 200;
  entry.headers = {{"Content-Type", "application/json"}, {"Via", "1.1 edgeflow"}, {"X-Backend", "a"},
                   {"Cache-Control", "max-age=60"}};
  entry.body = std::string(body, 'b');
  entry.stored_at_ms = 1700000000000;
  return entry;
}

void BM_CacheSerialize(benchmark::State& state) {
  const auto entry = sampleEntry(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(cache::serialize(entry));
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CacheSerialize)->Arg(1024)->Arg(64 * 1024);

void BM_CacheDeserialize(benchmark::State& state) {
  const auto bytes = cache::serialize(sampleEntry(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state) benchmark::DoNotOptimize(cache::deserialize(bytes));
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CacheDeserialize)->Arg(1024)->Arg(64 * 1024);

void BM_CacheKeyAndRules(benchmark::State& state) {
  config::CacheConfig config;
  auto client = idleRedis();
  cache::ResponseCache cache(client, config, "edgeflow", quietLogger());
  const auto request = clientRequest();
  for (auto _ : state) {
    benchmark::DoNotOptimize(cache::ResponseCache::requestUsesCache(request));
    benchmark::DoNotOptimize(cache.keyFor("orders", "/api/items?id=42&sort=asc"));
  }
  state.SetItemsProcessed(state.iterations());
  client->stop();
}
BENCHMARK(BM_CacheKeyAndRules);

// ----------------------------------------------------------------------------- observability

void BM_HistogramRecord(benchmark::State& state) {
  static observability::Histogram histogram;
  std::int64_t i = 0;
  for (auto _ : state) histogram.record(std::chrono::microseconds{200 + (i++ % 5000)});
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HistogramRecord)->Threads(1)->Threads(4);

void BM_WindowedStatsRecord(benchmark::State& state) {
  static observability::WindowedStats stats(10);
  std::int64_t i = 0;
  for (auto _ : state) {
    const std::int64_t n = i++;
    stats.record(std::chrono::microseconds{200 + (n % 5000)}, n % 50 == 0);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WindowedStatsRecord)->Threads(1)->Threads(4);

void BM_MetricsRecordRequest(benchmark::State& state) {
  static observability::Metrics metrics(config::MetricsConfig{true, "/metrics", 10});
  std::int64_t i = 0;
  for (auto _ : state) metrics.recordRequest(std::chrono::microseconds{200 + (i++ % 5000)}, 200, true);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MetricsRecordRequest)->Threads(1)->Threads(4);

void BM_MetricsRender(benchmark::State& state) {
  observability::Metrics metrics(config::MetricsConfig{true, "/metrics", 10});
  for (int i = 0; i < 10000; ++i) metrics.recordRequest(std::chrono::microseconds{200 + i % 5000}, 200, true);
  for (auto _ : state) benchmark::DoNotOptimize(metrics.render());
}
BENCHMARK(BM_MetricsRender);

}  // namespace

BENCHMARK_MAIN();
