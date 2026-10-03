#include "edgeflow/storage/Postgres.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace edgeflow::storage {

// --- PgResult ---------------------------------------------------------------------

PgResult::~PgResult() {
  if (result_ != nullptr) PQclear(result_);
}

PgResult::PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}

PgResult& PgResult::operator=(PgResult&& other) noexcept {
  if (this != &other) {
    if (result_ != nullptr) PQclear(result_);
    result_ = std::exchange(other.result_, nullptr);
  }
  return *this;
}

bool PgResult::ok() const noexcept {
  if (result_ == nullptr) return false;
  const auto status = PQresultStatus(result_);
  return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

int PgResult::rows() const noexcept { return result_ == nullptr ? 0 : PQntuples(result_); }

bool PgResult::isNull(int row, int column) const noexcept {
  return result_ == nullptr || PQgetisnull(result_, row, column) != 0;
}

std::string_view PgResult::text(int row, int column) const noexcept {
  if (result_ == nullptr) return {};
  return {PQgetvalue(result_, row, column), static_cast<std::size_t>(PQgetlength(result_, row, column))};
}

long long PgResult::affectedRows() const noexcept {
  if (result_ == nullptr) return 0;
  const char* text = PQcmdTuples(result_);
  return (text == nullptr || *text == '\0') ? 0 : std::atoll(text);
}

std::string PgResult::sqlstate() const {
  if (result_ == nullptr) return {};
  const char* state = PQresultErrorField(result_, PG_DIAG_SQLSTATE);
  return state == nullptr ? std::string{} : std::string{state};
}

std::string PgResult::constraint() const {
  if (result_ == nullptr) return {};
  const char* name = PQresultErrorField(result_, PG_DIAG_CONSTRAINT_NAME);
  return name == nullptr ? std::string{} : std::string{name};
}

std::string PgResult::message() const {
  if (result_ == nullptr) return "no result";
  const char* text = PQresultErrorMessage(result_);
  std::string message = text == nullptr ? std::string{} : std::string{text};
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
  return message;
}

// --- PgConnection -----------------------------------------------------------------

PgConnection::~PgConnection() {
  if (connection_ != nullptr) PQfinish(connection_);
}

std::unique_ptr<PgConnection> PgConnection::connect(const ConnectionParams& params,
                                                    std::string& error) {
  const std::string port = std::to_string(params.port);
  const std::string timeout = std::to_string(params.connect_timeout.count());
  const std::string options =
      "-c statement_timeout=" + std::to_string(params.statement_timeout.count());

  std::vector<const char*> keys{"host",     "port",    "dbname",           "user",
                                "connect_timeout", "application_name", "client_encoding",
                                "options"};
  std::vector<const char*> values{params.host.c_str(), port.c_str(),     params.database.c_str(),
                                  params.user.c_str(), timeout.c_str(),  "edgeflow",
                                  "UTF8",              options.c_str()};
  if (!params.password.empty()) {
    keys.push_back("password");
    values.push_back(params.password.c_str());
  }
  keys.push_back(nullptr);
  values.push_back(nullptr);

  PGconn* connection = PQconnectdbParams(keys.data(), values.data(), 0);
  if (connection == nullptr) {
    error = "out of memory creating a PostgreSQL connection";
    return nullptr;
  }
  if (PQstatus(connection) != CONNECTION_OK) {
    error = PQerrorMessage(connection);
    while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
    PQfinish(connection);
    return nullptr;
  }
  return std::unique_ptr<PgConnection>(new PgConnection(connection));
}

PgResult PgConnection::exec(const char* sql, const std::vector<PgParam>& params) {
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& param : params) values.push_back(param ? param->c_str() : nullptr);
  return PgResult{PQexecParams(connection_, sql, static_cast<int>(params.size()), nullptr,
                               values.data(), nullptr, nullptr, 0)};
}

PgResult PgConnection::execSimple(const char* sql) { return PgResult{PQexec(connection_, sql)}; }

bool PgConnection::alive() const noexcept {
  return connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK;
}

bool PgConnection::healthy() noexcept {
  if (connection_ == nullptr) return false;
  // The first read picks up what the server already sent (for example a FATAL notice when
  // the backend was terminated); only a following read observes the closed socket.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (PQconsumeInput(connection_) == 0) return false;
    if (PQstatus(connection_) != CONNECTION_OK) return false;
  }
  return true;
}

// --- PgPool -----------------------------------------------------------------------

PgPool::Lease::Lease(Lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      connection_(std::move(other.connection_)),
      error_(std::move(other.error_)) {}

PgPool::Lease& PgPool::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    if (pool_ != nullptr && connection_) pool_->release(std::move(connection_));
    pool_ = std::exchange(other.pool_, nullptr);
    connection_ = std::move(other.connection_);
    error_ = std::move(other.error_);
  }
  return *this;
}

PgPool::Lease::~Lease() {
  if (pool_ != nullptr && connection_) pool_->release(std::move(connection_));
}

PgPool::PgPool(ConnectionParams params, std::size_t size, std::shared_ptr<logging::Logger> logger)
    : params_(std::move(params)), size_(std::max<std::size_t>(size, 1)), logger_(std::move(logger)) {}

PgPool::~PgPool() = default;

PgPool::Lease PgPool::acquire(std::chrono::milliseconds timeout) {
  Lease lease;
  std::unique_lock lock(mutex_);

  const bool ready = available_.wait_for(lock, timeout, [this] {
    return !idle_.empty() || open_or_opening_ < size_;
  });
  if (!ready) {
    lease.error_ = "timed out waiting for a free database connection";
    return lease;
  }

  if (!idle_.empty()) {
    auto connection = std::move(idle_.back());
    idle_.pop_back();
    lock.unlock();

    if (connection->healthy()) {
      lease.pool_ = this;
      lease.connection_ = std::move(connection);
      return lease;
    }
    // The server closed this connection while it was idle (restart, failover, terminated
    // backend). Discard it and open a fresh one in its place; its slot stays reserved.
    logger_->info("discarding a database connection that was closed by the server");
    connection.reset();
  } else {
    // Open a new connection outside the lock; the slot is reserved meanwhile.
    ++open_or_opening_;
    lock.unlock();
  }

  std::string error;
  auto connection = PgConnection::connect(params_, error);
  if (!connection) {
    {
      const std::lock_guard relock(mutex_);
      --open_or_opening_;
    }
    available_.notify_one();
    logger_->warn("database connection failed ({}:{}): {}", params_.host, params_.port, error);
    lease.error_ = "cannot connect to the database: " + error;
    return lease;
  }
  lease.pool_ = this;
  lease.connection_ = std::move(connection);
  return lease;
}

void PgPool::release(std::unique_ptr<PgConnection> connection) noexcept {
  {
    const std::lock_guard lock(mutex_);
    idle_.push_back(std::move(connection));
  }
  available_.notify_one();
}

}  // namespace edgeflow::storage
