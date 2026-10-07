#include <gtest/gtest.h>

#include <chrono>
#include <limits>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/reliability/Failover.hpp"
#include "edgeflow/reliability/Retry.hpp"

namespace {

using namespace std::chrono_literals;
using edgeflow::reliability::AttemptKind;
using edgeflow::reliability::AttemptLedger;
using edgeflow::reliability::BackoffPolicy;
using edgeflow::reliability::RetryDecision;
using edgeflow::reliability::RetryPolicy;
using edgeflow::reliability::RetryReason;

edgeflow::config::RetryConfig retryConfig(unsigned attempts = 3) {
  edgeflow::config::RetryConfig config;
  config.max_attempts = attempts;
  return config;
}

// --- retry decisions ---

TEST(RetryPolicyTest, DefaultsAreConservative) {
  const edgeflow::config::RetryConfig config;
  EXPECT_EQ(config.max_attempts, 3U);
  EXPECT_EQ(config.retryable_statuses, (std::vector<unsigned>{502, 503, 504}));
  EXPECT_EQ(config.retryable_methods, (std::vector<std::string>{"GET", "HEAD", "OPTIONS"}))
      << "no method with side effects by default";
}

TEST(RetryPolicyTest, MaxAttemptsCountsTheFirstAttempt) {
  const RetryPolicy policy(retryConfig(3));
  EXPECT_EQ(policy.maxAttempts(), 3U);
  EXPECT_TRUE(policy.decide(AttemptKind::NotSent, "GET", 1).retry) << "after attempt 1: a retry";
  EXPECT_TRUE(policy.decide(AttemptKind::NotSent, "GET", 2).retry) << "after attempt 2: the last retry";
  const auto third = policy.decide(AttemptKind::NotSent, "GET", 3);
  EXPECT_FALSE(third.retry) << "three attempts made: that is all";
  EXPECT_EQ(third.reason, RetryReason::AttemptsExhausted);
}

TEST(RetryPolicyTest, OneAttemptMeansNeverRetry) {
  const RetryPolicy policy(retryConfig(1));
  const auto decision = policy.decide(AttemptKind::NotSent, "GET", 1);
  EXPECT_FALSE(decision.retry);
  EXPECT_EQ(decision.reason, RetryReason::AttemptsExhausted);
}

TEST(RetryPolicyTest, DisabledRetriesNeverRetryAndReportASingleAttempt) {
  auto config = retryConfig(5);
  config.enabled = false;
  const RetryPolicy policy(config);
  EXPECT_FALSE(policy.enabled());
  EXPECT_EQ(policy.maxAttempts(), 1U);
  const auto decision = policy.decide(AttemptKind::NotSent, "GET", 1);
  EXPECT_FALSE(decision.retry);
  EXPECT_EQ(decision.reason, RetryReason::RetryDisabled);
}

TEST(RetryPolicyTest, FinalOutcomesAreNeverRetried) {
  const RetryPolicy policy(retryConfig(10));
  for (const auto kind : {AttemptKind::Success, AttemptKind::Permanent, AttemptKind::Cancelled}) {
    const auto decision = policy.decide(kind, "GET", 1);
    EXPECT_FALSE(decision.retry) << toString(kind);
    EXPECT_EQ(decision.reason, RetryReason::NotRetryable);
  }
}

TEST(RetryPolicyTest, ARequestThatNeverReachedABackendIsRetriedWhateverItsMethod) {
  const RetryPolicy policy(retryConfig());
  for (const char* method : {"GET", "POST", "PUT", "DELETE", "PATCH"}) {
    EXPECT_TRUE(policy.decide(AttemptKind::NotSent, method, 1).retry) << method;
  }
}

TEST(RetryPolicyTest, AfterThePossibleProcessingOnlyListedMethodsAreRetried) {
  const RetryPolicy policy(retryConfig());
  for (const auto kind : {AttemptKind::MaybeProcessed, AttemptKind::RetryableStatus}) {
    for (const char* method : {"GET", "HEAD", "OPTIONS"}) {
      EXPECT_TRUE(policy.decide(kind, method, 1).retry) << toString(kind) << " " << method;
    }
    for (const char* method : {"POST", "PUT", "DELETE", "PATCH", "get"}) {
      const auto decision = policy.decide(kind, method, 1);
      EXPECT_FALSE(decision.retry) << toString(kind) << " " << method;
      EXPECT_EQ(decision.reason, RetryReason::UnsafeMethod);
    }
  }
}

TEST(RetryPolicyTest, TheMethodListIsConfigurable) {
  auto config = retryConfig();
  config.retryable_methods = {"GET", "PUT"};
  const RetryPolicy policy(config);
  EXPECT_TRUE(policy.decide(AttemptKind::MaybeProcessed, "PUT", 1).retry);
  EXPECT_FALSE(policy.decide(AttemptKind::MaybeProcessed, "HEAD", 1).retry);
  EXPECT_FALSE(policy.decide(AttemptKind::MaybeProcessed, "POST", 1).retry);
}

TEST(RetryPolicyTest, StatusClassificationFollowsTheConfiguredList) {
  auto config = retryConfig();
  config.retryable_statuses = {503, 429};
  const RetryPolicy policy(config);
  EXPECT_EQ(policy.classifyStatus(503), AttemptKind::RetryableStatus);
  EXPECT_EQ(policy.classifyStatus(429), AttemptKind::RetryableStatus);
  for (const unsigned status : {200U, 201U, 301U, 400U, 404U, 500U, 502U, 504U}) {
    EXPECT_EQ(policy.classifyStatus(status), AttemptKind::Success) << status;
  }
}

TEST(RetryPolicyTest, AnEmptyStatusListNeverRetriesOnStatus) {
  auto config = retryConfig();
  config.retryable_statuses.clear();
  const RetryPolicy policy(config);
  EXPECT_EQ(policy.classifyStatus(503), AttemptKind::Success);
}

TEST(RetryPolicyTest, ReasonsAndKindsHaveNames) {
  EXPECT_STREQ(toString(RetryReason::Retry), "retry");
  EXPECT_STREQ(toString(RetryReason::UnsafeMethod), "unsafe_method");
  EXPECT_STREQ(toString(AttemptKind::NotSent), "not_sent");
  EXPECT_STREQ(toString(AttemptKind::MaybeProcessed), "maybe_processed");
}

// --- exponential backoff ---

TEST(BackoffPolicyTest, DoublesFromTheBaseDelay) {
  const BackoffPolicy backoff(100ms, 100000ms, 0);
  EXPECT_EQ(backoff.nominal(1), 100ms);
  EXPECT_EQ(backoff.nominal(2), 200ms);
  EXPECT_EQ(backoff.nominal(3), 400ms);
  EXPECT_EQ(backoff.nominal(4), 800ms);
  EXPECT_EQ(backoff.nominal(5), 1600ms);
  EXPECT_EQ(backoff.nominal(0), 0ms) << "there is no retry number zero";
}

TEST(BackoffPolicyTest, IsCappedAtTheMaximumDelay) {
  const BackoffPolicy backoff(100ms, 1000ms, 0);
  EXPECT_EQ(backoff.nominal(4), 800ms);
  EXPECT_EQ(backoff.nominal(5), 1000ms) << "1600 capped";
  EXPECT_EQ(backoff.nominal(6), 1000ms);
  EXPECT_EQ(backoff.nominal(50), 1000ms);
}

TEST(BackoffPolicyTest, NeverOverflowsWhateverTheRetryNumber) {
  const BackoffPolicy backoff(60000ms, 600000ms, 0);
  for (const unsigned n : {10U, 31U, 32U, 33U, 62U, 63U, 64U, 65U, 1000U, std::numeric_limits<unsigned>::max()}) {
    const auto delay = backoff.nominal(n);
    EXPECT_GE(delay.count(), 0) << n;
    EXPECT_LE(delay, 600000ms) << n;
  }
  const BackoffPolicy huge(std::chrono::milliseconds{std::numeric_limits<std::int64_t>::max() / 2},
                           std::chrono::milliseconds{std::numeric_limits<std::int64_t>::max()}, 0);
  for (const unsigned n : {1U, 2U, 3U, 62U, 63U}) {
    EXPECT_GE(huge.nominal(n).count(), 0) << n;
  }
}

TEST(BackoffPolicyTest, ADelayIsNeverBelowItsPredecessorUntilTheCap) {
  const BackoffPolicy backoff(7ms, 5000ms, 0);
  auto previous = 0ms;
  for (unsigned n = 1; n < 40; ++n) {
    EXPECT_GE(backoff.nominal(n), previous) << n;
    previous = backoff.nominal(n);
  }
}

TEST(BackoffPolicyTest, JitterOnlyShortensTheDelayAndStaysWithinItsBound) {
  const BackoffPolicy backoff(1000ms, 1000ms, 25);
  EXPECT_EQ(backoff.delay(1, 0.0), 1000ms) << "no random reduction";
  EXPECT_EQ(backoff.delay(1, 1.0), 750ms) << "the largest reduction: 25%";
  for (double unit = 0.0; unit <= 1.0; unit += 0.05) {
    const auto delay = backoff.delay(1, unit);
    EXPECT_LE(delay, 1000ms) << unit;
    EXPECT_GE(delay, 750ms) << unit;
  }
  EXPECT_EQ(backoff.delay(1, -3.0), 1000ms) << "out-of-range input is clamped";
  EXPECT_EQ(backoff.delay(1, 7.0), 750ms);
}

TEST(BackoffPolicyTest, NoJitterGivesExactDelays) {
  const BackoffPolicy backoff(50ms, 400ms, 0);
  for (const double unit : {0.0, 0.5, 0.999}) EXPECT_EQ(backoff.delay(2, unit), 100ms);
}

TEST(BackoffPolicyTest, FullJitterNeverGoesBelowZero) {
  const BackoffPolicy backoff(100ms, 100ms, 100);
  EXPECT_EQ(backoff.delay(1, 1.0), 0ms);
  EXPECT_GE(backoff.delay(1, 0.5).count(), 0);
}

TEST(BackoffPolicyTest, AMaximumBelowTheBaseIsRaisedToTheBase) {
  const BackoffPolicy backoff(500ms, 100ms, 0);  // invalid configuration is rejected earlier
  EXPECT_EQ(backoff.nominal(1), 500ms);
  EXPECT_EQ(backoff.nominal(3), 500ms);
}

// --- failover bookkeeping ---

TEST(AttemptLedgerTest, RemembersTriedBackendsAndFailuresPerBackend) {
  AttemptLedger ledger;
  EXPECT_FALSE(ledger.tried("a"));
  ledger.markTried("a");
  ledger.markTried("a");
  ledger.markTried("b");
  EXPECT_TRUE(ledger.tried("a"));
  EXPECT_EQ(ledger.triedCount(), 2U);

  EXPECT_TRUE(ledger.firstFailureFor("a"));
  EXPECT_FALSE(ledger.firstFailureFor("a")) << "one request, one failure per backend";
  EXPECT_TRUE(ledger.firstFailureFor("b"));
}

TEST(FailoverTest, TheBackendKeyIdentifiesServiceInstanceAndEndpoint) {
  using edgeflow::reliability::backendKey;
  EXPECT_EQ(backendKey("orders", "a", "10.0.0.1", 8080), "orders/a@10.0.0.1:8080");
  EXPECT_NE(backendKey("orders", "a", "10.0.0.1", 8080), backendKey("orders", "a", "10.0.0.1", 8081));
  EXPECT_NE(backendKey("orders", "a", "h", 1), backendKey("payment", "a", "h", 1));
}

}  // namespace
