#include "edgeflow/proxy/ProxyHandler.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "edgeflow/proxy/ProxyHeaders.hpp"

namespace edgeflow::proxy {

namespace net = boost::asio;
namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

namespace {

constexpr std::chrono::seconds kStopWait{10};

http::status statusFor(discovery::RegistryErrorCode code) {
  using Code = discovery::RegistryErrorCode;
  switch (code) {
    case Code::InvalidArgument: return http::status::bad_request;
    case Code::ServiceNotFound: return http::status::not_found;
    case Code::NoRoutableInstance:
    case Code::DatabaseUnavailable: return http::status::service_unavailable;
    case Code::InstanceNotFound:
    case Code::DuplicateInstance:
    case Code::Internal: break;
  }
  return http::status::internal_server_error;
}

http::status statusFor(UpstreamError error) {
  switch (error) {
    case UpstreamError::Timeout:
    case UpstreamError::ConnectTimeout: return http::status::gateway_timeout;
    case UpstreamError::Cancelled: return http::status::service_unavailable;
    case UpstreamError::Resolve:
    case UpstreamError::Connect:
    case UpstreamError::Closed:
    case UpstreamError::Malformed:
    case UpstreamError::TooLarge:
    case UpstreamError::Internal: break;
  }
  return http::status::bad_gateway;
}

// What the client is told. Deliberately free of internal addresses and error texts; the log
// has those.
std::string_view publicDetail(UpstreamError error) {
  switch (error) {
    case UpstreamError::Resolve:
    case UpstreamError::Connect: return "the backend could not be reached";
    case UpstreamError::Closed: return "the backend closed the connection without a complete response";
    case UpstreamError::Malformed: return "the backend sent an invalid response";
    case UpstreamError::TooLarge: return "the backend response is too large";
    case UpstreamError::Timeout:
    case UpstreamError::ConnectTimeout: return "the backend did not respond in time";
    case UpstreamError::Cancelled: return "the request was cancelled";
    case UpstreamError::Internal: break;
  }
  return "the request could not be forwarded";
}

std::string_view publicDetail(const discovery::RegistryError& error) {
  using Code = discovery::RegistryErrorCode;
  switch (error.code) {
    case Code::DatabaseUnavailable: return "the service registry is unavailable";
    case Code::NoRoutableInstance:
    case Code::ServiceNotFound:
    case Code::InvalidArgument: return error.message;
    case Code::InstanceNotFound:
    case Code::DuplicateInstance:
    case Code::Internal: break;
  }
  return "routing failed";
}

}  // namespace

// One proxied request, from the moment it is accepted until its response was handed back and
// every connection count it took was released. Shared between the lookup thread pool, the
// upstream I/O threads and (as a weak reference) the client connection's cancel function.
//
// Phase 7: the request may make several ATTEMPTS. Each attempt is: route (failover-aware) ->
// circuit-breaker admission -> connection count +1 -> one upstream exchange -> classification
// -> breaker accounting -> connection count -1 -> decision (answer, or back off and go again).
// The count of an attempt is released before the next attempt starts and before any response
// is handed back. Without a ReliabilityManager the loop runs exactly once (Phase 6).
//
// Threads: attempt()/endAttempt() run on the lookup pool (they may block on PostgreSQL),
// forward()/onUpstream() on the upstream I/O threads, the backoff timer on this operation's
// own strand. No lock is held across any of that; `mutex_` guards only the few fields that
// cancel() touches from other threads.
class ProxyHandler::Operation : public std::enable_shared_from_this<Operation> {
 public:
  Operation(ProxyHandler& owner, const HttpRequest& request, const network::RequestContext& context,
            network::ResponseCallback done, std::string request_id, ProxyRoute route,
            std::uint64_t id)
      : owner_(owner),
        rel_(owner.reliability_.get()),
        request_(request),
        client_address_(context.client_address),
        request_id_(std::move(request_id)),
        route_(std::move(route)),
        id_(id),
        started_(std::chrono::steady_clock::now()),
        budget_end_(rel_ != nullptr ? started_ + rel_->totalTimeout()
                                    : std::chrono::steady_clock::time_point::max()),
        strand_(net::make_strand(owner.io_)),
        done_(std::move(done)) {}

