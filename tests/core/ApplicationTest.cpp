#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <future>
#include <thread>
#include <vector>

#include "edgeflow/core/Application.hpp"
#include "support/NetTestSupport.hpp"
#include "support/PgTestSupport.hpp"

namespace {

using edgeflow::core::Application;
using edgeflow::core::ApplicationOptions;
using edgeflow::core::ApplicationState;
using edgeflow::testing::CapturedLogger;
using edgeflow::testing::RecordingHandler;
using edgeflow::testing::ServerHarness;
using edgeflow::testing::TestClient;
namespace net = edgeflow::testing::net;
namespace http = edgeflow::testing::http;

const ApplicationOptions kNoSignals{.install_signal_handlers = false, .request_handler = nullptr};

edgeflow::config::Config testConfig() {
  edgeflow::config::Config config;
  config.shutdown.grace_period = std::chrono::seconds{1};
  config.server.host = "127.0.0.1";
  config.server.port = 0;  // let the OS pick a free port
  return config;
}

TEST(ApplicationTest, InitializeSucceedsWithValidConfig) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  EXPECT_EQ(app.state(), ApplicationState::Created);
  EXPECT_TRUE(app.initialize());
  EXPECT_EQ(app.state(), ApplicationState::Initialized);
  EXPECT_TRUE(log.contains("application initialized"));
}

TEST(ApplicationTest, InitializeTwiceFails) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  EXPECT_FALSE(app.initialize());
  EXPECT_TRUE(log.contains("invalid state"));
}

TEST(ApplicationTest, RunWithoutInitializeFails) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  EXPECT_EQ(app.run(), 1);
  EXPECT_TRUE(log.contains("requires a successfully initialized"));
}

TEST(ApplicationTest, RunsUntilShutdownRequestedThenStopsCleanly) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());

  auto result = std::async(std::launch::async, [&app] { return app.run(); });
  while (app.state() != ApplicationState::Running) std::this_thread::yield();
  EXPECT_EQ(result.wait_for(std::chrono::milliseconds(150)), std::future_status::timeout)
      << "run() must keep running until shutdown is requested";

  app.requestShutdown();
  ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(result.get(), 0);
  EXPECT_EQ(app.state(), ApplicationState::Stopped);

  const auto ready = log.position("EdgeFlow ready");
  const auto requested = log.position("shutdown requested");
  const auto started = log.position("shutdown sequence started");
  const auto completed = log.position("shutdown completed");
  ASSERT_NE(ready, std::string::npos);
  ASSERT_NE(requested, std::string::npos);
  ASSERT_NE(started, std::string::npos);
  ASSERT_NE(completed, std::string::npos);
  EXPECT_LT(ready, requested);
  EXPECT_LT(requested, started);
  EXPECT_LT(started, completed);
}

TEST(ApplicationTest, ShutdownBeforeRunIsSafe) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  app.shutdown();
  EXPECT_EQ(app.state(), ApplicationState::Stopped);
  EXPECT_EQ(app.run(), 1) << "a stopped application cannot be run";
}

TEST(ApplicationTest, ShutdownWithoutInitializeIsSafe) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  EXPECT_NO_THROW(app.shutdown());
  EXPECT_EQ(app.state(), ApplicationState::Stopped);
}

TEST(ApplicationTest, RepeatedShutdownRunsCleanupOnce) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  int stops = 0;
  app.shutdownCoordinator().registerComponent("counter", [&stops] { ++stops; });

  app.shutdown();
  app.shutdown();
  app.shutdown();
  EXPECT_EQ(stops, 1);

  const auto output = log.output();
  const auto first = output.find("shutdown completed");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(output.find("shutdown completed", first + 1), std::string::npos);
}

TEST(ApplicationTest, RequestShutdownBeforeRunMakesRunReturnImmediately) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  app.requestShutdown();
  EXPECT_EQ(app.run(), 0);
  EXPECT_EQ(app.state(), ApplicationState::Stopped);
}

TEST(ApplicationTest, ConcurrentShutdownCallsAreSafe) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  std::atomic<int> stops{0};
  app.shutdownCoordinator().registerComponent("counter", [&stops] { ++stops; });

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) threads.emplace_back([&app] { app.shutdown(); });
  for (auto& t : threads) t.join();
  EXPECT_EQ(stops.load(), 1);
}

