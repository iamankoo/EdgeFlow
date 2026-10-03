#include "edgeflow/config/ConfigManager.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace edgeflow::config {

namespace {

constexpr std::uintmax_t kMaxConfigBytes = 1024 * 1024;
constexpr std::size_t kMaxNameLength = 64;
constexpr std::size_t kMaxHostLength = 255;
constexpr long long kMaxGraceSeconds = 300;
constexpr long long kMinTimeoutMs = 10;
constexpr long long kMaxTimeoutMs = 600000;
constexpr long long kMaxBodyBytes = 64LL * 1024 * 1024;
constexpr long long kMinHeaderBytes = 1024;
constexpr long long kMaxHeaderBytes = 65536;
constexpr long long kMaxConnections = 100000;
constexpr long long kMaxWorkerThreads = 64;
constexpr long long kMaxPoolSize = 64;
constexpr long long kMinIntervalMs = 100;
constexpr long long kMaxIntervalMs = 3600000;
constexpr long long kMinProbeTimeoutMs = 10;
constexpr long long kMaxProbeTimeoutMs = 60000;
constexpr long long kMaxThreshold = 100;
constexpr long long kMaxConcurrentChecks = 1024;
constexpr std::size_t kMaxHttpPathLength = 256;
constexpr long long kMaxConnectTimeoutSeconds = 60;

using Errors = std::vector<std::string>;

std::optional<LogLevel> parseLogLevel(std::string_view value) {
  if (value == "trace") return LogLevel::Trace;
  if (value == "debug") return LogLevel::Debug;
  if (value == "info") return LogLevel::Info;
  if (value == "warn") return LogLevel::Warn;
  if (value == "error") return LogLevel::Error;
  if (value == "critical") return LogLevel::Critical;
  if (value == "off") return LogLevel::Off;
  return std::nullopt;
}

std::optional<Environment> parseEnvironment(std::string_view value) {
  if (value == "development") return Environment::Development;
  if (value == "staging") return Environment::Staging;
  if (value == "production") return Environment::Production;
  return std::nullopt;
}

bool isUnset(const YAML::Node& node) { return !node.IsDefined() || node.IsNull(); }

// Returns the named section as a map, or an empty node when it is absent.
// Reports an error (and returns an empty node) when it exists but is not a map.
YAML::Node sectionOf(const YAML::Node& root, const std::string& name, Errors& errors) {
  const YAML::Node section = root[name];
  if (isUnset(section)) return {};
  if (!section.IsMap()) {
    errors.push_back("'" + name + "' must be a mapping");
    return {};
  }
  return section;
}

void rejectUnknownKeys(const YAML::Node& map, const std::string& path,
                       std::initializer_list<std::string_view> allowed, Errors& errors) {
  if (!map.IsMap()) return;
  for (const auto& entry : map) {
    if (!entry.first.IsScalar()) {
      errors.push_back("'" + path + "' contains a non-scalar key");
      continue;
    }
    const std::string key = entry.first.Scalar();
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      errors.push_back("unknown key '" + path + (path.empty() ? "" : ".") + key + "'");
    }
  }
}

std::optional<std::string> readString(const YAML::Node& section, const std::string& path,
                                      const std::string& key, Errors& errors) {
  const YAML::Node value = section[key];
  if (isUnset(value)) return std::nullopt;
  if (!value.IsScalar()) {
    errors.push_back("'" + path + "." + key + "' must be a string");
    return std::nullopt;
  }
  return value.Scalar();
}

std::optional<long long> readInteger(const YAML::Node& section, const std::string& path,
                                     const std::string& key, Errors& errors) {
  const YAML::Node value = section[key];
  if (isUnset(value)) return std::nullopt;
  if (value.IsScalar()) {
    try {
      return value.as<long long>();
    } catch (const YAML::Exception&) {
      // Reported below with the offending text.
    }
  }
  errors.push_back("'" + path + "." + key + "' must be an integer (got '" +
                   (value.IsScalar() ? value.Scalar() : std::string{"<non-scalar>"}) + "')");
  return std::nullopt;
}

std::optional<long long> readBounded(const YAML::Node& section, const std::string& path,
                                     const std::string& key, long long min, long long max,
                                     Errors& errors) {
  const auto value = readInteger(section, path, key, errors);
  if (!value) return std::nullopt;
  if (*value < min || *value > max) {
    errors.push_back("'" + path + "." + key + "' must be between " + std::to_string(min) +
                     " and " + std::to_string(max) + " (got " + std::to_string(*value) + ")");
    return std::nullopt;
  }
  return value;
}

