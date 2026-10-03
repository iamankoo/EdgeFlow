#include "edgeflow/discovery/PostgresServiceRegistry.hpp"

#include <charconv>
#include <utility>

#include "edgeflow/discovery/Validation.hpp"

namespace edgeflow::discovery {

namespace {

using storage::PgConnection;
using storage::PgParam;
using storage::PgResult;

// Columns returned for an instance, in the order readInstance() expects. Timestamps are
// rendered as ISO-8601 UTC with microseconds.
#define EDGEFLOW_INSTANCE_COLUMNS                                                        \
  "i.instance_id, i.host, i.port, i.status, i.health_status, i.version, i.weight, "    \
  "i.connection_count, "                                                                 \
  "to_char(i.registered_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), " \
  "to_char(i.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

template <typename Int>
bool parseInt(std::string_view text, Int& out) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && end == text.data() + text.size();
}

// Reads the row `row` of a result shaped by EDGEFLOW_INSTANCE_COLUMNS starting at `first`.
bool readInstance(const PgResult& result, int row, int first, std::string_view service,
                  ServiceInstance& out) {
  unsigned port = 0;
  unsigned weight = 0;
  std::uint64_t connections = 0;
  const auto status = parseInstanceStatus(result.text(row, first + 3));
  const auto health = parseHealthStatus(result.text(row, first + 4));
  if (!parseInt(result.text(row, first + 2), port) || port > 65535 ||
      !parseInt(result.text(row, first + 6), weight) ||
      !parseInt(result.text(row, first + 7), connections) || !status || !health) {
    return false;
  }
  out.service = std::string{service};
  out.instance_id = std::string{result.text(row, first)};
  out.host = std::string{result.text(row, first + 1)};
  out.port = static_cast<std::uint16_t>(port);
  out.status = *status;
  out.health = *health;
  out.version = std::string{result.text(row, first + 5)};
  out.weight = weight;
  out.connection_count = connections;
  out.registered_at = std::string{result.text(row, first + 8)};
  out.updated_at = std::string{result.text(row, first + 9)};
  return true;
}

// Message of the error produced when a connection died while a statement was running.
constexpr const char* kConnectionLostMessage = "the database connection was lost";

bool startsWith(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

}  // namespace

PostgresServiceRegistry::PostgresServiceRegistry(std::shared_ptr<storage::PgPool> pool,
                                                 std::shared_ptr<logging::Logger> logger)
    : pool_(std::move(pool)), logger_(std::move(logger)) {}

namespace {

// Maps a failed statement to a registry error and logs it. Never reports success.
RegistryError toRegistryError(const PgResult& result, const PgConnection& connection,
                              logging::Logger& logger, const char* operation) {
  if (!connection.alive()) {
    logger.warn("registry {} failed: database connection lost ({})", operation, result.message());
    return {RegistryErrorCode::DatabaseUnavailable, kConnectionLostMessage};
  }
  const std::string state = result.sqlstate();
  const std::string constraint = result.constraint();
  if (state == "23505") {  // unique_violation
    logger.info("registry {} rejected: duplicate ({})", operation, constraint);
    if (constraint == "service_instances_endpoint") {
      return {RegistryErrorCode::DuplicateInstance,
              "an instance with this host and port is already registered for the service"};
    }
    return {RegistryErrorCode::DuplicateInstance,
            "an instance with this id is already registered for the service"};
  }
  if (state == "23514" || state == "23502" || startsWith(state, "22")) {
    logger.info("registry {} rejected by a database constraint ({})", operation, constraint);
    return {RegistryErrorCode::InvalidArgument,
            "the value violates a database constraint (" + constraint + ")"};
  }
  if (startsWith(state, "08") || state == "57P01" || state == "57P02" || state == "57P03" ||
      state == "53300" || state == "57014") {
    logger.warn("registry {} failed: database unavailable (SQLSTATE {})", operation, state);
    return {RegistryErrorCode::DatabaseUnavailable, "the database is unavailable"};
  }
  logger.error("registry {} failed: SQLSTATE {}: {}", operation, state, result.message());
  return {RegistryErrorCode::Internal, "unexpected database error"};
}

RegistryError unavailable(logging::Logger& logger, const char* operation,
                          const std::string& reason) {
  logger.warn("registry {} failed: {}", operation, reason);
  return {RegistryErrorCode::DatabaseUnavailable, "the database is unavailable"};
}

RegistryError badRow(logging::Logger& logger, const char* operation) {
  logger.error("registry {} returned a row that could not be parsed", operation);
  return {RegistryErrorCode::Internal, "unexpected data in the database"};
}

}  // namespace

Result<ServiceInstance> PostgresServiceRegistry::registerInstance(const NewInstance& instance) {
  if (auto invalid = validate(instance)) {
    logger_->info("registry register rejected: {}", *invalid);
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "register", lease.error());

  // One statement: ensure the service exists, then insert the instance. The no-op
  // DO UPDATE makes RETURNING work when the service already exists, and serialises
  // concurrent first registrations of the same service.
  const auto result = lease->exec(
      "WITH s AS ("
      "  INSERT INTO services (name) VALUES ($1)"
      "  ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name RETURNING id"
      ") "
      "INSERT INTO service_instances AS i "
      "  (service_id, instance_id, host, port, status, health_status, version, weight,"
      "   connection_count) "
      "SELECT s.id, COALESCE($2::text, gen_random_uuid()::text), $3::text, $4::integer,"
      "       $5::text, $6::text, $7::text,"
      "       $8::integer, $9::bigint FROM s "
      "RETURNING " EDGEFLOW_INSTANCE_COLUMNS,
      {PgParam{instance.service}, instance.instance_id ? PgParam{*instance.instance_id} : PgParam{},
       PgParam{instance.host}, PgParam{std::to_string(instance.port)},
       PgParam{std::string{toString(instance.status)}},
       PgParam{std::string{toString(instance.health)}}, PgParam{instance.version},
       PgParam{std::to_string(instance.weight)},
       PgParam{std::to_string(instance.connection_count)}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "register");
  if (result.rows() != 1) return badRow(*logger_, "register");

  ServiceInstance stored;
  if (!readInstance(result, 0, 0, instance.service, stored)) return badRow(*logger_, "register");
  logger_->info("registered instance {} of service {} at {}:{} (version '{}', weight {})",
                stored.instance_id, stored.service, stored.host, stored.port, stored.version,
                stored.weight);
  return stored;
}

Result<Unit> PostgresServiceRegistry::deregisterInstance(std::string_view service,
                                                         std::string_view instance_id) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  if (auto invalid = validateInstanceId(instance_id)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "deregister", lease.error());

  // A single statement distinguishes "no such service" from "no such instance" so the
  // caller gets a precise error without a second round trip or a race.
  const auto result = lease->exec(
      "WITH svc AS (SELECT id FROM services WHERE name = $1),"
      "     del AS (DELETE FROM service_instances i USING svc"
      "             WHERE i.service_id = svc.id AND i.instance_id = $2 RETURNING 1) "
      "SELECT (SELECT count(*) FROM svc), (SELECT count(*) FROM del)",
      {PgParam{std::string{service}}, PgParam{std::string{instance_id}}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "deregister");
  if (result.rows() != 1) return badRow(*logger_, "deregister");

  if (result.text(0, 0) == "0") {
    logger_->info("registry deregister: unknown service {}", service);
    return {RegistryErrorCode::ServiceNotFound, "service not found"};
  }
  if (result.text(0, 1) == "0") {
    logger_->info("registry deregister: unknown instance {} of service {}", instance_id, service);
    return {RegistryErrorCode::InstanceNotFound, "instance not found"};
  }
  logger_->info("deregistered instance {} of service {}", instance_id, service);
  return Unit{};
}

namespace {

// Runs an idempotent read; if the connection was lost mid-statement, runs it once more on
// a fresh connection.
template <typename T, typename Attempt>
Result<T> retryOnceIfConnectionLost(logging::Logger& logger, const char* operation,
                                    Attempt&& attempt) {
  auto result = attempt();
  if (!result.ok() && result.error().code == RegistryErrorCode::DatabaseUnavailable &&
      result.error().message == kConnectionLostMessage) {
    logger.info("registry {}: retrying once after a lost database connection", operation);
    return attempt();
  }
  return result;
}

}  // namespace

Result<std::vector<ServiceInstance>> PostgresServiceRegistry::lookupService(
    std::string_view service) {
  return retryOnceIfConnectionLost<std::vector<ServiceInstance>>(
      *logger_, "lookup", [&] { return lookupServiceOnce(service); });
}

Result<ServiceInstance> PostgresServiceRegistry::getInstance(std::string_view service,
                                                             std::string_view instance_id) {
  return retryOnceIfConnectionLost<ServiceInstance>(
      *logger_, "get", [&] { return getInstanceOnce(service, instance_id); });
}

Result<std::vector<std::string>> PostgresServiceRegistry::listServices() {
  return retryOnceIfConnectionLost<std::vector<std::string>>(
      *logger_, "list", [&] { return listServicesOnce(); });
}

Result<std::vector<ServiceInstance>> PostgresServiceRegistry::lookupServiceOnce(
    std::string_view service) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "lookup", lease.error());

