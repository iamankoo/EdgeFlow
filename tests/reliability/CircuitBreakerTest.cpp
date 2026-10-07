#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "edgeflow/reliability/CircuitBreaker.hpp"

namespace {

using namespace std::chrono_literals;
using edgeflow::reliability::Admission;
using edgeflow::reliability::CircuitBreaker;
using edgeflow::reliability::CircuitBreakerRegistry;
using edgeflow::reliability::CircuitBreakerSettings;
using edgeflow::reliability::CircuitState;

// A clock the test moves by hand: no sleeping, no flakiness.
class FakeClock {
 public:
  edgeflow::reliability::SteadyClock fn() {
    return [this] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{ms_.load()}; };
  }
  void advance(std::chrono::milliseconds by) { ms_ += by.count(); }

 private:
  std::atomic<std::int64_t> ms_{1000};
};

CircuitBreakerSettings settings(unsigned threshold = 3, std::chrono::milliseconds recovery = 1000ms,
                                unsigned half_open = 1) {
  return CircuitBreakerSettings{threshold, recovery, half_open};
}

// Fails `n` requests in a row through the breaker.
void failTimes(CircuitBreaker& breaker, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    const auto admission = breaker.tryAcquire();
    ASSERT_TRUE(admission.admitted);
    breaker.recordFailure(admission);
  }
}

TEST(CircuitBreakerTest, StartsClosedAndAdmitsEverything) {
  FakeClock clock;
  CircuitBreaker breaker(settings(), clock.fn());
  EXPECT_EQ(breaker.state(), CircuitState::Closed);
  for (int i = 0; i < 100; ++i) {
    const auto admission = breaker.tryAcquire();
    EXPECT_TRUE(admission.admitted);
    EXPECT_FALSE(admission.probe);
    breaker.recordSuccess(admission);
  }
  EXPECT_TRUE(breaker.isAvailable());
}

TEST(CircuitBreakerTest, OpensAfterTheThresholdOfConsecutiveFailures) {
  FakeClock clock;
  CircuitBreaker breaker(settings(3), clock.fn());
  failTimes(breaker, 2);
  EXPECT_EQ(breaker.state(), CircuitState::Closed) << "below the threshold";
  EXPECT_EQ(breaker.consecutiveFailures(), 2U);
  failTimes(breaker, 1);
  EXPECT_EQ(breaker.state(), CircuitState::Open);
}

TEST(CircuitBreakerTest, ASuccessResetsTheFailureStreak) {
  FakeClock clock;
  CircuitBreaker breaker(settings(3), clock.fn());
  failTimes(breaker, 2);
  breaker.recordSuccess(breaker.tryAcquire());
  EXPECT_EQ(breaker.consecutiveFailures(), 0U);
  failTimes(breaker, 2);
  EXPECT_EQ(breaker.state(), CircuitState::Closed) << "failures were not consecutive";
}

TEST(CircuitBreakerTest, OpenRejectsEverythingUntilTheRecoveryTimeout) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms), clock.fn());
  failTimes(breaker, 1);
  ASSERT_EQ(breaker.state(), CircuitState::Open);

  for (int i = 0; i < 50; ++i) EXPECT_FALSE(breaker.tryAcquire().admitted);
  EXPECT_FALSE(breaker.isAvailable());
  clock.advance(999ms);
  EXPECT_FALSE(breaker.tryAcquire().admitted) << "not yet";
  EXPECT_EQ(breaker.state(), CircuitState::Open);

  clock.advance(1ms);
  EXPECT_TRUE(breaker.isAvailable()) << "peeking takes no slot and changes no state";
  EXPECT_EQ(breaker.state(), CircuitState::Open);
}

TEST(CircuitBreakerTest, TheFirstRequestAfterRecoveryIsTheHalfOpenProbe) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms, 1), clock.fn());
  failTimes(breaker, 1);
  clock.advance(1000ms);

  const auto probe = breaker.tryAcquire();
  EXPECT_TRUE(probe.admitted);
  EXPECT_TRUE(probe.probe);
  EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
  EXPECT_FALSE(breaker.tryAcquire().admitted) << "only one probe at a time";
  EXPECT_FALSE(breaker.isAvailable());
}

