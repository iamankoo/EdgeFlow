#include "edgeflow/observability/MetricsHandler.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include "edgeflow/proxy/ProxyHeaders.hpp"

namespace edgeflow::observability {

namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

namespace {

constexpr std::string_view kExpositionContentType = "text/plain; version=0.0.4; charset=utf-8";

// The path of a request target without its query string.
std::string_view pathOf(std::string_view target) {
  return target.substr(0, target.find('?'));
}

}  // namespace

MetricsHandler::MetricsHandler(std::shared_ptr<network::RequestHandler> next,
                               std::shared_ptr<Metrics> metrics)
    : next_(std::move(next)), metrics_(std::move(metrics)) {}

bool MetricsHandler::isMetricsRequest(const HttpRequest& request) const {
  return pathOf(request.target()) == metrics_->config().path;
}

HttpResponse MetricsHandler::scrape(const HttpRequest& request) const {
  if (request.method() != http::verb::get && request.method() != http::verb::head) {
    auto response = network::makeErrorResponse(request.version(), request.keep_alive(),
                                               http::status::method_not_allowed, "use GET");
    response.set(http::field::allow, "GET, HEAD");
    return response;
  }
  auto response = network::makeResponse(request, http::status::ok, kExpositionContentType,
                                        metrics_->render());
  if (request.method() == http::verb::head) response.body().clear();  // Content-Length stays the GET length
  return response;
}

HttpResponse MetricsHandler::handle(const HttpRequest& request) {
  if (isMetricsRequest(request)) return scrape(request);
  const auto started = std::chrono::steady_clock::now();
  metrics_->requestStarted();
  HttpResponse response;
  try {
    response = next_->handle(request);
  } catch (...) {
    metrics_->requestEnded();
    metrics_->recordRequest(std::chrono::steady_clock::now() - started, 500,
                            proxy::isProxyTarget(request.target()));
    throw;  // the connection turns it into a 500
  }
  metrics_->requestEnded();
  metrics_->recordRequest(std::chrono::steady_clock::now() - started, response.result_int(),
                          proxy::isProxyTarget(request.target()));
  return response;
}

network::CancelFunction MetricsHandler::handleAsync(const HttpRequest& request,
                                                    const network::RequestContext& context,
                                                    network::ResponseCallback done) {
  if (isMetricsRequest(request)) {
    done(scrape(request));
    return {};
  }
  const auto started = std::chrono::steady_clock::now();
  const bool proxied = proxy::isProxyTarget(request.target());
  metrics_->requestStarted();
  auto metrics = metrics_;
  // `done` is called exactly once by the wrapped handler (the first answer wins downstream), so
  // the in-flight count goes back down exactly once; if the wrapped handler throws, it never is.
  try {
    return next_->handleAsync(
        request, context,
        [metrics, started, proxied, done = std::move(done)](HttpResponse response) {
          metrics->requestEnded();
          metrics->recordRequest(std::chrono::steady_clock::now() - started, response.result_int(), proxied);
          done(std::move(response));
        });
  } catch (...) {
    metrics_->requestEnded();
    throw;
  }
}

}  // namespace edgeflow::observability
