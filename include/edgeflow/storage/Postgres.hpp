#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "edgeflow/logging/Logger.hpp"

struct pg_conn;    // libpq's PGconn
struct pg_result;  // libpq's PGresult

namespace edgeflow::storage {

// Everything needed to open a connection. The password is carried only here, in memory,
// and is never logged.
struct ConnectionParams {
  std::string host{"127.0.0.1"};
  std::uint16_t port{5432};
  std::string database{"edgeflow"};
  std::string user{"edgeflow"};
  std::string password;
  std::chrono::seconds connect_timeout{5};
  // Server-side cap for a single statement, so a stuck query cannot pin a worker forever.
  std::chrono::milliseconds statement_timeout{10000};
};

// A query parameter: text, or std::nullopt for SQL NULL. Parameters are always sent
// out of band (PQexecParams), never spliced into the SQL string.
using PgParam = std::optional<std::string>;

// Owns one PGresult.
class PgResult {
 public:
  PgResult() = default;
  explicit PgResult(pg_result* result) noexcept : result_(result) {}
  ~PgResult();
  PgResult(PgResult&& other) noexcept;
  PgResult& operator=(PgResult&& other) noexcept;
  PgResult(const PgResult&) = delete;
  PgResult& operator=(const PgResult&) = delete;

  // True for a successful command or query (PGRES_COMMAND_OK / TUPLES_OK).
  [[nodiscard]] bool ok() const noexcept;
  [[nodiscard]] int rows() const noexcept;
  [[nodiscard]] bool isNull(int row, int column) const noexcept;
  [[nodiscard]] std::string_view text(int row, int column) const noexcept;
  // Number of rows affected by INSERT/UPDATE/DELETE.
  [[nodiscard]] long long affectedRows() const noexcept;

  // Failure details (valid when !ok()). `sqlstate` is the 5-character SQLSTATE or empty.
  [[nodiscard]] std::string sqlstate() const;
  [[nodiscard]] std::string constraint() const;
  [[nodiscard]] std::string message() const;

 private:
  pg_result* result_{nullptr};
};

// One libpq connection. Not thread-safe; use through PgPool.
class PgConnection {
 public:
  ~PgConnection();
  PgConnection(const PgConnection&) = delete;
  PgConnection& operator=(const PgConnection&) = delete;

  // Opens a connection; on failure returns nullptr and sets `error` (never contains the
  // password).
  [[nodiscard]] static std::unique_ptr<PgConnection> connect(const ConnectionParams& params,
                                                             std::string& error);

  // Parameterised statement ($1, $2, ...). A failed query still returns a PgResult
  // (check ok()); a dead connection yields a result with !ok() and connectionLost().
  [[nodiscard]] PgResult exec(const char* sql, const std::vector<PgParam>& params = {});
  // Simple-protocol statement(s) with no parameters (migrations, BEGIN/COMMIT).
  [[nodiscard]] PgResult execSimple(const char* sql);

  // True while the underlying socket is believed usable.
  [[nodiscard]] bool alive() const noexcept;
  // Like alive(), but first reads any data the server has already sent, so a connection
  // that was closed while idle (server restart, terminated backend) is noticed before use.
  [[nodiscard]] bool healthy() noexcept;
  // True when the last failure was the connection itself rather than the statement.
  [[nodiscard]] bool connectionLost() const noexcept { return !alive(); }

 private:
  explicit PgConnection(pg_conn* connection) noexcept : connection_(connection) {}
  pg_conn* connection_;
};

// Fixed-capacity pool of lazily opened connections. Thread-safe.
class PgPool : public std::enable_shared_from_this<PgPool> {
 public:
  class Lease {
   public:
    Lease() = default;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return connection_ != nullptr; }
    [[nodiscard]] PgConnection& operator*() const noexcept { return *connection_; }
    [[nodiscard]] PgConnection* operator->() const noexcept { return connection_.get(); }
    // Error text when the lease could not be obtained.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

   private:
    friend class PgPool;
    PgPool* pool_{nullptr};
    std::unique_ptr<PgConnection> connection_;
    std::string error_;
  };

  PgPool(ConnectionParams params, std::size_t size, std::shared_ptr<logging::Logger> logger);
  ~PgPool();
  PgPool(const PgPool&) = delete;
  PgPool& operator=(const PgPool&) = delete;

  // Borrows a connection, opening or re-opening one if needed. Blocks up to `timeout` when
  // all connections are busy. The returned lease is empty (with error()) on failure.
  [[nodiscard]] Lease acquire(std::chrono::milliseconds timeout = std::chrono::seconds{5});

  [[nodiscard]] std::size_t capacity() const noexcept { return size_; }

 private:
  void release(std::unique_ptr<PgConnection> connection) noexcept;

  const ConnectionParams params_;
  const std::size_t size_;
  std::shared_ptr<logging::Logger> logger_;

  std::mutex mutex_;
  std::condition_variable available_;
  std::vector<std::unique_ptr<PgConnection>> idle_;
  std::size_t open_or_opening_{0};  // idle + leased + being opened
};

}  // namespace edgeflow::storage
