#include "edgeflow/discovery/Validation.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

#include <boost/asio/ip/address.hpp>

namespace edgeflow::discovery {

namespace {

constexpr std::size_t kMaxServiceNameLength = 64;
constexpr std::size_t kMaxInstanceIdLength = 128;
constexpr std::size_t kMaxHostLength = 253;
constexpr std::size_t kMaxLabelLength = 63;

bool isLowerAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

}  // namespace

std::optional<std::string> validateServiceName(std::string_view name) {
  if (name.empty() || name.size() > kMaxServiceNameLength) {
    return "service name must be 1-64 characters";
  }
  const bool edges_ok = isLowerAlnum(name.front()) && isLowerAlnum(name.back());
  const bool chars_ok = std::all_of(name.begin(), name.end(), [](char c) {
    return isLowerAlnum(c) || c == '.' || c == '_' || c == '-';
  });
  if (!edges_ok || !chars_ok) {
    return "service name must contain only lowercase letters, digits, '.', '_' and '-', "
           "and start and end with a letter or digit";
  }
  return std::nullopt;
}

std::optional<std::string> validateInstanceId(std::string_view id) {
  if (id.empty() || id.size() > kMaxInstanceIdLength) {
    return "instance id must be 1-128 characters";
  }
  const bool ok = std::all_of(id.begin(), id.end(), [](char c) {
    return isAlnum(c) || c == '.' || c == '_' || c == '-';
  });
  if (!ok) return "instance id may only contain letters, digits, '.', '_' and '-'";
  return std::nullopt;
}

std::optional<std::string> validateHost(std::string_view host) {
  if (host.empty() || host.size() > kMaxHostLength) return "host must be 1-253 characters";

  boost::system::error_code ec;
  (void)boost::asio::ip::make_address(std::string{host}, ec);
  if (!ec) return std::nullopt;  // IPv4 or IPv6 literal

  // RFC 1123 host name: dot-separated labels of letters, digits and inner hyphens.
  std::size_t label_start = 0;
  while (label_start <= host.size()) {
    const auto dot = host.find('.', label_start);
    const auto label = host.substr(
        label_start, dot == std::string_view::npos ? std::string_view::npos : dot - label_start);
    const bool label_ok =
        !label.empty() && label.size() <= kMaxLabelLength && label.front() != '-' &&
        label.back() != '-' &&
        std::all_of(label.begin(), label.end(), [](char c) { return isAlnum(c) || c == '-'; });
    if (!label_ok) return "host must be an IP address or a valid host name";
    if (dot == std::string_view::npos) break;
    label_start = dot + 1;
  }
  return std::nullopt;
}

std::optional<std::string> validatePort(std::uint32_t port) {
  if (port < 1 || port > 65535) return "port must be between 1 and 65535";
  return std::nullopt;
}

std::optional<std::string> validateVersion(std::string_view version) {
  if (version.size() > kMaxVersionLength) return "version must be at most 64 characters";
  const bool printable = std::all_of(version.begin(), version.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x20 && u != 0x7f;
  });
  if (!printable) return "version must not contain control characters";
  return std::nullopt;
}

std::optional<std::string> validateWeight(std::uint64_t weight) {
  if (weight > kMaxWeight) return "weight must be between 0 and 1000";
  return std::nullopt;
}

std::optional<std::string> validateConnectionCount(std::uint64_t count) {
  if (count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return "connection_count is too large";
  }
  return std::nullopt;
}

std::optional<std::string> validate(const NewInstance& instance) {
  if (auto e = validateServiceName(instance.service)) return e;
  if (instance.instance_id) {
    if (auto e = validateInstanceId(*instance.instance_id)) return e;
  }
  if (auto e = validateHost(instance.host)) return e;
  if (auto e = validatePort(instance.port)) return e;
  if (auto e = validateVersion(instance.version)) return e;
  if (auto e = validateWeight(instance.weight)) return e;
  if (auto e = validateConnectionCount(instance.connection_count)) return e;
  return std::nullopt;
}

std::optional<std::string> validate(const InstanceUpdate& update) {
  if (update.empty()) return "no fields to update";
  if (update.version) {
    if (auto e = validateVersion(*update.version)) return e;
  }
  if (update.weight) {
    if (auto e = validateWeight(*update.weight)) return e;
  }
  if (update.connection_count) {
    if (auto e = validateConnectionCount(*update.connection_count)) return e;
  }
  return std::nullopt;
}

}  // namespace edgeflow::discovery
