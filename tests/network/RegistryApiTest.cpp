#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "edgeflow/discovery/PostgresServiceRegistry.hpp"
#include "edgeflow/network/RegistryRequestHandler.hpp"
#include "support/FakeRegistry.hpp"
#include "support/NetTestSupport.hpp"
#include "support/PgTestSupport.hpp"

namespace {

using namespace edgeflow;
using edgeflow::network::RegistryRequestHandler;
using edgeflow::testing::FakeRegistry;
using edgeflow::testing::ServerHarness;
using edgeflow::testing::TestClient;
namespace http = edgeflow::testing::http;
using nlohmann::json;

std::shared_ptr<RegistryRequestHandler> apiOver(std::shared_ptr<discovery::ServiceRegistry> registry,
                                                std::shared_ptr<logging::Logger> logger) {
  return std::make_shared<RegistryRequestHandler>(std::move(registry),
                                                  std::make_shared<network::LocalRequestHandler>(),
                                                  std::move(logger));
}

json bodyOf(const std::optional<network::HttpResponse>& response) {
  return json::parse(response->body(), nullptr, /*allow_exceptions=*/false);
}

// Server + client over an in-memory registry (no database).
class RegistryApiTest : public ::testing::Test {
 protected:
  RegistryApiTest()
      : registry(std::make_shared<FakeRegistry>()),
        harness(edgeflow::testing::testServerConfig(), apiOver(registry, log_logger())),
        client(harness.port()) {}

  std::shared_ptr<logging::Logger> log_logger() { return log.logger(); }

  std::optional<network::HttpResponse> post(const std::string& service, const json& body) {
    return client.request(http::verb::post, "/services/" + service + "/instances", body.dump());
  }

  edgeflow::testing::CapturedLogger log;
  std::shared_ptr<FakeRegistry> registry;
  ServerHarness harness;
  TestClient client;
};

TEST_F(RegistryApiTest, RegisterReturns201WithLocationAndFullMetadata) {
  ASSERT_TRUE(harness.started);
  const auto response = post("user-service", {{"instance_id", "u1"},
                                              {"host", "10.0.0.11"},
                                              {"port", 9001},
                                              {"version", "1.4.2"},
                                              {"weight", 20},
                                              {"status", "active"},
                                              {"health_status", "healthy"},
                                              {"connection_count", 3}});
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::created);
  EXPECT_EQ((*response)[http::field::location], "/services/user-service/instances/u1");
  const auto body = bodyOf(response);
  EXPECT_EQ(body["service"], "user-service");
  EXPECT_EQ(body["instance_id"], "u1");
  EXPECT_EQ(body["host"], "10.0.0.11");
  EXPECT_EQ(body["port"], 9001);
  EXPECT_EQ(body["status"], "active");
  EXPECT_EQ(body["health_status"], "healthy");
  EXPECT_EQ(body["version"], "1.4.2");
  EXPECT_EQ(body["weight"], 20);
  EXPECT_EQ(body["connection_count"], 3);
  EXPECT_TRUE(body.contains("registered_at"));
}

TEST_F(RegistryApiTest, MinimalRegistrationUsesDefaults) {
  const auto response = post("svc", {{"host", "backend.internal"}, {"port", 8080}});
  ASSERT_TRUE(response);
  ASSERT_EQ(response->result(), http::status::created);
  const auto body = bodyOf(response);
  EXPECT_EQ(body["status"], "active");
  EXPECT_EQ(body["health_status"], "unknown");
  EXPECT_EQ(body["weight"], 1);
  EXPECT_EQ(body["connection_count"], 0);
  EXPECT_FALSE(body["instance_id"].get<std::string>().empty());
}

TEST_F(RegistryApiTest, DiscoveryListsAllInstancesAndSingleInstanceLookupWorks) {
  ASSERT_EQ(post("svc", {{"instance_id", "a"}, {"host", "10.0.0.1"}, {"port", 1}})->result(), http::status::created);
  ASSERT_EQ(post("svc", {{"instance_id", "b"}, {"host", "10.0.0.2"}, {"port", 1}})->result(), http::status::created);

  const auto list = client.get("/services/svc/instances");
  ASSERT_TRUE(list);
  EXPECT_EQ(list->result(), http::status::ok);
  const auto body = bodyOf(list);
  EXPECT_EQ(body["service"], "svc");
  EXPECT_EQ(body["count"], 2);
  ASSERT_EQ(body["instances"].size(), 2U);

  const auto one = client.get("/services/svc/instances/b");
  ASSERT_TRUE(one);
  EXPECT_EQ(one->result(), http::status::ok);
  EXPECT_EQ(bodyOf(one)["host"], "10.0.0.2");

  const auto services = client.get("/services");
  ASSERT_TRUE(services);
  EXPECT_EQ(bodyOf(services)["services"], json::array({"svc"}));
}

