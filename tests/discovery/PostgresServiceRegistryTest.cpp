#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "support/NetTestSupport.hpp"
#include "support/PgTestSupport.hpp"

namespace {

using namespace edgeflow;
using namespace edgeflow::discovery;
using edgeflow::testing::makeInstance;
using edgeflow::testing::RegistryFixture;
using edgeflow::storage::PgParam;

using PostgresRegistryTest = RegistryFixture;

// --- registration ------------------------------------------------------------------

TEST_F(PostgresRegistryTest, RegisteredInstancePersistsAllMetadata) {
  const auto service = newService("user-service");
  NewInstance request = makeInstance(service, "10.0.0.11", 9001);
  request.instance_id = "user-1";
  request.status = InstanceStatus::Draining;
  request.health = HealthStatus::Healthy;
  request.version = "1.4.2";
  request.weight = 250;
  request.connection_count = 7;

  const auto registered = registry->registerInstance(request);
  ASSERT_TRUE(registered.ok()) << registered.error().message;
  EXPECT_FALSE(registered.value().registered_at.empty());

  const auto fetched = registry->getInstance(service, "user-1");
  ASSERT_TRUE(fetched.ok()) << fetched.error().message;
  const auto& i = fetched.value();
  EXPECT_EQ(i.service, service);
  EXPECT_EQ(i.instance_id, "user-1");
  EXPECT_EQ(i.host, "10.0.0.11");
  EXPECT_EQ(i.port, 9001);
  EXPECT_EQ(i.status, InstanceStatus::Draining);
  EXPECT_EQ(i.health, HealthStatus::Healthy);
  EXPECT_EQ(i.version, "1.4.2");
  EXPECT_EQ(i.weight, 250U);
  EXPECT_EQ(i.connection_count, 7U);
  EXPECT_EQ(i.registered_at, registered.value().registered_at);
  EXPECT_TRUE(log.contains("registered instance user-1"));
}

TEST_F(PostgresRegistryTest, DefaultsAndGeneratedId) {
  const auto service = newService();
  const auto registered = registry->registerInstance(makeInstance(service, "backend.internal", 8080));
  ASSERT_TRUE(registered.ok()) << registered.error().message;
  const auto& i = registered.value();
  EXPECT_FALSE(i.instance_id.empty()) << "the database generates an id";
  EXPECT_EQ(i.status, InstanceStatus::Active);
  EXPECT_EQ(i.health, HealthStatus::Unknown);
  EXPECT_EQ(i.version, "");
  EXPECT_EQ(i.weight, kDefaultWeight);
  EXPECT_EQ(i.connection_count, 0U);

  const auto second = registry->registerInstance(makeInstance(service, "backend.internal", 8081));
  ASSERT_TRUE(second.ok());
  EXPECT_NE(second.value().instance_id, i.instance_id);
}

TEST_F(PostgresRegistryTest, MultipleInstancesOfOneServiceAreAllDiscoverable) {
  const auto service = newService();
  std::set<std::string> ids;
  for (std::uint16_t n = 0; n < 5; ++n) {
    auto request = makeInstance(service, "10.0.0.1", static_cast<std::uint16_t>(9000 + n));
    request.instance_id = "i-" + std::to_string(n);
    request.weight = 10U * (n + 1);
    ASSERT_TRUE(registry->registerInstance(request).ok());
    ids.insert(request.instance_id.value());
  }
  const auto found = registry->lookupService(service);
  ASSERT_TRUE(found.ok()) << found.error().message;
  ASSERT_EQ(found.value().size(), 5U);
  std::set<std::string> seen;
  for (const auto& i : found.value()) {
    EXPECT_EQ(i.service, service);
    seen.insert(i.instance_id);
  }
  EXPECT_EQ(seen, ids);
}

TEST_F(PostgresRegistryTest, InstancesOfDifferentServicesAreSeparate) {
  const auto a = newService("svc-a");
  const auto b = newService("svc-b");
  ASSERT_TRUE(registry->registerInstance(makeInstance(a, "10.0.0.1", 9000)).ok());
  // The same host:port is fine for a different service.
  ASSERT_TRUE(registry->registerInstance(makeInstance(b, "10.0.0.1", 9000)).ok());
  EXPECT_EQ(registry->lookupService(a).value().size(), 1U);
  EXPECT_EQ(registry->lookupService(b).value().size(), 1U);
}

TEST_F(PostgresRegistryTest, DuplicateInstanceIdIsRejectedNotOverwritten) {
  const auto service = newService();
  auto first = makeInstance(service, "10.0.0.1", 9000);
  first.instance_id = "dup";
  first.version = "original";
  ASSERT_TRUE(registry->registerInstance(first).ok());

  auto second = makeInstance(service, "10.0.0.2", 9001);
  second.instance_id = "dup";
  second.version = "overwrite-attempt";
  const auto rejected = registry->registerInstance(second);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, RegistryErrorCode::DuplicateInstance);

