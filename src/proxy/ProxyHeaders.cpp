#include "edgeflow/proxy/ProxyHeaders.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <boost/beast/core/string.hpp>

#include "edgeflow/discovery/Validation.hpp"

namespace edgeflow::proxy {

namespace {

namespace http = network::http;
using network::HttpRequest;
using network::HttpResponse;

constexpr std::string_view kProxyRoot = "/proxy/";

bool iequals(std::string_view a, std::string_view b) { return boost::beast::iequals(a, b); }

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// Headers that describe one hop only and must never be forwarded (RFC 9110 7.6.1, plus the
// de-facto Proxy-Connection). Any other "Proxy-*" header is the client talking to a proxy.
bool isHopByHop(std::string_view name) {
  static constexpr std::array<std::string_view, 6> kNames = {
      "connection", "keep-alive", "te", "trailer", "transfer-encoding", "upgrade"};
  for (const auto hop : kNames) {
    if (iequals(name, hop)) return true;
  }
  return startsWithIgnoreCase(name, "proxy-");
}

// Header names listed (comma separated) in every occurrence of `list_header`, e.g. the
// names a Connection header marks as hop-by-hop for this message.
std::vector<std::string> namesListedIn(const http::fields& fields, std::string_view list_header) {
  std::vector<std::string> names;
  for (const auto& field : fields) {
    if (!iequals(field.name_string(), list_header)) continue;
    std::string_view rest = field.value();
    while (!rest.empty()) {
      const auto comma = rest.find(',');
      const auto token = trim(rest.substr(0, comma));
      if (!token.empty()) names.emplace_back(token);
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  return names;
}

bool contains(const std::vector<std::string>& names, std::string_view name) {
  return std::any_of(names.begin(), names.end(),
                     [&](const std::string& listed) { return iequals(listed, name); });
}

// All values of one header, joined into a single list value ("a, b, c").
std::string joinedValues(const http::fields& fields, std::string_view name) {
  std::string joined;
  for (const auto& field : fields) {
    if (!iequals(field.name_string(), name)) continue;
    const auto value = trim(field.value());
    if (value.empty()) continue;
    if (!joined.empty()) joined += ", ";
    joined.append(value);
  }
  return joined;
}

std::string appended(std::string existing, std::string_view addition) {
  if (!existing.empty()) existing += ", ";
  existing.append(addition);
  return existing;
}

std::string viaValue(unsigned version) {
  return std::string{version == 10 ? "1.0 " : "1.1 "} + std::string{kViaPseudonym};
}

}  // namespace

// --- service mapping -----------------------------------------------------------------

bool isProxyTarget(std::string_view target) noexcept {
  const auto path = target.substr(0, target.find('?'));
  return path == kProxyPrefix || path.substr(0, kProxyRoot.size()) == kProxyRoot;
}

ProxyTargetResult mapProxyTarget(std::string_view target) {
  ProxyTargetResult result;
  std::string_view rest = target.substr(std::min(target.size(), kProxyPrefix.size()));
  if (rest.empty() || rest.front() != '/') {
    result.error = ProxyTargetError::MissingService;
    result.detail = "the request target must be /proxy/{service}/...";
    return result;
  }
  rest.remove_prefix(1);
  const auto end = rest.find_first_of("/?");
  const auto service = rest.substr(0, end);
  if (service.empty()) {
    result.error = ProxyTargetError::MissingService;
    result.detail = "the request target must be /proxy/{service}/...";
    return result;
  }
  if (const auto invalid = discovery::validateServiceName(service)) {
    result.error = ProxyTargetError::InvalidService;
    result.detail = "invalid service name in the request target: " + *invalid;
    return result;
  }

  const auto tail = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
  ProxyRoute route;
  route.service = std::string{service};
  if (tail.empty()) {
    route.upstream_target = "/";
  } else if (tail.front() == '?') {
    route.upstream_target = "/" + std::string{tail};
  } else {
    route.upstream_target = std::string{tail};
  }
  result.route = std::move(route);
  return result;
}

// --- request ids ---------------------------------------------------------------------

std::string generateRequestId() {
  thread_local std::mt19937_64 engine = [] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device()};
    return std::mt19937_64{seed};
  }();
  std::uint64_t high = engine();
  std::uint64_t low = engine();
  high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
  low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;    // RFC 4122 variant

