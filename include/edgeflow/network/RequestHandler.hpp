#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "edgeflow/network/Http.hpp"

namespace edgeflow::network {

// What the connection knows about a request besides the HTTP message itself.
struct RequestContext {
  std::string client_address;  // peer IP address as text; empty when unknown
  std::uint16_t client_port{0};
};

// Delivers the response of an asynchronous request. May be called from any thread, and
// even before handleAsync() returns.
using ResponseCallback = std::function<void(HttpResponse)>;

// Cancels an asynchronous request that is still running. Called by the connection when it
// is closed (client gone, forced close during shutdown) before the response was delivered.
// May be empty. Must be safe to call at any time, from any thread, any number of times.
using CancelFunction = std::function<void()>;

// Turns a parsed request into a response. This is deliberately NOT the load-balancing
// router: it only answers requests. Implementations are called from I/O worker threads,
// possibly concurrently for different connections, and must be thread-safe.
//
// Two entry points:
//   handle()       synchronous. Runs on an I/O worker thread, so it must not block for long.
//   handleAsync()  what connections actually call. The default runs handle() inline; a
//                  handler that has to wait for something slow (the reverse proxy waiting
//                  for a backend) overrides it and completes later without holding a thread.
// An exception thrown from either is converted into a 500 response by the connection.
class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  [[nodiscard]] virtual HttpResponse handle(const HttpRequest& request) = 0;

  // `request` is only valid during the call: a handler that needs it later must copy what
  // it needs. `done` is called exactly once unless handleAsync() throws (then never).
  // Returns a function that cancels the request, or an empty function.
  virtual CancelFunction handleAsync(const HttpRequest& request, const RequestContext& context,
                                     ResponseCallback done) {
    (void)context;
    done(handle(request));
    return {};
  }
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