TEST(ApplicationTest, DestructorShutsDownInitializedApplication) {
  CapturedLogger log;
  {
    Application app(testConfig(), log.logger(), kNoSignals);
    ASSERT_TRUE(app.initialize());
  }
  EXPECT_TRUE(log.contains("shutdown completed"));
}

TEST(ApplicationTest, SignalTriggersGracefulShutdown) {
  CapturedLogger log;
  Application app(testConfig(), log.logger());  // installs signal handlers
  ASSERT_TRUE(app.initialize());
  ASSERT_EQ(std::raise(SIGTERM), 0);
  EXPECT_EQ(app.run(), 0);
  EXPECT_TRUE(log.contains("shutdown requested (SIGTERM)"));
  EXPECT_TRUE(log.contains("shutdown completed"));
}

TEST(ApplicationTest, SigintTriggersGracefulShutdown) {
  CapturedLogger log;
  Application app(testConfig(), log.logger());
  ASSERT_TRUE(app.initialize());
  ASSERT_EQ(std::raise(SIGINT), 0);
  EXPECT_EQ(app.run(), 0);
  EXPECT_TRUE(log.contains("shutdown requested (SIGINT)"));
}

TEST(ApplicationTest, SecondApplicationCannotInstallSignalHandlers) {
  CapturedLogger first_log;
  CapturedLogger second_log;
  Application first(testConfig(), first_log.logger());
  ASSERT_TRUE(first.initialize());

  Application second(testConfig(), second_log.logger());
  EXPECT_FALSE(second.initialize());
  EXPECT_TRUE(second_log.contains("failed to install signal handlers"));
  EXPECT_EQ(second.state(), ApplicationState::Created);

  first.shutdown();
  Application third(testConfig(), second_log.logger());
  EXPECT_TRUE(third.initialize()) << "handlers are released by shutdown";
}

TEST(ApplicationNetworkTest, InitializeStartsHttpServerThatAnswers) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  ASSERT_NE(app.httpPort(), 0);
  EXPECT_TRUE(log.contains("HTTP server listening"));

  TestClient client(app.httpPort());
  const auto response = client.get("/health");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
}

TEST(ApplicationNetworkTest, ShutdownStopsTheServerAndRefusesConnections) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  const auto port = app.httpPort();
  {
    TestClient client(port);
    ASSERT_TRUE(client.get("/health"));
  }
  app.shutdown();

  net::io_context io;
  net::ip::tcp::socket socket(io);
  boost::system::error_code ec;
  socket.connect({net::ip::make_address("127.0.0.1"), port}, ec);
  EXPECT_TRUE(ec);
  EXPECT_TRUE(log.contains("stopping component 'http-server'"));
  EXPECT_TRUE(log.contains("HTTP server stopped"));
}

TEST(ApplicationNetworkTest, InitializeFailsWhenThePortIsTaken) {
  ServerHarness holder;  // occupies a port
  ASSERT_TRUE(holder.started);

  CapturedLogger log;
  auto config = testConfig();
  config.server.port = holder.port();
  Application app(config, log.logger(), kNoSignals);
  EXPECT_FALSE(app.initialize());
  EXPECT_EQ(app.state(), ApplicationState::Created);
  EXPECT_TRUE(log.contains("failed to start the HTTP server"));
  EXPECT_EQ(app.run(), 1);
}

TEST(ApplicationNetworkTest, FailedInitializeReleasesSignalHandlers) {
  ServerHarness holder;
  ASSERT_TRUE(holder.started);

  CapturedLogger log;
  auto config = testConfig();
  config.server.port = holder.port();
  Application failing(config, log.logger());  // installs signal handlers first
  EXPECT_FALSE(failing.initialize());

  Application next(testConfig(), log.logger());
  EXPECT_TRUE(next.initialize()) << "handlers must have been released by the failed attempt";
}

