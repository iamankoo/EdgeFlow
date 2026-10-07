#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace edgeflow::reliability {

// The identity of a backend for failover and circuit breaking:
// "service/instance_id@host:port". The instance id alone is not enough: a deregistered and
// re-registered instance on another endpoint is a different backend with a clean history.
[[nodiscard]] inline std::string backendKey(std::string_view service, std::string_view instance_id,
                                            std::string_view host, std::uint16_t port) {
  std::string key;
  key.reserve(service.size() + instance_id.size() + host.size() + 12);
  key.append(service).append("/").append(instance_id).append("@").append(host).append(":");
  key.append(std::to_string(port));
  return key;
}

// What one proxied request has done so far. Failover prefers backends not yet tried; the
// circuit breaker of a backend hears at most one FAILURE per request (so a request that
// fails three times on one backend cannot trip its circuit on its own). Used by one request
// at a time (its attempts are sequential), so not synchronised.
class AttemptLedger {
 public:
  void markTried(const std::string& key) { tried_.insert(key); }
  [[nodiscard]] bool tried(const std::string& key) const { return tried_.count(key) != 0; }
  [[nodiscard]] std::size_t triedCount() const noexcept { return tried_.size(); }

  // True the first time a failure is recorded against `key` for this request.
  [[nodiscard]] bool firstFailureFor(const std::string& key) {
    return failed_.insert(key).second;
  }

 private:
  std::set<std::string> tried_;
  std::set<std::string> failed_;
};

}  // namespace edgeflow::reliability