  // Hands the registry work (routing + counter increment) to the lookup pool.
  void start() {
    net::post(owner_.lookup_pool_, [self = shared_from_this()] { self->attempt(); });
  }

  // Thread-safe, idempotent. The client went away, or the proxy is stopping.
  void cancel() {
    std::shared_ptr<UpstreamCall> call;
    std::shared_ptr<net::steady_timer> timer;
    {
      const std::lock_guard lock(mutex_);
      if (cancelled_) return;
      cancelled_ = true;
      call = call_;
      timer = timer_;
    }
    if (call) call->cancel();
    if (timer) net::post(strand_, [timer] { timer->cancel(); });
  }

 private:
  static constexpr unsigned kMaxAdmissionTries = 3;

  bool isCancelled() {
    const std::lock_guard lock(mutex_);
    return cancelled_;
  }

  // Time left in the request's total budget; effectively unlimited without reliability.
  std::chrono::milliseconds remainingBudget() const {
    if (rel_ == nullptr) return std::chrono::milliseconds::max();
    return std::chrono::duration_cast<std::chrono::milliseconds>(budget_end_ -
                                                                  std::chrono::steady_clock::now());
  }

  // --- one attempt: choose a backend (lookup thread: may block on PostgreSQL) ---

  discovery::Result<discovery::ServiceInstance> chooseBackend(
      const std::set<std::string>& refused) {
    const routing::RoutingContext context{client_address_};
    if (rel_ == nullptr) return owner_.router_->route(route_.service, context);

    reliability::CircuitBreakerRegistry* breakers = rel_->breakers();
    routing::Router::Filter filter;
    // Never a backend whose circuit refuses (or just refused) the request.
    filter.eligible = [breakers, &refused](const discovery::ServiceInstance& instance) {
      const auto key = reliability::backendKey(instance.service, instance.instance_id,
                                               instance.host, instance.port);
      if (refused.count(key) != 0) return false;
      if (breakers == nullptr) return true;
      const auto breaker = breakers->find(key);
      return breaker == nullptr || breaker->isAvailable();
    };
    // Failover: prefer a backend this request has not tried yet; when every eligible one was
    // tried (a single instance, or all failed), a tried one may be used again.
    filter.preferred = [this](const discovery::ServiceInstance& instance) {
      return !ledger_.tried(reliability::backendKey(instance.service, instance.instance_id,
                                                    instance.host, instance.port));
    };
    return owner_.router_->route(route_.service, context, filter);
  }

  void attempt() {
    if (isCancelled()) {
      finish(failure(http::status::service_unavailable, "the request was cancelled"));
      return;
    }
    if (rel_ != nullptr && remainingBudget() <= std::chrono::milliseconds{0}) {
      budgetExhausted("the request exceeded its time budget");
      return;
    }

    instance_ = discovery::ServiceInstance{};
    counted_ = false;
    std::set<std::string> refused;  // circuits that turned this request down just now
    for (unsigned tries = 1;; ++tries) {
      auto routed = chooseBackend(refused);
      if (!routed.ok()) {
        routingFailed(routed.error());
        return;
      }
      instance_ = std::move(routed).value();
      key_ = reliability::backendKey(instance_.service, instance_.instance_id, instance_.host,
                                     instance_.port);
      if (rel_ == nullptr || rel_->breakers() == nullptr) break;

      breaker_ = rel_->breakers()->get(key_);
      admission_ = breaker_->tryAcquire();
      if (admission_.admitted) break;
      // Raced with another request for the last half-open probe, or the circuit opened between
      // the filter and now: choose again, without this backend.
      rel_->noteCircuitRejection();
      breaker_.reset();
      refused.insert(key_);
      if (tries >= kMaxAdmissionTries) {
        routingFailed(discovery::RegistryError{discovery::RegistryErrorCode::NoRoutableInstance,
                                               "service " + route_.service +
                                                   " has no eligible instance"});
        return;
      }
    }

    if (isCancelled()) {
      settleBreaker(reliability::AttemptKind::Cancelled);
      finish(failure(http::status::service_unavailable, "the request was cancelled"));
      return;
    }

    ++attempts_made_;
    ledger_.markTried(key_);
    if (rel_ != nullptr) {
      rel_->noteAttempt();
      if (attempts_made_ > 1) rel_->noteRetry(key_ != previous_key_);
    }
    previous_key_ = key_;

    // From here on this attempt counts as one active connection of the chosen instance, until
    // endAttempt().
    const auto adjusted =
        owner_.registry_->adjustConnectionCount(instance_.service, instance_.instance_id, 1);
    if (adjusted.ok()) {
      counted_ = true;
    } else {
      owner_.logger_->warn("proxy [{}]: cannot count the request against {}/{}: {}", request_id_,
                           instance_.service, instance_.instance_id, adjusted.error().message);
    }
    net::post(owner_.io_, [self = shared_from_this()] { self->forward(); });
  }

