#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/routing/Strategies.hpp"

namespace {

using edgeflow::config::RoutingStrategy;
using edgeflow::discovery::ServiceInstance;
using edgeflow::routing::ConsistentHashing;
using edgeflow::routing::LeastConnections;
using edgeflow::routing::LoadBalancer;
using edgeflow::routing::RoundRobin;
using edgeflow::routing::RoutingContext;
using edgeflow::routing::WeightedRouting;

ServiceInstance instance(const std::string& id, std::uint32_t weight = 1, std::uint64_t connections = 0,
                         const std::string& service = "svc") {
  ServiceInstance i;
  i.service = service;
  i.instance_id = id;
  i.host = "10.0.0." + std::to_string(id.size());
  i.port = 9000;
  i.weight = weight;
  i.connection_count = connections;
  return i;
}

std::string pick(LoadBalancer& balancer, const std::vector<ServiceInstance>& set, std::string_view key = {}) {
  const auto* chosen = balancer.select(set, RoutingContext{key});
  return chosen == nullptr ? std::string{"<none>"} : chosen->instance_id;
}

// ============================================================ shared behaviour

class AnyStrategyTest : public ::testing::TestWithParam<RoutingStrategy> {
 protected:
  std::unique_ptr<LoadBalancer> balancer = edgeflow::routing::makeLoadBalancer(GetParam());
};

TEST_P(AnyStrategyTest, EmptySetSelectsNothing) {
  EXPECT_EQ(balancer->select({}), nullptr);
  EXPECT_EQ(balancer->select({}, RoutingContext{"key"}), nullptr);
}

TEST_P(AnyStrategyTest, SingleInstanceIsAlwaysChosen) {
  const std::vector<ServiceInstance> only = {instance("only", 7, 3)};
  for (int i = 0; i < 20; ++i) EXPECT_EQ(pick(*balancer, only, "k" + std::to_string(i)), "only");
}

TEST_P(AnyStrategyTest, ReturnsAPointerIntoTheCallersSet) {
  const std::vector<ServiceInstance> set = {instance("a"), instance("b")};
  const auto* chosen = balancer->select(set);
  ASSERT_NE(chosen, nullptr);
  EXPECT_TRUE(chosen == &set[0] || chosen == &set[1]);
}

TEST_P(AnyStrategyTest, NeverReadsOrChangesMetadataOfTheInput) {
  const std::vector<ServiceInstance> set = {instance("a", 5, 1), instance("b", 3, 2), instance("c", 2, 3)};
  const auto copy = set;
  for (int i = 0; i < 30; ++i) (void)balancer->select(set, RoutingContext{"k"});
  for (std::size_t i = 0; i < set.size(); ++i) {
    EXPECT_EQ(set[i].weight, copy[i].weight);
    EXPECT_EQ(set[i].connection_count, copy[i].connection_count);
    EXPECT_EQ(set[i].instance_id, copy[i].instance_id);
  }
}

TEST_P(AnyStrategyTest, OnlyEverChoosesAMemberOfTheGivenSet) {
  // The Phase 4 -> Phase 5 boundary: whatever is not in the routable set cannot be chosen,
  // however the strategy works. D stands for an unhealthy instance that discovery excluded.
  const std::vector<ServiceInstance> routable = {instance("a", 5), instance("b", 3), instance("c", 2)};
  for (int i = 0; i < 500; ++i) {
    const auto chosen = pick(*balancer, routable, "key-" + std::to_string(i));
    EXPECT_TRUE(chosen == "a" || chosen == "b" || chosen == "c") << chosen;
  }
}

TEST_P(AnyStrategyTest, InputOrderDoesNotMatter) {
  auto forward = std::vector<ServiceInstance>{instance("a", 4), instance("b", 4), instance("c", 4)};
  auto reversed = std::vector<ServiceInstance>{forward[2], forward[1], forward[0]};
  auto other = edgeflow::routing::makeLoadBalancer(GetParam());
  for (int i = 0; i < 30; ++i) {
    const auto key = "k" + std::to_string(i);
    EXPECT_EQ(pick(*balancer, forward, key), pick(*other, reversed, key)) << "call " << i;
  }
}

TEST_P(AnyStrategyTest, SurvivesInstancesAppearingAndDisappearingBetweenCalls) {
  std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  for (int round = 0; round < 50; ++round) {
    if (round % 3 == 0) set.erase(set.begin());
    if (round % 3 == 1) set.push_back(instance("n" + std::to_string(round)));
    if (set.size() < 2) set.push_back(instance("fill" + std::to_string(round)));
    const auto chosen = pick(*balancer, set, "k");
    const bool member = std::any_of(set.begin(), set.end(), [&](const auto& i) { return i.instance_id == chosen; });
    EXPECT_TRUE(member) << "round " << round << " chose " << chosen;
  }
}

TEST_P(AnyStrategyTest, ManyThreadsMayCallConcurrently) {
  const std::vector<ServiceInstance> set = {instance("a", 5, 4), instance("b", 3, 2), instance("c", 2, 9)};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 2000; ++i) {
        const auto* chosen = balancer->select(set, RoutingContext{"t" + std::to_string(t) + "-" + std::to_string(i)});
        if (chosen == nullptr || (chosen != &set[0] && chosen != &set[1] && chosen != &set[2])) ++bad;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(bad.load(), 0);
}

INSTANTIATE_TEST_SUITE_P(AllStrategies, AnyStrategyTest,
                         ::testing::Values(RoutingStrategy::RoundRobin, RoutingStrategy::LeastConnections,
                                           RoutingStrategy::Weighted, RoutingStrategy::ConsistentHashing));

// ============================================================ round robin

TEST(RoundRobinTest, CyclesThroughTheInstancesInOrder) {
  RoundRobin rr;
  const std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  std::string sequence;
  for (int i = 0; i < 9; ++i) sequence += pick(rr, set);
  EXPECT_EQ(sequence, "abcabcabc");
}

TEST(RoundRobinTest, TheCycleDoesNotDependOnTheInputOrder) {
  RoundRobin rr;
  std::vector<ServiceInstance> set = {instance("c"), instance("a"), instance("b")};
  std::string sequence;
  for (int i = 0; i < 6; ++i) {
    std::next_permutation(set.begin(), set.end(), [](const auto& x, const auto& y) { return x.instance_id < y.instance_id; });
    sequence += pick(rr, set);
  }
  EXPECT_EQ(sequence, "abcabc");
}

TEST(RoundRobinTest, ARemovedInstanceIsSkippedAndTheCycleContinues) {
  RoundRobin rr;
  std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  EXPECT_EQ(pick(rr, set), "a");
  EXPECT_EQ(pick(rr, set), "b");
  set.erase(set.begin() + 2);  // c disappears (for example it became unhealthy)
  EXPECT_EQ(pick(rr, set), "a");
  EXPECT_EQ(pick(rr, set), "b");
  EXPECT_EQ(pick(rr, set), "a");
}

TEST(RoundRobinTest, ANewInstanceJoinsTheCycleWhereItsPositionIs) {
  RoundRobin rr;
  std::vector<ServiceInstance> set = {instance("a"), instance("c")};
  EXPECT_EQ(pick(rr, set), "a");
  set.push_back(instance("b"));
  EXPECT_EQ(pick(rr, set), "b") << "b sorts between a and c, so it is next";
  EXPECT_EQ(pick(rr, set), "c");
  EXPECT_EQ(pick(rr, set), "a");
}

TEST(RoundRobinTest, TheInstanceThatWasChosenLastDisappearing) {
  RoundRobin rr;
  std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  (void)pick(rr, set);
  EXPECT_EQ(pick(rr, set), "b");
  set.erase(set.begin() + 1);  // b, the one chosen last, is gone
  EXPECT_EQ(pick(rr, set), "c") << "continues after where b was";
}

TEST(RoundRobinTest, AnInstanceListedTwiceIsOneInstance) {
  RoundRobin rr;
  const std::vector<ServiceInstance> set = {instance("a"), instance("a"), instance("b")};
  std::string sequence;
  for (int i = 0; i < 4; ++i) sequence += pick(rr, set);
  EXPECT_EQ(sequence, "abab");
}

TEST(RoundRobinTest, ConcurrentCallersShareOneStrictCycle) {
  RoundRobin rr;
  const std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  constexpr int kThreads = 8;
  constexpr int kPerThread = 3000;  // 24000 = 8000 full cycles
  std::vector<std::map<std::string, int>> counts(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) ++counts[static_cast<std::size_t>(t)][pick(rr, set)];
    });
  }
  for (auto& t : threads) t.join();
  std::map<std::string, int> total;
  for (const auto& c : counts) {
    for (const auto& [id, n] : c) total[id] += n;
  }
  EXPECT_EQ(total["a"], 8000);
  EXPECT_EQ(total["b"], 8000);
  EXPECT_EQ(total["c"], 8000) << "no selection was lost or duplicated";
}