  const auto still = registry->getInstance(service, "dup");
  ASSERT_TRUE(still.ok());
  EXPECT_EQ(still.value().version, "original");
  EXPECT_EQ(still.value().host, "10.0.0.1");
}

TEST_F(PostgresRegistryTest, DuplicateEndpointIsRejected) {
  const auto service = newService();
  auto first = makeInstance(service, "10.0.0.1", 9000);
  first.instance_id = "one";
  ASSERT_TRUE(registry->registerInstance(first).ok());
  auto second = makeInstance(service, "10.0.0.1", 9000);
  second.instance_id = "two";
  const auto rejected = registry->registerInstance(second);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, RegistryErrorCode::DuplicateInstance);
  EXPECT_EQ(registry->lookupService(service).value().size(), 1U);
}

TEST_F(PostgresRegistryTest, InvalidInputIsRejectedAndNothingIsStored) {
  const auto service = newService();
  const auto expectInvalid = [&](NewInstance instance) {
    const auto result = registry->registerInstance(instance);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, RegistryErrorCode::InvalidArgument);
  };
  auto base = makeInstance(service, "10.0.0.1", 9000);
  auto bad = base; bad.port = 0; expectInvalid(bad);
  bad = base; bad.host = "not a host"; expectInvalid(bad);
  bad = base; bad.host = ""; expectInvalid(bad);
  bad = base; bad.weight = 1001; expectInvalid(bad);
  bad = base; bad.instance_id = "has space"; expectInvalid(bad);
  bad = base; bad.version = std::string(65, 'x'); expectInvalid(bad);
  bad = base; bad.service = "Bad Service"; expectInvalid(bad);
  bad = base; bad.connection_count = 9223372036854775808ULL; expectInvalid(bad);

  // The service must not have been created by any rejected call.
  const auto found = registry->lookupService(service);
  ASSERT_FALSE(found.ok());
  EXPECT_EQ(found.error().code, RegistryErrorCode::ServiceNotFound);
}

TEST_F(PostgresRegistryTest, SqlMetacharactersInMetadataAreStoredAsData) {
  const auto service = newService();
  auto request = makeInstance(service, "10.0.0.1", 9000);
  request.instance_id = "inj";
  request.version = "'; DROP TABLE services; --";
  ASSERT_TRUE(registry->registerInstance(request).ok());
  const auto fetched = registry->getInstance(service, "inj");
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().version, "'; DROP TABLE services; --");
  EXPECT_TRUE(registry->listServices().ok()) << "the services table still exists";

  // And an injection attempt through an identifier is just an invalid identifier.
  const auto probe = registry->getInstance(service, "x' OR '1'='1");
  ASSERT_FALSE(probe.ok());
  EXPECT_EQ(probe.error().code, RegistryErrorCode::InvalidArgument);
}

// --- deregistration ----------------------------------------------------------------

TEST_F(PostgresRegistryTest, DeregisteredInstanceIsNoLongerDiscovered) {
  const auto service = newService();
  auto a = makeInstance(service, "10.0.0.1", 9000);
  a.instance_id = "a";
  auto b = makeInstance(service, "10.0.0.2", 9000);
  b.instance_id = "b";
  ASSERT_TRUE(registry->registerInstance(a).ok());
  ASSERT_TRUE(registry->registerInstance(b).ok());

  ASSERT_TRUE(registry->deregisterInstance(service, "a").ok());

  const auto remaining = registry->lookupService(service);
  ASSERT_TRUE(remaining.ok());
  ASSERT_EQ(remaining.value().size(), 1U);
  EXPECT_EQ(remaining.value()[0].instance_id, "b");
  const auto gone = registry->getInstance(service, "a");
  ASSERT_FALSE(gone.ok());
  EXPECT_EQ(gone.error().code, RegistryErrorCode::InstanceNotFound);
  EXPECT_TRUE(log.contains("deregistered instance a"));
}

