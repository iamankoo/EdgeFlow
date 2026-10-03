#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace edgeflow::core {

struct CommandLineOptions {
  std::filesystem::path config_path;
  bool show_help{false};
  bool show_version{false};
  bool healthcheck{false};  // probe a running instance's /health endpoint and exit
  std::string error;  // non-empty when the arguments are invalid

  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

inline constexpr std::string_view kDefaultConfigPath = "config/config.yaml";
inline constexpr std::string_view kConfigEnvVariable = "EDGEFLOW_CONFIG";

// Parses argv[1..]. Config path precedence: --config, then `env_config` (the value
// of EDGEFLOW_CONFIG, may be empty), then the default path.
[[nodiscard]] CommandLineOptions parseCommandLine(std::span<const std::string_view> args,
                                                  std::string_view env_config = {});

[[nodiscard]] std::string usage(std::string_view program);

}  // namespace edgeflow::core