TEST(RoundRobinTest, SeparateBalancersKeepSeparateCycles) {
  RoundRobin one;
  RoundRobin two;
  const std::vector<ServiceInstance> set = {instance("a"), instance("b")};
  EXPECT_EQ(pick(one, set), "a");
  EXPECT_EQ(pick(one, set), "b");
  EXPECT_EQ(pick(two, set), "a");
}

// ============================================================ least connections

TEST(LeastConnectionsTest, ChoosesTheFewestConnections) {
  LeastConnections lc;
  EXPECT_EQ(pick(lc, {instance("a", 1, 10), instance("b", 1, 4), instance("c", 1, 7)}), "b");
  EXPECT_EQ(pick(lc, {instance("a", 1, 0), instance("b", 1, 4), instance("c", 1, 7)}), "a");
  EXPECT_EQ(pick(lc, {instance("a", 1, 9), instance("b", 1, 4), instance("c", 1, 1)}), "c");
}

TEST(LeastConnectionsTest, TiesGoToTheSmallestIdentityRegardlessOfOrder) {
  LeastConnections lc;
  EXPECT_EQ(pick(lc, {instance("c", 1, 3), instance("a", 1, 3), instance("b", 1, 3)}), "a");
  EXPECT_EQ(pick(lc, {instance("b", 1, 3), instance("c", 1, 3), instance("a", 1, 9)}), "b");
}