  // Nothing could be chosen. After a failed attempt the client gets what that attempt
  // produced (it is the more useful news); with no attempt made at all it is a routing error.
  void routingFailed(const discovery::RegistryError& error) {
    if (attempts_made_ > 0) {
      owner_.logger_->warn("proxy [{}] {} {} (service {}): no backend left for another attempt "
                           "({}): {}",
                           request_id_, std::string_view{request_.method_string()},
                           std::string_view{request_.target()}, route_.service,
                           discovery::toString(error.code), error.message);
      if (rel_ != nullptr) rel_->noteExhausted();
      finishWith(result_);
      return;
    }
    logFailure(statusFor(error.code), "routing failed (" +
                                          std::string{discovery::toString(error.code)} + "): " +
                                          error.message);
    finish(failure(statusFor(error.code), publicDetail(error)));
  }

  // The total budget ran out between attempts.
  void budgetExhausted(std::string_view detail) {
    if (rel_ != nullptr) rel_->noteExhausted();
    if (attempts_made_ > 0) {
      finishWith(result_);
      return;
    }
    logFailure(http::status::gateway_timeout, std::string{detail});
    finish(failure(http::status::gateway_timeout, detail));
  }

  // --- one attempt: the exchange (upstream I/O thread) ---

  void forward() {
    if (isCancelled()) {
      cancelledBeforeSend();
      return;
    }
    ForwardInfo info;
    info.client_address = client_address_;
    info.upstream_host = hostHeaderFor(instance_.host, instance_.port);
    info.request_id = request_id_;  // the same id on every attempt
    auto upstream_request = makeUpstreamRequest(request_, route_.upstream_target, info);

    std::optional<std::chrono::milliseconds> cap;
    if (rel_ != nullptr) cap = std::max(remainingBudget(), std::chrono::milliseconds{1});

    auto call = owner_.client_->send(
        UpstreamEndpoint{instance_.host, instance_.port}, std::move(upstream_request),
        [self = shared_from_this()](UpstreamResult result) { self->onUpstream(std::move(result)); },
        cap);
    bool cancel_now = false;
    {
      const std::lock_guard lock(mutex_);
      call_ = call;
      cancel_now = cancelled_;
    }
    if (cancel_now) call->cancel();
  }

  void cancelledBeforeSend() {
    UpstreamResult cancelled;
    cancelled.failure = UpstreamFailure{UpstreamError::Cancelled, "cancelled"};
    onUpstream(std::move(cancelled));
  }

  void onUpstream(UpstreamResult result) {
    {
      const std::lock_guard lock(mutex_);
      call_.reset();
    }
    result_ = std::move(result);
    // Releasing the count blocks on PostgreSQL: not on this I/O thread.
    net::post(owner_.lookup_pool_, [self = shared_from_this()] { self->endAttempt(); });
  }

  // --- end of an attempt (lookup thread) ---