  // LEFT JOIN: zero rows = unknown service; one row with NULLs = known, no instances.
  // One round trip regardless of the number of instances; the unique index on
  // (service_id, instance_id) serves the join.
  const auto result = lease->exec(
      "SELECT s.id, " EDGEFLOW_INSTANCE_COLUMNS " FROM services s "
      "LEFT JOIN service_instances i ON i.service_id = s.id "
      "WHERE s.name = $1 ORDER BY i.registered_at, i.instance_id",
      {PgParam{std::string{service}}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "lookup");
  if (result.rows() == 0) {
    logger_->debug("registry lookup: unknown service {}", service);
    return {RegistryErrorCode::ServiceNotFound, "service not found"};
  }

  std::vector<ServiceInstance> instances;
  instances.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = 0; row < result.rows(); ++row) {
    if (result.isNull(row, 1)) continue;  // service without instances
    ServiceInstance instance;
    if (!readInstance(result, row, 1, service, instance)) return badRow(*logger_, "lookup");
    instances.push_back(std::move(instance));
  }
  return instances;
}

Result<ServiceInstance> PostgresServiceRegistry::getInstanceOnce(std::string_view service,
                                                                 std::string_view instance_id) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  if (auto invalid = validateInstanceId(instance_id)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "get", lease.error());

  const auto result = lease->exec(
      "SELECT " EDGEFLOW_INSTANCE_COLUMNS
      " FROM services s JOIN service_instances i ON i.service_id = s.id "
      "WHERE s.name = $1 AND i.instance_id = $2",
      {PgParam{std::string{service}}, PgParam{std::string{instance_id}}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "get");
  if (result.rows() == 1) {
    ServiceInstance instance;
    if (!readInstance(result, 0, 0, service, instance)) return badRow(*logger_, "get");
    return instance;
  }

  // Not found: say which part is missing.
  const auto exists = lease->exec("SELECT 1 FROM services WHERE name = $1",
                                  {PgParam{std::string{service}}});
  if (!exists.ok()) return toRegistryError(exists, *lease, *logger_, "get");
  if (exists.rows() == 0) return {RegistryErrorCode::ServiceNotFound, "service not found"};
  return {RegistryErrorCode::InstanceNotFound, "instance not found"};
}