TEST(CircuitBreakerTest, ASuccessfulProbeClosesTheCircuit) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms, 1), clock.fn());
  failTimes(breaker, 1);
  clock.advance(1000ms);
  breaker.recordSuccess(breaker.tryAcquire());
  EXPECT_EQ(breaker.state(), CircuitState::Closed);
  EXPECT_EQ(breaker.consecutiveFailures(), 0U);
  EXPECT_TRUE(breaker.tryAcquire().admitted);
}

TEST(CircuitBreakerTest, AFailedProbeReopensAndStartsANewRecoveryPeriod) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms, 1), clock.fn());
  failTimes(breaker, 1);
  clock.advance(1000ms);
  breaker.recordFailure(breaker.tryAcquire());
  EXPECT_EQ(breaker.state(), CircuitState::Open);

  clock.advance(999ms);
  EXPECT_FALSE(breaker.tryAcquire().admitted) << "the recovery period restarted";
  clock.advance(1ms);
  EXPECT_TRUE(breaker.tryAcquire().probe);
}

TEST(CircuitBreakerTest, HalfOpenAdmitsExactlyTheConfiguredNumberOfProbes) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms, 3), clock.fn());
  failTimes(breaker, 1);
  clock.advance(1000ms);

  std::vector<Admission> probes;
  for (int i = 0; i < 10; ++i) {
    const auto admission = breaker.tryAcquire();
    if (admission.admitted) probes.push_back(admission);
  }
  ASSERT_EQ(probes.size(), 3U);

  // All of them must succeed to close the circuit.
  breaker.recordSuccess(probes[0]);
  breaker.recordSuccess(probes[1]);
  EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
  EXPECT_FALSE(breaker.tryAcquire().admitted) << "successes count against the probe budget";
  breaker.recordSuccess(probes[2]);
  EXPECT_EQ(breaker.state(), CircuitState::Closed);
}

TEST(CircuitBreakerTest, OneFailedProbeReopensEvenWhenOthersSucceed) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms, 2), clock.fn());
  failTimes(breaker, 1);
  clock.advance(1000ms);
  const auto a = breaker.tryAcquire();
  const auto b = breaker.tryAcquire();
  breaker.recordSuccess(a);
  breaker.recordFailure(b);
  EXPECT_EQ(breaker.state(), CircuitState::Open);
}

TEST(CircuitBreakerTest, ReleasingAProbeFreesItsSlotWithoutAnyVerdict) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms, 1), clock.fn());
  failTimes(breaker, 1);
  clock.advance(1000ms);
  const auto probe = breaker.tryAcquire();
  ASSERT_TRUE(probe.admitted);
  breaker.release(probe);  // e.g. the client went away
  EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
  EXPECT_TRUE(breaker.tryAcquire().admitted) << "the slot is free again";
}

TEST(CircuitBreakerTest, ALateResultFromAnEarlierStateIsIgnored) {
  FakeClock clock;
  CircuitBreaker breaker(settings(2, 1000ms, 1), clock.fn());
  const auto slow = breaker.tryAcquire();  // admitted while closed, still running
  failTimes(breaker, 2);                   // the circuit opens meanwhile
  ASSERT_EQ(breaker.state(), CircuitState::Open);

  breaker.recordSuccess(slow);  // must not close or reset anything
  EXPECT_EQ(breaker.state(), CircuitState::Open);

  clock.advance(1000ms);
  const auto probe = breaker.tryAcquire();
  ASSERT_TRUE(probe.probe);
  breaker.recordFailure(slow);  // stale as well: must not reopen the half-open circuit
  EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
  breaker.recordSuccess(probe);
  EXPECT_EQ(breaker.state(), CircuitState::Closed);
}

TEST(CircuitBreakerTest, ResultsWhileOpenChangeNothing) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1, 1000ms), clock.fn());
  const auto in_flight = breaker.tryAcquire();
  failTimes(breaker, 1);
  breaker.recordFailure(in_flight);
  EXPECT_EQ(breaker.state(), CircuitState::Open);
  clock.advance(999ms);
  EXPECT_FALSE(breaker.tryAcquire().admitted) << "a late failure did not extend the open period";
}

