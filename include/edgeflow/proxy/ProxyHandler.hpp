#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/discovery/NameResolver.hpp"
#include "edgeflow/discovery/ServiceRegistry.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/RequestHandler.hpp"
#include "edgeflow/proxy/UpstreamClient.hpp"
#include "edgeflow/proxy/UpstreamPool.hpp"
#include "edgeflow/routing/Router.hpp"

namespace edgeflow::proxy {

// Reverse proxy (Phase 6). Mounted in front of another handler:
//
//   /proxy/{service}/rest?query   forwarded to a healthy instance of `service`
//   anything else                 passed to the wrapped handler, unchanged
//
// For a proxied request:
//   1. map the URL to {service, upstream path} (400 for a malformed /proxy URL);
//   2. Router::route(service, client address) picks an instance from the routable set
//      (active AND healthy) with the configured strategy: 404 unknown service, 503 when
//      nothing is routable or the registry is unavailable;
//   3. the instance's connection_count is incremented for the duration of the request
//      (Least Connections reads it) and decremented when the request ends, whatever the
//      outcome, BEFORE the response is handed back to the client;
//   4. the request is forwarded (hop-by-hop headers removed, Host / X-Forwarded-* / Via /
//      X-Request-Id set) over a pooled or new keep-alive connection;
//   5. the backend's response comes back to the client with its status, headers and body.
//
// Gateway errors are only for proxy failures: 400 bad /proxy URL, 404 unknown service,
// 502 the backend could not be reached or answered with garbage (refused, reset, closed
// early, invalid HTTP, response too large, name not resolvable), 503 nothing routable or
// registry unavailable (or the proxy is shutting down), 504 upstream timeout. A backend that
// answers 500 (or any other status) is NOT a gateway failure: the client gets that 500.
//
// Threading: the I/O thread of a client connection never waits for a backend or for the
// database. Upstream I/O runs on this class's own io_context (`io_threads`); the registry
// calls (route + counter updates, which block on PostgreSQL) run on a small separate
// thread pool, so a slow database delays proxied requests but never stalls upstream I/O, and
// a slow backend never stalls the client-side I/O workers. The routable set is still read
// from the registry for every request (no cache; that is a documented limitation).
//
// No retries, no circuit breaking, no failover: one attempt per request (the only repeat is
// the connection-level one described at UpstreamClient).
class ProxyHandler final : public network::RequestHandler {
 public:
  struct Stats {
    std::uint64_t requests{0};   // proxied requests started
    std::uint64_t active{0};     // currently in flight
    std::uint64_t responses{0};  // backend responses forwarded
    std::uint64_t gateway_errors{0};
    UpstreamPool::Stats pool;
  };

  // `lookup_threads` bounds concurrent registry calls (use the database pool size).
  ProxyHandler(std::shared_ptr<network::RequestHandler> next, std::shared_ptr<routing::Router> router,
               std::shared_ptr<discovery::ServiceRegistry> registry, config::ProxyConfig config,
               unsigned lookup_threads, std::shared_ptr<logging::Logger> logger,
               std::shared_ptr<discovery::NameResolver> names = nullptr,
               UpstreamPool::Clock pool_clock = nullptr);
  ~ProxyHandler() override;

  ProxyHandler(const ProxyHandler&) = delete;
  ProxyHandler& operator=(const ProxyHandler&) = delete;

  // Non-proxy requests go to the wrapped handler. A proxy request is answered by blocking
  // until the asynchronous path completes: connections never use this (they call
  // handleAsync), it exists for callers that need the synchronous interface.
  [[nodiscard]] network::HttpResponse handle(const network::HttpRequest& request) override;

  network::CancelFunction handleAsync(const network::HttpRequest& request,
                                      const network::RequestContext& context,
                                      network::ResponseCallback done) override;

  // Refuses new proxy requests (503), cancels the ones still running, waits for them to
  // finish and for their connection counts to be released, closes the connection pool and
  // stops the worker threads. Idempotent; also run by the destructor. Call it after the HTTP
  // server stopped (so no client is left waiting) and before the registry goes away.
  void stop();

  [[nodiscard]] Stats stats() const;
  [[nodiscard]] UpstreamPool& pool() noexcept { return *pool_; }

 private:
  class Operation;

  void registerOperation(const std::shared_ptr<Operation>& operation);
  void unregisterOperation(std::uint64_t id);

  const std::shared_ptr<network::RequestHandler> next_;
  const std::shared_ptr<routing::Router> router_;
  const std::shared_ptr<discovery::ServiceRegistry> registry_;
  const config::ProxyConfig config_;
  const std::shared_ptr<logging::Logger> logger_;

  // Destruction order matters: the client and pool must outlive nothing that uses them, the
  // pool must be destroyed before the io_context its sockets belong to.
  boost::asio::io_context io_;
  using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
  std::unique_ptr<WorkGuard> work_guard_;
  std::vector<std::thread> io_threads_;
  std::shared_ptr<UpstreamPool> pool_;
  std::unique_ptr<UpstreamClient> client_;
  boost::asio::thread_pool lookup_pool_;

  std::mutex stop_mutex_;  // serialises stop(); `stopped_done_` makes repeats no-ops
  bool stopped_done_{false};

  mutable std::mutex mutex_;
  std::condition_variable idle_cv_;
  std::map<std::uint64_t, std::weak_ptr<Operation>> operations_;
  std::uint64_t next_operation_id_{1};
  bool stopped_{false};

  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> responses_{0};
  std::atomic<std::uint64_t> gateway_errors_{0};
};

}  // namespace edgeflow::proxy
