#include "edgeflow/cache/ResponseCache.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>

#include <boost/beast/core/string.hpp>

#include "edgeflow/proxy/ProxyHeaders.hpp"
#include "edgeflow/redis/Sha256.hpp"

namespace edgeflow::cache {

namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

namespace {

constexpr std::string_view kMagic = "EFC1";
constexpr std::size_t kMaxHeaders = 1024;
constexpr std::string_view kCacheHeader = "X-Cache";

std::string lower(std::string_view text) {
  std::string out{text};
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

// All values of the header lines called `name`, joined with ", ".
template <class Message>
std::string joined(const Message& message, std::string_view name) {
  std::string out;
  for (const auto& field : message) {
    if (!boost::beast::iequals(field.name_string(), name)) continue;
    if (!out.empty()) out += ", ";
    out.append(field.value());
  }
  return out;
}

template <class Message>
bool has(const Message& message, std::string_view name) {
  return std::any_of(message.begin(), message.end(), [&](const auto& field) {
    return boost::beast::iequals(field.name_string(), name);
  });
}

bool skippedWhenStoring(std::string_view name) {
  static constexpr std::array<std::string_view, 6> kSkipped = {
      "content-length", "x-request-id", "x-cache", "age", "connection", "keep-alive"};
  const std::string lowered = lower(name);
  return std::find(kSkipped.begin(), kSkipped.end(), lowered) != kSkipped.end();
}

void putU32(std::string& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void putU64(std::string& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  bool u32(std::uint32_t& value) {
    if (bytes_.size() - pos_ < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      value = (value << 8U) | static_cast<unsigned char>(bytes_[pos_ + static_cast<std::size_t>(i)]);
    }
    pos_ += 4;
    return true;
  }
  bool u64(std::uint64_t& value) {
    if (bytes_.size() - pos_ < 8) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8U) | static_cast<unsigned char>(bytes_[pos_ + static_cast<std::size_t>(i)]);
    }
    pos_ += 8;
    return true;
  }
  bool text(std::uint64_t length, std::string& out) {
    if (length > bytes_.size() - pos_) return false;
    out.assign(bytes_.substr(pos_, static_cast<std::size_t>(length)));
    pos_ += static_cast<std::size_t>(length);
    return true;
  }
  bool literal(std::string_view expected) {
    if (bytes_.size() - pos_ < expected.size() || bytes_.substr(pos_, expected.size()) != expected) {
      return false;
    }
    pos_ += expected.size();
    return true;
  }
  [[nodiscard]] bool atEnd() const { return pos_ == bytes_.size(); }

 private:
  std::string_view bytes_;
  std::size_t pos_{0};
};

std::optional<long long> parseSeconds(std::string_view text) {
  text = trim(text);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text = trim(text.substr(1, text.size() - 2));
  }
  if (text.empty() || text.size() > 12) return std::nullopt;
  long long value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || value < 0) return std::nullopt;
  return value;
}

}  // namespace

CacheControl parseCacheControl(std::string_view value) {
  CacheControl out;
  std::size_t start = 0;
  while (start <= value.size()) {
    std::size_t end = value.find(',', start);
    if (end == std::string_view::npos) end = value.size();
    const std::string_view directive = trim(value.substr(start, end - start));
    start = end + 1;
    if (directive.empty()) continue;
    const std::size_t equals = directive.find('=');
    const std::string name = lower(trim(directive.substr(0, equals)));
    const std::string_view argument =
        equals == std::string_view::npos ? std::string_view{} : directive.substr(equals + 1);
    if (name == "no-store") {
      out.no_store = true;
    } else if (name == "no-cache") {
      out.no_cache = true;
    } else if (name == "private") {
      out.is_private = true;
    } else if (name == "max-age") {
      out.max_age = parseSeconds(argument);
    } else if (name == "s-maxage") {
      out.s_maxage = parseSeconds(argument);
    }
  }
  return out;
}

std::string serialize(const CachedResponse& response) {
  std::string out;
  out.reserve(64 + response.body.size());
  out.append(kMagic);
  out.push_back(static_cast<char>((response.status >> 8U) & 0xFFU));
  out.push_back(static_cast<char>(response.status & 0xFFU));
  putU64(out, static_cast<std::uint64_t>(response.stored_at_ms));
  putU32(out, static_cast<std::uint32_t>(response.headers.size()));
  for (const auto& [name, value] : response.headers) {
    putU32(out, static_cast<std::uint32_t>(name.size()));
    out.append(name);
    putU32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
  }
  putU64(out, response.body.size());
  out.append(response.body);
  return out;
}

