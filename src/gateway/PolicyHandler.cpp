#include "edgeflow/gateway/PolicyHandler.hpp"

#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "edgeflow/proxy/ProxyHeaders.hpp"

namespace edgeflow::gateway {

namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

namespace {

constexpr std::size_t kMaxApiKeyBytes = 1024;

std::string trimmed(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return std::string{text};
}

}  // namespace

// One proxied request while it passes the rate limiter and the cache. Shared between the
// Redis I/O threads (the steps), the proxy's threads (the answer) and, weakly, the client
// connection's cancel function.
class PolicyHandler::Operation : public std::enable_shared_from_this<Operation> {
 public:
  Operation(PolicyHandler& owner, const HttpRequest& request, const network::RequestContext& context,
            network::ResponseCallback done, std::string request_id, proxy::ProxyRoute route)
      : owner_(owner),
        request_(request),
        context_(context),
        request_id_(std::move(request_id)),
        route_(std::move(route)),
        done_(std::move(done)) {
    // The proxy keeps an id the client chose: handing it ours makes every answer agree.
    request_.set(proxy::kRequestIdHeader, request_id_);
  }

  void start() {
    if (owner_.limiter_) {
      ratelimit::Identity identity;
      identity.client_ip = context_.client_address;
      identity.service = route_.service;
      if (owner_.limiter_->apiKeyEnabled()) {
        const auto it = request_.find(owner_.limiter_->apiKeyHeader());
        if (it != request_.end()) {
          identity.api_key = trimmed(it->value());
          if (identity.api_key.size() > kMaxApiKeyBytes) identity.api_key.resize(kMaxApiKeyBytes);
        }
      }
      owner_.limiter_->acquire(identity, [self = shared_from_this()](ratelimit::Decision decision) {
        self->afterRateLimit(decision);
      });
      return;
    }
    cacheStep();
  }

  // Thread-safe, idempotent: the client went away or the server is shutting down.
  void cancel() {
    network::CancelFunction downstream;
    {
      const std::lock_guard lock(mutex_);
      if (cancelled_) return;
      cancelled_ = true;
      downstream = downstream_;
    }
    if (downstream) downstream();
  }

 private:
  bool isCancelled() {
    const std::lock_guard lock(mutex_);
    return cancelled_;
  }

  void afterRateLimit(const ratelimit::Decision& decision) {
    if (isCancelled()) {
      finish(proxy::makeGatewayError(request_, http::status::service_unavailable,
                                     "the request was cancelled", request_id_));
      return;
    }
    if (decision.allowed) {
      if (decision.outcome == ratelimit::Decision::Outcome::Degraded) {
        owner_.logger_->debug("policy [{}] {}: rate limiting unavailable ({}); letting the request "
                              "through", request_id_, std::string_view{request_.target()},
                              decision.detail);
      }
      cacheStep();
      return;
    }
    if (decision.outcome == ratelimit::Decision::Outcome::Limited) {
      owner_.rate_limited_.fetch_add(1);
      auto response = proxy::makeGatewayError(request_, http::status::too_many_requests,
                                              "rate limit exceeded", request_id_);
      const auto seconds = std::max<long long>(1, (decision.retry_after.count() + 999) / 1000);
      response.set(http::field::retry_after, std::to_string(seconds));
      if (decision.scope) response.set("X-RateLimit-Scope", ratelimit::toString(*decision.scope));
      owner_.logger_->debug("policy [{}] {} {} (service {}): rate limited by the {} bucket, retry "
                            "in {}ms", request_id_, std::string_view{request_.method_string()},
                            std::string_view{request_.target()}, route_.service,
                            decision.scope ? ratelimit::toString(*decision.scope) : "?",
                            decision.retry_after.count());
      finish(std::move(response));
      return;
    }
    // Redis could not be asked and the policy is to refuse.
    owner_.refused_degraded_.fetch_add(1);
    owner_.logger_->warn("policy [{}] {} {} (service {}): rate limiting is unavailable ({}); "
                         "refusing the request (rate_limit.on_redis_failure: reject)",
                         request_id_, std::string_view{request_.method_string()},
                         std::string_view{request_.target()}, route_.service, decision.detail);
    finish(proxy::makeGatewayError(request_, http::status::service_unavailable,
                                   "rate limiting is unavailable", request_id_));
  }

  void cacheStep() {
    if (!owner_.cache_ || !cache::ResponseCache::requestUsesCache(request_)) {
      forward(std::nullopt);
      return;
    }
    auto key = owner_.cache_->keyFor(route_.service, route_.upstream_target);
    owner_.cache_->lookup(key, [self = shared_from_this(), key](cache::ResponseCache::Lookup lookup) {
      self->afterLookup(key, std::move(lookup));
    });
  }