TEST(ApplicationNetworkTest, RunServesRequestsUntilSignalThenStopsCleanly) {
  CapturedLogger log;
  Application app(testConfig(), log.logger());
  ASSERT_TRUE(app.initialize());
  const auto port = app.httpPort();

  auto result = std::async(std::launch::async, [&app] { return app.run(); });
  {
    TestClient client(port);
    ASSERT_TRUE(client.get("/"));
    ASSERT_TRUE(client.get("/health"));
  }
  ASSERT_EQ(std::raise(SIGINT), 0);
  ASSERT_EQ(result.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_EQ(result.get(), 0);

  const auto requested = log.position("shutdown requested (SIGINT)");
  const auto http_stopped = log.position("HTTP server stopped");
  const auto completed = log.position("shutdown completed");
  ASSERT_NE(requested, std::string::npos);
  ASSERT_NE(http_stopped, std::string::npos);
  ASSERT_NE(completed, std::string::npos);
  EXPECT_LT(requested, http_stopped);
  EXPECT_LT(http_stopped, completed);
}

TEST(ApplicationNetworkTest, CustomRequestHandlerIsUsed) {
  CapturedLogger log;
  auto recorder = std::make_shared<RecordingHandler>();
  ApplicationOptions options;
  options.install_signal_handlers = false;
  options.request_handler = recorder;
  Application app(testConfig(), log.logger(), options);
  ASSERT_TRUE(app.initialize());
  TestClient client(app.httpPort());
  const auto response = client.get("/anything");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->body(), "recorded");
  EXPECT_EQ(recorder->count(), 1);
}

// --- service registry integration (Phase 3) -------------------------------------------

TEST(ApplicationRegistryTest, DisabledDatabaseLeavesPhase2BehaviourUntouched) {
  CapturedLogger log;
  Application app(testConfig(), log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize());
  TestClient client(app.httpPort());
  EXPECT_EQ(client.get("/health")->result(), http::status::ok);
  EXPECT_EQ(client.get("/services/svc/instances")->result(), http::status::not_found)
      << "the registry API is not mounted without a database";
  EXPECT_FALSE(log.contains("service registry ready"));
}

TEST(ApplicationRegistryTest, InitializeFailsWhenTheDatabaseIsUnreachable) {
  CapturedLogger log;
  auto config = testConfig();
  config.database.enabled = true;
  config.database.host = "127.0.0.1";
  config.database.port = 1;  // nothing listens here
  config.database.connect_timeout = std::chrono::seconds{1};
  config.database.password_env = "EDGEFLOW_TEST_PASSWORD_THAT_IS_NOT_SET";
  Application app(config, log.logger(), kNoSignals);
  EXPECT_FALSE(app.initialize()) << "a registry that cannot reach PostgreSQL must not start";
  EXPECT_TRUE(log.contains("cannot initialize the service registry database"));
  EXPECT_TRUE(log.contains("EDGEFLOW_TEST_PASSWORD_THAT_IS_NOT_SET is not set"));
  EXPECT_EQ(app.httpPort(), 0) << "no HTTP server is left listening";
  EXPECT_EQ(app.state(), ApplicationState::Created);
}

TEST(ApplicationRegistryTest, FailedDatabaseInitializationReleasesSignalHandlers) {
  CapturedLogger log;
  auto config = testConfig();
  config.database.enabled = true;
  config.database.port = 1;
  config.database.connect_timeout = std::chrono::seconds{1};
  {
    Application app(config, log.logger(), ApplicationOptions{});  // installs handlers
    EXPECT_FALSE(app.initialize());
  }
  CapturedLogger second_log;
  Application second(testConfig(), second_log.logger(), ApplicationOptions{});
  EXPECT_TRUE(second.initialize()) << "handlers were released by the failed attempt";
}

TEST(ApplicationRegistryTest, ServesTheRegistryApiBackedByPostgres) {
  EDGEFLOW_REQUIRE_TEST_DATABASE();
  CapturedLogger log;
  auto config = testConfig();
  config.database.enabled = true;
  config.database.host = test_db_params->host;
  config.database.port = test_db_params->port;
  config.database.name = test_db_params->database;
  config.database.user = test_db_params->user;
  config.database.password_env = "EDGEFLOW_TEST_DB_PASSWORD";
  Application app(config, log.logger(), kNoSignals);
  ASSERT_TRUE(app.initialize()) << log.output();
  EXPECT_TRUE(log.contains("service registry ready"));

  const std::string service = edgeflow::testing::uniqueName("app");
  TestClient client(app.httpPort());
  const auto created = client.request(http::verb::post, "/services/" + service + "/instances",
                                      R"({"instance_id":"a","host":"10.0.0.1","port":9000})");
  ASSERT_TRUE(created);
  EXPECT_EQ(created->result(), http::status::created);
  EXPECT_EQ(client.get("/services/" + service + "/instances/a")->result(), http::status::ok);
  EXPECT_EQ(client.get("/health")->result(), http::status::ok);
  EXPECT_EQ(client.request(http::verb::delete_, "/services/" + service + "/instances/a")->result(),
            http::status::no_content);
  app.shutdown();
  EXPECT_TRUE(log.contains("shutdown completed"));
}

}  // namespace