TEST_F(RegistryApiTest, DeregisterReturns204ThenInstanceIsGone) {
  ASSERT_TRUE(post("svc", {{"instance_id", "a"}, {"host", "10.0.0.1"}, {"port", 1}}));
  const auto removed = client.request(http::verb::delete_, "/services/svc/instances/a");
  ASSERT_TRUE(removed);
  EXPECT_EQ(removed->result(), http::status::no_content);
  EXPECT_TRUE(removed->body().empty());

  EXPECT_EQ(client.get("/services/svc/instances/a")->result(), http::status::not_found);
  const auto again = client.request(http::verb::delete_, "/services/svc/instances/a");
  EXPECT_EQ(again->result(), http::status::not_found) << "repeated deregistration";
  const auto list = client.get("/services/svc/instances");
  EXPECT_EQ(list->result(), http::status::ok);
  EXPECT_EQ(bodyOf(list)["count"], 0);
}

TEST_F(RegistryApiTest, PatchUpdatesMutableFields) {
  ASSERT_TRUE(post("svc", {{"instance_id", "a"}, {"host", "10.0.0.1"}, {"port", 1}, {"version", "1"}}));
  const auto patched = client.request(http::verb::patch, "/services/svc/instances/a",
                                      json{{"health_status", "unhealthy"}, {"weight", 9},
                                           {"status", "draining"}, {"connection_count", 5},
                                           {"version", "2"}}.dump());
  ASSERT_TRUE(patched);
  EXPECT_EQ(patched->result(), http::status::ok);
  const auto body = bodyOf(patched);
  EXPECT_EQ(body["health_status"], "unhealthy");
  EXPECT_EQ(body["weight"], 9);
  EXPECT_EQ(body["status"], "draining");
  EXPECT_EQ(body["connection_count"], 5);
  EXPECT_EQ(body["version"], "2");
  EXPECT_EQ(body["host"], "10.0.0.1");
}

TEST_F(RegistryApiTest, DuplicateRegistrationIs409) {
  ASSERT_TRUE(post("svc", {{"instance_id", "a"}, {"host", "10.0.0.1"}, {"port", 1}}));
  const auto dup = post("svc", {{"instance_id", "a"}, {"host", "10.0.0.2"}, {"port", 2}});
  ASSERT_TRUE(dup);
  EXPECT_EQ(dup->result(), http::status::conflict);
  const auto same_endpoint = post("svc", {{"instance_id", "z"}, {"host", "10.0.0.1"}, {"port", 1}});
  EXPECT_EQ(same_endpoint->result(), http::status::conflict);
}

TEST_F(RegistryApiTest, UnknownServiceAndInstanceAre404) {
  EXPECT_EQ(client.get("/services/nope/instances")->result(), http::status::not_found);
  EXPECT_EQ(client.get("/services/nope/instances/x")->result(), http::status::not_found);
  ASSERT_TRUE(post("svc", {{"host", "10.0.0.1"}, {"port", 1}}));
  EXPECT_EQ(client.get("/services/svc/instances/missing")->result(), http::status::not_found);
  EXPECT_EQ(client.request(http::verb::patch, "/services/svc/instances/missing", R"({"weight":1})")->result(),
            http::status::not_found);
  EXPECT_EQ(client.get("/services/svc/other")->result(), http::status::not_found);
  EXPECT_EQ(client.get("/services/svc/instances/a/extra")->result(), http::status::not_found);
}