TEST(LeastConnectionsTest, FollowsChangingCountsAndNeverUsesWeights) {
  LeastConnections lc;
  std::vector<ServiceInstance> set = {instance("a", 1000, 5), instance("b", 1, 2), instance("c", 1, 7)};
  EXPECT_EQ(pick(lc, set), "b") << "the heaviest instance does not win; connections decide";
  set[1].connection_count = 9;
  EXPECT_EQ(pick(lc, set), "a");
  set[0].connection_count = 12;
  EXPECT_EQ(pick(lc, set), "c");
}

TEST(LeastConnectionsTest, ReadsTheCountItIsGivenAndDoesNotInventConnections) {
  LeastConnections lc;
  const std::vector<ServiceInstance> set = {instance("a", 1, 0), instance("b", 1, 0)};
  for (int i = 0; i < 10; ++i) EXPECT_EQ(pick(lc, set), "a") << "selecting does not add a connection";
  EXPECT_EQ(set[0].connection_count, 0U);
}

TEST(LeastConnectionsTest, HandlesTheLargestCounts) {
  LeastConnections lc;
  const std::uint64_t huge = 9223372036854775807ULL;
  EXPECT_EQ(pick(lc, {instance("a", 1, huge), instance("b", 1, huge - 1)}), "b");
}

// ============================================================ weighted

std::map<std::string, int> tally(LoadBalancer& balancer, const std::vector<ServiceInstance>& set, int picks) {
  std::map<std::string, int> counts;
  for (int i = 0; i < picks; ++i) ++counts[pick(balancer, set)];
  return counts;
}