TEST_F(PostgresRegistryTest, RepeatedDeregistrationReportsNotFound) {
  const auto service = newService();
  auto a = makeInstance(service, "10.0.0.1", 9000);
  a.instance_id = "a";
  ASSERT_TRUE(registry->registerInstance(a).ok());
  ASSERT_TRUE(registry->deregisterInstance(service, "a").ok());

  const auto again = registry->deregisterInstance(service, "a");
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code, RegistryErrorCode::InstanceNotFound);

  const auto unknown_service = registry->deregisterInstance(newService(), "a");
  ASSERT_FALSE(unknown_service.ok());
  EXPECT_EQ(unknown_service.error().code, RegistryErrorCode::ServiceNotFound);
}

TEST_F(PostgresRegistryTest, ServiceStaysKnownWithNoInstancesAndInstanceCanBeReRegistered) {
  const auto service = newService();
  auto a = makeInstance(service, "10.0.0.1", 9000);
  a.instance_id = "a";
  ASSERT_TRUE(registry->registerInstance(a).ok());
  ASSERT_TRUE(registry->deregisterInstance(service, "a").ok());

  const auto empty = registry->lookupService(service);
  ASSERT_TRUE(empty.ok()) << "a known service with no instances is an empty list, not an error";
  EXPECT_TRUE(empty.value().empty());

  ASSERT_TRUE(registry->registerInstance(a).ok()) << "the id and endpoint are free again";
  EXPECT_EQ(registry->lookupService(service).value().size(), 1U);
}

// --- lookup ------------------------------------------------------------------------

TEST_F(PostgresRegistryTest, LookupOfUnknownServiceAndInstance) {
  const auto unknown = registry->lookupService(newService());
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, RegistryErrorCode::ServiceNotFound);

  const auto service = newService();
  ASSERT_TRUE(registry->registerInstance(makeInstance(service, "10.0.0.1", 9000)).ok());
  const auto missing = registry->getInstance(service, "nope");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, RegistryErrorCode::InstanceNotFound);
  const auto no_service = registry->getInstance(newService(), "nope");
  ASSERT_FALSE(no_service.ok());
  EXPECT_EQ(no_service.error().code, RegistryErrorCode::ServiceNotFound);

  const auto invalid = registry->lookupService("Not Valid");
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, RegistryErrorCode::InvalidArgument);
}

TEST_F(PostgresRegistryTest, ListServicesIncludesRegisteredServices) {
  const auto a = newService("alpha");
  const auto b = newService("beta");
  ASSERT_TRUE(registry->registerInstance(makeInstance(a, "10.0.0.1", 1)).ok());
  ASSERT_TRUE(registry->registerInstance(makeInstance(b, "10.0.0.1", 1)).ok());
  const auto names = registry->listServices();
  ASSERT_TRUE(names.ok());
  const std::set<std::string> all(names.value().begin(), names.value().end());
  EXPECT_TRUE(all.count(a) == 1 && all.count(b) == 1);
  EXPECT_TRUE(std::is_sorted(names.value().begin(), names.value().end()));
}

// --- mutable metadata ---------------------------------------------------------------

