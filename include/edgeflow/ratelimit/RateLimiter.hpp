#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/redis/RedisClient.hpp"

namespace edgeflow::ratelimit {

// What a bucket is shared by.
enum class Scope {
  Ip,       // one bucket per client address
  ApiKey,   // one bucket per API key
  Service   // one bucket per proxied service (shared by all its clients)
};

[[nodiscard]] std::string_view toString(Scope scope) noexcept;

// Who is asking.
struct Identity {
  std::string client_ip;  // empty is treated as "unknown": all such clients share one bucket
  std::string api_key;    // empty: the request carried none
  std::string service;
};

// One bucket a request must take a token from.
struct Bucket {
  Scope scope{Scope::Ip};
  std::string key;  // Redis key
  double capacity{0};
  double refill_per_second{0};
};

struct Decision {
  enum class Outcome {
    Allowed,   // every bucket had a token; one was taken from each
    Limited,   // a bucket was empty; nothing was taken from any bucket
    Degraded   // Redis could not be asked; `allowed` follows rate_limit.on_redis_failure
  };
  Outcome outcome{Outcome::Allowed};
  bool allowed{true};
  std::optional<Scope> scope;               // Limited: the bucket that was empty
  std::chrono::milliseconds retry_after{0};  // Limited: time until that bucket has a token
  std::string detail;                        // Degraded: why Redis could not be asked
};

// Distributed Token Bucket rate limiter.
//
// A bucket holds up to `capacity` tokens and gains `refill_per_second` tokens each second
// (continuously, not in steps); every request takes one token from EACH bucket that applies
// to it: its client address (ip), its API key (api_key, when the request carries one) and the
// service it addresses (service, with optional per-service limits).
//
// State and arithmetic live in Redis. One Lua script, executed atomically by Redis, reads all
// the request's buckets, refills them from Redis's own clock (so every EdgeFlow instance, and
// every thread of one, sees the same time), and takes a token from each ONLY if every bucket
// has one: a request refused by one scope does not use up the tokens of the others, and two
// instances can never both take the last token. Nothing is kept in EdgeFlow's memory.
//
// Keys: "<prefix>:rl:ip:<address>", "<prefix>:rl:key:<sha256 of the API key>" (the key itself
// is never written to Redis), "<prefix>:rl:svc:<service>". A bucket's key expires once it
// would be full again (plus a second), so idle clients leave nothing behind.
//
// acquire() never blocks: the callback runs on a Redis I/O thread.
class RateLimiter {
 public:
  using Callback = std::function<void(Decision)>;

  struct Stats {
    std::uint64_t allowed{0};
    std::uint64_t limited{0};
    std::uint64_t degraded_allowed{0};   // Redis unavailable, let through (fail open)
    std::uint64_t degraded_rejected{0};  // Redis unavailable, refused (fail closed)
  };

  RateLimiter(std::shared_ptr<redis::RedisClient> client, config::RateLimitConfig config,
              std::string key_prefix, std::shared_ptr<logging::Logger> logger);

  void acquire(const Identity& identity, Callback callback);

  // The buckets that apply to `identity` (empty when no scope applies). Public for tests.
  [[nodiscard]] std::vector<Bucket> bucketsFor(const Identity& identity) const;

  // The request header the API key is read from.
  [[nodiscard]] const std::string& apiKeyHeader() const noexcept { return config_.api_key.header; }
  [[nodiscard]] bool apiKeyEnabled() const noexcept { return config_.api_key.enabled; }

  [[nodiscard]] Stats stats() const;

 private:
  void evaluate(std::vector<Bucket> buckets, Callback callback, bool use_script_hash);
  void loadScript();
  [[nodiscard]] Decision degraded(std::string detail);
  [[nodiscard]] std::string scriptHash();

  const std::shared_ptr<redis::RedisClient> client_;
  const config::RateLimitConfig config_;
  const std::string prefix_;
  const std::shared_ptr<logging::Logger> logger_;

  std::mutex script_mutex_;
  std::string script_hash_;  // SHA1 reported by SCRIPT LOAD; empty until known
  bool loading_{false};

  std::atomic<std::uint64_t> allowed_{0};
  std::atomic<std::uint64_t> limited_{0};
  std::atomic<std::uint64_t> degraded_allowed_{0};
  std::atomic<std::uint64_t> degraded_rejected_{0};
};

}  // namespace edgeflow::ratelimit
