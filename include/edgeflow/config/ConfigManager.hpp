#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "edgeflow/config/Config.hpp"

namespace edgeflow::config {

// Loads and validates YAML configuration into typed structures.
//
// Missing sections and fields fall back to defaults; unknown keys, wrong types
// and out-of-range values are rejected. On failure the previously loaded
// configuration is left untouched and errors() describes every problem found.
class ConfigManager {
 public:
  [[nodiscard]] bool load(const std::filesystem::path& path);
  [[nodiscard]] bool loadFromString(std::string_view yaml, std::string_view source = "<string>");

  [[nodiscard]] const Config& config() const noexcept { return config_; }
  [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

 private:
  Config config_;
  std::vector<std::string> errors_;
};

}  // namespace edgeflow::config
