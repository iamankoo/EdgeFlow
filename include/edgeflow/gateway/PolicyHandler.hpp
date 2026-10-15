#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "edgeflow/cache/ResponseCache.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/RequestHandler.hpp"
#include "edgeflow/ratelimit/RateLimiter.hpp"

namespace edgeflow::gateway {

// Phase 8: rate limiting and response caching in front of the reverse proxy.
//
//   /proxy/{service}/...   1. rate limit (RateLimiter, shared Token Buckets in Redis)
//                              refused  -> 429 Too Many Requests (Retry-After), or
//                                          503 when Redis cannot be asked and
//                                          rate_limit.on_redis_failure is `reject`
//                          2. cache lookup (ResponseCache) for cacheable GET requests
//                              hit      -> answered from Redis, the backend is not contacted
//                              miss     -> forwarded, the answer is stored when it may be
//                              error    -> forwarded as if there were no cache
//                          3. everything else -> the wrapped handler (the proxy, with its
//                             retries, circuit breakers and failover)
//   anything else          passed to the wrapped handler untouched: /health, the registry
//                          API and the local endpoints are never limited or cached.
//
// Every step is asynchronous (Redis I/O threads): no HTTP I/O worker waits for Redis, and the
// proxy is only entered after the rate limit and the cache have answered. A request that is
// refused or answered from the cache never reaches the proxy, so it consumes no retry budget,
// no connection count and no backend capacity. The request id is chosen once here and handed
// to the proxy, so a cache hit, a 429 and a forwarded request all report the id the client
// sent (or one generated for it).
//
// Either collaborator may be null (that feature is off). Redis trouble never crashes or hangs
// a request: the cache is bypassed, the rate limiter follows its failure policy.
class PolicyHandler final : public network::RequestHandler {
 public:
  struct Stats {
    std::uint64_t seen{0};            // proxied requests that entered this handler
    std::uint64_t rate_limited{0};    // answered 429
    std::uint64_t refused_degraded{0};  // answered 503 because Redis was down and the policy is reject
    std::uint64_t served_from_cache{0};
    std::uint64_t forwarded{0};       // handed to the proxy
  };

  PolicyHandler(std::shared_ptr<network::RequestHandler> next,
                std::shared_ptr<ratelimit::RateLimiter> limiter,
                std::shared_ptr<cache::ResponseCache> cache,
                std::shared_ptr<logging::Logger> logger);

  [[nodiscard]] network::HttpResponse handle(const network::HttpRequest& request) override;
  network::CancelFunction handleAsync(const network::HttpRequest& request,
                                      const network::RequestContext& context,
                                      network::ResponseCallback done) override;

  [[nodiscard]] Stats stats() const;
  [[nodiscard]] ratelimit::RateLimiter* limiter() const noexcept { return limiter_.get(); }
  [[nodiscard]] cache::ResponseCache* cache() const noexcept { return cache_.get(); }

 private:
  class Operation;

  const std::shared_ptr<network::RequestHandler> next_;
  const std::shared_ptr<ratelimit::RateLimiter> limiter_;
  const std::shared_ptr<cache::ResponseCache> cache_;
  const std::shared_ptr<logging::Logger> logger_;

  std::atomic<std::uint64_t> seen_{0};
  std::atomic<std::uint64_t> rate_limited_{0};
  std::atomic<std::uint64_t> refused_degraded_{0};
  std::atomic<std::uint64_t> served_from_cache_{0};
  std::atomic<std::uint64_t> forwarded_{0};
};

}  // namespace edgeflow::gateway
