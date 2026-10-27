#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "edgeflow/network/RequestHandler.hpp"

namespace edgeflow::network {

// Phase 10 hardening: when admin.token_env is configured, every request to the registry API (/services and
// below) must carry `Authorization: Bearer <token>`. Anything else is passed to `next` untouched, so
// /health, /metrics and /proxy stay open. A missing or wrong token is answered 401 before the registry is
// touched. The comparison takes the same time whatever the first differing byte is.
class AdminAuthHandler final : public RequestHandler {
 public:
  AdminAuthHandler(std::shared_ptr<RequestHandler> next, std::string token);

  [[nodiscard]] HttpResponse handle(const HttpRequest& request) override;

  // True when `presented` equals `expected`, in time that depends only on their lengths.
  [[nodiscard]] static bool constantTimeEquals(std::string_view presented, std::string_view expected) noexcept;

 private:
  std::shared_ptr<RequestHandler> next_;
  std::string token_;
};

}  // namespace edgeflow::network
