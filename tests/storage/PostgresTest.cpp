#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "support/HealthTestSupport.hpp"
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

// --- an unreachable database must not pin a thread per request (Phase 10) -------------

namespace {
std::size_t countOf(CapturedLogger& log, const std::string& text) {
  std::size_t count = 0;
  for (auto at = log.output().find(text); at != std::string::npos; at = log.output().find(text, at + 1)) ++count;
  return count;
}
}  // namespace

TEST(PgPoolUnreachableTest, AfterASlowFailureFurtherAttemptsFailAtOnceUntilTheBackoffIsOver) {
  // A server that accepts the connection and never answers: libpq waits for connect_timeout (1 s), the way it
  // does for a host that drops packets.
  edgeflow::testing::RawServer silent(edgeflow::testing::RawServer::Mode::Stall);
  CapturedLogger log;
  auto params = edgeflow::testing::unreachableDatabase();
  params.port = silent.port();
  params.connect_timeout = std::chrono::seconds{1};
  params.unreachable_backoff = std::chrono::milliseconds{1500};
  PgPool pool(params, 4, log.logger());

  auto began = std::chrono::steady_clock::now();
  EXPECT_FALSE(pool.acquire()) << "the first attempt has to wait out the connect timeout";
  EXPECT_GE(std::chrono::steady_clock::now() - began, std::chrono::milliseconds{900});
  EXPECT_EQ(countOf(log, "database connection failed"), 1U);

  began = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    const auto lease = pool.acquire();
    ASSERT_FALSE(lease);
    EXPECT_NE(lease.error().find("not retrying yet"), std::string::npos) << lease.error();
  }
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::milliseconds{300})
      << "20 more attempts inside the back-off cost nothing";
  EXPECT_EQ(countOf(log, "database connection failed"), 1U) << "and are not logged again";

  std::this_thread::sleep_for(std::chrono::milliseconds{1500});
  began = std::chrono::steady_clock::now();
  EXPECT_FALSE(pool.acquire());
  EXPECT_GE(std::chrono::steady_clock::now() - began, std::chrono::milliseconds{900})
      << "after the back-off one attempt probes the database again";
  EXPECT_EQ(countOf(log, "database connection failed"), 2U);
}

TEST(PgPoolUnreachableTest, OnlyOneThreadProbesAtATimeAfterTheBackoff) {
  edgeflow::testing::RawServer silent(edgeflow::testing::RawServer::Mode::Stall);
  CapturedLogger log;
  auto params = edgeflow::testing::unreachableDatabase();
  params.port = silent.port();
  params.connect_timeout = std::chrono::seconds{1};
  params.unreachable_backoff = std::chrono::milliseconds{200};
  PgPool pool(params, 4, log.logger());
  ASSERT_FALSE(pool.acquire());
  std::this_thread::sleep_for(std::chrono::milliseconds{300});

  std::atomic<int> slow{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      const auto began = std::chrono::steady_clock::now();
      const auto lease = pool.acquire();
      if (!lease && std::chrono::steady_clock::now() - began > std::chrono::milliseconds{500}) slow.fetch_add(1);
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(slow.load(), 1) << "one probe waited out the timeout, the other seven failed at once";
}

TEST(PgPoolUnreachableTest, AQuickRefusalNeverStartsABackoff) {
  CapturedLogger log;
  PgPool pool(edgeflow::testing::unreachableDatabase(), 2, log.logger());  // nothing listens: refused at once
  for (int i = 0; i < 5; ++i) {
    const auto lease = pool.acquire();
    ASSERT_FALSE(lease);
    EXPECT_EQ(lease.error().find("not retrying yet"), std::string::npos) << lease.error();
  }
  EXPECT_EQ(countOf(log, "database connection failed"), 5U) << "every attempt really tried";
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
