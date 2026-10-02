#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "edgeflow/core/ShutdownCoordinator.hpp"
#include "support/TestLogger.hpp"

namespace {

using edgeflow::core::ShutdownCoordinator;
using edgeflow::testing::CapturedLogger;

TEST(ShutdownCoordinatorTest, StopsComponentsInReverseRegistrationOrder) {
  CapturedLogger log;
  ShutdownCoordinator coordinator(log.logger());
  std::vector<std::string> order;
  coordinator.registerComponent("first", [&] { order.push_back("first"); });
  coordinator.registerComponent("second", [&] { order.push_back("second"); });
  coordinator.registerComponent("third", [&] { order.push_back("third"); });

  EXPECT_TRUE(coordinator.shutdown(std::chrono::seconds{1}));
  EXPECT_EQ(order, (std::vector<std::string>{"third", "second", "first"}));
  EXPECT_TRUE(coordinator.completed());
}

TEST(ShutdownCoordinatorTest, SecondShutdownIsANoOp) {
  CapturedLogger log;
  ShutdownCoordinator coordinator(log.logger());
  int stops = 0;
  coordinator.registerComponent("c", [&] { ++stops; });

  EXPECT_TRUE(coordinator.shutdown(std::chrono::seconds{1}));
  EXPECT_FALSE(coordinator.shutdown(std::chrono::seconds{1}));
  EXPECT_EQ(stops, 1);
}

TEST(ShutdownCoordinatorTest, ThrowingComponentDoesNotBlockOthers) {
  CapturedLogger log;
  ShutdownCoordinator coordinator(log.logger());
  bool later_ran = false;
  coordinator.registerComponent("survivor", [&] { later_ran = true; });
  coordinator.registerComponent("broken", [] { throw std::runtime_error("boom"); });

  EXPECT_NO_THROW(coordinator.shutdown(std::chrono::seconds{1}));
  EXPECT_TRUE(later_ran);
  EXPECT_TRUE(log.contains("component 'broken' failed to stop cleanly: boom"));
  EXPECT_TRUE(log.contains("shutdown completed"));
}

TEST(ShutdownCoordinatorTest, RegistrationAfterShutdownIsRejected) {
  CapturedLogger log;
  ShutdownCoordinator coordinator(log.logger());
  ASSERT_TRUE(coordinator.shutdown(std::chrono::seconds{1}));
  EXPECT_FALSE(coordinator.registerComponent("late", [] {}));
}

TEST(ShutdownCoordinatorTest, EmptyCoordinatorShutsDownCleanly) {
  CapturedLogger log;
  ShutdownCoordinator coordinator(log.logger());
  EXPECT_FALSE(coordinator.completed());
  EXPECT_TRUE(coordinator.shutdown(std::chrono::seconds{0}));
  EXPECT_TRUE(coordinator.completed());
}

}  // namespace
