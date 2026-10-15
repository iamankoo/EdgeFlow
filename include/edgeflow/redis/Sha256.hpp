#pragma once

#include <string>
#include <string_view>

namespace edgeflow::redis {

// SHA-256 of `data` as 64 lower-case hex characters (FIPS 180-4). Used to derive Redis keys
// from values that must not appear in Redis as they are (API keys) or are of unbounded
// length and content (cache keys): a collision would need a SHA-256 collision.
[[nodiscard]] std::string sha256Hex(std::string_view data);

}  // namespace edgeflow::redis