std::optional<CachedResponse> deserialize(std::string_view bytes) {
  if (bytes.size() < kMagic.size() + 2 || bytes.substr(0, kMagic.size()) != kMagic) {
    return std::nullopt;
  }
  CachedResponse response;
  response.status =
      (static_cast<unsigned>(static_cast<unsigned char>(bytes[kMagic.size()])) << 8U) |
      static_cast<unsigned>(static_cast<unsigned char>(bytes[kMagic.size() + 1]));
  if (response.status < 100 || response.status > 599) return std::nullopt;

  Reader reader{bytes.substr(kMagic.size() + 2)};
  std::uint64_t stored_at = 0;
  if (!reader.u64(stored_at)) return std::nullopt;
  response.stored_at_ms = static_cast<std::int64_t>(stored_at);
  std::uint32_t count = 0;
  if (!reader.u32(count) || count > kMaxHeaders) return std::nullopt;
  response.headers.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t name_length = 0;
    std::uint32_t value_length = 0;
    std::string name;
    std::string value;
    if (!reader.u32(name_length) || !reader.text(name_length, name)) return std::nullopt;
    if (!reader.u32(value_length) || !reader.text(value_length, value)) return std::nullopt;
    if (name.empty()) return std::nullopt;
    response.headers.emplace_back(std::move(name), std::move(value));
  }
  std::uint64_t body_length = 0;
  if (!reader.u64(body_length) || !reader.text(body_length, response.body)) return std::nullopt;
  if (!reader.atEnd()) return std::nullopt;
  return response;
}

ResponseCache::ResponseCache(std::shared_ptr<redis::RedisClient> client, config::CacheConfig config,
                             std::string key_prefix, std::shared_ptr<logging::Logger> logger)
    : client_(std::move(client)),
      config_(std::move(config)),
      prefix_(std::move(key_prefix)),
      logger_(std::move(logger)) {}

bool ResponseCache::isUnsafeMethod(const HttpRequest& request) {
  const auto method = request.method();
  return method == http::verb::post || method == http::verb::put || method == http::verb::patch ||
         method == http::verb::delete_;
}

bool ResponseCache::requestUsesCache(const HttpRequest& request) {
  if (request.method() != http::verb::get) return false;
  if (!request.body().empty()) return false;
  for (const auto name : {"Authorization", "Cookie", "Range", "If-Match", "If-None-Match",
                          "If-Modified-Since", "If-Unmodified-Since", "If-Range"}) {
    if (has(request, name)) return false;
  }
  const CacheControl control = parseCacheControl(joined(request, "Cache-Control"));
  if (control.no_store || control.no_cache) return false;
  if (lower(joined(request, "Pragma")).find("no-cache") != std::string::npos) return false;
  return true;
}

std::string ResponseCache::keyFor(std::string_view service, std::string_view upstream_target) const {
  std::string material = "GET\n";
  material.append(service).append("\n").append(upstream_target);
  return prefix_ + ":cache:v1:" + redis::sha256Hex(material);
}

std::optional<std::chrono::seconds> ResponseCache::storableTtl(const HttpResponse& response) const {
  const unsigned status = response.result_int();
  if (std::find(config_.cacheable_statuses.begin(), config_.cacheable_statuses.end(), status) ==
      config_.cacheable_statuses.end()) {
    return std::nullopt;
  }
  if (response.body().size() > config_.max_entry_bytes) return std::nullopt;
  if (has(response, "Set-Cookie") || has(response, "Vary") || has(response, "Content-Range")) {
    return std::nullopt;
  }
  // Only what a backend answered: the proxy appended its own Via to it. A gateway-generated
  // error (404 unknown service, 429, 502, ...) never has one.
  bool from_backend = false;
  for (const auto& field : response) {
    if (!boost::beast::iequals(field.name_string(), "Via")) continue;
    const std::string_view via = trim(field.value());
    if (via.size() >= proxy::kViaPseudonym.size() &&
        via.substr(via.size() - proxy::kViaPseudonym.size()) == proxy::kViaPseudonym) {
      from_backend = true;
    }
  }
  if (!from_backend) return std::nullopt;

  const CacheControl control = parseCacheControl(joined(response, "Cache-Control"));
  if (control.no_store || control.no_cache || control.is_private) return std::nullopt;
  std::optional<long long> lifetime = control.s_maxage ? control.s_maxage : control.max_age;
  long long seconds = lifetime ? *lifetime : static_cast<long long>(config_.default_ttl.count());
  if (seconds <= 0) return std::nullopt;
  seconds = std::min<long long>(seconds, config_.max_ttl.count());
  return std::chrono::seconds{seconds};
}

