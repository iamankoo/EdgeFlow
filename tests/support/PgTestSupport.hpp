#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "edgeflow/discovery/PostgresServiceRegistry.hpp"
#include "edgeflow/storage/Migrator.hpp"
#include "edgeflow/storage/Postgres.hpp"
#include "support/TestLogger.hpp"

namespace edgeflow::testing {

// Tests that need a real PostgreSQL read its address from the environment and are
// SKIPPED (visibly, with the reason) when EDGEFLOW_TEST_DB_HOST is not set:
//   EDGEFLOW_TEST_DB_HOST      required to enable the tests
//   EDGEFLOW_TEST_DB_PORT      default 5432
//   EDGEFLOW_TEST_DB_NAME      default edgeflow
//   EDGEFLOW_TEST_DB_USER      default edgeflow
//   EDGEFLOW_TEST_DB_PASSWORD  default empty
inline std::string envOr(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value != nullptr ? value : fallback;
}

inline std::optional<storage::ConnectionParams> testDatabase() {
  const char* host = std::getenv("EDGEFLOW_TEST_DB_HOST");
  if (host == nullptr || *host == '\0') return std::nullopt;
  storage::ConnectionParams params;
  params.host = host;
  params.port = static_cast<std::uint16_t>(std::stoi(envOr("EDGEFLOW_TEST_DB_PORT", "5432")));
  params.database = envOr("EDGEFLOW_TEST_DB_NAME", "edgeflow");
  params.user = envOr("EDGEFLOW_TEST_DB_USER", "edgeflow");
  params.password = envOr("EDGEFLOW_TEST_DB_PASSWORD", "");
  params.connect_timeout = std::chrono::seconds{5};
  return params;
}

#define EDGEFLOW_REQUIRE_TEST_DATABASE()                                                      \
  const auto test_db_params = ::edgeflow::testing::testDatabase();                           \
  if (!test_db_params) GTEST_SKIP() << "EDGEFLOW_TEST_DB_HOST is not set: no PostgreSQL available"

// A service name unique to this process and call, so parallel or repeated runs never
// collide and tests never touch each other's rows.
inline std::string uniqueName(const std::string& prefix) {
  static std::atomic<unsigned> counter{0};
  static const unsigned run = std::random_device{}();
  return prefix + "-" + std::to_string(run % 100000) + "-" + std::to_string(++counter);
}

// Connection parameters that cannot connect (nothing listens on port 1).
inline storage::ConnectionParams unreachableDatabase() {
  storage::ConnectionParams params;
  params.host = "127.0.0.1";
  params.port = 1;
  params.connect_timeout = std::chrono::seconds{1};
  return params;
}

// Pool + migrated schema + PostgreSQL registry, with cleanup of every service the test
// created. Skips the test when no database is configured.
class RegistryFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto params = testDatabase();
    if (!params) GTEST_SKIP() << "EDGEFLOW_TEST_DB_HOST is not set: no PostgreSQL available";
    params_ = *params;
    pool = std::make_shared<storage::PgPool>(params_, 8, log.logger());
    const auto report = storage::migrate(*pool, storage::builtinMigrations(), *log.logger());
    ASSERT_TRUE(report.ok) << report.error;
    registry = std::make_shared<discovery::PostgresServiceRegistry>(pool, log.logger());
  }

  void TearDown() override {
    if (!pool) return;
    auto lease = pool->acquire();
    if (!lease) return;
    for (const auto& name : created_) {
      (void)lease->exec("DELETE FROM services WHERE name = $1", {storage::PgParam{name}});
    }
  }

  // Returns a fresh service name and schedules its removal after the test.
  std::string newService(const std::string& prefix = "svc") {
    auto name = uniqueName(prefix);
    created_.push_back(name);
    return name;
  }

  storage::ConnectionParams params_;
  CapturedLogger log;
  std::shared_ptr<storage::PgPool> pool;
  std::shared_ptr<discovery::PostgresServiceRegistry> registry;

 private:
  std::vector<std::string> created_;
};

inline discovery::NewInstance makeInstance(const std::string& service, const std::string& host,
                                           std::uint16_t port) {
  discovery::NewInstance instance;
  instance.service = service;
  instance.host = host;
  instance.port = port;
  return instance;
}

}  // namespace edgeflow::testing
