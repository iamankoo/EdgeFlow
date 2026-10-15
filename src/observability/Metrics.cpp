#include "edgeflow/observability/Metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace edgeflow::observability {

namespace {

constexpr std::size_t kFiniteBuckets = 78;  // 100 us * 2^(i/4), i = 0..77 -> last bound about 62 s

struct Bounds {
  std::vector<double> seconds;
  std::vector<std::uint64_t> nanos;
  Bounds() {
    seconds.reserve(kFiniteBuckets);
    nanos.reserve(kFiniteBuckets);
    for (std::size_t i = 0; i < kFiniteBuckets; ++i) {
      const double value = 100e-6 * std::pow(2.0, static_cast<double>(i) / 4.0);
      seconds.push_back(value);
      nanos.push_back(static_cast<std::uint64_t>(std::llround(value * 1e9)));
    }
  }
};

const Bounds& bounds() {
  static const Bounds instance;
  return instance;
}

std::size_t bucketFor(std::chrono::nanoseconds duration) noexcept {
  const auto ns = duration.count() < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(duration.count());
  const auto& nanos = bounds().nanos;
  return static_cast<std::size_t>(std::lower_bound(nanos.begin(), nanos.end(), ns) - nanos.begin());
}

}  // namespace

const std::vector<double>& latencyBounds() { return bounds().seconds; }
std::size_t latencyBucketCount() { return kFiniteBuckets + 1; }

// ------------------------------------------------------------------ snapshot, histogram

void HistogramSnapshot::add(const HistogramSnapshot& other) {
  if (counts.size() < other.counts.size()) counts.resize(other.counts.size(), 0);
  for (std::size_t i = 0; i < other.counts.size(); ++i) counts[i] += other.counts[i];
  count += other.count;
  sum_seconds += other.sum_seconds;
}

double HistogramSnapshot::quantile(double q) const {
  std::uint64_t total = 0;
  for (const auto c : counts) total += c;
  if (total == 0) return std::numeric_limits<double>::quiet_NaN();
  q = std::clamp(q, 0.0, 1.0);
  const double rank = q * static_cast<double>(total);
  const auto& upper_bounds = latencyBounds();
  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (counts[i] == 0) continue;
    const std::uint64_t before = cumulative;
    cumulative += counts[i];
    if (static_cast<double>(cumulative) < rank) continue;
    if (i >= upper_bounds.size()) return upper_bounds.back();  // beyond the last bound
    const double lower = i == 0 ? 0.0 : upper_bounds[i - 1];
    const double upper = upper_bounds[i];
    const double fraction = (rank - static_cast<double>(before)) / static_cast<double>(counts[i]);
    return lower + (upper - lower) * std::clamp(fraction, 0.0, 1.0);
  }
  return upper_bounds.back();
}

Histogram::Histogram() : buckets_(latencyBucketCount()) {}

void Histogram::record(std::chrono::nanoseconds duration) noexcept {
  buckets_[bucketFor(duration)].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_ns_.fetch_add(duration.count() < 0 ? 0 : static_cast<std::uint64_t>(duration.count()),
                    std::memory_order_relaxed);
}

HistogramSnapshot Histogram::snapshot() const {
  HistogramSnapshot out;
  out.counts.resize(buckets_.size());
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    out.counts[i] = buckets_[i].load(std::memory_order_relaxed);
    total += out.counts[i];
  }
  out.count = total;  // the buckets are the truth: count and buckets always agree
  out.sum_seconds = static_cast<double>(sum_ns_.load(std::memory_order_relaxed)) / 1e9;
  return out;
}

// ------------------------------------------------------------------ windowed stats

WindowedStats::WindowedStats(unsigned window_seconds, Clock clock)
    : window_(std::max(1U, window_seconds)),
      clock_(clock ? std::move(clock) : Clock{[] { return std::chrono::steady_clock::now(); }}),
      base_(clock_()) {
  slots_.reserve(window_ + 1);
  for (unsigned i = 0; i <= window_; ++i) slots_.push_back(std::make_unique<Slot>(latencyBucketCount()));
}

std::int64_t WindowedStats::nowSecond() const {
  return std::chrono::duration_cast<std::chrono::seconds>(clock_() - base_).count();
}