  static constexpr char kHex[] = "0123456789abcdef";
  std::string id(36, '-');
  const auto put = [&](std::size_t position, std::uint64_t value, int nibbles) {
    for (int i = nibbles - 1; i >= 0; --i) {
      id[position + static_cast<std::size_t>(i)] = kHex[value & 0xF];
      value >>= 4;
    }
  };
  put(0, high >> 32, 8);
  put(9, high >> 16, 4);
  put(14, high, 4);
  put(19, low >> 48, 4);
  put(24, low, 12);
  return id;
}

std::string chooseRequestId(const HttpRequest& request) {
  const auto found = request.find(kRequestIdHeader);
  if (found != request.end()) {
    const std::string_view value = trim(found->value());
    const bool usable =
        !value.empty() && value.size() <= kMaxRequestIdLength &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
    if (usable) return std::string{value};
  }
  return generateRequestId();
}

// --- headers -------------------------------------------------------------------------

std::string hostHeaderFor(std::string_view host, unsigned port) {
  std::string value;
  if (host.find(':') != std::string_view::npos) {
    value = "[" + std::string{host} + "]";
  } else {
    value = std::string{host};
  }
  if (port != 80) value += ":" + std::to_string(port);
  return value;
}

HttpRequest makeUpstreamRequest(const HttpRequest& client, std::string upstream_target,
                                const ForwardInfo& info) {
  HttpRequest out{client.method(), upstream_target, 11};
  if (client.method() == http::verb::unknown) out.method_string(client.method_string());

  const auto named_by_connection = namesListedIn(client, "connection");
  for (const auto& field : client) {
    const std::string_view name = field.name_string();
    if (isHopByHop(name) || contains(named_by_connection, name)) continue;
    if (iequals(name, "host") || iequals(name, "expect") || iequals(name, "content-length") ||
        iequals(name, "x-forwarded-for") || iequals(name, "x-forwarded-proto") ||
        iequals(name, "via") || iequals(name, kRequestIdHeader)) {
      continue;
    }
    out.insert(field.name(), name, field.value());
  }

  out.set(http::field::host, info.upstream_host);
  out.set(kRequestIdHeader, info.request_id);
  out.set("X-Forwarded-Proto", "http");
  if (const auto chain = joinedValues(client, "x-forwarded-for");
      !chain.empty() || !info.client_address.empty()) {
    out.set("X-Forwarded-For", info.client_address.empty() ? chain : appended(chain, info.client_address));
  }
  out.set(http::field::via, appended(joinedValues(client, "via"), viaValue(client.version())));

  out.body() = client.body();
  out.prepare_payload();
  return out;
}

HttpResponse makeClientResponse(HttpResponse upstream, const HttpRequest& client,
                                std::string_view request_id) {
  HttpResponse out;
  out.result(upstream.result_int());
  out.version(client.version());
  out.reason(upstream.reason());
  const bool head = client.method() == http::verb::head;

  auto named_by_backend = namesListedIn(upstream, "connection");
  for (auto& name : namesListedIn(upstream, "trailer")) named_by_backend.push_back(std::move(name));
  for (const auto& field : upstream) {
    const std::string_view name = field.name_string();
    if (isHopByHop(name) || contains(named_by_backend, name)) continue;
    if (iequals(name, "via") || iequals(name, kRequestIdHeader)) continue;
    if (iequals(name, "content-length") && !head) continue;  // recomputed from the body
    out.insert(field.name(), name, field.value());
  }
  out.set(http::field::via, appended(joinedValues(upstream, "via"), viaValue(upstream.version())));
  out.set(kRequestIdHeader, request_id);
  out.body() = std::move(upstream.body());
  return out;
}

HttpResponse makeGatewayError(const HttpRequest& client, http::status status,
                              std::string_view detail, std::string_view request_id) {
  auto response = network::makeErrorResponse(client.version(), client.keep_alive(), status, detail);
  response.set(kRequestIdHeader, request_id);
  return response;
}

}  // namespace edgeflow::proxy
