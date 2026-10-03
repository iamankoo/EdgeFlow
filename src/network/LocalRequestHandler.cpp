#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "edgeflow/network/RequestHandler.hpp"

namespace edgeflow::network {

namespace {

std::string_view pathOf(std::string_view target) {
  return target.substr(0, target.find('?'));
}

std::string dump(const nlohmann::json& json) {
  return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

HttpResponse methodNotAllowed(const HttpRequest& request, std::string_view allow) {
  auto response = makeErrorResponse(request.version(), request.keep_alive(),
                                    http::status::method_not_allowed,
                                    std::string{request.method_string()} + " is not allowed here");
  response.set(http::field::allow, allow);
  return response;
}

}  // namespace

HttpResponse LocalRequestHandler::handle(const HttpRequest& request) {
  const std::string_view path = pathOf(request.target());
  const bool is_get = request.method() == http::verb::get;

  if (path == "/") {
    if (!is_get) return methodNotAllowed(request, "GET");
    return makeResponse(request, http::status::ok, kJsonContentType,
                        dump({{"service", "edgeflow"},
                              {"version", EDGEFLOW_VERSION},
                              {"message", "EdgeFlow is running"}}));
  }
  if (path == "/health") {
    if (!is_get) return methodNotAllowed(request, "GET");
    return makeResponse(request, http::status::ok, kJsonContentType,
                        dump({{"status", "ok"}, {"service", "edgeflow"}}));
  }
  if (path == "/echo") {
    if (request.method() != http::verb::post) return methodNotAllowed(request, "POST");
    return makeResponse(request, http::status::ok, kJsonContentType,
                        dump({{"received_bytes", request.body().size()}}));
  }
  return makeErrorResponse(request.version(), request.keep_alive(), http::status::not_found,
                           "no route for " + std::string{request.method_string()} + " " +
                               std::string{path});
}

}  // namespace edgeflow::network
