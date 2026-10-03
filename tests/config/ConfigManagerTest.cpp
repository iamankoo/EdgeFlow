#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
  EXPECT_EQ(config.server.request_timeout.count(), 1500);
  EXPECT_EQ(config.server.keep_alive_timeout.count(), 2500);
  EXPECT_EQ(config.server.max_request_body_bytes, 2048U);
  EXPECT_EQ(config.server.max_header_bytes, 4096U);
  EXPECT_EQ(config.server.max_connections, 50U);
  EXPECT_EQ(config.server.worker_threads, 3U);
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
  EXPECT_EQ(config.server.request_timeout.count(), 5000);
  EXPECT_EQ(config.server.keep_alive_timeout.count(), 10000);
  EXPECT_EQ(config.server.max_request_body_bytes, 1024U * 1024U);
  EXPECT_EQ(config.server.max_header_bytes, 8192U);
  EXPECT_EQ(config.server.max_connections, 1024U);
  EXPECT_EQ(config.server.worker_threads, 2U);
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

TEST(ConfigManagerTest, ShippedConfigMatchesDocumentedDefaults) {
  ConfigManager shipped;
  ASSERT_TRUE(shipped.load(EDGEFLOW_DEFAULT_CONFIG));
  ConfigManager defaults;
  ASSERT_TRUE(defaults.loadFromString("application:\n  name: edgeflow\n"));
  const auto& a = shipped.config().server;
  const auto& b = defaults.config().server;
  EXPECT_EQ(a.request_timeout, b.request_timeout);
  EXPECT_EQ(a.keep_alive_timeout, b.keep_alive_timeout);
  EXPECT_EQ(a.max_request_body_bytes, b.max_request_body_bytes);
  EXPECT_EQ(a.max_header_bytes, b.max_header_bytes);
  EXPECT_EQ(a.max_connections, b.max_connections);
  EXPECT_EQ(a.worker_threads, b.worker_threads);
}

TEST(ConfigManagerTest, RejectsOutOfRangeNetworkSettings) {
  const char* const invalid[] = {
      "request_timeout_ms: 9",       "request_timeout_ms: 600001",
      "keep_alive_timeout_ms: 0",    "keep_alive_timeout_ms: -5",
      "max_request_body_bytes: -1",  "max_request_body_bytes: 67108865",
      "max_header_bytes: 1023",      "max_header_bytes: 65537",
      "max_connections: 0",          "max_connections: 100001",
      "worker_threads: 0",           "worker_threads: 65",
      "worker_threads: many",        "request_timeout_ms: 1.5"};
  for (const char* line : invalid) {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{"server:\n  "} + line + "\n")) << line;
    EXPECT_FALSE(manager.errors().empty()) << line;
  }
}

TEST(ConfigManagerTest, AcceptsNetworkSettingBoundaries) {
  const char* const valid[] = {
      "request_timeout_ms: 10",           "request_timeout_ms: 600000",
      "keep_alive_timeout_ms: 10",        "max_request_body_bytes: 0",
      "max_request_body_bytes: 67108864", "max_header_bytes: 1024",
      "max_header_bytes: 65536",          "max_connections: 1",
      "max_connections: 100000",          "worker_threads: 1",
      "worker_threads: 64"};
  for (const char* line : valid) {
    ConfigManager manager;
    EXPECT_TRUE(manager.loadFromString(std::string{"server:\n  "} + line + "\n")) << line;
  }
}

TEST(ConfigManagerTest, NetworkErrorsNameTheOffendingKey) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("server:\n  max_connections: 0\n"));
  EXPECT_TRUE(anyErrorContains(manager, "server.max_connections"));
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

TEST(ConfigManagerDatabaseTest, DefaultsKeepTheRegistryDisabled) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString("server:\n  port: 8080\n"));
  const auto& db = manager.config().database;
  EXPECT_FALSE(db.enabled) << "Phase 2 configurations keep working without a database";
  EXPECT_EQ(db.host, "127.0.0.1");
  EXPECT_EQ(db.port, 5432);
  EXPECT_EQ(db.name, "edgeflow");
  EXPECT_EQ(db.user, "edgeflow");
  EXPECT_EQ(db.password_env, "EDGEFLOW_DB_PASSWORD");
  EXPECT_EQ(db.pool_size, 4U);
  EXPECT_EQ(db.connect_timeout, std::chrono::seconds{5});
}

TEST(ConfigManagerDatabaseTest, ParsesEveryKey) {
  ConfigManager manager;
  ASSERT_TRUE(manager.loadFromString(
      "database:\n  enabled: true\n  host: postgres\n  port: 5433\n  name: registry\n"
      "  user: gateway\n  password_env: MY_DB_PW\n  pool_size: 9\n  connect_timeout_seconds: 12\n"));
  const auto& db = manager.config().database;
  EXPECT_TRUE(db.enabled);
  EXPECT_EQ(db.host, "postgres");
  EXPECT_EQ(db.port, 5433);
  EXPECT_EQ(db.name, "registry");
  EXPECT_EQ(db.user, "gateway");
  EXPECT_EQ(db.password_env, "MY_DB_PW");
  EXPECT_EQ(db.pool_size, 9U);
  EXPECT_EQ(db.connect_timeout, std::chrono::seconds{12});
}

TEST(ConfigManagerDatabaseTest, RejectsUnknownKeysAndInvalidValues) {
  ConfigManager unknown;
  EXPECT_FALSE(unknown.loadFromString("database:\n  passwrd: x\n"));
  EXPECT_TRUE(anyErrorContains(unknown, "unknown key 'database.passwrd'"));

  for (const char* bad : {"enabled: maybe", "port: 0", "port: 70000", "pool_size: 0", "pool_size: 65",
                          "connect_timeout_seconds: 0", "connect_timeout_seconds: 61", "host: 'a b'",
                          "name: 'bad name'", "user: ''", "password_env: 'has space'",
                          "password_env: 1STARTS_WITH_DIGIT", "pool_size: lots"}) {
    ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(std::string{"database:\n  "} + bad + "\n")) << bad;
    EXPECT_TRUE(anyErrorContains(manager, "database.")) << bad;
  }
}

TEST(ConfigManagerDatabaseTest, PasswordItselfCanNeverBeConfigured) {
  ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString("database:\n  password: hunter2\n"));
  EXPECT_TRUE(anyErrorContains(manager, "unknown key 'database.password'"));
  // password_env is a variable NAME: a value that is not a valid name is refused.
  ConfigManager literal;
  EXPECT_FALSE(literal.loadFromString("database:\n  password_env: 'p@ss:word!'\n"));
}

TEST(ConfigManagerDatabaseTest, ShippedConfigurationsParse) {
  ConfigManager shipped;
  ASSERT_TRUE(shipped.load(std::filesystem::path{EDGEFLOW_DEFAULT_CONFIG}));
  EXPECT_FALSE(shipped.config().database.enabled);
  ConfigManager compose;
  ASSERT_TRUE(compose.load(std::filesystem::path{EDGEFLOW_DEFAULT_CONFIG}.parent_path() / "config.compose.yaml"));
  EXPECT_TRUE(compose.config().database.enabled);
  EXPECT_EQ(compose.config().database.host, "postgres");
}

}  // namespace