TEST(WeightedRoutingTest, EveryWindowOfTheWeightSumFollowsTheWeightsExactly) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 5), instance("b", 3), instance("c", 2)};
  for (int window = 0; window < 100; ++window) {
    const auto counts = tally(w, set, 10);
    ASSERT_EQ(counts.at("a"), 5) << "window " << window;
    ASSERT_EQ(counts.at("b"), 3) << "window " << window;
    ASSERT_EQ(counts.at("c"), 2) << "window " << window;
  }
}

TEST(WeightedRoutingTest, LargeSampleMatchesTheProportions) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 5), instance("b", 3), instance("c", 2)};
  const auto counts = tally(w, set, 100000);
  EXPECT_EQ(counts.at("a"), 50000);
  EXPECT_EQ(counts.at("b"), 30000);
  EXPECT_EQ(counts.at("c"), 20000);
}

TEST(WeightedRoutingTest, HeavyInstancesAreInterleavedNotChosenInABurst) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 5), instance("b", 3), instance("c", 2)};
  std::string sequence;
  for (int i = 0; i < 10; ++i) sequence += pick(w, set);
  EXPECT_EQ(sequence, "abcaabacba") << "the smooth interleaving of weights 5, 3, 2 (a and b tie at round 5; the smaller key wins)";
  int longest = 0;
  int run = 0;
  char previous = 0;
  for (const char c : sequence) {
    run = (c == previous) ? run + 1 : 1;
    previous = c;
    longest = std::max(longest, run);
  }
  EXPECT_LE(longest, 2) << sequence;
}

TEST(WeightedRoutingTest, EqualWeightsBehaveLikeRoundRobin) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 4), instance("b", 4), instance("c", 4)};
  std::string sequence;
  for (int i = 0; i < 6; ++i) sequence += pick(w, set);
  EXPECT_EQ(sequence, "abcabc");
}

TEST(WeightedRoutingTest, AWeightOfOneAgainstAHugeWeight) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("big", 1000), instance("tiny", 1)};
  const auto counts = tally(w, set, 1001 * 10);
  EXPECT_EQ(counts.at("big"), 10000);
  EXPECT_EQ(counts.at("tiny"), 10);
}

TEST(WeightedRoutingTest, ZeroWeightInstancesGetNoShareWhileOthersHaveOne) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 0), instance("b", 3), instance("c", 1)};
  const auto counts = tally(w, set, 400);
  EXPECT_EQ(counts.count("a"), 0U);
  EXPECT_EQ(counts.at("b"), 300);
  EXPECT_EQ(counts.at("c"), 100);
}

TEST(WeightedRoutingTest, AllZeroWeightsFallBackToEqualSharesInsteadOfRefusing) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 0), instance("b", 0), instance("c", 0)};
  const auto counts = tally(w, set, 300);
  EXPECT_EQ(counts.at("a"), 100);
  EXPECT_EQ(counts.at("b"), 100);
  EXPECT_EQ(counts.at("c"), 100);
}

TEST(WeightedRoutingTest, WeightsAreSummedWithoutOverflow) {
  WeightedRouting w;
  constexpr std::uint32_t kMax = 4294967295U;
  const std::vector<ServiceInstance> set = {instance("a", kMax), instance("b", kMax), instance("c", kMax)};
  const auto counts = tally(w, set, 30);
  EXPECT_EQ(counts.at("a"), 10);
  EXPECT_EQ(counts.at("b"), 10);
  EXPECT_EQ(counts.at("c"), 10);
}

TEST(WeightedRoutingTest, FollowsChangesToTheSetAndToTheWeights) {
  WeightedRouting w;
  std::vector<ServiceInstance> set = {instance("a", 1), instance("b", 1), instance("c", 1)};
  (void)tally(w, set, 30);
  set.erase(set.begin() + 1);  // b leaves
  auto counts = tally(w, set, 100);
  EXPECT_EQ(counts.count("b"), 0U);
  EXPECT_EQ(counts.at("a"), 50);
  EXPECT_EQ(counts.at("c"), 50);

  set.push_back(instance("d", 2));  // d joins with weight 2
  counts = tally(w, set, 400);
  EXPECT_EQ(counts.at("a"), 100);
  EXPECT_EQ(counts.at("c"), 100);
  EXPECT_EQ(counts.at("d"), 200);

  set[0].weight = 4;  // a is re-weighted
  counts = tally(w, set, 700);
  EXPECT_EQ(counts.at("a"), 400);
  EXPECT_EQ(counts.at("c"), 100);
  EXPECT_EQ(counts.at("d"), 200);
}