TEST_F(PostgresRegistryTest, UpdateChangesOnlyTheGivenFields) {
  const auto service = newService();
  auto request = makeInstance(service, "10.0.0.1", 9000);
  request.instance_id = "u";
  request.version = "1.0";
  request.weight = 5;
  const auto created = registry->registerInstance(request);
  ASSERT_TRUE(created.ok());

  InstanceUpdate update;
  update.health = HealthStatus::Unhealthy;
  update.weight = 77;
  const auto updated = registry->updateInstance(service, "u", update);
  ASSERT_TRUE(updated.ok()) << updated.error().message;
  EXPECT_EQ(updated.value().health, HealthStatus::Unhealthy);
  EXPECT_EQ(updated.value().weight, 77U);
  EXPECT_EQ(updated.value().version, "1.0") << "unmentioned fields are untouched";
  EXPECT_EQ(updated.value().status, InstanceStatus::Active);
  EXPECT_EQ(updated.value().host, "10.0.0.1") << "identity never changes";
  EXPECT_GE(updated.value().updated_at, created.value().updated_at);
  EXPECT_EQ(updated.value().registered_at, created.value().registered_at);

  InstanceUpdate rest;
  rest.status = InstanceStatus::Disabled;
  rest.version = "2.0";
  rest.connection_count = 42;
  const auto again = registry->updateInstance(service, "u", rest);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value().status, InstanceStatus::Disabled);
  EXPECT_EQ(again.value().version, "2.0");
  EXPECT_EQ(again.value().connection_count, 42U);
  EXPECT_EQ(again.value().health, HealthStatus::Unhealthy);

  const auto persisted = registry->getInstance(service, "u");
  EXPECT_EQ(persisted.value().connection_count, 42U);
}

TEST_F(PostgresRegistryTest, UpdateErrors) {
  const auto service = newService();
  auto request = makeInstance(service, "10.0.0.1", 9000);
  request.instance_id = "u";
  ASSERT_TRUE(registry->registerInstance(request).ok());

  InstanceUpdate none;
  EXPECT_EQ(registry->updateInstance(service, "u", none).error().code, RegistryErrorCode::InvalidArgument);
  InstanceUpdate heavy;
  heavy.weight = 5000;
  EXPECT_EQ(registry->updateInstance(service, "u", heavy).error().code, RegistryErrorCode::InvalidArgument);
  InstanceUpdate fine;
  fine.weight = 1;
  EXPECT_EQ(registry->updateInstance(service, "ghost", fine).error().code, RegistryErrorCode::InstanceNotFound);
  EXPECT_EQ(registry->updateInstance(newService(), "u", fine).error().code, RegistryErrorCode::ServiceNotFound);
}

TEST_F(PostgresRegistryTest, ConnectionCountAdjustmentIsAtomicAndClampedAtZero) {
  const auto service = newService();
  auto request = makeInstance(service, "10.0.0.1", 9000);
  request.instance_id = "c";
  ASSERT_TRUE(registry->registerInstance(request).ok());

  EXPECT_EQ(registry->adjustConnectionCount(service, "c", 3).value().connection_count, 3U);
  EXPECT_EQ(registry->adjustConnectionCount(service, "c", -1).value().connection_count, 2U);
  EXPECT_EQ(registry->adjustConnectionCount(service, "c", -50).value().connection_count, 0U);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 50;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int n = 0; n < kPerThread; ++n) (void)registry->adjustConnectionCount(service, "c", 1);
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(registry->getInstance(service, "c").value().connection_count,
            static_cast<std::uint64_t>(kThreads * kPerThread));

  EXPECT_EQ(registry->adjustConnectionCount(service, "ghost", 1).error().code,
            RegistryErrorCode::InstanceNotFound);
}

// --- database defence in depth ------------------------------------------------------

TEST_F(PostgresRegistryTest, DatabaseConstraintsRejectBadRowsEvenIfApplicationCodeDoesNot) {
  const auto service = newService();
  ASSERT_TRUE(registry->registerInstance(makeInstance(service, "10.0.0.1", 9000)).ok());
  auto lease = pool->acquire();
  ASSERT_TRUE(lease);
  const auto bad_weight = lease->exec(
      "UPDATE service_instances SET weight = 5000 WHERE service_id = "
      "(SELECT id FROM services WHERE name = $1)",
      {PgParam{service}});
  EXPECT_FALSE(bad_weight.ok());
  EXPECT_EQ(bad_weight.sqlstate(), "23514");
  const auto bad_status = lease->exec(
      "UPDATE service_instances SET status = 'healthy' WHERE service_id = "
      "(SELECT id FROM services WHERE name = $1)",
      {PgParam{service}});
  EXPECT_FALSE(bad_status.ok()) << "health values are not valid registration statuses";
}

