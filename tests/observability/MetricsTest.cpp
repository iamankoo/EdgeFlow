#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "edgeflow/observability/Metrics.hpp"
#include "support/MetricsTestSupport.hpp"

namespace edgeflow::observability {
namespace {

using namespace std::chrono_literals;
using testing::Exposition;

// A clock the test moves by hand.
struct FakeClock {
  std::atomic<std::int64_t> ns{0};
  Clock clock() {
    return [this] { return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{ns.load()}}; };
  }
  void advance(std::chrono::milliseconds by) { ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(by).count()); }
};

// ------------------------------------------------------------------ buckets and quantiles

TEST(LatencyBucketsTest, BoundsGrowByAQuarterOctaveFromOneHundredMicroseconds) {
  const auto& bounds = latencyBounds();
  ASSERT_EQ(latencyBucketCount(), bounds.size() + 1);
  EXPECT_DOUBLE_EQ(bounds.front(), 100e-6);
  for (std::size_t i = 1; i < bounds.size(); ++i) {
    EXPECT_NEAR(bounds[i] / bounds[i - 1], std::pow(2.0, 0.25), 1e-9) << i;
  }
  EXPECT_GT(bounds.back(), 60.0);
  EXPECT_LT(bounds.back(), 75.0);
}

TEST(HistogramTest, EmptyHistogramHasNoQuantiles) {
  Histogram histogram;
  const auto snapshot = histogram.snapshot();
  EXPECT_EQ(snapshot.count, 0U);
  EXPECT_TRUE(std::isnan(snapshot.quantile(0.5)));
}

TEST(HistogramTest, EverySampleLandsInTheBucketOfItsDuration) {
  Histogram histogram;
  histogram.record(50us);    // <= 100 us: first bucket
  histogram.record(100us);   // exactly on the first bound: still the first bucket
  histogram.record(101us);   // just above: second bucket
  histogram.record(-5ns);    // a clock going backwards must not crash or underflow
  histogram.record(10min);   // beyond the last finite bound: the +Inf bucket
  const auto snapshot = histogram.snapshot();
  EXPECT_EQ(snapshot.count, 5U);
  EXPECT_EQ(snapshot.counts[0], 3U);
  EXPECT_EQ(snapshot.counts[1], 1U);
  EXPECT_EQ(snapshot.counts.back(), 1U);
  EXPECT_NEAR(snapshot.sum_seconds, 600.0, 0.01);
  EXPECT_DOUBLE_EQ(snapshot.quantile(1.0), latencyBounds().back()) << "the +Inf bucket reports the last bound";
}

TEST(HistogramTest, QuantilesOfAUniformSpreadAreWithinTheBucketResolution) {
  Histogram histogram;
  for (int ms = 1; ms <= 1000; ++ms) histogram.record(std::chrono::milliseconds{ms});  // 1 ms .. 1 s, uniform
  const auto snapshot = histogram.snapshot();
  ASSERT_EQ(snapshot.count, 1000U);
  // Four buckets per octave: an interpolated quantile is within 2^(1/4) = 19 % of the truth, in practice far less.
  EXPECT_NEAR(snapshot.quantile(0.50), 0.500, 0.500 * 0.19);
  EXPECT_NEAR(snapshot.quantile(0.95), 0.950, 0.950 * 0.19);
  EXPECT_NEAR(snapshot.quantile(0.99), 0.990, 0.990 * 0.19);
  EXPECT_LE(snapshot.quantile(0.50), snapshot.quantile(0.95));
  EXPECT_LE(snapshot.quantile(0.95), snapshot.quantile(0.99));
}

TEST(HistogramTest, AConstantLatencyHasAllItsQuantilesInOneBucket) {
  Histogram histogram;
  for (int i = 0; i < 100; ++i) histogram.record(20ms);
  const auto snapshot = histogram.snapshot();
  const auto& bounds = latencyBounds();
  const auto upper = *std::lower_bound(bounds.begin(), bounds.end(), 0.020);
  const auto lower = upper / std::pow(2.0, 0.25);
  for (const double q : {0.0, 0.5, 0.95, 0.99, 1.0}) {
    EXPECT_GE(snapshot.quantile(q), lower - 1e-12) << q;
    EXPECT_LE(snapshot.quantile(q), upper + 1e-12) << q;
  }
}

