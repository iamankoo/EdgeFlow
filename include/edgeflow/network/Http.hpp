#pragma once

#include <string>
#include <string_view>

#include <boost/beast/http.hpp>

namespace edgeflow::network {

namespace http = boost::beast::http;

using HttpRequest = http::request<http::string_body>;
using HttpResponse = http::response<http::string_body>;

inline constexpr std::string_view kServerName = "EdgeFlow/" EDGEFLOW_VERSION;
inline constexpr std::string_view kJsonContentType = "application/json";

// Builds a complete response (Server, Content-Type, Content-Length, keep-alive set).
[[nodiscard]] HttpResponse makeResponse(unsigned version, bool keep_alive, http::status status,
                                        std::string_view content_type, std::string body);

// Convenience overload mirroring the request's HTTP version and keep-alive preference.
[[nodiscard]] HttpResponse makeResponse(const HttpRequest& request, http::status status,
                                        std::string_view content_type, std::string body);

// JSON error body: {"error": <reason phrase>, "status": <code>, "detail": <detail>}.
[[nodiscard]] HttpResponse makeErrorResponse(unsigned version, bool keep_alive,
                                             http::status status, std::string_view detail);

}  // namespace edgeflow::network