// --- persistence ----------------------------------------------------------------------

TEST_F(PostgresRegistryTest, RegistryStateSurvivesDroppingAllInMemoryState) {
  const auto service = newService("persist");
  auto request = makeInstance(service, "10.0.0.7", 9007);
  request.instance_id = "survivor";
  request.version = "3.1";
  request.weight = 42;
  request.health = HealthStatus::Healthy;
  ASSERT_TRUE(registry->registerInstance(request).ok());

  // Simulate an EdgeFlow restart: discard the registry and the whole connection pool.
  registry.reset();
  pool.reset();

  edgeflow::testing::CapturedLogger fresh_log;
  auto fresh_pool = std::make_shared<storage::PgPool>(params_, 2, fresh_log.logger());
  ASSERT_TRUE(storage::migrate(*fresh_pool, storage::builtinMigrations(), *fresh_log.logger()).ok);
  PostgresServiceRegistry restarted(fresh_pool, fresh_log.logger());

  const auto found = restarted.lookupService(service);
  ASSERT_TRUE(found.ok()) << found.error().message;
  ASSERT_EQ(found.value().size(), 1U);
  EXPECT_EQ(found.value()[0].instance_id, "survivor");
  EXPECT_EQ(found.value()[0].version, "3.1");
  EXPECT_EQ(found.value()[0].weight, 42U);
  EXPECT_EQ(found.value()[0].health, HealthStatus::Healthy);

  ASSERT_TRUE(restarted.deregisterInstance(service, "survivor").ok());
  EXPECT_TRUE(restarted.lookupService(service).value().empty());

  // Re-create the fixture's handles so TearDown can clean up.
  pool = fresh_pool;
}

// --- concurrency ----------------------------------------------------------------------

TEST_F(PostgresRegistryTest, ConcurrentRegistrationsOfDistinctInstancesAllSucceed) {
  const auto service = newService();
  constexpr int kThreads = 8;
  constexpr int kPerThread = 10;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int n = 0; n < kPerThread; ++n) {
        auto request = makeInstance(service, "10.1." + std::to_string(t) + ".1",
                                    static_cast<std::uint16_t>(9000 + n));
        if (!registry->registerInstance(request).ok()) ++failures;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
  const auto found = registry->lookupService(service);
  ASSERT_TRUE(found.ok());
  EXPECT_EQ(found.value().size(), static_cast<std::size_t>(kThreads * kPerThread));
  std::set<std::string> ids;
  for (const auto& i : found.value()) ids.insert(i.instance_id);
  EXPECT_EQ(ids.size(), found.value().size()) << "no duplicate or corrupted ids";
}

