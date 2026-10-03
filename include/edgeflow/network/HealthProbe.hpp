#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace edgeflow::network {

// One-shot HTTP client used by `edgeflow --healthcheck` (and the container HEALTHCHECK):
// sends `GET /health` over a fresh connection and succeeds only on a 200 response.
// On failure `detail` explains why. Bounded by `timeout`.
[[nodiscard]] bool probeHealth(const std::string& host, std::uint16_t port,
                               std::chrono::milliseconds timeout, std::string& detail);

}  // namespace edgeflow::network