Result<std::vector<std::string>> PostgresServiceRegistry::listServicesOnce() {
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "list", lease.error());
  const auto result = lease->exec("SELECT name FROM services ORDER BY name");
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "list");
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = 0; row < result.rows(); ++row) names.emplace_back(result.text(row, 0));
  return names;
}

Result<ServiceInstance> PostgresServiceRegistry::updateInstance(std::string_view service,
                                                                std::string_view instance_id,
                                                                const InstanceUpdate& update) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  if (auto invalid = validateInstanceId(instance_id)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  if (auto invalid = validate(update)) {
    logger_->info("registry update rejected: {}", *invalid);
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "update", lease.error());

  const auto opt = [](const auto& value, auto convert) -> PgParam {
    return value ? PgParam{convert(*value)} : PgParam{};
  };
  const auto result = lease->exec(
      "UPDATE service_instances i SET"
      "  status           = COALESCE($3::text, i.status),"
      "  health_status    = COALESCE($4::text, i.health_status),"
      "  version          = COALESCE($5::text, i.version),"
      "  weight           = COALESCE($6::integer, i.weight),"
      "  connection_count = COALESCE($7::bigint, i.connection_count),"
      "  updated_at       = now() "
      "FROM services s WHERE s.id = i.service_id AND s.name = $1 AND i.instance_id = $2 "
      "RETURNING " EDGEFLOW_INSTANCE_COLUMNS,
      {PgParam{std::string{service}}, PgParam{std::string{instance_id}},
       opt(update.status, [](auto v) { return std::string{toString(v)}; }),
       opt(update.health, [](auto v) { return std::string{toString(v)}; }),
       opt(update.version, [](const auto& v) { return v; }),
       opt(update.weight, [](auto v) { return std::to_string(v); }),
       opt(update.connection_count, [](auto v) { return std::to_string(v); })});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "update");
  if (result.rows() == 0) {
    const auto exists = lease->exec("SELECT 1 FROM services WHERE name = $1",
                                    {PgParam{std::string{service}}});
    if (!exists.ok()) return toRegistryError(exists, *lease, *logger_, "update");
    if (exists.rows() == 0) return {RegistryErrorCode::ServiceNotFound, "service not found"};
    return {RegistryErrorCode::InstanceNotFound, "instance not found"};
  }
  ServiceInstance stored;
  if (!readInstance(result, 0, 0, service, stored)) return badRow(*logger_, "update");
  logger_->info("updated instance {} of service {}", stored.instance_id, stored.service);
  return stored;
}