TEST(WeightedRoutingTest, ConcurrentCallersStillGetTheExactProportions) {
  WeightedRouting w;
  const std::vector<ServiceInstance> set = {instance("a", 5), instance("b", 3), instance("c", 2)};
  constexpr int kThreads = 8;
  constexpr int kPerThread = 1250;  // 10000 picks in total = 1000 windows
  std::vector<std::map<std::string, int>> counts(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) ++counts[static_cast<std::size_t>(t)][pick(w, set)];
    });
  }
  for (auto& t : threads) t.join();
  std::map<std::string, int> total;
  for (const auto& c : counts) {
    for (const auto& [id, n] : c) total[id] += n;
  }
  EXPECT_EQ(total["a"], 5000);
  EXPECT_EQ(total["b"], 3000);
  EXPECT_EQ(total["c"], 2000);
}

// ============================================================ consistent hashing

std::vector<std::string> keys(int n) {
  std::vector<std::string> out;
  for (int i = 0; i < n; ++i) out.push_back("client-" + std::to_string(i));
  return out;
}

std::map<std::string, std::string> mapping(ConsistentHashing& ch, const std::vector<ServiceInstance>& set,
                                           const std::vector<std::string>& all_keys) {
  std::map<std::string, std::string> result;
  for (const auto& key : all_keys) result[key] = pick(ch, set, key);
  return result;
}

TEST(ConsistentHashingTest, TheSameKeyAlwaysMapsToTheSameInstance) {
  ConsistentHashing ch;
  const std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  for (const auto& key : keys(200)) {
    const auto first = pick(ch, set, key);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(pick(ch, set, key), first) << key;
  }
}

TEST(ConsistentHashingTest, TheMappingIsIdenticalAcrossBalancersAndInputOrder) {
  ConsistentHashing first;
  ConsistentHashing second;
  const std::vector<ServiceInstance> forward = {instance("a"), instance("b"), instance("c")};
  const std::vector<ServiceInstance> reversed = {instance("c"), instance("b"), instance("a")};
  for (const auto& key : keys(300)) EXPECT_EQ(pick(first, forward, key), pick(second, reversed, key)) << key;
}

TEST(ConsistentHashingTest, KeysSpreadOverAllInstancesReasonablyEvenly) {
  ConsistentHashing ch;
  const std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c"), instance("d")};
  std::map<std::string, int> counts;
  for (const auto& key : keys(20000)) ++counts[pick(ch, set, key)];
  ASSERT_EQ(counts.size(), 4U);
  // The hash is deterministic, so this is not flaky; the bounds are generous around the
  // ideal 25% to describe "no instance is starved or flooded".
  for (const auto& [id, n] : counts) {
    EXPECT_GT(n, 20000 * 15 / 100) << id;
    EXPECT_LT(n, 20000 * 35 / 100) << id;
  }
}

TEST(ConsistentHashingTest, AddingAnInstanceOnlyMovesKeysToTheNewInstance) {
  ConsistentHashing ch;
  std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  const auto all_keys = keys(10000);
  const auto before = mapping(ch, set, all_keys);

  set.push_back(instance("d"));
  const auto after = mapping(ch, set, all_keys);

  int moved = 0;
  for (const auto& key : all_keys) {
    if (before.at(key) == after.at(key)) continue;
    ++moved;
    EXPECT_EQ(after.at(key), "d") << key << " moved between two OLD instances: " << before.at(key) << " -> " << after.at(key);
  }
  EXPECT_GT(moved, 10000 * 10 / 100) << "the new instance takes over a fair share";
  EXPECT_LT(moved, 10000 * 40 / 100) << "but most keys stay where they were (hash % N would move ~75%)";
}