bool isValidName(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameLength) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
  });
}

bool isValidHost(const std::string& host) {
  if (host.empty() || host.size() > kMaxHostLength) return false;
  return std::none_of(host.begin(), host.end(), [](unsigned char c) {
    return std::isspace(c) != 0 || std::iscntrl(c) != 0;
  });
}

void parseApplication(const YAML::Node& root, ApplicationConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "application", errors);
  rejectUnknownKeys(section, "application", {"name", "environment", "log_level"}, errors);

  if (auto name = readString(section, "application", "name", errors)) {
    if (isValidName(*name)) {
      out.name = *name;
    } else {
      errors.push_back("'application.name' must be 1-" + std::to_string(kMaxNameLength) +
                       " characters of [A-Za-z0-9_.-]");
    }
  }
  if (auto env = readString(section, "application", "environment", errors)) {
    if (auto parsed = parseEnvironment(*env)) {
      out.environment = *parsed;
    } else {
      errors.push_back("'application.environment' must be one of development, staging, "
                       "production (got '" + *env + "')");
    }
  }
  if (auto level = readString(section, "application", "log_level", errors)) {
    if (auto parsed = parseLogLevel(*level)) {
      out.log_level = *parsed;
    } else {
      errors.push_back("'application.log_level' must be one of trace, debug, info, warn, "
                       "error, critical, off (got '" + *level + "')");
    }
  }
}

void parseServer(const YAML::Node& root, ServerConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "server", errors);
  rejectUnknownKeys(section, "server",
                    {"host", "port", "request_timeout_ms", "keep_alive_timeout_ms",
                     "max_request_body_bytes", "max_header_bytes", "max_connections",
                     "worker_threads"},
                    errors);

  if (auto host = readString(section, "server", "host", errors)) {
    if (isValidHost(*host)) {
      out.host = *host;
    } else {
      errors.push_back("'server.host' must be a non-empty string without whitespace");
    }
  }
  if (auto v = readBounded(section, "server", "port", 1, 65535, errors)) {
    out.port = static_cast<std::uint16_t>(*v);
  }
  if (auto v = readBounded(section, "server", "request_timeout_ms", kMinTimeoutMs,
                           kMaxTimeoutMs, errors)) {
    out.request_timeout = std::chrono::milliseconds{*v};
  }
  if (auto v = readBounded(section, "server", "keep_alive_timeout_ms", kMinTimeoutMs,
                           kMaxTimeoutMs, errors)) {
    out.keep_alive_timeout = std::chrono::milliseconds{*v};
  }
  if (auto v = readBounded(section, "server", "max_request_body_bytes", 0, kMaxBodyBytes,
                           errors)) {
    out.max_request_body_bytes = static_cast<std::size_t>(*v);
  }
  if (auto v = readBounded(section, "server", "max_header_bytes", kMinHeaderBytes,
                           kMaxHeaderBytes, errors)) {
    out.max_header_bytes = static_cast<std::size_t>(*v);
  }
  if (auto v = readBounded(section, "server", "max_connections", 1, kMaxConnections, errors)) {
    out.max_connections = static_cast<std::size_t>(*v);
  }
  if (auto v = readBounded(section, "server", "worker_threads", 1, kMaxWorkerThreads, errors)) {
    out.worker_threads = static_cast<unsigned>(*v);
  }
}

bool isValidEnvName(const std::string& name) {
  if (name.empty()) return true;  // no password
  if (name.size() > kMaxNameLength) return false;
  if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_';
  });
}