Result<std::vector<ServiceInstance>> PostgresServiceRegistry::listInstances() {
  return retryOnceIfConnectionLost<std::vector<ServiceInstance>>(
      *logger_, "list instances", [&] { return listInstancesOnce(); });
}

Result<std::vector<ServiceInstance>> PostgresServiceRegistry::lookupRoutable(
    std::string_view service) {
  return retryOnceIfConnectionLost<std::vector<ServiceInstance>>(
      *logger_, "routable lookup", [&] { return lookupRoutableOnce(service); });
}

Result<ServiceInstance> PostgresServiceRegistry::updateHealth(std::string_view service,
                                                              std::string_view instance_id,
                                                              std::string_view registered_at,
                                                              HealthStatus health) {
  return retryOnceIfConnectionLost<ServiceInstance>(*logger_, "health update", [&] {
    return updateHealthOnce(service, instance_id, registered_at, health);
  });
}

Result<std::vector<ServiceInstance>> PostgresServiceRegistry::listInstancesOnce() {
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "list instances", lease.error());

  // One statement for the whole registry: no per-service or per-instance queries.
  const auto result = lease->exec(
      "SELECT s.name, " EDGEFLOW_INSTANCE_COLUMNS " FROM service_instances i "
      "JOIN services s ON s.id = i.service_id "
      "ORDER BY s.name, i.registered_at, i.instance_id");
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "list instances");

  std::vector<ServiceInstance> instances;
  instances.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = 0; row < result.rows(); ++row) {
    ServiceInstance instance;
    if (!readInstance(result, row, 1, result.text(row, 0), instance)) {
      return badRow(*logger_, "list instances");
    }
    instances.push_back(std::move(instance));
  }
  return instances;
}

