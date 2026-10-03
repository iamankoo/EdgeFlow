#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "edgeflow/discovery/ServiceRegistry.hpp"
#include "edgeflow/discovery/Validation.hpp"

namespace edgeflow::testing {

// In-memory ServiceRegistry with the same observable semantics as the PostgreSQL one
// (validation, duplicates, not-found distinctions). Used ONLY to unit-test the HTTP API
// without a database; persistence is proven against real PostgreSQL elsewhere.
class FakeRegistry final : public discovery::ServiceRegistry {
 public:
  using Code = discovery::RegistryErrorCode;

  discovery::Result<discovery::ServiceInstance> registerInstance(
      const discovery::NewInstance& instance) override {
    if (auto invalid = discovery::validate(instance)) return {Code::InvalidArgument, *invalid};
    const std::lock_guard lock(mutex_);
    auto& list = services_[instance.service];
    const std::string id = instance.instance_id ? *instance.instance_id
                                                : "generated-" + std::to_string(++counter_);
    for (const auto& existing : list) {
      if (existing.instance_id == id || (existing.host == instance.host && existing.port == instance.port)) {
        return {Code::DuplicateInstance, "duplicate"};
      }
    }
    discovery::ServiceInstance stored;
    stored.service = instance.service;
    stored.instance_id = id;
    stored.host = instance.host;
    stored.port = instance.port;
    stored.status = instance.status;
    stored.health = instance.health;
    stored.version = instance.version;
    stored.weight = instance.weight;
    stored.connection_count = instance.connection_count;
    // Distinct per registration, like the database's timestamp: it identifies the incarnation.
    char stamp[48];
    std::snprintf(stamp, sizeof(stamp), "2026-01-01T00:00:00.%06dZ", ++registrations_);
    stored.registered_at = stored.updated_at = stamp;
    list.push_back(stored);
    return stored;
  }

  discovery::Result<discovery::Unit> deregisterInstance(std::string_view service,
                                                        std::string_view instance_id) override {
    const std::lock_guard lock(mutex_);
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    auto& list = it->second;
    const auto found = std::find_if(list.begin(), list.end(), [&](const auto& i) {
      return i.instance_id == instance_id;
    });
    if (found == list.end()) return {Code::InstanceNotFound, "instance not found"};
    list.erase(found);
    return discovery::Unit{};
  }

  discovery::Result<std::vector<discovery::ServiceInstance>> lookupService(
      std::string_view service) override {
    if (auto invalid = discovery::validateServiceName(service)) return {Code::InvalidArgument, *invalid};
    const std::lock_guard lock(mutex_);
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    return it->second;
  }

  discovery::Result<discovery::ServiceInstance> getInstance(std::string_view service,
                                                            std::string_view instance_id) override {
    if (auto invalid = discovery::validateServiceName(service)) return {Code::InvalidArgument, *invalid};
    const std::lock_guard lock(mutex_);
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    for (const auto& i : it->second) {
      if (i.instance_id == instance_id) return i;
    }
    return {Code::InstanceNotFound, "instance not found"};
  }

  discovery::Result<std::vector<std::string>> listServices() override {
    const std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [name, list] : services_) {
      (void)list;
      names.push_back(name);
    }
    return names;
  }

  discovery::Result<discovery::ServiceInstance> updateInstance(
      std::string_view service, std::string_view instance_id,
      const discovery::InstanceUpdate& update) override {
    if (auto invalid = discovery::validate(update)) return {Code::InvalidArgument, *invalid};
    const std::lock_guard lock(mutex_);
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    for (auto& i : it->second) {
      if (i.instance_id != instance_id) continue;
      if (update.status) i.status = *update.status;
      if (update.health) i.health = *update.health;
      if (update.version) i.version = *update.version;
      if (update.weight) i.weight = *update.weight;
      if (update.connection_count) i.connection_count = *update.connection_count;
      return i;
    }
    return {Code::InstanceNotFound, "instance not found"};
  }

  discovery::Result<std::vector<discovery::ServiceInstance>> listInstances() override {
    const std::lock_guard lock(mutex_);
    if (unavailable_) return {Code::DatabaseUnavailable, "the database is unavailable"};
    std::vector<discovery::ServiceInstance> all;
    for (const auto& [name, list] : services_) {
      (void)name;
      all.insert(all.end(), list.begin(), list.end());
    }
    return all;
  }

  discovery::Result<std::vector<discovery::ServiceInstance>> lookupRoutable(
      std::string_view service) override {
    if (auto invalid = discovery::validateServiceName(service)) return {Code::InvalidArgument, *invalid};
    const std::lock_guard lock(mutex_);
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    std::vector<discovery::ServiceInstance> routable;
    for (const auto& i : it->second) {
      if (i.status == discovery::InstanceStatus::Active && i.health == discovery::HealthStatus::Healthy) {
        routable.push_back(i);
      }
    }
    return routable;
  }

  discovery::Result<discovery::ServiceInstance> updateHealth(
      std::string_view service, std::string_view instance_id, std::string_view registered_at,
      discovery::HealthStatus health) override {
    const std::lock_guard lock(mutex_);
    ++health_updates_;
    if (unavailable_) return {Code::DatabaseUnavailable, "the database is unavailable"};
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    for (auto& i : it->second) {
      if (i.instance_id != instance_id || i.registered_at != registered_at) continue;
      i.health = health;
      return i;
    }
    return {Code::InstanceNotFound, "instance not found (or re-registered)"};
  }

  // Test controls -------------------------------------------------------------------

  // Makes every listInstances()/updateHealth() fail as if the database were down.
  void setUnavailable(bool unavailable) {
    const std::lock_guard lock(mutex_);
    unavailable_ = unavailable;
  }
  [[nodiscard]] int healthUpdates() {
    const std::lock_guard lock(mutex_);
    return health_updates_;
  }

  discovery::Result<discovery::ServiceInstance> adjustConnectionCount(
      std::string_view service, std::string_view instance_id, std::int64_t delta) override {
    const std::lock_guard lock(mutex_);
    const auto it = services_.find(std::string{service});
    if (it == services_.end()) return {Code::ServiceNotFound, "service not found"};
    for (auto& i : it->second) {
      if (i.instance_id != instance_id) continue;
      const auto next = static_cast<std::int64_t>(i.connection_count) + delta;
      i.connection_count = next < 0 ? 0 : static_cast<std::uint64_t>(next);
      return i;
    }
    return {Code::InstanceNotFound, "instance not found"};
  }

 private:
  std::mutex mutex_;
  std::map<std::string, std::vector<discovery::ServiceInstance>> services_;
  int counter_{0};
  bool unavailable_{false};
  int health_updates_{0};
  int registrations_{0};
};

}  // namespace edgeflow::testing
