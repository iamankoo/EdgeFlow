#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/observability/MetricsHandler.hpp"
#include "support/MetricsTestSupport.hpp"

namespace edgeflow::observability {
namespace {

using namespace std::chrono_literals;
using testing::Exposition;
namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

// A wrapped handler whose answer the test controls: synchronous with a status and a delay, or
// asynchronous (the test completes it later), or throwing.
class ScriptedHandler final : public network::RequestHandler {
 public:
  unsigned status{200};
  std::chrono::milliseconds delay{0};
  bool throws{false};
  bool async{false};
  std::function<void(HttpResponse)> pending;  // the callback of an async request, to complete later
  std::atomic<int> cancelled{0};
  std::atomic<int> calls{0};

  HttpResponse handle(const HttpRequest& request) override {
    ++calls;
    if (throws) throw std::runtime_error("handler failure");
    if (delay.count() > 0) std::this_thread::sleep_for(delay);
    return network::makeResponse(request, static_cast<http::status>(status), "text/plain", "answer");
  }

  network::CancelFunction handleAsync(const HttpRequest& request, const network::RequestContext& context,
                                      network::ResponseCallback done) override {
    if (!async) return RequestHandler::handleAsync(request, context, std::move(done));
    ++calls;
    pending = [done = std::move(done)](HttpResponse response) { done(std::move(response)); };
    return [this] { ++cancelled; };
  }
};

HttpRequest request(http::verb method, const std::string& target) {
  HttpRequest r{method, target, 11};
  r.set(http::field::host, "gateway");
  return r;
}

struct Fixture {
  std::shared_ptr<ScriptedHandler> inner = std::make_shared<ScriptedHandler>();
  std::shared_ptr<Metrics> metrics = [] {
    config::MetricsConfig config;
    config.enabled = true;
    return std::make_shared<Metrics>(config);
  }();
  std::shared_ptr<MetricsHandler> handler = std::make_shared<MetricsHandler>(inner, metrics);

  [[nodiscard]] Exposition scrape() const { return Exposition(metrics->render()); }
};

TEST(MetricsHandlerTest, TheMetricsPathIsServedInThePrometheusTextFormat) {
  Fixture f;
  const auto response = f.handler->handle(request(http::verb::get, "/metrics"));
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(std::string{response[http::field::content_type]}, "text/plain; version=0.0.4; charset=utf-8");
  const Exposition exposition(response.body());
  EXPECT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_TRUE(exposition.has("edgeflow_http_requests_total"));
  EXPECT_EQ(f.inner->calls.load(), 0) << "the wrapped handler never sees the scrape";
}

TEST(MetricsHandlerTest, AQueryStringDoesNotHideTheEndpointAndHeadHasNoBody) {
  Fixture f;
  EXPECT_EQ(f.handler->handle(request(http::verb::get, "/metrics?debug=1")).result(), http::status::ok);
  const auto head = f.handler->handle(request(http::verb::head, "/metrics"));
  EXPECT_EQ(head.result(), http::status::ok);
  EXPECT_TRUE(head.body().empty());
  EXPECT_NE(std::string{head[http::field::content_length]}, "0") << "HEAD reports the length a GET would have";
}

TEST(MetricsHandlerTest, OtherMethodsAreRefusedWithAllow) {
  Fixture f;
  for (const auto verb : {http::verb::post, http::verb::put, http::verb::delete_}) {
    const auto response = f.handler->handle(request(verb, "/metrics"));
    EXPECT_EQ(response.result(), http::status::method_not_allowed);
    EXPECT_EQ(std::string{response[http::field::allow]}, "GET, HEAD");
  }
  EXPECT_EQ(f.inner->calls.load(), 0);
}

TEST(MetricsHandlerTest, ScrapesAreNotCountedAsTraffic) {
  Fixture f;
  for (int i = 0; i < 5; ++i) (void)f.handler->handle(request(http::verb::get, "/metrics"));
  auto exposition = f.scrape();
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), 0);
}

TEST(MetricsHandlerTest, SimilarPathsAreNotTheEndpoint) {
  Fixture f;
  for (const char* target : {"/metrics/", "/metricsx", "/Metrics", "/x/metrics", "/"}) {
    (void)f.handler->handle(request(http::verb::get, target));
  }
  EXPECT_EQ(f.inner->calls.load(), 5) << "all five went to the wrapped handler";
}

TEST(MetricsHandlerTest, ARequestIsCountedWithItsStatusKindAndLatency) {
  Fixture f;
  f.inner->status = 200;
  f.inner->delay = 30ms;
  (void)f.handler->handle(request(http::verb::get, "/proxy/orders/items"));
  f.inner->status = 502;
  f.inner->delay = 0ms;
  (void)f.handler->handle(request(http::verb::get, "/proxy/orders/items"));
  (void)f.handler->handle(request(http::verb::get, "/health"));
  const auto exposition = f.scrape();
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "proxy"}}), 2);
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "local"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "5xx"}}), 2);
  EXPECT_EQ(exposition.must("edgeflow_http_errors_total"), 2);
  EXPECT_GT(exposition.must("edgeflow_http_request_duration_seconds_sum"), 0.029);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), 3);
}

TEST(MetricsHandlerTest, TheAsynchronousPathIsTimedFromTheRequestToTheAnswer) {
  Fixture f;
  f.inner->async = true;
  std::promise<HttpResponse> answered;
  auto cancel = f.handler->handleAsync(request(http::verb::get, "/proxy/orders/x"), network::RequestContext{},
                                       [&answered](HttpResponse response) { answered.set_value(std::move(response)); });
  EXPECT_EQ(f.scrape().must("edgeflow_http_in_flight_requests"), 1);
  EXPECT_EQ(f.scrape().must("edgeflow_http_request_duration_seconds_count"), 0) << "not finished yet";
  std::this_thread::sleep_for(60ms);
  f.inner->pending(network::makeResponse(11, true, http::status::ok, "text/plain", "late"));
  EXPECT_EQ(answered.get_future().get().body(), "late");
  const auto exposition = f.scrape();
  EXPECT_EQ(exposition.must("edgeflow_http_in_flight_requests"), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), 1);
  EXPECT_GT(exposition.must("edgeflow_http_request_duration_seconds_sum"), 0.055);
  ASSERT_TRUE(cancel);
  cancel();
  EXPECT_EQ(f.inner->cancelled.load(), 1) << "the cancel function of the wrapped handler is passed through";
}

TEST(MetricsHandlerTest, AThrowingHandlerIsRecordedAsAnErrorAndRethrown) {
  Fixture f;
  f.inner->throws = true;
  EXPECT_THROW((void)f.handler->handle(request(http::verb::get, "/proxy/orders/x")), std::runtime_error);
  const auto exposition = f.scrape();
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "5xx"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_in_flight_requests"), 0);
}

TEST(MetricsHandlerTest, ConcurrentRequestsAreAllCounted) {
  Fixture f;
  f.inner->delay = 1ms;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 100; ++i) (void)f.handler->handle(request(http::verb::get, "/proxy/orders/x"));
    });
  }
  std::thread scraper([&] {
    for (int i = 0; i < 50; ++i) {
      EXPECT_TRUE(f.scrape().valid());
      (void)f.handler->handle(request(http::verb::get, "/metrics"));
    }
  });
  for (auto& thread : threads) thread.join();
  scraper.join();
  const auto exposition = f.scrape();
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "proxy"}}), 800);
  EXPECT_EQ(exposition.must("edgeflow_http_in_flight_requests"), 0);
}

}  // namespace
}  // namespace edgeflow::observability
