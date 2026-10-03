#pragma once

#include <memory>

#include "edgeflow/discovery/ServiceRegistry.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/storage/Postgres.hpp"

namespace edgeflow::discovery {

// ServiceRegistry backed by PostgreSQL, which is the only store of registry state: there
// is no in-memory copy that could drift from it, so every read sees what is persisted
// and a restart loses nothing.
//
// Each operation is a single parameterised SQL statement (atomic on the server) executed
// on a pooled connection, so concurrent callers need no locking here and PostgreSQL's
// constraints decide races (for example two simultaneous registrations of one instance
// id: exactly one wins, the other gets DuplicateInstance).
//
// Connection loss: a pooled connection can be closed by the server between the pool's
// health check and its use (restart, failover). Reads (lookup, get, list) and the
// idempotent updateHealth are retried once on a fresh connection. Writes are never retried: after a lost
// connection it is unknown whether they committed, so they report DatabaseUnavailable
// and the caller decides.
//
// The schema must already exist (see storage::migrate).
class PostgresServiceRegistry final : public ServiceRegistry {
 public:
  PostgresServiceRegistry(std::shared_ptr<storage::PgPool> pool,
                          std::shared_ptr<logging::Logger> logger);

  [[nodiscard]] Result<ServiceInstance> registerInstance(const NewInstance& instance) override;
  [[nodiscard]] Result<Unit> deregisterInstance(std::string_view service,
                                                std::string_view instance_id) override;
  [[nodiscard]] Result<std::vector<ServiceInstance>> lookupService(
      std::string_view service) override;
  [[nodiscard]] Result<ServiceInstance> getInstance(std::string_view service,
                                                    std::string_view instance_id) override;
  [[nodiscard]] Result<std::vector<std::string>> listServices() override;
  [[nodiscard]] Result<ServiceInstance> updateInstance(std::string_view service,
                                                       std::string_view instance_id,
                                                       const InstanceUpdate& update) override;
  [[nodiscard]] Result<std::vector<ServiceInstance>> listInstances() override;
  [[nodiscard]] Result<std::vector<ServiceInstance>> lookupRoutable(
      std::string_view service) override;
  [[nodiscard]] Result<ServiceInstance> updateHealth(std::string_view service,
                                                     std::string_view instance_id,
                                                     std::string_view registered_at,
                                                     HealthStatus health) override;
  [[nodiscard]] Result<ServiceInstance> adjustConnectionCount(std::string_view service,
                                                              std::string_view instance_id,
                                                              std::int64_t delta) override;

 private:
  [[nodiscard]] Result<std::vector<ServiceInstance>> lookupServiceOnce(std::string_view service);
  [[nodiscard]] Result<ServiceInstance> getInstanceOnce(std::string_view service,
                                                        std::string_view instance_id);
  [[nodiscard]] Result<std::vector<std::string>> listServicesOnce();
  [[nodiscard]] Result<std::vector<ServiceInstance>> listInstancesOnce();
  [[nodiscard]] Result<std::vector<ServiceInstance>> lookupRoutableOnce(std::string_view service);
  [[nodiscard]] Result<ServiceInstance> updateHealthOnce(std::string_view service,
                                                         std::string_view instance_id,
                                                         std::string_view registered_at,
                                                         HealthStatus health);

  std::shared_ptr<storage::PgPool> pool_;
  std::shared_ptr<logging::Logger> logger_;
};

}  // namespace edgeflow::discovery