TEST_F(RegistryApiTest, MalformedBodiesAre400) {
  const std::string path = "/services/svc/instances";
  const auto expect400 = [&](const std::string& body, const char* why) {
    const auto response = client.request(http::verb::post, path, body);
    ASSERT_TRUE(response) << why;
    EXPECT_EQ(response->result(), http::status::bad_request) << why << ": " << body;
    EXPECT_TRUE(bodyOf(response).contains("detail")) << why;
  };
  expect400("not json", "invalid JSON");
  expect400("", "empty body");
  expect400("[1,2]", "array body");
  expect400("\"text\"", "string body");
  expect400(R"({"port":80})", "missing host");
  expect400(R"({"host":"10.0.0.1"})", "missing port");
  expect400(R"({"host":5,"port":80})", "host wrong type");
  expect400(R"({"host":"10.0.0.1","port":"80"})", "port wrong type");
  expect400(R"({"host":"10.0.0.1","port":0})", "port zero");
  expect400(R"({"host":"10.0.0.1","port":70000})", "port too large");
  expect400(R"({"host":"10.0.0.1","port":80.5})", "port fractional");
  expect400(R"({"host":"bad host","port":80})", "invalid host");
  expect400(R"({"host":"10.0.0.1","port":80,"weight":1001})", "weight too large");
  expect400(R"({"host":"10.0.0.1","port":80,"weight":-1})", "negative weight");
  expect400(R"({"host":"10.0.0.1","port":80,"connection_count":-1})", "negative connection count");
  expect400(R"({"host":"10.0.0.1","port":80,"status":"healthy"})", "health value as status");
  expect400(R"({"host":"10.0.0.1","port":80,"health_status":"active"})", "status value as health");
  expect400(R"({"host":"10.0.0.1","port":80,"instance_id":"bad id"})", "invalid instance id");
  expect400(R"({"host":"10.0.0.1","port":80,"version":7})", "version wrong type");
  expect400(R"({"host":"10.0.0.1","port":80,"colour":"red"})", "unknown field");
  expect400(R"({"host":"10.0.0.1","port":80,"service":"other"})", "service is not a body field");
  EXPECT_EQ(client.get("/services/svc/instances")->result(), http::status::not_found)
      << "no rejected request created the service";
}

TEST_F(RegistryApiTest, PatchRejectsImmutableAndUnknownFields) {
  ASSERT_TRUE(post("svc", {{"instance_id", "a"}, {"host", "10.0.0.1"}, {"port", 1}}));
  const std::string path = "/services/svc/instances/a";
  for (const char* body : {R"({"host":"10.9.9.9"})", R"({"port":2})", R"({"instance_id":"b"})",
                           R"({"service":"x"})"}) {
    const auto response = client.request(http::verb::patch, path, body);
    EXPECT_EQ(response->result(), http::status::bad_request) << body;
    EXPECT_NE(bodyOf(response)["detail"].get<std::string>().find("immutable"), std::string::npos) << body;
  }
  EXPECT_EQ(client.request(http::verb::patch, path, R"({"colour":1})")->result(), http::status::bad_request);
  EXPECT_EQ(client.request(http::verb::patch, path, "{}")->result(), http::status::bad_request) << "nothing to update";
  EXPECT_EQ(client.request(http::verb::patch, path, "nope")->result(), http::status::bad_request);
  EXPECT_EQ(client.request(http::verb::patch, path, R"({"weight":1001})")->result(), http::status::bad_request);
}

TEST_F(RegistryApiTest, InvalidServiceNamesInThePathAre400) {
  EXPECT_EQ(client.get("/services/Bad_Service/instances")->result(), http::status::bad_request);
  EXPECT_EQ(client.get("/services/bad%20name/instances")->result(), http::status::bad_request);
  EXPECT_EQ(post("UPPER", {{"host", "10.0.0.1"}, {"port", 1}})->result(), http::status::bad_request);
}

TEST_F(RegistryApiTest, WrongMethodsGet405WithAllow) {
  const auto root = client.request(http::verb::post, "/services");
  EXPECT_EQ(root->result(), http::status::method_not_allowed);
  EXPECT_EQ((*root)[http::field::allow], "GET");
  const auto collection = client.request(http::verb::delete_, "/services/svc/instances");
  EXPECT_EQ(collection->result(), http::status::method_not_allowed);
  EXPECT_EQ((*collection)[http::field::allow], "GET, POST");
  const auto item = client.request(http::verb::post, "/services/svc/instances/a", "{}");
  EXPECT_EQ(item->result(), http::status::method_not_allowed);
  EXPECT_EQ((*item)[http::field::allow], "GET, PATCH, DELETE");
}

