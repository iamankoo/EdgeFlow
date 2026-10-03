#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edgeflow/config/ConfigManager.hpp"
#include "edgeflow/core/Application.hpp"
#include "edgeflow/core/CommandLine.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/HealthProbe.hpp"

namespace {
constexpr int kExitOk = 0;
constexpr int kExitInitFailure = 1;
constexpr int kExitUsageOrConfig = 2;
constexpr std::chrono::milliseconds kHealthProbeTimeout{2000};
}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  const char* env_config = std::getenv(std::string{edgeflow::core::kConfigEnvVariable}.c_str());
  const auto options =
      edgeflow::core::parseCommandLine(args, env_config != nullptr ? env_config : "");

  const std::string_view program = argc > 0 ? argv[0] : "edgeflow";
  if (!options.ok()) {
    std::cerr << "error: " << options.error << "\n\n" << edgeflow::core::usage(program);
    return kExitUsageOrConfig;
  }
  if (options.show_help) {
    std::cout << edgeflow::core::usage(program);
    return kExitOk;
  }
  if (options.show_version) {
    std::cout << "edgeflow " << EDGEFLOW_VERSION << '\n';
    return kExitOk;
  }

  // Configuration is loaded before logging exists, so failures go to stderr.
  edgeflow::config::ConfigManager manager;
  if (!manager.load(options.config_path)) {
    std::cerr << "error: failed to load configuration:\n";
    for (const auto& error : manager.errors()) std::cerr << "  - " << error << '\n';
    return kExitUsageOrConfig;
  }
  const auto& config = manager.config();

  if (options.healthcheck) {
    // A wildcard bind address is not connectable; probe the loopback interface instead.
    const bool wildcard = config.server.host == "0.0.0.0" || config.server.host == "::";
    const std::string host = wildcard ? "127.0.0.1" : config.server.host;
    std::string detail;
    if (!edgeflow::network::probeHealth(host, config.server.port, kHealthProbeTimeout, detail)) {
      std::cerr << "unhealthy: " << host << ':' << config.server.port << ": " << detail << '\n';
      return kExitInitFailure;
    }
    return kExitOk;
  }

  try {
    edgeflow::logging::LoggerOptions logger_options;
    logger_options.name = config.application.name;
    logger_options.level = config.application.log_level;
    auto logger = std::make_shared<edgeflow::logging::Logger>(std::move(logger_options));

    logger->info("EdgeFlow starting (version {})", EDGEFLOW_VERSION);
    logger->info("configuration loaded from {}", options.config_path.string());

    edgeflow::core::Application application(config, logger);
    if (!application.initialize()) {
      logger->error("application initialization failed");
      logger->flush();
      return kExitInitFailure;
    }
    return application.run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return kExitInitFailure;
  }
}
