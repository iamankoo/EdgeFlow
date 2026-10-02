#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "edgeflow/config/ConfigManager.hpp"

namespace {

using edgeflow::config::ConfigManager;
using edgeflow::config::Environment;
using edgeflow::config::LogLevel;

const std::filesystem::path kFixtures{EDGEFLOW_TEST_FIXTURES_DIR};

bool anyErrorContains(const ConfigManager& manager, const std::string& text) {
  const auto& errors = manager.errors();
  return std::any_of(errors.begin(), errors.end(),
                     [&](const std::string& e) { return e.find(text) != std::string::npos; });
}

TEST(ConfigManagerTest, LoadsValidFile) {
  ConfigManager manager;
  ASSERT_TRUE(manager.load(kFixtures / "valid.yaml"));
  const auto& config = manager.config();
  EXPECT_EQ(config.application.name, "edgeflow-test");
  EXPECT_EQ(config.application.environment, Environment::Staging);
  EXPECT_EQ(config.application.log_level, LogLevel::Debug);
  EXPECT_EQ(config.server.host, "127.0.0.1");
  EXPECT_EQ(config.server.port, 9090);
  EXPECT_EQ(config.shutdown.grace_period.count(), 3);
  EXPECT_TRUE(manager.errors().empty());
}

TEST(ConfigManagerTest, ShippedDefaultConfigIsValid) {
  ConfigManager manager;
  ASSERT_TRUE(manager.load(EDGEFLOW_DEFAULT_CONFIG));
  EXPECT_EQ(manager.config().application.name, "edgeflow");
  EXPECT_EQ(manager.config().server.port, 8080);
}

TEST(ConfigManagerTest, MissingFileFails) {
  ConfigManager manager;
  EXPECT_FALSE(manager.load(kFixtures / "does_not_exist.yaml"));
  EXPECT_TRUE(anyErrorContains(manager, "not found"));
}

TEST(ConfigManagerTest, DirectoryPathFails) {
  ConfigManager manager;
  EXPECT_FALSE(manager.load(kFixtures));
  EXPECT_TRUE(anyErrorContains(manager, "not a regular file"));
}

TEST(ConfigManagerTest, InvalidPortFileFails) {
  ConfigManager manager;
  EXPECT_FALSE(manager.load(kFixtures / "invalid_port.yaml"));
  EXPECT_TRUE(anyErrorContains(manager, "server.port"));
}

TEST(ConfigManagerTest, PartialConfigUsesDefaults) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("application:\n  name: only-name\n"));
  const auto& config = manager.config();
  EXPECT_EQ(config.application.name, "only-name");
  EXPECT_EQ(config.application.environment, Environment::Development);
  EXPECT_EQ(config.application.log_level, LogLevel::Info);
  EXPECT_EQ(config.server.host, "0.0.0.0");
  EXPECT_EQ(config.server.port, 8080);
  EXPECT_EQ(config.shutdown.grace_period.count(), 5);
}

TEST(ConfigManagerTest, NullSectionUsesDefaults) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("application:\nserver:\n"));
  EXPECT_EQ(manager.config().server.port, 8080);
}

TEST(ConfigManagerTest, EmptyDocumentFails) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString(""));
  EXPECT_TRUE(anyErrorContains(manager, "empty"));
}

TEST(ConfigManagerTest, MalformedYamlFails) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("application: [unclosed\n  - : :"));
  EXPECT_TRUE(anyErrorContains(manager, "failed to parse"));
}

TEST(ConfigManagerTest, NonMapRootFails) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("- a\n- b\n"));
  EXPECT_TRUE(anyErrorContains(manager, "mapping"));
}

TEST(ConfigManagerTest, SectionMustBeMapping) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("server: 8080\n"));
  EXPECT_TRUE(anyErrorContains(manager, "'server' must be a mapping"));
}

TEST(ConfigManagerTest, UnknownKeysAreRejected) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("server:\n  prot: 80\nextra: 1\n"));
  EXPECT_TRUE(anyErrorContains(manager, "unknown key 'server.prot'"));
  EXPECT_TRUE(anyErrorContains(manager, "unknown key 'extra'"));
}

TEST(ConfigManagerTest, RejectsOutOfRangePorts) {
  for (const char* port : {"0", "-1", "65536", "99999"}) {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{"server:\n  port: "} + port + "\n"))
        << "port " << port;
  }
}

TEST(ConfigManagerTest, AcceptsPortBoundaries) {
  for (const char* port : {"1", "65535"}) {
    ConfigManager manager;
    EXPECT_TRUE(manager.loadFromString(std::string{"server:\n  port: "} + port + "\n"))
        << "port " << port;
  }
}

TEST(ConfigManagerTest, RejectsNonIntegerPort) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("server:\n  port: eighty\n"));
  EXPECT_TRUE(anyErrorContains(manager, "must be an integer"));
}

TEST(ConfigManagerTest, RejectsFractionalPort) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("server:\n  port: 80.5\n"));
}

TEST(ConfigManagerTest, RejectsInvalidLogLevelAndEnvironment) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString(
      "application:\n  log_level: verbose\n  environment: prod\n"));
  EXPECT_TRUE(anyErrorContains(manager, "log_level"));
  EXPECT_TRUE(anyErrorContains(manager, "environment"));
}

TEST(ConfigManagerTest, RejectsInvalidNameAndHost) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString(
      "application:\n  name: \"bad name!\"\nserver:\n  host: \"has space\"\n"));
  EXPECT_TRUE(anyErrorContains(manager, "application.name"));
  EXPECT_TRUE(anyErrorContains(manager, "server.host"));
}

TEST(ConfigManagerTest, GracePeriodBounds) {
  ConfigManager ok;
  EXPECT_TRUE(ok.loadFromString("shutdown:\n  grace_period_seconds: 0\n"));
  EXPECT_EQ(ok.config().shutdown.grace_period.count(), 0);

  ConfigManager too_big;
  EXPECT_FALSE(too_big.loadFromString("shutdown:\n  grace_period_seconds: 301\n"));
  ConfigManager negative;
  EXPECT_FALSE(negative.loadFromString("shutdown:\n  grace_period_seconds: -1\n"));
}

TEST(ConfigManagerTest, ReportsAllErrorsAtOnce) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("server:\n  port: 0\nshutdown:\n  grace_period_seconds: 999\n"));
  EXPECT_GE(manager.errors().size(), 2U);
}

TEST(ConfigManagerTest, FailedLoadKeepsPreviousConfig) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("server:\n  port: 1234\n"));
  EXPECT_FALSE(manager.loadFromString("server:\n  port: 0\n"));
  EXPECT_EQ(manager.config().server.port, 1234);
}

TEST(ConfigManagerTest, LoadsFileWrittenToTemporaryDirectory) {
  const auto path = std::filesystem::temp_directory_path() / "edgeflow_config_test.yaml";
  {
    std::ofstream out(path);
    out << "application:\n  environment: production\n";
  }
  ConfigManager manager;
  EXPECT_TRUE(manager.load(path));
  EXPECT_EQ(manager.config().application.environment, Environment::Production);
  std::filesystem::remove(path);
}

}  // namespace