TEST(HistogramTest, ConcurrentRecordingLosesNothing) {
  Histogram histogram;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 50000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) histogram.record(std::chrono::microseconds{(i % 2000) + t});
    });
  }
  std::thread reader([&] {
    for (int i = 0; i < 200; ++i) (void)histogram.snapshot();  // reading while writing is allowed
  });
  for (auto& thread : threads) thread.join();
  reader.join();
  EXPECT_EQ(histogram.snapshot().count, static_cast<std::uint64_t>(kThreads) * kPerThread);
}

// ------------------------------------------------------------------ windowed stats

TEST(WindowedStatsTest, ASampleStaysInTheWindowForExactlyThatLong) {
  FakeClock time;
  WindowedStats stats(5, time.clock());
  stats.record(10ms, false);
  for (int second = 0; second <= 5; ++second) {
    EXPECT_EQ(stats.window().samples, 1U) << "second " << second;
    time.advance(1000ms);
  }
  EXPECT_EQ(stats.window().samples, 0U) << "gone after the window";
  EXPECT_EQ(stats.total(), 1U) << "the lifetime total never forgets";
  EXPECT_EQ(stats.lifetimeLatency().count, 1U);
}

TEST(WindowedStatsTest, TheRateIsTheMeanOverTheCompletedSecondsOfTheWindow) {
  FakeClock time;
  WindowedStats stats(10, time.clock());
  for (int second = 0; second < 4; ++second) {
    for (int i = 0; i < 100; ++i) stats.record(1ms, false);  // 100 requests every second
    time.advance(1000ms);
  }
  // 4 seconds completed (0..3), nothing in the current one yet.
  EXPECT_NEAR(stats.window().per_second, 100.0, 1e-9);
  for (int i = 0; i < 7; ++i) stats.record(1ms, false);  // a few in the second in progress: not part of the rate
  EXPECT_NEAR(stats.window().per_second, 100.0, 1e-9);
  EXPECT_EQ(stats.window().samples, 407U) << "but they are in the window's samples";
}

TEST(WindowedStatsTest, ARateAfterALongIdleStretchFallsBackToZero) {
  FakeClock time;
  WindowedStats stats(3, time.clock());
  for (int i = 0; i < 50; ++i) stats.record(1ms, false);
  time.advance(60000ms);
  const auto window = stats.window();
  EXPECT_EQ(window.samples, 0U);
  EXPECT_DOUBLE_EQ(window.per_second, 0.0);
  EXPECT_TRUE(std::isnan(window.error_ratio)) << "no requests: the ratio is undefined, not 0";
  EXPECT_TRUE(std::isnan(window.latency.quantile(0.99)));
}

TEST(WindowedStatsTest, TheErrorRatioIsErrorsOverSamplesOfTheWindow) {
  FakeClock time;
  WindowedStats stats(10, time.clock());
  for (int i = 0; i < 90; ++i) stats.record(1ms, false);
  for (int i = 0; i < 10; ++i) stats.record(1ms, true);
  const auto window = stats.window();
  EXPECT_EQ(window.errors, 10U);
  EXPECT_NEAR(window.error_ratio, 0.10, 1e-12);
  EXPECT_EQ(stats.totalErrors(), 10U);
}

TEST(WindowedStatsTest, QuantilesFollowTheWindowNotTheHistory) {
  FakeClock time;
  WindowedStats stats(5, time.clock());
  for (int i = 0; i < 1000; ++i) stats.record(500ms, false);  // slow, long ago
  time.advance(30000ms);
  for (int i = 0; i < 1000; ++i) stats.record(2ms, false);     // fast, now
  const auto window = stats.window();
  EXPECT_LT(window.latency.quantile(0.99), 0.005) << "the slow history is out of the window";
  EXPECT_GT(stats.lifetimeLatency().quantile(0.99), 0.1) << "but it is in the lifetime histogram";
}

TEST(WindowedStatsTest, AWholeWindowOfSlotsIsReusedCleanly) {
  FakeClock time;
  WindowedStats stats(3, time.clock());  // 4 slots
  for (int second = 0; second < 40; ++second) {
    for (int i = 0; i <= second % 5; ++i) stats.record(1ms, second % 7 == 0);
    time.advance(1000ms);
    const auto window = stats.window();
    EXPECT_LE(window.samples, 5U * 4) << second;
  }
  EXPECT_EQ(stats.windowSeconds(), 3U);
}

