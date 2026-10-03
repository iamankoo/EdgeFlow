#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "edgeflow/storage/Postgres.hpp"

namespace edgeflow::storage {

struct Migration {
  std::string_view name;  // e.g. "001_service_registry"
  std::string_view sql;
};

// The migrations under db/migrations/, embedded into the binary at build time so the
// runtime image needs no SQL files. Ordered by name.
[[nodiscard]] const std::vector<Migration>& builtinMigrations();

struct MigrationReport {
  bool ok{false};
  unsigned applied{0};  // migrations applied by this call
  std::string error;
};

// Brings the schema up to date. Safe to call on every start and from several processes
// at once: a PostgreSQL advisory lock serialises runs, each migration is applied in its
// own transaction, and applied migrations are recorded in `schema_migrations`.
[[nodiscard]] MigrationReport migrate(PgPool& pool, const std::vector<Migration>& migrations,
                                      logging::Logger& logger);

}  // namespace edgeflow::storage
