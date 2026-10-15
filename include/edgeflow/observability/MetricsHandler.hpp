#pragma once

#include <memory>

#include "edgeflow/network/RequestHandler.hpp"
#include "edgeflow/observability/Metrics.hpp"

namespace edgeflow::observability {

// The outermost request handler (Phase 9):
//
//   GET <metrics.path>   the Prometheus text exposition of Metrics::render(); 405 for other methods
//   everything else      passed to the wrapped handler; its latency (from the request reaching this
//                        handler to its response being ready) and status are recorded
//
// The metrics endpoint itself is never counted, never limited and never cached: it sits above the
// rate limiter and the cache, so a scrape cannot be refused by a client's bucket and does not show
// up as traffic. Recording is a few atomic increments per request (see WindowedStats); a request
// that is cancelled before it was answered is not recorded.
class MetricsHandler final : public network::RequestHandler {
 public:
  MetricsHandler(std::shared_ptr<network::RequestHandler> next, std::shared_ptr<Metrics> metrics);

  [[nodiscard]] network::HttpResponse handle(const network::HttpRequest& request) override;
  network::CancelFunction handleAsync(const network::HttpRequest& request,
                                      const network::RequestContext& context,
                                      network::ResponseCallback done) override;

 private:
  [[nodiscard]] bool isMetricsRequest(const network::HttpRequest& request) const;
  [[nodiscard]] network::HttpResponse scrape(const network::HttpRequest& request) const;

  const std::shared_ptr<network::RequestHandler> next_;
  const std::shared_ptr<Metrics> metrics_;
};

}  // namespace edgeflow::observability
