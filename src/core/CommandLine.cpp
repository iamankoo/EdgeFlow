#include "edgeflow/core/CommandLine.hpp"

namespace edgeflow::core {

CommandLineOptions parseCommandLine(std::span<const std::string_view> args,
                                    std::string_view env_config) {
  CommandLineOptions options;
  std::string_view config_path;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "-h" || arg == "--help") {
      options.show_help = true;
    } else if (arg == "--version") {
      options.show_version = true;
    } else if (arg == "--healthcheck") {
      options.healthcheck = true;
    } else if (arg == "-c" || arg == "--config") {
      if (i + 1 >= args.size() || args[i + 1].empty()) {
        options.error = "option '" + std::string{arg} + "' requires a path argument";
        return options;
      }
      config_path = args[++i];
    } else {
      options.error = "unknown argument '" + std::string{arg} + "'";
      return options;
    }
  }

  if (config_path.empty()) config_path = env_config;
  if (config_path.empty()) config_path = kDefaultConfigPath;
  options.config_path = std::filesystem::path{config_path};
  return options;
}

std::string usage(std::string_view program) {
  std::string text = "Usage: " + std::string{program} + " [options]\n\n"
                     "Options:\n"
                     "  -c, --config <path>  Configuration file (default: ";
  text += kDefaultConfigPath;
  text += ", or $";
  text += kConfigEnvVariable;
  text += ")\n"
          "      --version        Print the version and exit\n"
          "      --healthcheck    Probe GET /health on the configured endpoint; exit 0 if healthy\n"
          "  -h, --help           Show this help and exit\n";
  return text;
}

}  // namespace edgeflow::core