TEST(CircuitBreakerTest, ASettledTicketOfARejectedRequestIsHarmless) {
  FakeClock clock;
  CircuitBreaker breaker(settings(1), clock.fn());
  failTimes(breaker, 1);
  const auto rejected = breaker.tryAcquire();
  ASSERT_FALSE(rejected.admitted);
  breaker.recordFailure(rejected);
  breaker.recordSuccess(rejected);
  breaker.release(rejected);
  EXPECT_EQ(breaker.state(), CircuitState::Open);
}

TEST(CircuitBreakerTest, TheListenerSeesTheFullLifecycleInOrder) {
  FakeClock clock;
  std::vector<std::pair<CircuitState, CircuitState>> seen;
  CircuitBreaker breaker(settings(1, 1000ms), clock.fn(),
                         [&](CircuitState from, CircuitState to) { seen.emplace_back(from, to); });
  failTimes(breaker, 1);
  clock.advance(1000ms);
  breaker.recordFailure(breaker.tryAcquire());  // half-open -> open
  clock.advance(1000ms);
  breaker.recordSuccess(breaker.tryAcquire());  // half-open -> closed

  using S = CircuitState;
  const std::vector<std::pair<S, S>> expected = {{S::Closed, S::Open},
                                                 {S::Open, S::HalfOpen},
                                                 {S::HalfOpen, S::Open},
                                                 {S::Open, S::HalfOpen},
                                                 {S::HalfOpen, S::Closed}};
  EXPECT_EQ(seen, expected);
}

TEST(CircuitBreakerTest, StateNames) {
  EXPECT_STREQ(toString(CircuitState::Closed), "closed");
  EXPECT_STREQ(toString(CircuitState::Open), "open");
  EXPECT_STREQ(toString(CircuitState::HalfOpen), "half-open");
}

// --- concurrency ---

TEST(CircuitBreakerConcurrencyTest, ManyRacingRequestsGetExactlyTheProbeBudget) {
  FakeClock clock;
  std::atomic<int> to_half_open{0};
  CircuitBreaker breaker(settings(1, 1000ms, 4), clock.fn(), [&](CircuitState, CircuitState to) {
    if (to == CircuitState::HalfOpen) ++to_half_open;
  });
  failTimes(breaker, 1);
  clock.advance(1000ms);

  constexpr int kThreads = 64;
  std::atomic<int> admitted{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      while (!go.load()) std::this_thread::yield();
      if (breaker.tryAcquire().admitted) ++admitted;
    });
  }
  go.store(true);
  for (auto& t : threads) t.join();

  EXPECT_EQ(admitted.load(), 4) << "no thundering herd: exactly the configured probes";
  EXPECT_EQ(to_half_open.load(), 1) << "one thread made the transition";
}

TEST(CircuitBreakerConcurrencyTest, ConcurrentFailuresOpenTheCircuitOnce) {
  FakeClock clock;
  std::atomic<int> opened{0};
  CircuitBreaker breaker(settings(10), clock.fn(), [&](CircuitState, CircuitState to) {
    if (to == CircuitState::Open) ++opened;
  });
  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 100; ++j) breaker.recordFailure(breaker.tryAcquire());
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(breaker.state(), CircuitState::Open);
  EXPECT_EQ(opened.load(), 1);
}