  reliability::AttemptKind classify() const {
    using Kind = reliability::AttemptKind;
    if (result_.ok()) {
      if (rel_ == nullptr) return Kind::Success;
      return rel_->retryPolicy().classifyStatus(result_.response.result_int());
    }
    switch (result_.failure->error) {
      case UpstreamError::Resolve:
      case UpstreamError::Connect:
      case UpstreamError::ConnectTimeout: return Kind::NotSent;
      case UpstreamError::Timeout:
      case UpstreamError::Closed:
      case UpstreamError::Malformed: return Kind::MaybeProcessed;
      case UpstreamError::TooLarge:
      case UpstreamError::Internal: return Kind::Permanent;
      case UpstreamError::Cancelled: return Kind::Cancelled;
    }
    return Kind::Permanent;
  }

  // What the instance's circuit hears about this attempt. A backend that answered (any status
  // not configured as retryable) is a success; transport failures and retryable statuses are
  // failures, but at most ONE failure per backend per request (the rest only free a probe
  // slot), so one request cannot trip a circuit on its own; failures that say nothing about
  // the backend (too large, internal, cancelled) change nothing.
  void settleBreaker(reliability::AttemptKind kind) {
    using Kind = reliability::AttemptKind;
    if (!breaker_) return;
    switch (kind) {
      case Kind::Success: breaker_->recordSuccess(admission_); break;
      case Kind::RetryableStatus:
      case Kind::NotSent:
      case Kind::MaybeProcessed:
        if (ledger_.firstFailureFor(key_)) {
          breaker_->recordFailure(admission_);
        } else {
          breaker_->release(admission_);
        }
        break;
      case Kind::Permanent:
      case Kind::Cancelled: breaker_->release(admission_); break;
    }
    breaker_.reset();
    admission_ = reliability::Admission{};
  }

  void releaseCount() {
    if (!counted_) return;
    counted_ = false;
    const auto adjusted =
        owner_.registry_->adjustConnectionCount(instance_.service, instance_.instance_id, -1);
    if (!adjusted.ok()) {
      owner_.logger_->warn("proxy [{}]: cannot release the connection count of {}/{}: {}",
                           request_id_, instance_.service, instance_.instance_id,
                           adjusted.error().message);
    }
  }

  void endAttempt() {
    const auto kind = classify();
    logAttempt(kind);
    settleBreaker(kind);
    releaseCount();  // before anything is handed back or retried

    if (rel_ == nullptr || isCancelled()) {
      finishWith(result_);
      return;
    }
    const auto decision = rel_->retryPolicy().decide(
        kind, std::string_view{request_.method_string()}, attempts_made_);
    if (!decision.retry) {
      if (decision.reason == reliability::RetryReason::AttemptsExhausted) rel_->noteExhausted();
      finishWith(result_);
      return;
    }
    const auto delay = rel_->backoffDelay(attempts_made_);
    if (delay >= remainingBudget()) {
      owner_.logger_->warn("proxy [{}] {} {}: no time budget left for another attempt after {}ms "
                           "of backoff; answering with the last result",
                           request_id_, std::string_view{request_.method_string()},
                           std::string_view{request_.target()}, delay.count());
      rel_->noteExhausted();
      finishWith(result_);
      return;
    }
    owner_.logger_->info("proxy [{}] {} {} (service {}): attempt {} of {} failed ({}); retrying "
                         "in {}ms",
                         request_id_, std::string_view{request_.method_string()},
                         std::string_view{request_.target()}, route_.service, attempts_made_,
                         rel_->maxAttempts(), reliability::toString(kind), delay.count());
    scheduleRetry(delay);
  }