TEST_F(RegistryApiTest, QueryStringsDoNotAffectRouting) {
  ASSERT_TRUE(post("svc", {{"instance_id", "a"}, {"host", "10.0.0.1"}, {"port", 1}}));
  EXPECT_EQ(client.get("/services/svc/instances?x=1")->result(), http::status::ok);
}

TEST_F(RegistryApiTest, RoutableViewShowsOnlyActiveAndHealthyInstances) {
  ASSERT_TRUE(post("svc", {{"instance_id", "ok"}, {"host", "10.0.0.1"}, {"port", 1}, {"health_status", "healthy"}}));
  ASSERT_TRUE(post("svc", {{"instance_id", "sick"}, {"host", "10.0.0.2"}, {"port", 1}, {"health_status", "unhealthy"}}));
  ASSERT_TRUE(post("svc", {{"instance_id", "new"}, {"host", "10.0.0.3"}, {"port", 1}}));
  ASSERT_TRUE(post("svc", {{"instance_id", "drain"}, {"host", "10.0.0.4"}, {"port", 1}, {"health_status", "healthy"}, {"status", "draining"}}));

  const auto routable = client.get("/services/svc/routable");
  ASSERT_TRUE(routable);
  EXPECT_EQ(routable->result(), http::status::ok);
  const auto body = bodyOf(routable);
  EXPECT_EQ(body["count"], 1);
  EXPECT_EQ(body["instances"][0]["instance_id"], "ok");
  EXPECT_EQ(bodyOf(client.get("/services/svc/instances"))["count"], 4) << "plain discovery is unfiltered";

  // The instance recovers: it becomes routable.
  ASSERT_EQ(client.request(http::verb::patch, "/services/svc/instances/sick", R"({"health_status":"healthy"})")->result(),
            http::status::ok);
  EXPECT_EQ(bodyOf(client.get("/services/svc/routable"))["count"], 2);
}

TEST_F(RegistryApiTest, RoutableViewErrors) {
  EXPECT_EQ(client.get("/services/nope/routable")->result(), http::status::not_found);
  EXPECT_EQ(client.get("/services/Bad_Name/routable")->result(), http::status::bad_request);
  const auto wrong = client.request(http::verb::post, "/services/svc/routable", "{}");
  EXPECT_EQ(wrong->result(), http::status::method_not_allowed);
  EXPECT_EQ((*wrong)[http::field::allow], "GET");
  ASSERT_TRUE(post("svc", {{"host", "10.0.0.1"}, {"port", 1}}));
  const auto empty = client.get("/services/svc/routable");
  EXPECT_EQ(empty->result(), http::status::ok);
  EXPECT_EQ(bodyOf(empty)["count"], 0) << "a known service with nothing routable is an empty list";
}

