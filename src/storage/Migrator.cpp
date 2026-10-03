#include "edgeflow/storage/Migrator.hpp"

#include <string>

namespace edgeflow::storage {

namespace {

// Arbitrary application-wide key for the migration advisory lock.
constexpr const char* kLockSql = "SELECT pg_advisory_lock(727301)";
constexpr const char* kUnlockSql = "SELECT pg_advisory_unlock(727301)";

constexpr const char* kCreateTrackingTable =
    "CREATE TABLE IF NOT EXISTS schema_migrations ("
    " name       TEXT        PRIMARY KEY,"
    " applied_at TIMESTAMPTZ NOT NULL DEFAULT now())";

MigrationReport fail(std::string message) {
  MigrationReport report;
  report.error = std::move(message);
  return report;
}

}  // namespace

MigrationReport migrate(PgPool& pool, const std::vector<Migration>& migrations,
                        logging::Logger& logger) {
  auto lease = pool.acquire();
  if (!lease) return fail(lease.error());
  PgConnection& connection = *lease;

  if (auto locked = connection.execSimple(kLockSql); !locked.ok()) {
    return fail("cannot take the migration lock: " + locked.message());
  }

  MigrationReport report;
  const auto unlock = [&connection] { (void)connection.execSimple(kUnlockSql); };

  if (auto created = connection.execSimple(kCreateTrackingTable); !created.ok()) {
    const auto message = created.message();
    unlock();
    return fail("cannot create schema_migrations: " + message);
  }

  for (const auto& migration : migrations) {
    const std::string name{migration.name};
    const auto existing =
        connection.exec("SELECT 1 FROM schema_migrations WHERE name = $1", {PgParam{name}});
    if (!existing.ok()) {
      const auto message = existing.message();
      unlock();
      return fail("cannot read schema_migrations: " + message);
    }
    if (existing.rows() > 0) continue;

    const std::string sql{migration.sql};
    bool failed = false;
    std::string message;
    if (auto begin = connection.execSimple("BEGIN"); !begin.ok()) {
      failed = true;
      message = begin.message();
    } else if (auto applied = connection.execSimple(sql.c_str()); !applied.ok()) {
      failed = true;
      message = applied.message();
    } else if (auto recorded = connection.exec(
                   "INSERT INTO schema_migrations (name) VALUES ($1)", {PgParam{name}});
               !recorded.ok()) {
      failed = true;
      message = recorded.message();
    } else if (auto committed = connection.execSimple("COMMIT"); !committed.ok()) {
      failed = true;
      message = committed.message();
    }
    if (failed) {
      (void)connection.execSimple("ROLLBACK");
      unlock();
      return fail("migration " + name + " failed: " + message);
    }
    ++report.applied;
    logger.info("applied database migration {}", name);
  }

  unlock();
  report.ok = true;
  return report;
}

}  // namespace edgeflow::storage
