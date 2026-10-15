#include "edgeflow/ratelimit/RateLimiter.hpp"

#include <cstdio>
#include <utility>

#include "edgeflow/redis/Sha256.hpp"

namespace edgeflow::ratelimit {

namespace {

// KEYS: one bucket key per scope that applies. ARGV: capacity and refill_per_second of each,
// in the same order. Returns {1, 0, 0} when every bucket had a token (one was taken from
// each), or {0, <1-based index of the first empty bucket>, <milliseconds until it has one>}
// without having changed anything.
//
// Redis's TIME is the only clock: gateways with different clocks agree. Tokens are stored as
// a decimal string with 6 digits, so a bucket refilling 0.001 tokens/s still adds up.
constexpr const char* kScript = R"lua(
local time = redis.call('TIME')
local now = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
local count = #KEYS
local available = {}
for i = 1, count do
  local capacity = tonumber(ARGV[2 * i - 1])
  local rate = tonumber(ARGV[2 * i]) / 1000
  local state = redis.call('HMGET', KEYS[i], 'tokens', 'ts')
  local tokens = tonumber(state[1])
  local stamp = tonumber(state[2])
  if tokens == nil or stamp == nil then
    tokens = capacity
  else
    local elapsed = now - stamp
    if elapsed < 0 then elapsed = 0 end
    tokens = math.min(capacity, tokens + elapsed * rate)
  end
  available[i] = tokens
  if tokens < 1 then
    return {0, i, math.ceil((1 - tokens) / rate)}
  end
end
for i = 1, count do
  local capacity = tonumber(ARGV[2 * i - 1])
  local rate = tonumber(ARGV[2 * i]) / 1000
  redis.call('HSET', KEYS[i], 'tokens', string.format('%.6f', available[i] - 1), 'ts', now)
  redis.call('PEXPIRE', KEYS[i], math.ceil(capacity / rate) + 1000)
end
return {1, 0, 0}
)lua";

std::string number(double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.9g", value);
  return buffer;
}

}  // namespace

std::string_view toString(Scope scope) noexcept {
  switch (scope) {
    case Scope::Ip: return "ip";
    case Scope::ApiKey: return "api_key";
    case Scope::Service: return "service";
  }
  return "ip";
}

RateLimiter::RateLimiter(std::shared_ptr<redis::RedisClient> client, config::RateLimitConfig config,
                         std::string key_prefix, std::shared_ptr<logging::Logger> logger)
    : client_(std::move(client)),
      config_(std::move(config)),
      prefix_(std::move(key_prefix)),
      logger_(std::move(logger)) {}

std::vector<Bucket> RateLimiter::bucketsFor(const Identity& identity) const {
  std::vector<Bucket> buckets;
  if (config_.ip.enabled) {
    buckets.push_back(Bucket{Scope::Ip,
                             prefix_ + ":rl:ip:" +
                                 (identity.client_ip.empty() ? std::string{"unknown"}
                                                             : identity.client_ip),
                             static_cast<double>(config_.ip.capacity),
                             config_.ip.refill_per_second});
  }
  if (config_.api_key.enabled && !identity.api_key.empty()) {
    buckets.push_back(Bucket{Scope::ApiKey, prefix_ + ":rl:key:" + redis::sha256Hex(identity.api_key),
                             static_cast<double>(config_.api_key.capacity),
                             config_.api_key.refill_per_second});
  }
  if (config_.service.enabled && !identity.service.empty()) {
    config::TokenBucketConfig limits = config_.service;
    if (const auto it = config_.service.overrides.find(identity.service);
        it != config_.service.overrides.end()) {
      limits = it->second;
    }
    buckets.push_back(Bucket{Scope::Service, prefix_ + ":rl:svc:" + identity.service,
                             static_cast<double>(limits.capacity), limits.refill_per_second});
  }
  return buckets;
}

std::string RateLimiter::scriptHash() {
  const std::lock_guard lock(script_mutex_);
  return script_hash_;
}