  // The pause between attempts: a timer on this operation's strand that cancel() can abort at
  // once (a request cancelled while backing off never waits for the timer).
  void scheduleRetry(std::chrono::milliseconds delay) {
    auto timer = std::make_shared<net::steady_timer>(strand_);
    {
      const std::lock_guard lock(mutex_);
      timer_ = timer;
    }
    net::post(strand_, [self = shared_from_this(), timer, delay] {
      if (self->isCancelled()) {
        self->clearTimer();
        self->finish(self->failure(http::status::service_unavailable, "the request was cancelled"));
        return;
      }
      timer->expires_after(delay);
      timer->async_wait([self, timer](const boost::system::error_code& error) {
        self->clearTimer();
        if (error) {  // cancelled while backing off
          self->finish(self->failure(http::status::service_unavailable,
                                     "the request was cancelled"));
          return;
        }
        net::post(self->owner_.lookup_pool_, [self] { self->attempt(); });
      });
    });
  }

  void clearTimer() {
    const std::lock_guard lock(mutex_);
    timer_.reset();
  }

  // --- answering ---

  HttpResponse failure(http::status status, std::string_view detail) {
    owner_.gateway_errors_.fetch_add(1);
    return makeGatewayError(request_, status, detail, request_id_);
  }

  // The client's answer is the outcome of the last attempt.
  void finishWith(UpstreamResult& last) {
    if (last.ok()) {
      owner_.responses_.fetch_add(1);
      finish(makeClientResponse(std::move(last.response), request_, request_id_));
      return;
    }
    finish(failure(statusFor(last.failure->error), publicDetail(last.failure->error)));
  }

  std::int64_t elapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started_)
        .count();
  }

  void logAttempt(reliability::AttemptKind kind) {
    const std::string attempt_text =
        rel_ != nullptr ? " [attempt " + std::to_string(attempts_made_) + "/" +
                              std::to_string(rel_->maxAttempts()) + "]"
                        : std::string{};
    if (result_.ok()) {
      owner_.logger_->debug("proxy [{}]{} {} {} -> {}/{} {}:{}: {} in {}ms{}{}{}", request_id_,
                            attempt_text, std::string_view{request_.method_string()},
                            std::string_view{request_.target()}, instance_.service,
                            instance_.instance_id, instance_.host, instance_.port,
                            result_.response.result_int(), elapsedMs(),
                            result_.reused_connection ? " (reused connection)" : "",
                            result_.stale_connection_replaced
                                ? " (stale pooled connection replaced)"
                                : "",
                            kind == reliability::AttemptKind::RetryableStatus
                                ? " (retryable status)"
                                : "");
      return;
    }
    const auto& failure_info = *result_.failure;
    logFailure(statusFor(failure_info.error), std::string{"upstream "} +
                                                  toString(failure_info.error) + ": " +
                                                  failure_info.detail + attempt_text);
  }

  void logFailure(http::status status, const std::string& what) {
    if (!instance_.instance_id.empty()) {
      owner_.logger_->warn("proxy [{}] {} {} -> {}/{} {}:{}: {} ({}) in {}ms", request_id_,
                           std::string_view{request_.method_string()},
                           std::string_view{request_.target()}, instance_.service,
                           instance_.instance_id, instance_.host, instance_.port, what,
                           static_cast<unsigned>(status), elapsedMs());
      return;
    }
    // No backend was chosen: a client mistake (4xx) is informational, an outage is not.
    const auto text = std::string{std::string_view{request_.method_string()}} + " " +
                      std::string{std::string_view{request_.target()}};
    if (status >= http::status::internal_server_error) {
      owner_.logger_->warn("proxy [{}] {} (service {}): {} ({})", request_id_, text,
                           route_.service, what, static_cast<unsigned>(status));
    } else {
      owner_.logger_->info("proxy [{}] {} (service {}): {} ({})", request_id_, text,
                           route_.service, what, static_cast<unsigned>(status));
    }
  }

  // The single exit: every attempt's connection count is already released, so the response
  // can be handed back at once; a client that sends its next request immediately (and Least
  // Connections, which reads the count) never sees a finished request as active.
  void finish(HttpResponse response) {
    if (completed_.exchange(true)) return;
    network::ResponseCallback done;
    {
      const std::lock_guard lock(mutex_);
      done = std::move(done_);
      done_ = nullptr;
    }
    if (done) done(std::move(response));
    owner_.unregisterOperation(id_);
  }

  ProxyHandler& owner_;
  reliability::ReliabilityManager* const rel_;  // null: Phase 6 behaviour, one attempt
  const HttpRequest request_;
  const std::string client_address_;
  const std::string request_id_;
  const ProxyRoute route_;
  const std::uint64_t id_;
  const std::chrono::steady_clock::time_point started_;
  const std::chrono::steady_clock::time_point budget_end_;
  net::strand<net::io_context::executor_type> strand_;

  std::mutex mutex_;  // guards done_, call_, timer_, cancelled_
  network::ResponseCallback done_;
  std::shared_ptr<UpstreamCall> call_;
  std::shared_ptr<net::steady_timer> timer_;
  bool cancelled_{false};

  // The fields below belong to one step at a time; the hand-offs between steps (post to the
  // lookup pool, the I/O threads or the strand) order every access.
  unsigned attempts_made_{0};
  reliability::AttemptLedger ledger_;
  std::string previous_key_;
  discovery::ServiceInstance instance_;  // the backend of the current attempt
  std::string key_;                      // its reliability::backendKey
  bool counted_{false};                  // its connection count is +1 at the registry
  std::shared_ptr<reliability::CircuitBreaker> breaker_;  // set while an admission is held
  reliability::Admission admission_;
  UpstreamResult result_;  // outcome of the latest attempt (the answer when no more are made)
  std::atomic<bool> completed_{false};
};