TEST(WindowedStatsTest, ZeroWindowIsTreatedAsOneSecond) {
  FakeClock time;
  WindowedStats stats(0, time.clock());
  EXPECT_EQ(stats.windowSeconds(), 1U);
  stats.record(1ms, false);
  EXPECT_EQ(stats.window().samples, 1U);
}

TEST(WindowedStatsTest, ConcurrentWritersAndReadersWithTheRealClock) {
  WindowedStats stats(2);
  constexpr int kThreads = 6;
  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  std::atomic<std::uint64_t> written{0};
  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&] {
      while (!stop.load()) {
        stats.record(std::chrono::microseconds{300}, false);
        written.fetch_add(1);
      }
    });
  }
  std::thread reader([&] {
    while (!stop.load()) {
      const auto window = stats.window();
      EXPECT_LE(window.samples, written.load() + kThreads);
    }
  });
  std::this_thread::sleep_for(2500ms);  // long enough for slots to rotate under load
  stop.store(true);
  for (auto& writer : writers) writer.join();
  reader.join();
  EXPECT_EQ(stats.total(), written.load());
  EXPECT_EQ(stats.lifetimeLatency().count, written.load());
}

// ------------------------------------------------------------------ exposition format

TEST(MetricsWriterTest, CountersAndGaugesHaveHelpTypeAndSeries) {
  MetricsWriter w;
  w.counter("edgeflow_things_total", "Things that happened.", 42);
  w.gauge("edgeflow_level", "A level.", 1.5);
  EXPECT_EQ(w.str(),
            "# HELP edgeflow_things_total Things that happened.\n# TYPE edgeflow_things_total counter\n"
            "edgeflow_things_total 42\n"
            "# HELP edgeflow_level A level.\n# TYPE edgeflow_level gauge\nedgeflow_level 1.5\n");
}

TEST(MetricsWriterTest, LabelledSeriesShareOneFamilyHeader) {
  MetricsWriter w;
  w.counter("edgeflow_by_kind_total", "By kind.",
            {{Labels{{"kind", "a"}}, 1}, {Labels{{"kind", "b"}, {"zone", "x"}}, 2}});
  EXPECT_EQ(w.str(),
            "# HELP edgeflow_by_kind_total By kind.\n# TYPE edgeflow_by_kind_total counter\n"
            "edgeflow_by_kind_total{kind=\"a\"} 1\nedgeflow_by_kind_total{kind=\"b\",zone=\"x\"} 2\n");
}

TEST(MetricsWriterTest, LabelValuesAndHelpAreEscaped) {
  EXPECT_EQ(MetricsWriter::escapeLabelValue("a\\b\"c\nd"), "a\\\\b\\\"c\\nd");
  MetricsWriter w;
  w.gauge("edgeflow_x", "line one\nline two \\ end", {{Labels{{"v", "q\"uote"}}, 1}});
  const Exposition exposition(w.str());
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.samples().front().labels.at("v"), "q\"uote");
  EXPECT_NE(w.str().find("line one\\nline two \\\\ end"), std::string::npos);
}

TEST(MetricsWriterTest, SpecialValuesUseThePrometheusSpelling) {
  EXPECT_EQ(MetricsWriter::formatDouble(std::nan("")), "NaN");
  EXPECT_EQ(MetricsWriter::formatDouble(HUGE_VAL), "+Inf");
  EXPECT_EQ(MetricsWriter::formatDouble(-HUGE_VAL), "-Inf");
  EXPECT_EQ(MetricsWriter::formatDouble(0.0), "0");
  EXPECT_EQ(MetricsWriter::formatDouble(0.0001), "0.0001");
  EXPECT_EQ(MetricsWriter::formatDouble(12345678.0), "12345678");
}