void parseDatabase(const YAML::Node& root, DatabaseConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "database", errors);
  rejectUnknownKeys(section, "database",
                    {"enabled", "host", "port", "name", "user", "password_env", "pool_size",
                     "connect_timeout_seconds"},
                    errors);

  if (auto enabled = readString(section, "database", "enabled", errors)) {
    if (*enabled == "true") {
      out.enabled = true;
    } else if (*enabled == "false") {
      out.enabled = false;
    } else {
      errors.push_back("'database.enabled' must be true or false (got '" + *enabled + "')");
    }
  }
  if (auto host = readString(section, "database", "host", errors)) {
    if (isValidHost(*host)) {
      out.host = *host;
    } else {
      errors.push_back("'database.host' must be a non-empty string without whitespace");
    }
  }
  if (auto v = readBounded(section, "database", "port", 1, 65535, errors)) {
    out.port = static_cast<std::uint16_t>(*v);
  }
  if (auto name = readString(section, "database", "name", errors)) {
    if (isValidName(*name)) {
      out.name = *name;
    } else {
      errors.push_back("'database.name' must be 1-" + std::to_string(kMaxNameLength) +
                       " characters of [A-Za-z0-9_.-]");
    }
  }
  if (auto user = readString(section, "database", "user", errors)) {
    if (isValidName(*user)) {
      out.user = *user;
    } else {
      errors.push_back("'database.user' must be 1-" + std::to_string(kMaxNameLength) +
                       " characters of [A-Za-z0-9_.-]");
    }
  }
  if (auto env = readString(section, "database", "password_env", errors)) {
    if (isValidEnvName(*env)) {
      out.password_env = *env;
    } else {
      errors.push_back("'database.password_env' must be the name of an environment variable "
                       "([A-Za-z_][A-Za-z0-9_]*), never the password itself");
    }
  }
  if (auto v = readBounded(section, "database", "pool_size", 1, kMaxPoolSize, errors)) {
    out.pool_size = static_cast<unsigned>(*v);
  }
  if (auto v = readBounded(section, "database", "connect_timeout_seconds", 1,
                           kMaxConnectTimeoutSeconds, errors)) {
    out.connect_timeout = std::chrono::seconds{*v};
  }
}

bool isValidHttpPath(const std::string& path) {
  if (path.empty() || path.size() > kMaxHttpPathLength || path.front() != '/') return false;
  // Visible ASCII only, no '#': the path is sent verbatim in a request line.
  return std::all_of(path.begin(), path.end(), [](unsigned char c) {
    return c > 0x20 && c < 0x7f && c != '#';
  });
}

void parseHealthCheck(const YAML::Node& root, HealthCheckConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "health_check", errors);
  rejectUnknownKeys(section, "health_check",
                    {"enabled", "type", "interval_ms", "timeout_ms", "http_path",
                     "failure_threshold", "success_threshold", "refresh_interval_ms",
                     "max_concurrent_checks"},
                    errors);

  if (auto enabled = readString(section, "health_check", "enabled", errors)) {
    if (*enabled == "true") {
      out.enabled = true;
    } else if (*enabled == "false") {
      out.enabled = false;
    } else {
      errors.push_back("'health_check.enabled' must be true or false (got '" + *enabled + "')");
    }
  }
  if (auto type = readString(section, "health_check", "type", errors)) {
    if (*type == "tcp") {
      out.type = HealthCheckType::Tcp;
    } else if (*type == "http") {
      out.type = HealthCheckType::Http;
    } else {
      errors.push_back("'health_check.type' must be tcp or http (got '" + *type + "')");
    }
  }
  if (auto v = readBounded(section, "health_check", "interval_ms", kMinIntervalMs, kMaxIntervalMs,
                           errors)) {
    out.interval = std::chrono::milliseconds{*v};
  }
  if (auto v = readBounded(section, "health_check", "timeout_ms", kMinProbeTimeoutMs,
                           kMaxProbeTimeoutMs, errors)) {
    out.timeout = std::chrono::milliseconds{*v};
  }
  if (auto path = readString(section, "health_check", "http_path", errors)) {
    if (isValidHttpPath(*path)) {
      out.http_path = *path;
    } else {
      errors.push_back("'health_check.http_path' must start with '/' and contain only visible "
                       "ASCII characters without '#' (at most " +
                       std::to_string(kMaxHttpPathLength) + ")");
    }
  }
  if (auto v = readBounded(section, "health_check", "failure_threshold", 1, kMaxThreshold,
                           errors)) {
    out.failure_threshold = static_cast<unsigned>(*v);
  }
  if (auto v = readBounded(section, "health_check", "success_threshold", 1, kMaxThreshold,
                           errors)) {
    out.success_threshold = static_cast<unsigned>(*v);
  }
  if (auto v = readBounded(section, "health_check", "refresh_interval_ms", kMinIntervalMs,
                           kMaxIntervalMs, errors)) {
    out.refresh_interval = std::chrono::milliseconds{*v};
  }
  if (auto v = readBounded(section, "health_check", "max_concurrent_checks", 1,
                           kMaxConcurrentChecks, errors)) {
    out.max_concurrent_checks = static_cast<unsigned>(*v);
  }
  if (out.timeout > out.interval) {
    errors.push_back("'health_check.timeout_ms' (" + std::to_string(out.timeout.count()) +
                     ") must not exceed 'health_check.interval_ms' (" +
                     std::to_string(out.interval.count()) + ")");
  }
}

