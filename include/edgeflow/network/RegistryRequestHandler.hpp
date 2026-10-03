#pragma once

#include <memory>

#include "edgeflow/discovery/ServiceRegistry.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/RequestHandler.hpp"

namespace edgeflow::network {

// JSON API over a ServiceRegistry, mounted under /services. Everything else is passed to
// `next` untouched, so the Phase 2 endpoints (/, /health, /echo) behave exactly as before.
// This is registry management only: it never forwards client traffic to a backend.
//
//   GET    /services                                  list service names
//   POST   /services/{service}/instances              register   -> 201 + Location
//   GET    /services/{service}/instances              discover   -> 200 (404 unknown service)
//   GET    /services/{service}/instances/{instance}   one instance
//   PATCH  /services/{service}/instances/{instance}   update mutable metadata
//   DELETE /services/{service}/instances/{instance}   deregister -> 204
//
// Error mapping: 400 invalid input/JSON, 404 unknown service or instance, 405 wrong
// method, 409 duplicate, 503 database unavailable, 500 anything else.
//
// handle() calls into the registry synchronously on an I/O worker thread. With PostgreSQL
// that means a worker blocks for the duration of a query; see architecture.md.
class RegistryRequestHandler final : public RequestHandler {
 public:
  RegistryRequestHandler(std::shared_ptr<discovery::ServiceRegistry> registry,
                         std::shared_ptr<RequestHandler> next,
                         std::shared_ptr<logging::Logger> logger);

  [[nodiscard]] HttpResponse handle(const HttpRequest& request) override;

 private:
  std::shared_ptr<discovery::ServiceRegistry> registry_;
  std::shared_ptr<RequestHandler> next_;
  std::shared_ptr<logging::Logger> logger_;
};

}  // namespace edgeflow::network