TEST(MetricsWriterTest, HistogramsAreCumulativeWithInfSumAndCount) {
  Histogram histogram;
  for (int i = 0; i < 10; ++i) histogram.record(1ms);
  for (int i = 0; i < 5; ++i) histogram.record(100ms);
  histogram.record(5min);  // +Inf bucket
  MetricsWriter w;
  w.histogram("edgeflow_lat_seconds", "Latency.", histogram.snapshot(), 1);
  const Exposition exposition(w.str());
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.types().at("edgeflow_lat_seconds"), "histogram");
  double previous = 0;
  std::size_t buckets = 0;
  for (const auto& sample : exposition.samples()) {
    if (sample.name != "edgeflow_lat_seconds_bucket") continue;
    EXPECT_GE(sample.value, previous) << "cumulative counts never decrease (le=" << sample.labels.at("le") << ")";
    previous = sample.value;
    ++buckets;
  }
  EXPECT_EQ(buckets, latencyBounds().size() + 1) << "step 1 writes every bound plus +Inf";
  EXPECT_EQ(exposition.must("edgeflow_lat_seconds_bucket", {{"le", "+Inf"}}), 16);
  EXPECT_EQ(exposition.must("edgeflow_lat_seconds_count"), 16);
  EXPECT_NEAR(exposition.must("edgeflow_lat_seconds_sum"), 0.01 + 0.5 + 300.0, 0.01);
  EXPECT_EQ(exposition.must("edgeflow_lat_seconds_bucket", {{"le", "0.0016"}}), 10)
      << "the 1 ms samples are below 1.6 ms";
}

TEST(MetricsWriterTest, ASteppedHistogramWritesFewerBucketsButTheSameTotals) {
  Histogram histogram;
  for (int i = 0; i < 20; ++i) histogram.record(3ms);
  MetricsWriter w;
  w.histogram("edgeflow_lat_seconds", "Latency.", histogram.snapshot(), 2);
  const Exposition exposition(w.str());
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  std::size_t buckets = 0;
  for (const auto& sample : exposition.samples()) buckets += sample.name == "edgeflow_lat_seconds_bucket" ? 1U : 0U;
  EXPECT_EQ(buckets, latencyBounds().size() / 2 + 1);
  EXPECT_EQ(exposition.must("edgeflow_lat_seconds_count"), 20);
}

// ------------------------------------------------------------------ the process metrics

config::MetricsConfig metricsConfig(unsigned window = 10) {
  config::MetricsConfig config;
  config.enabled = true;
  config.window_seconds = window;
  return config;
}

TEST(MetricsTest, RequestsAreClassifiedByStatusAndKind) {
  FakeClock time;
  Metrics metrics(metricsConfig(), time.clock());
  metrics.recordRequest(2ms, 200, true);
  metrics.recordRequest(2ms, 204, true);
  metrics.recordRequest(2ms, 301, false);
  metrics.recordRequest(2ms, 404, true);
  metrics.recordRequest(2ms, 429, true);
  metrics.recordRequest(2ms, 503, true);
  metrics.recordRequest(2ms, 504, false);
  metrics.recordRequest(2ms, 101, false);  // not in 2xx..5xx
  const Exposition exposition(metrics.render());
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), 2);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "3xx"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "4xx"}}), 2);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "5xx"}}), 2);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "other"}}), 1);
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "proxy"}}), 5);
  EXPECT_EQ(exposition.must("edgeflow_http_requests_total", {{"kind", "local"}}), 3);
  EXPECT_EQ(exposition.must("edgeflow_http_errors_total"), 2) << "errors are 5xx; a 429 is the limiter working";
  EXPECT_NEAR(exposition.must("edgeflow_http_error_ratio"), 2.0 / 8.0, 1e-9);
}

TEST(MetricsTest, TheRenderedDocumentIsValidAndHasEveryRequestPathFamily) {
  FakeClock time;
  Metrics metrics(metricsConfig(7), time.clock());
  for (int i = 0; i < 200; ++i) metrics.recordRequest(std::chrono::milliseconds{1 + i % 50}, 200, i % 2 == 0);
  metrics.backend().record(4ms, false);
  time.advance(2000ms);
  const Exposition exposition(metrics.render());
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  for (const char* name :
       {"edgeflow_build_info", "edgeflow_uptime_seconds", "edgeflow_http_requests_total",
        "edgeflow_http_responses_total", "edgeflow_http_errors_total", "edgeflow_http_in_flight_requests",
        "edgeflow_http_request_duration_seconds", "edgeflow_http_window_seconds",
        "edgeflow_http_requests_per_second", "edgeflow_http_error_ratio", "edgeflow_http_request_latency_seconds",
        "edgeflow_backend_latency_seconds", "edgeflow_backend_latency_window_seconds",
        "edgeflow_backend_answers_per_second"}) {
    EXPECT_TRUE(exposition.has(name)) << name;
  }
  EXPECT_EQ(exposition.must("edgeflow_http_window_seconds"), 7);
  EXPECT_NEAR(exposition.must("edgeflow_uptime_seconds"), 2.0, 1e-6);
  EXPECT_NEAR(exposition.must("edgeflow_http_requests_per_second"), 100.0, 1e-6)
      << "200 requests in the first of two completed seconds, averaged over those 2";
  const double p50 = exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.5"}});
  const double p95 = exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.95"}});
  const double p99 = exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.99"}});
  EXPECT_LE(p50, p95);
  EXPECT_LE(p95, p99);
  EXPECT_GT(p50, 0.010);
  EXPECT_LT(p99, 0.070);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), 200);
  EXPECT_TRUE(exposition.value("edgeflow_build_info", {{"version", EDGEFLOW_VERSION}}));
}