TEST(CircuitBreakerConcurrencyTest, RandomTrafficKeepsTheStateConsistent) {
  FakeClock clock;
  std::mutex log_mutex;
  std::vector<std::pair<CircuitState, CircuitState>> transitions;
  CircuitBreaker breaker(settings(3, 20ms, 2), clock.fn(), [&](CircuitState from, CircuitState to) {
    const std::lock_guard lock(log_mutex);
    transitions.emplace_back(from, to);
  });

  std::atomic<bool> stop{false};
  std::thread ticker([&] {
    while (!stop.load()) {
      clock.advance(5ms);
      std::this_thread::sleep_for(1ms);
    }
  });
  std::vector<std::thread> workers;
  for (int t = 0; t < 8; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      for (int i = 0; i < 5000; ++i) {
        const auto admission = breaker.tryAcquire();
        if (!admission.admitted) continue;
        switch (rng() % 4) {
          case 0: breaker.recordFailure(admission); break;
          case 1: breaker.release(admission); break;
          default: breaker.recordSuccess(admission); break;
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  stop.store(true);
  ticker.join();

  // Every recorded transition is one of the four legal ones. (Listeners run after the breaker's
  // lock is released, so under contention their calls may be logged out of order: only the
  // legality of each transition is asserted, not their sequence.)
  using S = CircuitState;
  const std::vector<std::pair<S, S>> legal = {
      {S::Closed, S::Open}, {S::Open, S::HalfOpen}, {S::HalfOpen, S::Open}, {S::HalfOpen, S::Closed}};
  for (const auto& [from, to] : transitions) {
    EXPECT_NE(std::find(legal.begin(), legal.end(), std::make_pair(from, to)), legal.end());
  }
  EXPECT_FALSE(transitions.empty());
}

// --- one breaker per backend ---

TEST(CircuitBreakerRegistryTest, BackendsHaveIndependentCircuits) {
  FakeClock clock;
  CircuitBreakerRegistry registry(settings(2), clock.fn());
  auto a = registry.get("orders/a@10.0.0.1:80");
  auto b = registry.get("orders/b@10.0.0.2:80");
  auto other = registry.get("payment/a@10.0.0.1:80");
  EXPECT_EQ(registry.get("orders/a@10.0.0.1:80"), a) << "the same backend, the same breaker";
  EXPECT_NE(a, b);
  EXPECT_EQ(registry.size(), 3U);

  failTimes(*a, 2);
  EXPECT_EQ(a->state(), CircuitState::Open);
  EXPECT_EQ(b->state(), CircuitState::Closed);
  EXPECT_EQ(other->state(), CircuitState::Closed) << "same instance id, another service";
  EXPECT_TRUE(registry.find("orders/b@10.0.0.2:80")->isAvailable());
  EXPECT_EQ(registry.find("never/seen@x:1"), nullptr);
}

TEST(CircuitBreakerRegistryTest, AReRegisteredEndpointStartsWithACleanCircuit) {
  FakeClock clock;
  CircuitBreakerRegistry registry(settings(1), clock.fn());
  failTimes(*registry.get("orders/a@10.0.0.1:80"), 1);
  EXPECT_EQ(registry.get("orders/a@10.0.0.9:80")->state(), CircuitState::Closed);
}

TEST(CircuitBreakerRegistryTest, TheKeyedListenerNamesTheBackend) {
  FakeClock clock;
  std::string key;
  CircuitBreakerRegistry registry(settings(1), clock.fn(),
                                  [&](const std::string& k, CircuitState, CircuitState) { key = k; });
  failTimes(*registry.get("orders/a@10.0.0.1:80"), 1);
  EXPECT_EQ(key, "orders/a@10.0.0.1:80");
}

TEST(CircuitBreakerRegistryTest, TheSoftCapForgetsOnlyCircuitsThatCarryNoInformation) {
  FakeClock clock;
  CircuitBreakerRegistry registry(settings(1), clock.fn(), nullptr, /*max_entries=*/4);
  failTimes(*registry.get("s/open@h:1"), 1);  // must survive
  for (int i = 0; i < 10; ++i) (void)registry.get("s/idle" + std::to_string(i) + "@h:1");
  ASSERT_NE(registry.find("s/open@h:1"), nullptr);
  EXPECT_EQ(registry.find("s/open@h:1")->state(), CircuitState::Open);
  EXPECT_LE(registry.size(), 6U) << "idle breakers were forgotten when the cap was hit";
}

TEST(CircuitBreakerRegistryTest, ConcurrentLookupsOfTheSameKeyShareOneBreaker) {
  FakeClock clock;
  CircuitBreakerRegistry registry(settings(), clock.fn());
  std::vector<std::shared_ptr<CircuitBreaker>> got(32);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < got.size(); ++i) {
    threads.emplace_back([&, i] { got[i] = registry.get("orders/a@h:1"); });
  }
  for (auto& t : threads) t.join();
  for (const auto& breaker : got) EXPECT_EQ(breaker, got[0]);
  EXPECT_EQ(registry.size(), 1U);
}

}  // namespace