Result<std::vector<ServiceInstance>> PostgresServiceRegistry::lookupRoutableOnce(
    std::string_view service) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "routable lookup", lease.error());

  // Same shape as lookupService, with the routing rule in the join condition so that a
  // known service with nothing routable still yields its (NULL) row.
  const auto result = lease->exec(
      "SELECT s.id, " EDGEFLOW_INSTANCE_COLUMNS " FROM services s "
      "LEFT JOIN service_instances i ON i.service_id = s.id "
      "  AND i.status = 'active' AND i.health_status = 'healthy' "
      "WHERE s.name = $1 ORDER BY i.registered_at, i.instance_id",
      {PgParam{std::string{service}}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "routable lookup");
  if (result.rows() == 0) return {RegistryErrorCode::ServiceNotFound, "service not found"};

  std::vector<ServiceInstance> instances;
  for (int row = 0; row < result.rows(); ++row) {
    if (result.isNull(row, 1)) continue;
    ServiceInstance instance;
    if (!readInstance(result, row, 1, service, instance)) {
      return badRow(*logger_, "routable lookup");
    }
    instances.push_back(std::move(instance));
  }
  return instances;
}

Result<ServiceInstance> PostgresServiceRegistry::updateHealthOnce(std::string_view service,
                                                                  std::string_view instance_id,
                                                                  std::string_view registered_at,
                                                                  HealthStatus health) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  if (auto invalid = validateInstanceId(instance_id)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "health update", lease.error());

  // The registered_at match pins the update to one incarnation of the instance.
  const auto result = lease->exec(
      "UPDATE service_instances i SET health_status = $4::text, updated_at = now() "
      "FROM services s WHERE s.id = i.service_id AND s.name = $1 AND i.instance_id = $2 "
      "AND i.registered_at = $3::timestamptz "
      "RETURNING " EDGEFLOW_INSTANCE_COLUMNS,
      {PgParam{std::string{service}}, PgParam{std::string{instance_id}},
       PgParam{std::string{registered_at}}, PgParam{std::string{toString(health)}}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "health update");
  if (result.rows() == 0) {
    // Gone, or re-registered since the caller read it: either way not the same instance.
    return {RegistryErrorCode::InstanceNotFound, "instance not found (or re-registered)"};
  }
  ServiceInstance stored;
  if (!readInstance(result, 0, 0, service, stored)) return badRow(*logger_, "health update");
  return stored;
}

Result<ServiceInstance> PostgresServiceRegistry::adjustConnectionCount(
    std::string_view service, std::string_view instance_id, std::int64_t delta) {
  if (auto invalid = validateServiceName(service)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  if (auto invalid = validateInstanceId(instance_id)) {
    return {RegistryErrorCode::InvalidArgument, *invalid};
  }
  auto lease = pool_->acquire();
  if (!lease) return unavailable(*logger_, "adjust", lease.error());

  const auto result = lease->exec(
      "UPDATE service_instances i SET"
      "  connection_count = GREATEST(0, i.connection_count + $3::bigint),"
      "  updated_at = now() "
      "FROM services s WHERE s.id = i.service_id AND s.name = $1 AND i.instance_id = $2 "
      "RETURNING " EDGEFLOW_INSTANCE_COLUMNS,
      {PgParam{std::string{service}}, PgParam{std::string{instance_id}},
       PgParam{std::to_string(delta)}});
  if (!result.ok()) return toRegistryError(result, *lease, *logger_, "adjust");
  if (result.rows() == 0) {
    const auto exists = lease->exec("SELECT 1 FROM services WHERE name = $1",
                                    {PgParam{std::string{service}}});
    if (!exists.ok()) return toRegistryError(exists, *lease, *logger_, "adjust");
    if (exists.rows() == 0) return {RegistryErrorCode::ServiceNotFound, "service not found"};
    return {RegistryErrorCode::InstanceNotFound, "instance not found"};
  }
  ServiceInstance stored;
  if (!readInstance(result, 0, 0, service, stored)) return badRow(*logger_, "adjust");
  return stored;
}

}  // namespace edgeflow::discovery
