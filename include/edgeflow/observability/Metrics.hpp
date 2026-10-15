#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edgeflow/config/Config.hpp"

namespace edgeflow::observability {

using Clock = std::function<std::chrono::steady_clock::time_point()>;

// Upper bounds (seconds) of the latency buckets: 100 us * 2^(i/4), four buckets per octave up to
// about 70 s, so a quantile read back from the buckets is within about 9 % of the true value.
// A last bucket holds everything above the final bound (+Inf).
[[nodiscard]] const std::vector<double>& latencyBounds();
[[nodiscard]] std::size_t latencyBucketCount();  // finite bounds + the +Inf bucket

// A copy of a histogram's counters (not cumulative: counts[i] is the number of samples that fell
// into bucket i only).
struct HistogramSnapshot {
  std::vector<std::uint64_t> counts;
  std::uint64_t count{0};
  double sum_seconds{0};

  void add(const HistogramSnapshot& other);
  // Quantile q in [0, 1] by linear interpolation inside the bucket that holds the rank; NaN when
  // there are no samples; the largest finite bound for samples in the +Inf bucket.
  [[nodiscard]] double quantile(double q) const;
};

// Lock-free latency histogram: record() is a few relaxed atomic increments and may be called from
// any thread; snapshot() may run at any time (it is consistent per counter, not across counters).
class Histogram {
 public:
  Histogram();
  void record(std::chrono::nanoseconds duration) noexcept;
  [[nodiscard]] HistogramSnapshot snapshot() const;

 private:
  std::vector<std::atomic<std::uint64_t>> buckets_;
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_ns_{0};
};

// What a sliding window of whole seconds saw.
struct WindowSummary {
  std::uint64_t samples{0};   // requests (or attempts) in the window, current second included
  std::uint64_t errors{0};
  HistogramSnapshot latency;  // of those samples
  double per_second{0};       // rate over the completed seconds of the window (see below)
  double error_ratio{0};      // errors / samples; NaN without samples
};

// Latency and outcome of the last `window` seconds, plus the same since the start (as a
// Histogram, for the Prometheus bucket series). One slot per second; a slot is zeroed (under a
// mutex, once per second) when its second comes round again, everything else is atomic.
//
// per_second: samples of the completed seconds of the window divided by the number of those
// seconds (at most `window`, and no more than the process has been running); the second in
// progress is not part of the rate, so it does not make a busy gateway look slow for 1 s.
class WindowedStats {
 public:
  explicit WindowedStats(unsigned window_seconds, Clock clock = nullptr);

  void record(std::chrono::nanoseconds latency, bool error) noexcept;

  [[nodiscard]] WindowSummary window() const;
  [[nodiscard]] HistogramSnapshot lifetimeLatency() const { return lifetime_.snapshot(); }
  [[nodiscard]] std::uint64_t total() const noexcept { return total_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t totalErrors() const noexcept {
    return total_errors_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] unsigned windowSeconds() const noexcept { return window_; }

 private:
  struct Slot {
    explicit Slot(std::size_t buckets) : latency(buckets) {}
    std::atomic<std::int64_t> epoch{-1};
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> errors{0};
    std::vector<std::atomic<std::uint64_t>> latency;
  };

  [[nodiscard]] std::int64_t nowSecond() const;

  const unsigned window_;
  const Clock clock_;
  const std::chrono::steady_clock::time_point base_;
  std::vector<std::unique_ptr<Slot>> slots_;  // window_ + 1 slots
  std::mutex rotate_mutex_;
  Histogram lifetime_;
  std::atomic<std::uint64_t> total_{0};
  std::atomic<std::uint64_t> total_errors_{0};
};

using Labels = std::vector<std::pair<std::string, std::string>>;

// Writes the Prometheus text exposition format (version 0.0.4): every family has a HELP and a TYPE
// line followed by its series. Names and label names are the caller's responsibility (fixed
// strings in this code base); label VALUES are escaped.
class MetricsWriter {
 public:
  void counter(std::string_view name, std::string_view help, std::uint64_t value);
  void counter(std::string_view name, std::string_view help,
               const std::vector<std::pair<Labels, std::uint64_t>>& series);
  void gauge(std::string_view name, std::string_view help, double value);
  void gauge(std::string_view name, std::string_view help,
             const std::vector<std::pair<Labels, double>>& series);
  // `step`: every step-th finite bucket bound is written (the +Inf bucket always), so the output
  // stays small while the in-process resolution stays fine.
  void histogram(std::string_view name, std::string_view help, const HistogramSnapshot& snapshot,
                 std::size_t step = 2);

  [[nodiscard]] const std::string& str() const noexcept { return out_; }

  [[nodiscard]] static std::string formatDouble(double value);
  [[nodiscard]] static std::string escapeLabelValue(std::string_view value);

 private:
  void header(std::string_view name, std::string_view help, std::string_view type);
  void series(std::string_view name, const Labels& labels, std::string_view value);

  std::string out_;
};

// The metrics of one EdgeFlow process. Request-path code feeds the windowed stats and counters; every
// other component contributes through a collector that reads its own counters when /metrics is
// scraped (so nothing is copied or duplicated on the request path).
class Metrics {
 public:
  using Collector = std::function<void(MetricsWriter&)>;

  explicit Metrics(config::MetricsConfig config, Clock clock = nullptr);

  // Client-facing request finished: `status` is the HTTP status the client got.
  void recordRequest(std::chrono::nanoseconds latency, unsigned status, bool proxied) noexcept;
  void requestStarted() noexcept { in_flight_.fetch_add(1, std::memory_order_relaxed); }
  void requestEnded() noexcept { in_flight_.fetch_sub(1, std::memory_order_relaxed); }

  [[nodiscard]] WindowedStats& requests() noexcept { return requests_; }
  // The latency of one attempt to reach a backend that was answered (see ProxyHandler).
  [[nodiscard]] WindowedStats& backend() noexcept { return backend_; }

  // Collectors run in registration order when the metrics are rendered. Register before the
  // server starts serving: not thread-safe against render().
  void addCollector(Collector collector) { collectors_.push_back(std::move(collector)); }

  [[nodiscard]] std::string render() const;

  [[nodiscard]] const config::MetricsConfig& config() const noexcept { return config_; }

 private:
  const config::MetricsConfig config_;
  const Clock clock_;
  const std::chrono::steady_clock::time_point started_;
  WindowedStats requests_;
  WindowedStats backend_;
  std::array<std::atomic<std::uint64_t>, 5> responses_{};  // 1xx-3xx are folded: [2xx, 3xx, 4xx, 5xx, other]
  std::atomic<std::uint64_t> proxied_{0};
  std::atomic<std::uint64_t> local_{0};
  std::atomic<std::int64_t> in_flight_{0};
  std::vector<Collector> collectors_;
};

}  // namespace edgeflow::observability