TEST_F(PostgresRegistryTest, RacingRegistrationsOfOneInstanceHaveExactlyOneWinner) {
  const auto service = newService();
  constexpr int kThreads = 8;
  std::atomic<int> winners{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      auto request = makeInstance(service, "10.0.0.1", static_cast<std::uint16_t>(9000 + t));
      request.instance_id = "contended";
      const auto result = registry->registerInstance(request);
      if (result.ok()) {
        ++winners;
      } else if (result.error().code == RegistryErrorCode::DuplicateInstance) {
        ++duplicates;
      } else {
        ++other;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(duplicates.load(), kThreads - 1);
  EXPECT_EQ(other.load(), 0);
  EXPECT_EQ(registry->lookupService(service).value().size(), 1U);
}

TEST_F(PostgresRegistryTest, ConcurrentRegisterLookupAndDeregisterStayConsistent) {
  const auto service = newService();
  constexpr int kWorkers = 6;
  constexpr int kRounds = 20;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWorkers; ++w) {
    threads.emplace_back([&, w] {
      for (int round = 0; round < kRounds; ++round) {
        auto request = makeInstance(service, "10.2." + std::to_string(w) + ".1",
                                    static_cast<std::uint16_t>(9000 + round));
        request.instance_id = "w" + std::to_string(w) + "-r" + std::to_string(round);
        if (!registry->registerInstance(request).ok()) ++errors;
        const auto seen = registry->lookupService(service);
        if (!seen.ok()) ++errors;
        // Lookups must always return well-formed rows.
        else for (const auto& i : seen.value()) {
          if (i.instance_id.empty() || i.host.empty() || i.port == 0) ++errors;
        }
        if (round % 2 == 0 && !registry->deregisterInstance(service, request.instance_id.value()).ok()) {
          ++errors;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(errors.load(), 0);
  // Each worker keeps its odd rounds: kRounds / 2 instances.
  EXPECT_EQ(registry->lookupService(service).value().size(),
            static_cast<std::size_t>(kWorkers * (kRounds / 2)));
}

// --- connection loss and recovery ------------------------------------------------------

TEST_F(PostgresRegistryTest, RecoversAfterTheServerKillsItsConnection) {
  const auto service = newService();
  ASSERT_TRUE(registry->registerInstance(makeInstance(service, "10.0.0.1", 9000)).ok());

  // Terminate every pooled backend from a separate, unpooled connection, as a database
  // restart or failover would.
  std::string connect_error;
  auto admin = storage::PgConnection::connect(params_, connect_error);
  ASSERT_TRUE(admin) << connect_error;
  const auto killed = admin->exec(
      "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
      "WHERE pid <> pg_backend_pid() AND datname = current_database() "
      "AND usename = current_user AND application_name = 'edgeflow'");
  ASSERT_TRUE(killed.ok()) << killed.message();
  ASSERT_GE(std::stoi(std::string{killed.text(0, 0)}), 1) << "the pooled connection was killed";
  // pg_terminate_backend only signals the backend; wait until it has really gone, as it
  // would long have done by the time the next request arrives after a real restart.
  ASSERT_TRUE(edgeflow::testing::waitFor([&] {
    const auto left = admin->exec(
        "SELECT count(*) FROM pg_stat_activity WHERE pid <> pg_backend_pid() "
        "AND datname = current_database() AND usename = current_user "
        "AND application_name = 'edgeflow'");
    return left.ok() && left.text(0, 0) == "0";
  }));
  // The pool must notice the dead connections and reconnect; the next call succeeds.
  const auto after = registry->lookupService(service);
  ASSERT_TRUE(after.ok()) << after.error().message << " | log: " << log.output();
  EXPECT_EQ(after.value().size(), 1U);
}

// --- database unavailable (no PostgreSQL needed) -------------------------------------------

TEST(PostgresRegistryUnavailableTest, EveryOperationReportsFailureRatherThanSuccess) {
  edgeflow::testing::CapturedLogger log;
  auto pool = std::make_shared<storage::PgPool>(edgeflow::testing::unreachableDatabase(), 2, log.logger());
  PostgresServiceRegistry registry(pool, log.logger());

  const auto isUnavailable = [](const auto& result) {
    return !result.ok() && result.error().code == RegistryErrorCode::DatabaseUnavailable;
  };
  EXPECT_TRUE(isUnavailable(registry.registerInstance(makeInstance("svc", "10.0.0.1", 9000))));
  EXPECT_TRUE(isUnavailable(registry.deregisterInstance("svc", "a")));
  EXPECT_TRUE(isUnavailable(registry.lookupService("svc")));
  EXPECT_TRUE(isUnavailable(registry.getInstance("svc", "a")));
  EXPECT_TRUE(isUnavailable(registry.listServices()));
  InstanceUpdate update;
  update.weight = 1;
  EXPECT_TRUE(isUnavailable(registry.updateInstance("svc", "a", update)));
  EXPECT_TRUE(isUnavailable(registry.adjustConnectionCount("svc", "a", 1)));
  EXPECT_TRUE(log.contains("database connection failed"));
}

TEST(PostgresRegistryUnavailableTest, InvalidInputIsStillRejectedAsInvalidNotUnavailable) {
  edgeflow::testing::CapturedLogger log;
  auto pool = std::make_shared<storage::PgPool>(edgeflow::testing::unreachableDatabase(), 1, log.logger());
  PostgresServiceRegistry registry(pool, log.logger());
  const auto result = registry.registerInstance(makeInstance("svc", "10.0.0.1", 0));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, RegistryErrorCode::InvalidArgument);
}

}  // namespace
