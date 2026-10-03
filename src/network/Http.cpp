#include "edgeflow/network/Http.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace edgeflow::network {

HttpResponse makeResponse(unsigned version, bool keep_alive, http::status status,
                          std::string_view content_type, std::string body) {
  HttpResponse response{status, version};
  response.set(http::field::server, kServerName);
  response.set(http::field::content_type, content_type);
  response.keep_alive(keep_alive);
  response.body() = std::move(body);
  response.prepare_payload();
  return response;
}

HttpResponse makeResponse(const HttpRequest& request, http::status status,
                          std::string_view content_type, std::string body) {
  return makeResponse(request.version(), request.keep_alive(), status, content_type,
                      std::move(body));
}

HttpResponse makeErrorResponse(unsigned version, bool keep_alive, http::status status,
                               std::string_view detail) {
  const nlohmann::json body = {{"error", std::string{http::obsolete_reason(status)}},
                               {"status", static_cast<unsigned>(status)},
                               {"detail", std::string{detail}}};
  // `replace` keeps invalid UTF-8 taken from client input from throwing.
  return makeResponse(version, keep_alive, status, kJsonContentType,
                      body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

}  // namespace edgeflow::network
