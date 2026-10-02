#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <future>
#include <thread>
#include <vector>

#include "edgeflow/core/Application.hpp"
#include "support/TestLogger.hpp"

namespace {

using edgeflow::core::Application;
using edgeflow::core::ApplicationOptions;
using edgeflow::core::ApplicationState;
using edgeflow::testing::CapturedLogger;

constexpr ApplicationOptions kNoSignals{.install_signal_handlers = false};

edgeflow::config::Config testConfig() {
  edgeflow::config::Config config;
  config.shutdown.grace_period = std::chrono::seconds{1};
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

}  // namespace