TEST(ConsistentHashingTest, RemovingAnInstanceOnlyRemapsItsOwnKeys) {
  ConsistentHashing ch;
  std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c"), instance("d")};
  const auto all_keys = keys(10000);
  const auto before = mapping(ch, set, all_keys);

  set.erase(set.begin() + 1);  // b is removed
  const auto after = mapping(ch, set, all_keys);

  int remapped = 0;
  for (const auto& key : all_keys) {
    EXPECT_NE(after.at(key), "b") << "a removed instance is never selected";
    if (before.at(key) == "b") {
      ++remapped;
    } else {
      EXPECT_EQ(after.at(key), before.at(key)) << key << " did not belong to b but moved";
    }
  }
  EXPECT_GT(remapped, 0);
}

TEST(ConsistentHashingTest, RemovingThenReAddingRestoresTheOriginalMapping) {
  ConsistentHashing ch;
  const std::vector<ServiceInstance> full = {instance("a"), instance("b"), instance("c")};
  const std::vector<ServiceInstance> degraded = {instance("a"), instance("c")};
  const auto all_keys = keys(2000);
  const auto original = mapping(ch, full, all_keys);
  (void)mapping(ch, degraded, all_keys);
  EXPECT_EQ(mapping(ch, full, all_keys), original) << "the ring depends only on the member identities";
}

TEST(ConsistentHashingTest, AnEmptyKeyIsAKeyLikeAnyOther) {
  ConsistentHashing ch;
  const std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c")};
  const auto first = pick(ch, set, "");
  for (int i = 0; i < 10; ++i) EXPECT_EQ(pick(ch, set, ""), first);
}

TEST(ConsistentHashingTest, VirtualNodesImproveTheSpread) {
  const std::vector<ServiceInstance> set = {instance("a"), instance("b"), instance("c"), instance("d")};
  const auto spread = [&](unsigned virtual_nodes) {
    ConsistentHashing ch(virtual_nodes);
    std::map<std::string, int> counts;
    for (const auto& key : keys(20000)) ++counts[pick(ch, set, key)];
    int low = 20000;
    int high = 0;
    for (const auto& [id, n] : counts) {
      low = std::min(low, n);
      high = std::max(high, n);
    }
    return high - low;
  };
  EXPECT_LT(spread(160), spread(1)) << "one point per instance is much lumpier than 160";
}

TEST(ConsistentHashingTest, ZeroVirtualNodesIsTreatedAsOne) {
  ConsistentHashing ch(0);
  const std::vector<ServiceInstance> set = {instance("a"), instance("b")};
  EXPECT_NE(ch.select(set, RoutingContext{"k"}), nullptr);
}

TEST(ConsistentHashingTest, ConcurrentSelectionWhileTheSetKeepsChanging) {
  ConsistentHashing ch;
  const std::vector<ServiceInstance> three = {instance("a"), instance("b"), instance("c")};
  const std::vector<ServiceInstance> four = {instance("a"), instance("b"), instance("c"), instance("d")};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 1500; ++i) {
        const auto& set = ((i + t) % 2 == 0) ? three : four;  // the ring is rebuilt back and forth
        const auto* chosen = ch.select(set, RoutingContext{"k" + std::to_string(i)});
        const bool member = chosen != nullptr && std::any_of(set.begin(), set.end(), [&](const ServiceInstance& s) {
                              return &s == chosen;
                            });
        if (!member) ++bad;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(bad.load(), 0);
}

TEST(StableHashTest, IsDeterministicAndSpreadsSimilarInputs) {
  EXPECT_EQ(edgeflow::routing::stableHash("client-1"), edgeflow::routing::stableHash("client-1"));
  EXPECT_NE(edgeflow::routing::stableHash("client-1"), edgeflow::routing::stableHash("client-2"));
  EXPECT_NE(edgeflow::routing::stableHash(""), edgeflow::routing::stableHash("a"));
  std::set<std::uint64_t> distinct;
  for (int i = 0; i < 1000; ++i) distinct.insert(edgeflow::routing::stableHash("svc/inst#" + std::to_string(i)));
  EXPECT_EQ(distinct.size(), 1000U);
}

}  // namespace
