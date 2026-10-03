#pragma once

#include "edgeflow/network/Http.hpp"

namespace edgeflow::network {

// Turns a parsed request into a response. This is deliberately NOT the load-balancing
// router: Phase 2 only answers locally. Implementations are called from I/O worker
// threads, possibly concurrently for different connections, and must be thread-safe.
// An exception thrown from handle() is converted into a 500 response.
class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  [[nodiscard]] virtual HttpResponse handle(const HttpRequest& request) = 0;
};

// Local endpoints:
//   GET  /        service banner
//   GET  /health  liveness: {"status":"ok","service":"edgeflow"}
//   POST /echo    reports the size of the received body (exercises body handling)
// Anything else is 404; a known path with the wrong method is 405 with an Allow header.
class LocalRequestHandler final : public RequestHandler {
 public:
  [[nodiscard]] HttpResponse handle(const HttpRequest& request) override;
};

}  // namespace edgeflow::network