void RateLimiter::loadScript() {
  {
    const std::lock_guard lock(script_mutex_);
    if (loading_) return;
    loading_ = true;
  }
  client_->command({"SCRIPT", "LOAD", kScript}, [this](redis::RedisResult result) {
    const std::lock_guard lock(script_mutex_);
    loading_ = false;
    if (result.ok() && result.reply->type == redis::Reply::Type::Bulk) {
      script_hash_ = result.reply->text;
    }
  });
}

Decision RateLimiter::degraded(std::string detail) {
  Decision decision;
  decision.outcome = Decision::Outcome::Degraded;
  decision.allowed = config_.on_redis_failure == config::RateLimitFailurePolicy::Allow;
  decision.detail = std::move(detail);
  if (decision.allowed) {
    degraded_allowed_.fetch_add(1);
  } else {
    degraded_rejected_.fetch_add(1);
  }
  return decision;
}

void RateLimiter::acquire(const Identity& identity, Callback callback) {
  auto buckets = bucketsFor(identity);
  if (buckets.empty()) {
    allowed_.fetch_add(1);
    callback(Decision{});
    return;
  }
  const bool hash_known = !scriptHash().empty();
  if (!hash_known) loadScript();
  evaluate(std::move(buckets), std::move(callback), hash_known);
}

void RateLimiter::evaluate(std::vector<Bucket> buckets, Callback callback, bool use_script_hash) {
  std::vector<std::string> arguments;
  arguments.reserve(3 + buckets.size() * 3);
  arguments.emplace_back(use_script_hash ? "EVALSHA" : "EVAL");
  arguments.emplace_back(use_script_hash ? scriptHash() : std::string{kScript});
  arguments.emplace_back(std::to_string(buckets.size()));
  for (const auto& bucket : buckets) arguments.push_back(bucket.key);
  for (const auto& bucket : buckets) {
    arguments.push_back(number(bucket.capacity));
    arguments.push_back(number(bucket.refill_per_second));
  }

  client_->command(
      std::move(arguments),
      [this, buckets = std::move(buckets), callback = std::move(callback),
       use_script_hash](redis::RedisResult result) mutable {
        if (!result.ok()) {
          callback(degraded(std::string{redis::toString(result.error->kind)} + ": " +
                            result.error->message));
          return;
        }
        const redis::Reply& reply = *result.reply;
        if (reply.isError()) {
          if (use_script_hash && reply.text.rfind("NOSCRIPT", 0) == 0) {
            // Redis lost its script cache (restart, SCRIPT FLUSH): run it in full, reload it.
            {
              const std::lock_guard lock(script_mutex_);
              script_hash_.clear();
            }
            loadScript();
            evaluate(std::move(buckets), std::move(callback), false);
            return;
          }
          callback(degraded("Redis error: " + reply.text));
          return;
        }
        if (reply.type != redis::Reply::Type::Array || reply.elements.size() != 3 ||
            reply.elements[0].type != redis::Reply::Type::Integer ||
            reply.elements[1].type != redis::Reply::Type::Integer ||
            reply.elements[2].type != redis::Reply::Type::Integer) {
          callback(degraded("unexpected reply from the rate limit script"));
          return;
        }
        Decision decision;
        if (reply.elements[0].integer == 1) {
          allowed_.fetch_add(1);
          callback(std::move(decision));
          return;
        }
        decision.outcome = Decision::Outcome::Limited;
        decision.allowed = false;
        const long long index = reply.elements[1].integer;
        if (index >= 1 && static_cast<std::size_t>(index) <= buckets.size()) {
          decision.scope = buckets[static_cast<std::size_t>(index - 1)].scope;
        }
        decision.retry_after = std::chrono::milliseconds{reply.elements[2].integer};
        limited_.fetch_add(1);
        callback(std::move(decision));
      });
}

RateLimiter::Stats RateLimiter::stats() const {
  Stats stats;
  stats.allowed = allowed_.load();
  stats.limited = limited_.load();
  stats.degraded_allowed = degraded_allowed_.load();
  stats.degraded_rejected = degraded_rejected_.load();
  return stats;
}

}  // namespace edgeflow::ratelimit