TEST(MetricsTest, WithoutTrafficQuantilesAndRatiosAreNaNAndCountersAreZero) {
  FakeClock time;
  Metrics metrics(metricsConfig(), time.clock());
  const Exposition exposition(metrics.render());
  ASSERT_TRUE(exposition.valid()) << exposition.error();
  EXPECT_TRUE(std::isnan(exposition.must("edgeflow_http_request_latency_seconds", {{"quantile", "0.99"}})));
  EXPECT_TRUE(std::isnan(exposition.must("edgeflow_http_error_ratio")));
  EXPECT_EQ(exposition.must("edgeflow_http_errors_total"), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_requests_per_second"), 0);
  EXPECT_EQ(exposition.must("edgeflow_http_in_flight_requests"), 0);
}

TEST(MetricsTest, InFlightRequestsCountUpAndDown) {
  Metrics metrics(metricsConfig());
  metrics.requestStarted();
  metrics.requestStarted();
  EXPECT_EQ(Exposition(metrics.render()).must("edgeflow_http_in_flight_requests"), 2);
  metrics.requestEnded();
  metrics.requestEnded();
  EXPECT_EQ(Exposition(metrics.render()).must("edgeflow_http_in_flight_requests"), 0);
  metrics.requestEnded();  // an unbalanced end must never show a negative gauge
  EXPECT_EQ(Exposition(metrics.render()).must("edgeflow_http_in_flight_requests"), 0);
}

TEST(MetricsTest, CollectorsContributeInOrderAndSeeLiveValues) {
  Metrics metrics(metricsConfig());
  std::atomic<std::uint64_t> counter{5};
  metrics.addCollector([&](MetricsWriter& w) { w.counter("edgeflow_test_first_total", "First.", counter.load()); });
  metrics.addCollector([&](MetricsWriter& w) { w.gauge("edgeflow_test_second", "Second.", 2.5); });
  auto first = metrics.render();
  EXPECT_LT(first.find("edgeflow_test_first_total"), first.find("edgeflow_test_second"));
  counter.store(9);
  EXPECT_EQ(Exposition(metrics.render()).must("edgeflow_test_first_total"), 9);
}

TEST(MetricsTest, ScrapingWhileRequestsAreRecordedIsSafeAndTotalsAreExact) {
  Metrics metrics(metricsConfig(3));
  constexpr int kThreads = 8;
  constexpr int kPerThread = 20000;
  std::atomic<bool> done{false};
  std::thread scraper([&] {
    while (!done.load()) {
      const Exposition exposition(metrics.render());
      EXPECT_TRUE(exposition.valid()) << exposition.error();
    }
  });
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        metrics.requestStarted();
        metrics.recordRequest(std::chrono::microseconds{200 + i % 900}, (i % 10 == 0) ? 500U : 200U, t % 2 == 0);
        metrics.requestEnded();
      }
    });
  }
  for (auto& thread : threads) thread.join();
  done.store(true);
  scraper.join();
  const Exposition exposition(metrics.render());
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "2xx"}}), kThreads * kPerThread * 9 / 10);
  EXPECT_EQ(exposition.must("edgeflow_http_responses_total", {{"class", "5xx"}}), kThreads * kPerThread / 10);
  EXPECT_EQ(exposition.must("edgeflow_http_request_duration_seconds_count"), kThreads * kPerThread);
  EXPECT_EQ(exposition.must("edgeflow_http_in_flight_requests"), 0);
}

}  // namespace
}  // namespace edgeflow::observability
