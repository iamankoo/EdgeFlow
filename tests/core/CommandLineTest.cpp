#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "edgeflow/core/CommandLine.hpp"

namespace {

using edgeflow::core::parseCommandLine;

TEST(CommandLineTest, DefaultsToBuiltInConfigPath) {
  const auto options = parseCommandLine({});
  ASSERT_TRUE(options.ok());
  EXPECT_EQ(options.config_path, "config/config.yaml");
}

TEST(CommandLineTest, EnvironmentOverridesDefault) {
  const auto options = parseCommandLine({}, "/etc/edgeflow/config.yaml");
  EXPECT_EQ(options.config_path, "/etc/edgeflow/config.yaml");
}

TEST(CommandLineTest, FlagOverridesEnvironment) {
  const std::vector<std::string_view> args{"--config", "custom.yaml"};
  const auto options = parseCommandLine(args, "/etc/edgeflow/config.yaml");
  ASSERT_TRUE(options.ok());
  EXPECT_EQ(options.config_path, "custom.yaml");
}

TEST(CommandLineTest, ShortFlagWorks) {
  const std::vector<std::string_view> args{"-c", "short.yaml"};
  EXPECT_EQ(parseCommandLine(args).config_path, "short.yaml");
}

TEST(CommandLineTest, HelpAndVersion) {
  const std::vector<std::string_view> help{"--help"};
  const std::vector<std::string_view> version{"--version"};
  EXPECT_TRUE(parseCommandLine(help).show_help);
  EXPECT_TRUE(parseCommandLine(version).show_version);
}

TEST(CommandLineTest, HealthcheckFlag) {
  const std::vector<std::string_view> args{"--healthcheck", "-c", "x.yaml"};
  const auto options = parseCommandLine(args);
  ASSERT_TRUE(options.ok());
  EXPECT_TRUE(options.healthcheck);
  EXPECT_EQ(options.config_path, "x.yaml");
  EXPECT_FALSE(parseCommandLine({}).healthcheck);
}

TEST(CommandLineTest, MissingConfigValueIsAnError) {
  const std::vector<std::string_view> args{"--config"};
  const auto options = parseCommandLine(args);
  EXPECT_FALSE(options.ok());
}

TEST(CommandLineTest, UnknownArgumentIsAnError) {
  const std::vector<std::string_view> args{"--bogus"};
  const auto options = parseCommandLine(args);
  EXPECT_FALSE(options.ok());
  EXPECT_NE(options.error.find("--bogus"), std::string::npos);
}

}  // namespace
