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
  rejectUnknownKeys(section, "server", {"host", "port"}, errors);

  if (auto host = readString(section, "server", "host", errors)) {
    if (isValidHost(*host)) {
      out.host = *host;
    } else {
      errors.push_back("'server.host' must be a non-empty string without whitespace");
    }
  }
  if (auto port = readInteger(section, "server", "port", errors)) {
    if (*port >= 1 && *port <= 65535) {
      out.port = static_cast<std::uint16_t>(*port);
    } else {
      errors.push_back("'server.port' must be between 1 and 65535 (got " +
                       std::to_string(*port) + ")");
    }
  }
}

void parseShutdown(const YAML::Node& root, ShutdownConfig& out, Errors& errors) {
  const YAML::Node section = sectionOf(root, "shutdown", errors);
  rejectUnknownKeys(section, "shutdown", {"grace_period_seconds"}, errors);

  if (auto grace = readInteger(section, "shutdown", "grace_period_seconds", errors)) {
    if (*grace >= 0 && *grace <= kMaxGraceSeconds) {
      out.grace_period = std::chrono::seconds{*grace};
    } else {
      errors.push_back("'shutdown.grace_period_seconds' must be between 0 and " +
                       std::to_string(kMaxGraceSeconds) + " (got " + std::to_string(*grace) +
                       ")");
    }
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
  rejectUnknownKeys(root, "", {"application", "server", "shutdown"}, errors);
  parseApplication(root, parsed.application, errors);
  parseServer(root, parsed.server, errors);
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
