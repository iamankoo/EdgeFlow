#include "edgeflow/proxy/ProxyHandler.hpp"

#include <algorithm>
#include <chrono>
#include <future>
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
// its connection count was released. Shared between the lookup thread pool, the upstream I/O
// threads and (as a weak reference) the client connection's cancel function.
class ProxyHandler::Operation : public std::enable_shared_from_this<Operation> {
 public:
  Operation(ProxyHandler& owner, const HttpRequest& request, const network::RequestContext& context,
            network::ResponseCallback done, std::string request_id, ProxyRoute route,
            std::uint64_t id)
      : owner_(owner),
        request_(request),
        client_address_(context.client_address),
        request_id_(std::move(request_id)),
        route_(std::move(route)),
        id_(id),
        started_(std::chrono::steady_clock::now()),
        done_(std::move(done)) {}

  // Hands the registry work (routing + counter increment) to the lookup pool.
  void start() {
    net::post(owner_.lookup_pool_, [self = shared_from_this()] { self->lookup(); });
  }

  // Thread-safe, idempotent. The client went away, or the proxy is stopping.
  void cancel() {
    std::shared_ptr<UpstreamCall> call;
    {
      const std::lock_guard lock(mutex_);
      if (cancelled_) return;
      cancelled_ = true;
      call = call_;
    }
    if (call) call->cancel();
  }

 private:
  bool isCancelled() {
    const std::lock_guard lock(mutex_);
    return cancelled_;
  }

  // Runs on a lookup thread: may block on PostgreSQL.
  void lookup() {
    if (isCancelled()) {
      finish(failure(http::status::service_unavailable, "the request was cancelled"));
      return;
    }
    auto routed = owner_.router_->route(route_.service, routing::RoutingContext{client_address_});
    if (!routed.ok()) {
      const auto& error = routed.error();
      logFailure(statusFor(error.code), "routing failed (" +
                                            std::string{discovery::toString(error.code)} + "): " +
                                            error.message);
      finish(failure(statusFor(error.code), publicDetail(error)));
      return;
    }
    instance_ = std::move(routed).value();
    if (isCancelled()) {
      finish(failure(http::status::service_unavailable, "the request was cancelled"));
      return;
    }

    // From here on this request counts as one active connection of the chosen instance, until
    // release() (always reached through finish()).
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

  // Runs on an upstream I/O thread.
  void forward() {
    if (isCancelled()) {
      finish(failure(http::status::service_unavailable, "the request was cancelled"));
      return;
    }
    ForwardInfo info;
    info.client_address = client_address_;
    info.upstream_host = hostHeaderFor(instance_.host, instance_.port);
    info.request_id = request_id_;
    auto upstream_request = makeUpstreamRequest(request_, route_.upstream_target, info);

    auto call = owner_.client_->send(
        UpstreamEndpoint{instance_.host, instance_.port}, std::move(upstream_request),
        [self = shared_from_this()](UpstreamResult result) { self->onUpstream(std::move(result)); });
    bool cancel_now = false;
    {
      const std::lock_guard lock(mutex_);
      call_ = call;
      cancel_now = cancelled_;
    }
    if (cancel_now) call->cancel();
  }

  void onUpstream(UpstreamResult result) {
    {
      const std::lock_guard lock(mutex_);
      call_.reset();
    }
    if (result.ok()) {
      owner_.responses_.fetch_add(1);
      logForwarded(result.response.result_int(), result.reused_connection,
                   result.stale_connection_replaced);
      finish(makeClientResponse(std::move(result.response), request_, request_id_));
      return;
    }
    const auto& failure_info = *result.failure;
    const auto status = statusFor(failure_info.error);
    logFailure(status, std::string{"upstream "} + toString(failure_info.error) + ": " +
                           failure_info.detail);
    finish(failure(status, publicDetail(failure_info.error)));
  }

  HttpResponse failure(http::status status, std::string_view detail) {
    owner_.gateway_errors_.fetch_add(1);
    return makeGatewayError(request_, status, detail, request_id_);
  }

  std::int64_t elapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started_)
        .count();
  }

  void logForwarded(unsigned status, bool reused, bool replaced) {
    owner_.logger_->debug("proxy [{}] {} {} -> {}/{} {}:{}: {} in {}ms{}{}", request_id_,
                          std::string_view{request_.method_string()},
                          std::string_view{request_.target()}, instance_.service,
                          instance_.instance_id, instance_.host, instance_.port, status,
                          elapsedMs(), reused ? " (reused connection)" : "",
                          replaced ? " (stale pooled connection replaced)" : "");
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

  // The single exit. The response is parked and the rest happens on a lookup thread:
  // first the connection count is released (a blocking registry call), THEN the response is
  // handed back and the operation unregistered. So by the time a client holds its response
  // the instance no longer counts the request: a client that sends its next request at once
  // (and Least Connections, which reads the count) never sees a finished request as active.
  void finish(HttpResponse response) {
    if (completed_.exchange(true)) return;
    {
      const std::lock_guard lock(mutex_);
      response_ = std::move(response);
    }
    net::post(owner_.lookup_pool_, [self = shared_from_this()] { self->release(); });
  }

  void release() {
    if (counted_) {
      const auto adjusted =
          owner_.registry_->adjustConnectionCount(instance_.service, instance_.instance_id, -1);
      if (!adjusted.ok()) {
        owner_.logger_->warn("proxy [{}]: cannot release the connection count of {}/{}: {}",
                             request_id_, instance_.service, instance_.instance_id,
                             adjusted.error().message);
      }
    }
    network::ResponseCallback done;
    HttpResponse response;
    {
      const std::lock_guard lock(mutex_);
      done = std::move(done_);
      done_ = nullptr;
      response = std::move(response_);
    }
    if (done) done(std::move(response));
    owner_.unregisterOperation(id_);
  }

  ProxyHandler& owner_;
  const HttpRequest request_;
  const std::string client_address_;
  const std::string request_id_;
  const ProxyRoute route_;
  const std::uint64_t id_;
  const std::chrono::steady_clock::time_point started_;

  std::mutex mutex_;  // guards done_, response_, call_, cancelled_
  network::ResponseCallback done_;
  HttpResponse response_;  // parked by finish() until the count is released
  std::shared_ptr<UpstreamCall> call_;
  bool cancelled_{false};

  discovery::ServiceInstance instance_;  // set by lookup(), read afterwards on other threads
  bool counted_{false};                  // likewise: ordering is given by the task hand-offs
  std::atomic<bool> completed_{false};
};

ProxyHandler::ProxyHandler(std::shared_ptr<network::RequestHandler> next,
                           std::shared_ptr<routing::Router> router,
                           std::shared_ptr<discovery::ServiceRegistry> registry,
                           config::ProxyConfig config, unsigned lookup_threads,
                           std::shared_ptr<logging::Logger> logger,
                           std::shared_ptr<discovery::NameResolver> names,
                           UpstreamPool::Clock pool_clock)
    : next_(std::move(next)),
      router_(std::move(router)),
      registry_(std::move(registry)),
      config_(config),
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
  return stats;
}

}  // namespace edgeflow::proxy