ProxyHandler::ProxyHandler(std::shared_ptr<network::RequestHandler> next,
                           std::shared_ptr<routing::Router> router,
                           std::shared_ptr<discovery::ServiceRegistry> registry,
                           config::ProxyConfig config, unsigned lookup_threads,
                           std::shared_ptr<logging::Logger> logger,
                           std::shared_ptr<discovery::NameResolver> names,
                           UpstreamPool::Clock pool_clock,
                           std::shared_ptr<reliability::ReliabilityManager> reliability)
    : next_(std::move(next)),
      router_(std::move(router)),
      registry_(std::move(registry)),
      config_(config),
      reliability_(std::move(reliability)),
      logger_(std::move(logger)),
      io_(static_cast<int>(config.io_threads)),
      work_guard_(std::make_unique<WorkGuard>(io_.get_executor())),
      pool_(std::make_shared<UpstreamPool>(
          UpstreamPool::Settings{config.max_idle_connections, config.idle_timeout},
          std::move(pool_clock))),
      client_(std::make_unique<UpstreamClient>(
          io_, pool_, names ? std::move(names) : std::make_shared<discovery::NameResolver>(),
          UpstreamSettings{config.connect_timeout, config.upstream_timeout,
                           config.max_response_bytes},
          logger_)),
      lookup_pool_(std::max(1U, lookup_threads)) {
  io_threads_.reserve(config_.io_threads);
  for (unsigned i = 0; i < config_.io_threads; ++i) {
    io_threads_.emplace_back([this] {
      while (!io_.stopped()) {
        try {
          io_.run();
        } catch (const std::exception& e) {
          logger_->error("proxy I/O thread: unexpected exception: {}", e.what());
        }
      }
    });
  }
  logger_->info("reverse proxy ready: /proxy/{{service}}/..., connect timeout {}ms, upstream "
                "timeout {}ms, up to {} idle connection(s) per backend, {} I/O thread(s)",
                config_.connect_timeout.count(), config_.upstream_timeout.count(),
                config_.max_idle_connections, config_.io_threads);
  if (reliability_) {
    const auto& r = reliability_->config();
    logger_->info("reliability ready: total timeout {}ms, retries {} (up to {} attempt(s), "
                  "backoff {}-{}ms), circuit breaker {} (opens after {} consecutive failure(s), "
                  "recovery {}ms, {} probe(s))",
                  r.timeout.total_timeout.count(), r.retry.enabled ? "on" : "off",
                  reliability_->maxAttempts(), r.retry.base_delay.count(),
                  r.retry.max_delay.count(), r.circuit_breaker.enabled ? "on" : "off",
                  r.circuit_breaker.failure_threshold, r.circuit_breaker.recovery_timeout.count(),
                  r.circuit_breaker.half_open_max_requests);
  }
}

