#include <gtest/gtest.h>

#include <vector>

#include "edgeflow/discovery/HealthState.hpp"

namespace {

using namespace edgeflow::discovery;

constexpr HealthPolicy kPolicy{3, 2};  // 3 failures to fall, 2 successes to recover

TEST(HealthTrackerTest, UnknownBecomesHealthyOnTheFirstSuccess) {
  HealthTracker tracker;
  EXPECT_EQ(tracker.status(), HealthStatus::Unknown);
  const auto transition = tracker.record(true, kPolicy);
  ASSERT_TRUE(transition);
  EXPECT_EQ(*transition, HealthStatus::Healthy);
  EXPECT_EQ(tracker.status(), HealthStatus::Healthy);
}

TEST(HealthTrackerTest, UnknownBecomesUnhealthyOnTheFirstFailure) {
  HealthTracker tracker;
  const auto transition = tracker.record(false, kPolicy);
  ASSERT_TRUE(transition) << "no prior state to flap from: thresholds do not delay the first verdict";
  EXPECT_EQ(*transition, HealthStatus::Unhealthy);
}

TEST(HealthTrackerTest, HealthyNeedsConsecutiveFailuresToFall) {
  HealthTracker tracker{HealthStatus::Healthy};
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_EQ(tracker.status(), HealthStatus::Healthy) << "two failures are below the threshold of three";
  EXPECT_EQ(tracker.consecutiveFailures(), 2U);
  const auto fell = tracker.record(false, kPolicy);
  ASSERT_TRUE(fell);
  EXPECT_EQ(*fell, HealthStatus::Unhealthy);
}

TEST(HealthTrackerTest, ASuccessResetsTheFailureStreak) {
  HealthTracker tracker{HealthStatus::Healthy};
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_FALSE(tracker.record(true, kPolicy));
  EXPECT_EQ(tracker.consecutiveFailures(), 0U);
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_EQ(tracker.status(), HealthStatus::Healthy) << "failures were not consecutive";
}

TEST(HealthTrackerTest, UnhealthyNeedsConsecutiveSuccessesToRecover) {
  HealthTracker tracker{HealthStatus::Unhealthy};
  EXPECT_FALSE(tracker.record(true, kPolicy));
  EXPECT_EQ(tracker.status(), HealthStatus::Unhealthy) << "one success is below the threshold of two";
  EXPECT_EQ(tracker.consecutiveSuccesses(), 1U);
  const auto recovered = tracker.record(true, kPolicy);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(*recovered, HealthStatus::Healthy);
}

TEST(HealthTrackerTest, AFailureResetsTheRecoveryStreak) {
  HealthTracker tracker{HealthStatus::Unhealthy};
  EXPECT_FALSE(tracker.record(true, kPolicy));
  EXPECT_FALSE(tracker.record(false, kPolicy));
  EXPECT_FALSE(tracker.record(true, kPolicy));
  EXPECT_EQ(tracker.status(), HealthStatus::Unhealthy) << "successes were not consecutive";
  EXPECT_TRUE(tracker.record(true, kPolicy));
}

TEST(HealthTrackerTest, ThresholdOfOneTransitionsImmediately) {
  constexpr HealthPolicy eager{1, 1};
  HealthTracker tracker{HealthStatus::Healthy};
  ASSERT_TRUE(tracker.record(false, eager));
  EXPECT_EQ(tracker.status(), HealthStatus::Unhealthy);
  ASSERT_TRUE(tracker.record(true, eager));
  EXPECT_EQ(tracker.status(), HealthStatus::Healthy);
}

TEST(HealthTrackerTest, OnlyTransitionsAreReported) {
  HealthTracker tracker{HealthStatus::Healthy};
  EXPECT_FALSE(tracker.record(true, kPolicy)) << "healthy + success is not a transition";
  HealthTracker down{HealthStatus::Unhealthy};
  EXPECT_FALSE(down.record(false, kPolicy)) << "unhealthy + failure is not a transition";
}

TEST(HealthTrackerTest, NeverReturnsToUnknown) {
  HealthTracker tracker;
  std::vector<bool> sequence = {true, false, false, false, true, true, false, true, false, false, false};
  for (bool ok : sequence) {
    (void)tracker.record(ok, kPolicy);
    EXPECT_NE(tracker.status(), HealthStatus::Unknown);
  }
}

TEST(HealthTrackerTest, RepeatedFailureAndRecoveryCycles) {
  HealthTracker tracker{HealthStatus::Healthy};
  for (int cycle = 0; cycle < 5; ++cycle) {
    EXPECT_FALSE(tracker.record(false, kPolicy));
    EXPECT_FALSE(tracker.record(false, kPolicy));
    ASSERT_EQ(tracker.record(false, kPolicy), HealthStatus::Unhealthy) << "cycle " << cycle;
    EXPECT_FALSE(tracker.record(true, kPolicy));
    ASSERT_EQ(tracker.record(true, kPolicy), HealthStatus::Healthy) << "cycle " << cycle;
  }
}

TEST(HealthTrackerTest, TransitionStartsAFreshStreak) {
  HealthTracker tracker{HealthStatus::Healthy};
  for (int i = 0; i < 3; ++i) (void)tracker.record(false, kPolicy);
  ASSERT_EQ(tracker.status(), HealthStatus::Unhealthy);
  EXPECT_EQ(tracker.consecutiveFailures(), 0U);
  EXPECT_EQ(tracker.consecutiveSuccesses(), 0U);
}

}  // namespace
