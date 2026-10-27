#include "edgeflow/network/AdminAuthHandler.hpp"

#include <utility>

namespace edgeflow::network {
namespace {

bool isRegistryPath(std::string_view target) {
  const auto end = target.find_first_of("?#");
  const auto path = target.substr(0, end);
  constexpr std::string_view root = "/services";
  return path == root || (path.size() > root.size() && path.substr(0, root.size()) == root && path[root.size()] == '/');
}

}  // namespace

AdminAuthHandler::AdminAuthHandler(std::shared_ptr<RequestHandler> next, std::string token)
    : next_(std::move(next)), token_(std::move(token)) {}

bool AdminAuthHandler::constantTimeEquals(std::string_view presented, std::string_view expected) noexcept {
  // Always walk the whole expected token; a length difference is folded into the result.
  unsigned char diff = presented.size() == expected.size() ? 0 : 1;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const unsigned char a = i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0;
    diff |= static_cast<unsigned char>(a ^ static_cast<unsigned char>(expected[i]));
  }
  return diff == 0;
}

HttpResponse AdminAuthHandler::handle(const HttpRequest& request) {
  if (!isRegistryPath(request.target())) return next_->handle(request);

  constexpr std::string_view scheme = "Bearer ";
  const std::string_view header = request[http::field::authorization];
  const bool allowed = header.size() > scheme.size() && header.substr(0, scheme.size()) == scheme &&
                       constantTimeEquals(header.substr(scheme.size()), token_);
  if (allowed) return next_->handle(request);

  auto response = makeErrorResponse(request.version(), request.keep_alive(), http::status::unauthorized,
                                    "the registry API requires 'Authorization: Bearer <token>'");
  response.set(http::field::www_authenticate, "Bearer");
  return response;
}

}  // namespace edgeflow::network