void WindowedStats::record(std::chrono::nanoseconds latency, bool error) noexcept {
  lifetime_.record(latency);
  total_.fetch_add(1, std::memory_order_relaxed);
  if (error) total_errors_.fetch_add(1, std::memory_order_relaxed);

  const std::int64_t now = nowSecond();
  Slot& slot = *slots_[static_cast<std::size_t>(now) % slots_.size()];
  std::int64_t epoch = slot.epoch.load(std::memory_order_acquire);
  if (epoch != now) {
    if (epoch > now) return;  // this thread is a whole window behind: its sample is history
    const std::lock_guard lock(rotate_mutex_);
    epoch = slot.epoch.load(std::memory_order_relaxed);
    if (epoch < now) {
      slot.samples.store(0, std::memory_order_relaxed);
      slot.errors.store(0, std::memory_order_relaxed);
      for (auto& bucket : slot.latency) bucket.store(0, std::memory_order_relaxed);
      slot.epoch.store(now, std::memory_order_release);
    } else if (epoch > now) {
      return;
    }
  }
  slot.samples.fetch_add(1, std::memory_order_relaxed);
  if (error) slot.errors.fetch_add(1, std::memory_order_relaxed);
  slot.latency[bucketFor(latency)].fetch_add(1, std::memory_order_relaxed);
}

WindowSummary WindowedStats::window() const {
  WindowSummary out;
  out.latency.counts.assign(latencyBucketCount(), 0);
  const std::int64_t now = nowSecond();
  std::uint64_t completed_samples = 0;
  std::uint64_t current_samples = 0;
  for (const auto& slot_ptr : slots_) {
    const Slot& slot = *slot_ptr;
    const std::int64_t epoch = slot.epoch.load(std::memory_order_acquire);
    if (epoch < 0 || epoch > now || epoch < now - static_cast<std::int64_t>(window_)) continue;
    const auto samples = slot.samples.load(std::memory_order_relaxed);
    out.samples += samples;
    out.errors += slot.errors.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < slot.latency.size(); ++i) {
      out.latency.counts[i] += slot.latency[i].load(std::memory_order_relaxed);
    }
    if (epoch == now) {
      current_samples = samples;
    } else {
      completed_samples += samples;
    }
  }
  out.latency.count = out.samples;
  const std::int64_t completed_seconds = std::min<std::int64_t>(window_, now);
  out.per_second = completed_seconds > 0 ? static_cast<double>(completed_samples) /
                                               static_cast<double>(completed_seconds)
                                         : static_cast<double>(current_samples);
  out.error_ratio = out.samples > 0 ? static_cast<double>(out.errors) / static_cast<double>(out.samples)
                                    : std::numeric_limits<double>::quiet_NaN();
  return out;
}

// ------------------------------------------------------------------ exposition format

std::string MetricsWriter::formatDouble(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "+Inf" : "-Inf";
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.9g", value);
  return buffer;
}

std::string MetricsWriter::escapeLabelValue(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void MetricsWriter::header(std::string_view name, std::string_view help, std::string_view type) {
  out_ += "# HELP ";
  out_ += name;
  out_ += ' ';
  for (const char c : help) {
    if (c == '\\') {
      out_ += "\\\\";
    } else if (c == '\n') {
      out_ += "\\n";
    } else {
      out_.push_back(c);
    }
  }
  out_ += "\n# TYPE ";
  out_ += name;
  out_ += ' ';
  out_ += type;
  out_ += '\n';
}

void MetricsWriter::series(std::string_view name, const Labels& labels, std::string_view value) {
  out_ += name;
  if (!labels.empty()) {
    out_ += '{';
    bool first = true;
    for (const auto& [label, label_value] : labels) {
      if (!first) out_ += ',';
      first = false;
      out_ += label;
      out_ += "=\"";
      out_ += escapeLabelValue(label_value);
      out_ += '"';
    }
    out_ += '}';
  }
  out_ += ' ';
  out_ += value;
  out_ += '\n';
}

void MetricsWriter::counter(std::string_view name, std::string_view help, std::uint64_t value) {
  header(name, help, "counter");
  series(name, {}, std::to_string(value));
}

void MetricsWriter::counter(std::string_view name, std::string_view help,
                            const std::vector<std::pair<Labels, std::uint64_t>>& values) {
  header(name, help, "counter");
  for (const auto& [labels, value] : values) series(name, labels, std::to_string(value));
}

void MetricsWriter::gauge(std::string_view name, std::string_view help, double value) {
  header(name, help, "gauge");
  series(name, {}, formatDouble(value));
}

void MetricsWriter::gauge(std::string_view name, std::string_view help,
                          const std::vector<std::pair<Labels, double>>& values) {
  header(name, help, "gauge");
  for (const auto& [labels, value] : values) series(name, labels, formatDouble(value));
}

void MetricsWriter::histogram(std::string_view name, std::string_view help,
                              const HistogramSnapshot& snapshot, std::size_t step) {
  step = std::max<std::size_t>(1, step);
  header(name, help, "histogram");
  const auto& upper_bounds = latencyBounds();
  const std::string bucket_name = std::string{name} + "_bucket";
  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < upper_bounds.size(); ++i) {
    if (i < snapshot.counts.size()) cumulative += snapshot.counts[i];
    if ((i + 1) % step == 0) {
      series(bucket_name, {{"le", formatDouble(upper_bounds[i])}}, std::to_string(cumulative));
    }
  }
  std::uint64_t total = cumulative;
  if (snapshot.counts.size() > upper_bounds.size()) total += snapshot.counts[upper_bounds.size()];
  series(bucket_name, {{"le", "+Inf"}}, std::to_string(total));
  series(std::string{name} + "_sum", {}, formatDouble(snapshot.sum_seconds));
  series(std::string{name} + "_count", {}, std::to_string(total));
}