TEST_F(RegistryApiTest, Phase2EndpointsAreUnchanged) {
  const auto health = client.get("/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->result(), http::status::ok);
  EXPECT_EQ(bodyOf(health)["status"], "ok");
  EXPECT_EQ(client.get("/")->result(), http::status::ok);
  EXPECT_EQ(client.request(http::verb::post, "/echo", "abc")->result(), http::status::ok);
  EXPECT_EQ(client.get("/servicesx")->result(), http::status::not_found) << "prefix match is exact";
  EXPECT_EQ(client.get("/nope")->result(), http::status::not_found);
}

TEST_F(RegistryApiTest, ConcurrentClientsRegisterAndDiscoverConsistently) {
  constexpr int kClients = 20;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([&, c] {
      TestClient mine(harness.port());
      const json body = {{"instance_id", "c" + std::to_string(c)},
                         {"host", "10.0.0." + std::to_string(c + 1)},
                         {"port", 9000}};
      const auto created = mine.request(http::verb::post, "/services/svc/instances", body.dump());
      if (!created || created->result() != http::status::created) ++failures;
      const auto listed = mine.get("/services/svc/instances");
      if (!listed || listed->result() != http::status::ok) ++failures;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(bodyOf(client.get("/services/svc/instances"))["count"], kClients);
}

// --- database unavailable: no PostgreSQL needed ------------------------------------------

TEST(RegistryApiUnavailableTest, DatabaseOutageIs503NotFalseSuccess) {
  edgeflow::testing::CapturedLogger log;
  auto pool = std::make_shared<storage::PgPool>(edgeflow::testing::unreachableDatabase(), 1, log.logger());
  auto registry = std::make_shared<discovery::PostgresServiceRegistry>(pool, log.logger());
  ServerHarness harness(edgeflow::testing::testServerConfig(), apiOver(registry, log.logger()));
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto registered = client.request(http::verb::post, "/services/svc/instances",
                                         R"({"host":"10.0.0.1","port":9000})");
  ASSERT_TRUE(registered);
  EXPECT_EQ(registered->result(), http::status::service_unavailable);
  EXPECT_EQ(client.get("/services/svc/instances")->result(), http::status::service_unavailable);
  EXPECT_EQ(client.get("/services")->result(), http::status::service_unavailable);
  EXPECT_EQ(client.request(http::verb::delete_, "/services/svc/instances/a")->result(),
            http::status::service_unavailable);
  // Validation does not need the database, so bad input is still a 400.
  EXPECT_EQ(client.request(http::verb::post, "/services/svc/instances", R"({"host":"x y","port":1})")->result(),
            http::status::bad_request);
  // And the rest of EdgeFlow keeps working.
  EXPECT_EQ(client.get("/health")->result(), http::status::ok);
}

// --- real PostgreSQL: end to end, including an EdgeFlow restart ------------------------------

TEST(RegistryApiPostgresTest, InstancesRegisteredOverHttpSurviveARestartOfTheServer) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  const std::string service = edgeflow::testing::uniqueName("e2e");

  const auto start = [&](edgeflow::testing::CapturedLogger& log, std::shared_ptr<storage::PgPool>& pool) {
    pool = std::make_shared<storage::PgPool>(*test_db_params, 4, log.logger());
    EXPECT_TRUE(storage::migrate(*pool, storage::builtinMigrations(), *log.logger()).ok);
    auto registry = std::make_shared<discovery::PostgresServiceRegistry>(pool, log.logger());
    return std::make_unique<ServerHarness>(edgeflow::testing::testServerConfig(), apiOver(registry, log.logger()));
  };

  {
    edgeflow::testing::CapturedLogger log;
    std::shared_ptr<storage::PgPool> pool;
    auto first = start(log, pool);
    ASSERT_TRUE(first->started);
    TestClient client(first->port());
    for (int n = 1; n <= 3; ++n) {
      const json body = {{"instance_id", "i" + std::to_string(n)}, {"host", "10.0.0." + std::to_string(n)},
                         {"port", 9000 + n}, {"version", "1." + std::to_string(n)}, {"weight", n * 10}};
      const auto created = client.request(http::verb::post, "/services/" + service + "/instances", body.dump());
      ASSERT_TRUE(created);
      ASSERT_EQ(created->result(), http::status::created) << created->body();
    }
    EXPECT_EQ(bodyOf(client.get("/services/" + service + "/instances"))["count"], 3);
  }  // server, handler, registry and pool are all destroyed: EdgeFlow "restarts"

  edgeflow::testing::CapturedLogger log;
  std::shared_ptr<storage::PgPool> pool;
  auto second = start(log, pool);
  ASSERT_TRUE(second->started);
  TestClient client(second->port());
  const auto listed = client.get("/services/" + service + "/instances");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->result(), http::status::ok);
  const auto body = bodyOf(listed);
  ASSERT_EQ(body["count"], 3) << "persisted instances are discovered after the restart";
  EXPECT_EQ(body["instances"][1]["instance_id"], "i2");
  EXPECT_EQ(body["instances"][1]["version"], "1.2");
  EXPECT_EQ(body["instances"][1]["weight"], 20);

  EXPECT_EQ(client.request(http::verb::delete_, "/services/" + service + "/instances/i2")->result(),
            http::status::no_content);
  const auto after = bodyOf(client.get("/services/" + service + "/instances"));
  EXPECT_EQ(after["count"], 2);
  for (const auto& instance : after["instances"]) EXPECT_NE(instance["instance_id"], "i2");

  auto lease = pool->acquire();
  ASSERT_TRUE(lease);
  (void)lease->exec("DELETE FROM services WHERE name = $1", {edgeflow::storage::PgParam{service}});
}

}  // namespace
