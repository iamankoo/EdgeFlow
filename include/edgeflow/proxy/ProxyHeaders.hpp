#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "edgeflow/network/Http.hpp"

namespace edgeflow::proxy {

// Pure request/response translation for the reverse proxy: which URL maps to which
// service, which headers cross the proxy and which are added. No I/O, no state, so every
// rule is unit-testable on its own.

inline constexpr std::string_view kProxyPrefix = "/proxy";
inline constexpr std::string_view kRequestIdHeader = "X-Request-Id";
// The pseudonym EdgeFlow uses in Via headers.
inline constexpr std::string_view kViaPseudonym = "edgeflow";
inline constexpr std::size_t kMaxRequestIdLength = 128;

// --- service mapping ---------------------------------------------------------------

// True when the request target addresses the proxy: the path is exactly "/proxy" or starts
// with "/proxy/". "/proxyfoo" and "/services/..." are not proxy targets.
[[nodiscard]] bool isProxyTarget(std::string_view target) noexcept;

struct ProxyRoute {
  std::string service;
  // Path (and query string) to request from the backend: always starts with '/'.
  std::string upstream_target;
};

enum class ProxyTargetError { MissingService, InvalidService };

struct ProxyTargetResult {
  std::optional<ProxyRoute> route;
  std::optional<ProxyTargetError> error;
  std::string detail;  // human-readable reason when `error` is set
};

// Maps /proxy/{service}/rest?query to {service, "/rest?query"}:
//   /proxy/orders/             -> orders, "/"
//   /proxy/orders              -> orders, "/"
//   /proxy/orders/api/users?id=42 -> orders, "/api/users?id=42"
//   /proxy/orders?x=1          -> orders, "/?x=1"
// The rest of the path is forwarded verbatim (no decoding or normalisation). The service
// must be a valid service name; anything else (empty, uppercase, percent-escapes, ...) is
// rejected rather than guessed at. Precondition: isProxyTarget(target).
[[nodiscard]] ProxyTargetResult mapProxyTarget(std::string_view target);

// --- request ids -------------------------------------------------------------------

// A fresh random identifier (UUID version 4 text form). Carries no information about the
// host, the process or the client.
[[nodiscard]] std::string generateRequestId();

// The client's X-Request-Id when it is usable: 1..128 visible ASCII characters. Anything
// else (empty, too long, control or non-ASCII bytes) is ignored and a new id is generated.
[[nodiscard]] std::string chooseRequestId(const network::HttpRequest& request);

// --- headers -----------------------------------------------------------------------

struct ForwardInfo {
  std::string client_address;  // appended to X-Forwarded-For (omitted when empty)
  std::string upstream_host;   // value of the upstream Host header
  std::string request_id;
};

// Host header value for an upstream address: "host:port" (IPv6 literals bracketed), without
// the port when it is the HTTP default.
[[nodiscard]] std::string hostHeaderFor(std::string_view host, unsigned port);

// The request EdgeFlow sends to a backend: HTTP/1.1, same method and body, `upstream_target`
// as the target.
//   dropped    hop-by-hop headers (Connection, Keep-Alive, TE, Trailer, Transfer-Encoding,
//              Upgrade, Proxy-*, every header named by the client's Connection header),
//              Expect (EdgeFlow already has the whole body), Content-Length (recomputed),
//              Host (rewritten)
//   rewritten  Host = info.upstream_host; X-Request-Id = info.request_id;
//              X-Forwarded-Proto = http (what the listener speaks; a client-supplied value
//              is not believed)
//   appended   X-Forwarded-For (existing chain kept, client address added at the end);
//              Via (existing chain kept, "1.1 edgeflow" added at the end)
//   everything else, including repeated headers, is passed through unchanged.
[[nodiscard]] network::HttpRequest makeUpstreamRequest(const network::HttpRequest& client,
                                                       std::string upstream_target,
                                                       const ForwardInfo& info);

// The response EdgeFlow sends to the client for a backend response: same status, reason and
// body; the client's HTTP version; hop-by-hop headers (as above, plus those named by the
// backend's Connection and Trailer headers) and Content-Length (recomputed from the body,
// except for HEAD where the backend's value describes the unsent body) removed; Via
// appended; X-Request-Id set. Connection handling is left to the connection that writes it.
[[nodiscard]] network::HttpResponse makeClientResponse(network::HttpResponse upstream,
                                                       const network::HttpRequest& client,
                                                       std::string_view request_id);

// A gateway-generated error (JSON body) that carries the request id.
[[nodiscard]] network::HttpResponse makeGatewayError(const network::HttpRequest& client,
                                                     network::http::status status,
                                                     std::string_view detail,
                                                     std::string_view request_id);

}  // namespace edgeflow::proxy
