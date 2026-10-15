#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/Http.hpp"
#include "edgeflow/redis/RedisClient.hpp"

namespace edgeflow::cache {

// The directives of a Cache-Control header (several header lines are joined with commas)
// that the cache looks at. Directive names are case-insensitive; unknown ones are ignored.
struct CacheControl {
  bool no_store{false};
  bool no_cache{false};
  bool is_private{false};
  std::optional<long long> max_age;    // seconds; absent when missing or not a number
  std::optional<long long> s_maxage;   // seconds
};

[[nodiscard]] CacheControl parseCacheControl(std::string_view value);

// A stored response: what is needed to answer a later request without the backend.
struct CachedResponse {
  unsigned status{200};
  std::vector<std::pair<std::string, std::string>> headers;  // in order, repeated names kept
  std::string body;
  std::int64_t stored_at_ms{0};  // wall clock of the gateway that stored it (for Age)
};

// Binary form kept in Redis: "EFC1", status, headers, body, every length explicit. A value
// that does not parse is never served (deserialize returns nullopt).
[[nodiscard]] std::string serialize(const CachedResponse& response);
[[nodiscard]] std::optional<CachedResponse> deserialize(std::string_view bytes);

// Response cache in Redis for proxied requests.
//
// WHICH REQUESTS use the cache (lookup and store): GET requests without a body and without
// Authorization, Cookie, Range, If-* conditions, `Cache-Control: no-store|no-cache` or
// `Pragma: no-cache`. Every other request bypasses it. A successful (< 400) unsafe request
// (POST, PUT, PATCH, DELETE) removes the entry of the same URL.
//
// WHICH RESPONSES are stored: those that came from a backend through the proxy (they carry
// the proxy's Via; gateway-generated errors never do), whose status is in
// cache.cacheable_statuses (200 by default), without Set-Cookie, Vary, Content-Range,
// `Cache-Control: no-store|no-cache|private`, `max-age=0` / `s-maxage=0`, with a body of at most
// cache.max_entry_bytes.
//
// LIFETIME: `s-maxage`, else `max-age`, else cache.default_ttl, never more than cache.max_ttl.
// Redis expires the entry (SET ... PX); an expired entry is gone, not served.
//
// KEY: "<prefix>:cache:v1:" + SHA-256 of "GET", the service and the upstream target (path and
// query string exactly as sent). No header takes part in the key, which is why a response
// with a Vary header is never stored.
//
// Not implemented (and not claimed): revalidation (ETag, If-None-Match, 304), Expires, Vary,
// stale-while-revalidate, request coalescing. A concurrent miss of the same URL is forwarded
// to the backend by each request.
//
// Redis problems never fail a request: a lookup that cannot be answered is reported as an
// Error (the caller bypasses the cache), a store that fails is only logged.
class ResponseCache {
 public:
  struct Lookup {
    enum class Outcome { Hit, Miss, Error };
    Outcome outcome{Outcome::Miss};
    CachedResponse entry;  // Hit only
    std::string detail;    // Error only
  };
  using LookupCallback = std::function<void(Lookup)>;

  struct Stats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t errors{0};          // lookups Redis could not answer (or an unreadable entry)
    std::uint64_t stores{0};          // entries written successfully
    std::uint64_t store_failures{0};
    std::uint64_t invalidations{0};
  };

  ResponseCache(std::shared_ptr<redis::RedisClient> client, config::CacheConfig config,
                std::string key_prefix, std::shared_ptr<logging::Logger> logger);

  [[nodiscard]] static bool requestUsesCache(const network::HttpRequest& request);
  [[nodiscard]] static bool isUnsafeMethod(const network::HttpRequest& request);

  [[nodiscard]] std::string keyFor(std::string_view service, std::string_view upstream_target) const;

  void lookup(const std::string& key, LookupCallback callback);

  // The lifetime the response may be stored for, or nullopt when it must not be stored.
  [[nodiscard]] std::optional<std::chrono::seconds> storableTtl(
      const network::HttpResponse& response) const;

  // Stores `response` for `ttl` (fire and forget).
  void store(const std::string& key, const network::HttpResponse& response,
             std::chrono::seconds ttl);
  void invalidate(const std::string& key);

  // The answer to `request` from a stored entry: the stored status, headers and body, the
  // client's HTTP version, `X-Request-Id` of THIS request, `Age`, and `X-Cache: HIT`.
  [[nodiscard]] static network::HttpResponse toResponse(const CachedResponse& entry,
                                                        const network::HttpRequest& request,
                                                        std::string_view request_id);

  [[nodiscard]] Stats stats() const;

 private:
  const std::shared_ptr<redis::RedisClient> client_;
  const config::CacheConfig config_;
  const std::string prefix_;
  const std::shared_ptr<logging::Logger> logger_;

  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
  std::atomic<std::uint64_t> errors_{0};
  std::atomic<std::uint64_t> stores_{0};
  std::atomic<std::uint64_t> store_failures_{0};
  std::atomic<std::uint64_t> invalidations_{0};
};

}  // namespace edgeflow::cache