void parseRouting(const YAML::Node& root, RoutingConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "routing", errors);
  rejectUnknownKeys(section, "routing", {"strategy"}, errors);

  if (auto strategy = readString(section, "routing", "strategy", errors)) {
    if (*strategy == "round_robin") {
      out.strategy = RoutingStrategy::RoundRobin;
    } else if (*strategy == "least_connections") {
      out.strategy = RoutingStrategy::LeastConnections;
    } else if (*strategy == "weighted") {
      out.strategy = RoutingStrategy::Weighted;
    } else if (*strategy == "consistent_hashing") {
      out.strategy = RoutingStrategy::ConsistentHashing;
    } else {
      errors.push_back("'routing.strategy' must be one of round_robin, least_connections, "
                       "weighted, consistent_hashing (got '" + *strategy + "')");
    }
  }
}

void parseShutdown(const YAML::Node& root, ShutdownConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "shutdown", errors);
  rejectUnknownKeys(section, "shutdown", {"grace_period_seconds"}, errors);

  if (auto v = readBounded(section, "shutdown", "grace_period_seconds", 0, kMaxGraceSeconds,
                           errors)) {
    out.grace_period = std::chrono::seconds{*v};
  }
}

}  // namespace

bool ConfigManager::load(const std::filesystem::path& path) {
  errors_.clear();
  const std::string source = path.string();

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    errors_.push_back("configuration file not found: " + source);
    return false;
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    errors_.push_back("configuration path is not a regular file: " + source);
    return false;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    errors_.push_back("cannot determine size of configuration file " + source + ": " +
                      ec.message());
    return false;
  }
  if (size > kMaxConfigBytes) {
    errors_.push_back("configuration file is too large (" + std::to_string(size) +
                      " bytes, limit " + std::to_string(kMaxConfigBytes) + "): " + source);
    return false;
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    errors_.push_back("cannot open configuration file: " + source);
    return false;
  }
  const std::string contents((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
  return loadFromString(contents, source);
}

bool ConfigManager::loadFromString(std::string_view yaml, std::string_view source) {
  errors_.clear();

  YAML::Node root;
  try {
    root = YAML::Load(std::string{yaml});
  } catch (const YAML::Exception& e) {
    errors_.push_back("failed to parse YAML in " + std::string{source} + ": " + e.what());
    return false;
  }

  if (root.IsNull() || !root.IsDefined()) {
    errors_.push_back("configuration is empty: " + std::string{source});
    return false;
  }
  if (!root.IsMap()) {
    errors_.push_back("configuration root must be a mapping: " + std::string{source});
    return false;
  }

  Config parsed;
  Errors errors;
  rejectUnknownKeys(root, "", {"application", "server", "database", "health_check", "routing", "shutdown"}, errors);
  parseApplication(root, parsed.application, errors);
  parseServer(root, parsed.server, errors);
  parseDatabase(root, parsed.database, errors);
  parseHealthCheck(root, parsed.health_check, errors);
  parseRouting(root, parsed.routing, errors);
  if (parsed.health_check.enabled && !parsed.database.enabled) {
    errors.push_back("'health_check.enabled' requires 'database.enabled': health checking reads "
                     "instances from, and writes health to, the service registry");
  }
  parseShutdown(root, parsed.shutdown, errors);

  if (!errors.empty()) {
    errors_ = std::move(errors);
    return false;
  }
  config_ = std::move(parsed);
  return true;
}

std::string_view toString(Environment environment) noexcept {
  switch (environment) {
    case Environment::Development: return "development";
    case Environment::Staging: return "staging";
    case Environment::Production: return "production";
  }
  return "unknown";
}

std::string_view toString(HealthCheckType type) noexcept {
  switch (type) {
    case HealthCheckType::Tcp: return "tcp";
    case HealthCheckType::Http: return "http";
  }
  return "tcp";
}

std::string_view toString(RoutingStrategy strategy) noexcept {
  switch (strategy) {
    case RoutingStrategy::RoundRobin: return "round_robin";
    case RoutingStrategy::LeastConnections: return "least_connections";
    case RoutingStrategy::Weighted: return "weighted";
    case RoutingStrategy::ConsistentHashing: return "consistent_hashing";
  }
  return "round_robin";
}

std::string_view toString(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Critical: return "critical";
    case LogLevel::Off: return "off";
  }
  return "unknown";
}

}  // namespace edgeflow::config