  void afterLookup(const std::string& key, cache::ResponseCache::Lookup lookup) {
    using Outcome = cache::ResponseCache::Lookup::Outcome;
    if (isCancelled()) {
      finish(proxy::makeGatewayError(request_, http::status::service_unavailable,
                                     "the request was cancelled", request_id_));
      return;
    }
    switch (lookup.outcome) {
      case Outcome::Hit:
        owner_.served_from_cache_.fetch_add(1);
        finish(cache::ResponseCache::toResponse(lookup.entry, request_, request_id_));
        return;
      case Outcome::Miss: forward(key); return;
      case Outcome::Error:
        owner_.logger_->debug("policy [{}] {}: cache unavailable ({}); bypassing it", request_id_,
                              std::string_view{request_.target()}, lookup.detail);
        forward(std::nullopt);
        return;
    }
  }

  // `key`: the cache entry to fill from the answer (a lookup just missed).
  void forward(std::optional<std::string> key) {
    owner_.forwarded_.fetch_add(1);
    auto cancel = owner_.next_->handleAsync(
        request_, context_,
        [self = shared_from_this(), key = std::move(key)](HttpResponse response) mutable {
          self->onResponse(key, std::move(response));
        });
    bool cancel_now = false;
    {
      const std::lock_guard lock(mutex_);
      downstream_ = cancel;
      cancel_now = cancelled_;
    }
    if (cancel_now && cancel) cancel();
  }

  void onResponse(const std::optional<std::string>& key, HttpResponse response) {
    if (owner_.cache_) {
      if (key) {
        if (const auto ttl = owner_.cache_->storableTtl(response)) {
          owner_.cache_->store(*key, response, *ttl);
        }
        response.set("X-Cache", "MISS");
      } else if (cache::ResponseCache::isUnsafeMethod(request_) && response.result_int() < 400) {
        // The resource changed: what is stored under its URL is no longer true.
        owner_.cache_->invalidate(owner_.cache_->keyFor(route_.service, route_.upstream_target));
      }
    }
    finish(std::move(response));
  }

  void finish(HttpResponse response) {
    if (completed_.exchange(true)) return;
    network::ResponseCallback done;
    {
      const std::lock_guard lock(mutex_);
      done = std::move(done_);
      done_ = nullptr;
    }
    if (done) done(std::move(response));
  }

  PolicyHandler& owner_;
  HttpRequest request_;
  const network::RequestContext context_;
  const std::string request_id_;
  const proxy::ProxyRoute route_;

  std::mutex mutex_;  // guards done_, downstream_, cancelled_
  network::ResponseCallback done_;
  network::CancelFunction downstream_;
  bool cancelled_{false};
  std::atomic<bool> completed_{false};
};

PolicyHandler::PolicyHandler(std::shared_ptr<network::RequestHandler> next,
                             std::shared_ptr<ratelimit::RateLimiter> limiter,
                             std::shared_ptr<cache::ResponseCache> cache,
                             std::shared_ptr<logging::Logger> logger)
    : next_(std::move(next)),
      limiter_(std::move(limiter)),
      cache_(std::move(cache)),
      logger_(std::move(logger)) {
  logger_->info("gateway policy ready: rate limiting {}, response cache {}",
                limiter_ ? "on" : "off", cache_ ? "on" : "off");
}

HttpResponse PolicyHandler::handle(const HttpRequest& request) {
  if (!proxy::isProxyTarget(request.target())) return next_->handle(request);
  std::promise<HttpResponse> promise;
  auto future = promise.get_future();
  (void)handleAsync(request, network::RequestContext{},
                    [&promise](HttpResponse response) { promise.set_value(std::move(response)); });
  return future.get();
}

network::CancelFunction PolicyHandler::handleAsync(const HttpRequest& request,
                                                   const network::RequestContext& context,
                                                   network::ResponseCallback done) {
  if (!proxy::isProxyTarget(request.target())) return next_->handleAsync(request, context, std::move(done));
  auto mapped = proxy::mapProxyTarget(request.target());
  if (!mapped.route) {
    // A malformed /proxy URL: the proxy answers it (400); there is nothing to limit or cache.
    return next_->handleAsync(request, context, std::move(done));
  }
  seen_.fetch_add(1);
  auto operation = std::make_shared<Operation>(*this, request, context, std::move(done),
                                               proxy::chooseRequestId(request),
                                               std::move(*mapped.route));
  operation->start();
  return [weak = std::weak_ptr<Operation>(operation)] {
    if (const auto live = weak.lock()) live->cancel();
  };
}

PolicyHandler::Stats PolicyHandler::stats() const {
  Stats stats;
  stats.seen = seen_.load();
  stats.rate_limited = rate_limited_.load();
  stats.refused_degraded = refused_degraded_.load();
  stats.served_from_cache = served_from_cache_.load();
  stats.forwarded = forwarded_.load();
  return stats;
}

}  // namespace edgeflow::gateway