ProxyHandler::~ProxyHandler() {
  try {
    stop();
  } catch (...) {
    // Destructors must not throw; stop() only fails on resource exhaustion.
  }
}

HttpResponse ProxyHandler::handle(const HttpRequest& request) {
  if (!isProxyTarget(request.target())) return next_->handle(request);
  std::promise<HttpResponse> promise;
  auto future = promise.get_future();
  (void)handleAsync(request, network::RequestContext{},
                    [&promise](HttpResponse response) { promise.set_value(std::move(response)); });
  return future.get();
}

network::CancelFunction ProxyHandler::handleAsync(const HttpRequest& request,
                                                  const network::RequestContext& context,
                                                  network::ResponseCallback done) {
  if (!isProxyTarget(request.target())) return next_->handleAsync(request, context, std::move(done));

  const std::string request_id = chooseRequestId(request);
  auto mapped = mapProxyTarget(request.target());
  if (!mapped.route) {
    logger_->info("proxy [{}] {} {}: rejected: {}", request_id,
                  std::string_view{request.method_string()}, std::string_view{request.target()},
                  mapped.detail);
    done(makeGatewayError(request, http::status::bad_request, mapped.detail, request_id));
    return {};
  }

  std::shared_ptr<Operation> operation;
  {
    const std::lock_guard lock(mutex_);
    if (!stopped_) {
      const auto id = next_operation_id_++;
      operation = std::make_shared<Operation>(*this, request, context, done, request_id,
                                              std::move(*mapped.route), id);
      operations_.emplace(id, operation);
    }
  }
  if (!operation) {
    done(makeGatewayError(request, http::status::service_unavailable, "EdgeFlow is shutting down",
                          request_id));
    return {};
  }
  requests_.fetch_add(1);
  operation->start();
  return [weak = std::weak_ptr<Operation>(operation)] {
    if (const auto live = weak.lock()) live->cancel();
  };
}

void ProxyHandler::unregisterOperation(std::uint64_t id) {
  const std::lock_guard lock(mutex_);
  operations_.erase(id);
  if (operations_.empty()) idle_cv_.notify_all();
}

void ProxyHandler::stop() {
  const std::lock_guard stop_lock(stop_mutex_);
  if (stopped_done_) return;

  std::vector<std::shared_ptr<Operation>> running;
  {
    const std::lock_guard lock(mutex_);
    stopped_ = true;  // from now on new proxy requests are answered 503
    for (const auto& [id, weak] : operations_) {
      (void)id;
      if (auto operation = weak.lock()) running.push_back(std::move(operation));
    }
  }
  for (const auto& operation : running) operation->cancel();
  running.clear();

  {
    // Cancelled operations finish on their own threads; wait until every one has handed its
    // response back and released its connection count.
    std::unique_lock lock(mutex_);
    if (!idle_cv_.wait_for(lock, kStopWait, [this] { return operations_.empty(); })) {
      logger_->error("reverse proxy: {} request(s) did not finish within {}s of stopping",
                     operations_.size(), kStopWait.count());
    }
  }

  pool_->close();
  work_guard_.reset();
  io_.stop();
  for (auto& thread : io_threads_) {
    if (thread.joinable()) thread.join();
  }
  io_threads_.clear();
  lookup_pool_.join();
  stopped_done_ = true;
  logger_->info("reverse proxy stopped");
}

ProxyHandler::Stats ProxyHandler::stats() const {
  Stats stats;
  stats.requests = requests_.load();
  stats.responses = responses_.load();
  stats.gateway_errors = gateway_errors_.load();
  {
    const std::lock_guard lock(mutex_);
    stats.active = operations_.size();
  }
  stats.pool = pool_->stats();
  if (reliability_) stats.reliability = reliability_->stats();
  return stats;
}

}  // namespace edgeflow::proxy
