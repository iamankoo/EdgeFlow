#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "support/PgTestSupport.hpp"

namespace {

using namespace edgeflow;
using edgeflow::storage::PgParam;
using edgeflow::storage::PgPool;
using edgeflow::testing::CapturedLogger;

// --- no database required ---------------------------------------------------------

TEST(PgPoolUnavailableTest, AcquireFailsCleanlyWhenNothingListens) {
  CapturedLogger log;
  PgPool pool(edgeflow::testing::unreachableDatabase(), 2, log.logger());
  const auto lease = pool.acquire();
  EXPECT_FALSE(lease);
  EXPECT_NE(lease.error().find("cannot connect to the database"), std::string::npos) << lease.error();
}

TEST(PgPoolUnavailableTest, PasswordNeverAppearsInErrorsOrLogs) {
  CapturedLogger log;
  auto params = edgeflow::testing::unreachableDatabase();
  params.password = "s3cr3t-do-not-log";
  PgPool pool(params, 1, log.logger());
  const auto lease = pool.acquire();
  ASSERT_FALSE(lease);
  EXPECT_EQ(lease.error().find("s3cr3t-do-not-log"), std::string::npos);
  EXPECT_FALSE(log.contains("s3cr3t-do-not-log"));
}

TEST(MigratorUnavailableTest, ReportsFailureWhenDatabaseIsDown) {
  CapturedLogger log;
  PgPool pool(edgeflow::testing::unreachableDatabase(), 1, log.logger());
  const auto report = storage::migrate(pool, storage::builtinMigrations(), *log.logger());
  EXPECT_FALSE(report.ok);
  EXPECT_FALSE(report.error.empty());
  EXPECT_EQ(report.applied, 0U);
}

TEST(MigratorTest, BuiltinMigrationsAreEmbeddedAndOrdered) {
  const auto& migrations = storage::builtinMigrations();
  ASSERT_FALSE(migrations.empty());
  EXPECT_EQ(migrations.front().name, "001_service_registry");
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE services"), std::string_view::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE service_instances"), std::string_view::npos);
  for (std::size_t i = 1; i < migrations.size(); ++i) {
    EXPECT_LT(migrations[i - 1].name, migrations[i].name);
  }
}

// --- real PostgreSQL ---------------------------------------------------------------

TEST(PgPoolTest, ExecutesParameterisedQueriesIncludingNull) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  PgPool pool(*test_db_params, 2, log.logger());
  auto lease = pool.acquire();
  ASSERT_TRUE(lease) << lease.error();

  const auto result = lease->exec("SELECT $1::text, $2::int, $3::text IS NULL",
                                  {PgParam{"it's \"quoted\"; DROP TABLE x"}, PgParam{"42"}, PgParam{}});
  ASSERT_TRUE(result.ok()) << result.message();
  ASSERT_EQ(result.rows(), 1);
  EXPECT_EQ(result.text(0, 0), "it's \"quoted\"; DROP TABLE x");
  EXPECT_EQ(result.text(0, 1), "42");
  EXPECT_EQ(result.text(0, 2), "t");
}

TEST(PgPoolTest, FailedStatementsReportSqlstateAndKeepTheConnectionUsable) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  PgPool pool(*test_db_params, 1, log.logger());
  auto lease = pool.acquire();
  ASSERT_TRUE(lease);
  const auto bad = lease->exec("SELECT * FROM table_that_does_not_exist");
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.sqlstate(), "42P01");
  EXPECT_TRUE(lease->alive());
  EXPECT_TRUE(lease->exec("SELECT 1").ok());
}

TEST(PgPoolTest, ExhaustedPoolTimesOutInsteadOfBlockingForever) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  PgPool pool(*test_db_params, 1, log.logger());
  auto held = pool.acquire();
  ASSERT_TRUE(held);
  const auto begin = std::chrono::steady_clock::now();
  const auto second = pool.acquire(std::chrono::milliseconds{150});
  EXPECT_FALSE(second);
  EXPECT_NE(second.error().find("timed out"), std::string::npos);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds{3});
}

TEST(PgPoolTest, ConnectionsAreReusedAndReturnedByLeases) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  PgPool pool(*test_db_params, 1, log.logger());
  std::string first_pid;
  {
    auto lease = pool.acquire();
    ASSERT_TRUE(lease);
    first_pid = std::string{lease->exec("SELECT pg_backend_pid()").text(0, 0)};
  }
  auto lease = pool.acquire(std::chrono::milliseconds{500});
  ASSERT_TRUE(lease) << "the first lease returned its connection on destruction";
  EXPECT_EQ(std::string{lease->exec("SELECT pg_backend_pid()").text(0, 0)}, first_pid);
}

TEST(PgPoolTest, ManyThreadsShareASmallPool) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  PgPool pool(*test_db_params, 3, log.logger());
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 12; ++t) {
    threads.emplace_back([&, t] {
      for (int n = 0; n < 20; ++n) {
        auto lease = pool.acquire(std::chrono::seconds{10});
        if (!lease) {
          ++failures;
          continue;
        }
        const auto result = lease->exec("SELECT $1::int", {PgParam{std::to_string(t * 100 + n)}});
        if (!result.ok() || result.text(0, 0) != std::to_string(t * 100 + n)) ++failures;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST(MigratorTest, IsIdempotentAndSafeToRunConcurrently) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  {
    PgPool pool(*test_db_params, 1, log.logger());
    const auto first = storage::migrate(pool, storage::builtinMigrations(), *log.logger());
    ASSERT_TRUE(first.ok) << first.error;
    const auto second = storage::migrate(pool, storage::builtinMigrations(), *log.logger());
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(second.applied, 0U) << "nothing is applied twice";
  }
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      PgPool pool(*test_db_params, 1, log.logger());
      if (!storage::migrate(pool, storage::builtinMigrations(), *log.logger()).ok) ++failures;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);

  PgPool pool(*test_db_params, 1, log.logger());
  auto lease = pool.acquire();
  ASSERT_TRUE(lease);
  const auto recorded = lease->exec("SELECT count(*) FROM schema_migrations WHERE name = '001_service_registry'");
  ASSERT_TRUE(recorded.ok());
  EXPECT_EQ(recorded.text(0, 0), "1") << "recorded exactly once";
}

}  // namespace