// ------------------------------------------------------------------ the process metrics

Metrics::Metrics(config::MetricsConfig config, Clock clock)
    : config_(std::move(config)),
      clock_(clock ? clock : Clock{[] { return std::chrono::steady_clock::now(); }}),
      started_(clock_()),
      requests_(config_.window_seconds, clock_),
      backend_(config_.window_seconds, clock_) {}

void Metrics::recordRequest(std::chrono::nanoseconds latency, unsigned status, bool proxied) noexcept {
  const std::size_t index = status >= 200 && status < 300   ? 0
                            : status >= 300 && status < 400 ? 1
                            : status >= 400 && status < 500 ? 2
                            : status >= 500 && status < 600 ? 3
                                                            : 4;
  responses_[index].fetch_add(1, std::memory_order_relaxed);
  (proxied ? proxied_ : local_).fetch_add(1, std::memory_order_relaxed);
  requests_.record(latency, status >= 500);
}

std::string Metrics::render() const {
  MetricsWriter w;
  const double uptime = std::chrono::duration<double>(clock_() - started_).count();
  w.gauge("edgeflow_build_info", "Build information (the value is always 1).",
          {{Labels{{"version", std::string{EDGEFLOW_VERSION}}}, 1.0}});
  w.gauge("edgeflow_uptime_seconds", "Seconds since the metrics were created (process start).", uptime);

  const auto window = requests_.window();
  w.counter("edgeflow_http_requests_total", "Client-facing requests completed, by kind (proxy = /proxy/..., local = everything else except /metrics).",
            {{{{"kind", "proxy"}}, proxied_.load(std::memory_order_relaxed)},
             {{{"kind", "local"}}, local_.load(std::memory_order_relaxed)}});
  w.counter("edgeflow_http_responses_total", "Client-facing responses by status class.",
            {{{{"class", "2xx"}}, responses_[0].load(std::memory_order_relaxed)},
             {{{"class", "3xx"}}, responses_[1].load(std::memory_order_relaxed)},
             {{{"class", "4xx"}}, responses_[2].load(std::memory_order_relaxed)},
             {{{"class", "5xx"}}, responses_[3].load(std::memory_order_relaxed)},
             {{{"class", "other"}}, responses_[4].load(std::memory_order_relaxed)}});
  w.counter("edgeflow_http_errors_total", "Client-facing responses with a 5xx status.", requests_.totalErrors());
  w.gauge("edgeflow_http_in_flight_requests", "Requests being handled right now.",
          static_cast<double>(std::max<std::int64_t>(0, in_flight_.load(std::memory_order_relaxed))));
  w.histogram("edgeflow_http_request_duration_seconds",
              "Time from the request reaching the handler to its response being ready, since start.",
              requests_.lifetimeLatency());
  w.gauge("edgeflow_http_window_seconds", "Length of the sliding window of the gauges below.",
          static_cast<double>(requests_.windowSeconds()));
  w.gauge("edgeflow_http_requests_per_second", "Requests completed per second over the completed seconds of the window.",
          window.per_second);
  w.gauge("edgeflow_http_error_ratio", "Share of 5xx responses among the requests of the window (NaN without requests).",
          window.error_ratio);
  w.gauge("edgeflow_http_request_latency_seconds", "Request latency quantiles over the window (NaN without requests).",
          {{{{"quantile", "0.5"}}, window.latency.quantile(0.5)},
           {{{"quantile", "0.95"}}, window.latency.quantile(0.95)},
           {{{"quantile", "0.99"}}, window.latency.quantile(0.99)}});

  const auto backend = backend_.window();
  w.histogram("edgeflow_backend_latency_seconds",
              "Time of one exchange with a backend that answered (any status), since start.",
              backend_.lifetimeLatency());
  w.gauge("edgeflow_backend_latency_window_seconds", "Backend latency quantiles over the window (NaN without answers).",
          {{{{"quantile", "0.5"}}, backend.latency.quantile(0.5)},
           {{{"quantile", "0.95"}}, backend.latency.quantile(0.95)},
           {{{"quantile", "0.99"}}, backend.latency.quantile(0.99)}});
  w.gauge("edgeflow_backend_answers_per_second", "Backend answers per second over the completed seconds of the window.",
          backend.per_second);

  for (const auto& collector : collectors_) collector(w);
  return w.str();
}

}  // namespace edgeflow::observability