void ResponseCache::lookup(const std::string& key, LookupCallback callback) {
  client_->command({"GET", key}, [this, key, callback = std::move(callback)](redis::RedisResult result) {
    Lookup lookup;
    if (!result.ok()) {
      lookup.outcome = Lookup::Outcome::Error;
      lookup.detail = std::string{redis::toString(result.error->kind)} + ": " + result.error->message;
      errors_.fetch_add(1);
      callback(std::move(lookup));
      return;
    }
    const redis::Reply& reply = *result.reply;
    if (reply.isNil()) {
      misses_.fetch_add(1);
      callback(std::move(lookup));
      return;
    }
    if (reply.type != redis::Reply::Type::Bulk) {
      lookup.outcome = Lookup::Outcome::Error;
      lookup.detail = reply.isError() ? "Redis error: " + reply.text : "unexpected reply type";
      errors_.fetch_add(1);
      callback(std::move(lookup));
      return;
    }
    auto entry = deserialize(reply.text);
    if (!entry) {
      // Not something this version wrote (or damaged): never served, and removed so it
      // cannot hide a good entry for the rest of its lifetime.
      logger_->warn("cache: entry {} is unreadable; removing it", key);
      lookup.outcome = Lookup::Outcome::Error;
      lookup.detail = "unreadable cache entry";
      errors_.fetch_add(1);
      invalidate(key);
      callback(std::move(lookup));
      return;
    }
    lookup.outcome = Lookup::Outcome::Hit;
    lookup.entry = std::move(*entry);
    hits_.fetch_add(1);
    callback(std::move(lookup));
  });
}

void ResponseCache::store(const std::string& key, const HttpResponse& response,
                          std::chrono::seconds ttl) {
  CachedResponse entry;
  entry.status = response.result_int();
  entry.body = response.body();
  entry.stored_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  for (const auto& field : response) {
    const std::string_view name = field.name_string();
    if (skippedWhenStoring(name)) continue;
    entry.headers.emplace_back(std::string{name}, std::string{field.value()});
  }
  client_->command({"SET", key, serialize(entry), "PX", std::to_string(ttl.count() * 1000)},
                   [this, key](redis::RedisResult result) {
                     if (result.ok() && result.reply->isOk()) {
                       stores_.fetch_add(1);
                       return;
                     }
                     store_failures_.fetch_add(1);
                     logger_->debug("cache: could not store {}: {}", key,
                                    result.ok() ? result.reply->text : result.error->message);
                   });
}

void ResponseCache::invalidate(const std::string& key) {
  invalidations_.fetch_add(1);
  client_->command({"DEL", key}, [this, key](redis::RedisResult result) {
    if (!result.ok()) logger_->debug("cache: could not remove {}: {}", key, result.error->message);
  });
}

HttpResponse ResponseCache::toResponse(const CachedResponse& entry, const HttpRequest& request,
                                       std::string_view request_id) {
  HttpResponse response{static_cast<http::status>(entry.status), request.version()};
  for (const auto& [name, value] : entry.headers) response.insert(name, value);
  response.body() = entry.body;
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const long long age = std::max<long long>(0, (now_ms - entry.stored_at_ms) / 1000);
  response.set("Age", std::to_string(age));
  response.set(kCacheHeader, "HIT");
  response.set(proxy::kRequestIdHeader, request_id);
  response.prepare_payload();
  return response;
}

ResponseCache::Stats ResponseCache::stats() const {
  Stats stats;
  stats.hits = hits_.load();
  stats.misses = misses_.load();
  stats.errors = errors_.load();
  stats.stores = stores_.load();
  stats.store_failures = store_failures_.load();
  stats.invalidations = invalidations_.load();
  return stats;
}

}  // namespace edgeflow::cache
