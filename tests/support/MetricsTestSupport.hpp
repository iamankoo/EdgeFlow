#pragma once

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace edgeflow::testing {

// One sample of a Prometheus text exposition.
struct MetricSample {
  std::string name;
  std::map<std::string, std::string> labels;
  double value{0};
};

// A strict-enough parser of the Prometheus text format (version 0.0.4) for tests: it checks the
// structure the real scrapers rely on and reports the first violation in `error`.
//   - every sample line is `name[{label="value",...}] value`
//   - every sample belongs to a family announced by a preceding `# TYPE`
//   - no family is announced twice
class Exposition {
 public:
  explicit Exposition(const std::string& text) { parse(text); }

  [[nodiscard]] bool valid() const { return error_.empty(); }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] const std::vector<MetricSample>& samples() const { return samples_; }
  [[nodiscard]] const std::map<std::string, std::string>& types() const { return types_; }

  [[nodiscard]] bool has(const std::string& name) const { return types_.count(name) != 0; }

  // The value of the sample `name` whose labels include all of `labels`; nullopt when absent.
  [[nodiscard]] std::optional<double> value(const std::string& name,
                                            const std::map<std::string, std::string>& labels = {}) const {
    for (const auto& sample : samples_) {
      if (sample.name != name) continue;
      bool match = true;
      for (const auto& [key, wanted] : labels) {
        const auto it = sample.labels.find(key);
        if (it == sample.labels.end() || it->second != wanted) match = false;
      }
      if (match) return sample.value;
    }
    return std::nullopt;
  }

  // value() that fails loudly in a test: 0 would hide a missing metric.
  [[nodiscard]] double must(const std::string& name, const std::map<std::string, std::string>& labels = {}) const {
    const auto v = value(name, labels);
    if (!v) {
      ADD_FAILURE() << "metric not found: " << name;
      return std::nan("");
    }
    return *v;
  }

 private:
  static bool validName(const std::string& name) {
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
      const char c = name[i];
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
                      (i > 0 && c >= '0' && c <= '9');
      if (!ok) return false;
    }
    return true;
  }

  void fail(const std::string& message) {
    if (error_.empty()) error_ = message;
  }

  // Family of a sample name: histogram series carry suffixes.
  [[nodiscard]] std::string familyOf(const std::string& name) const {
    if (types_.count(name) != 0) return name;
    for (const char* suffix : {"_bucket", "_sum", "_count"}) {
      const std::string s = suffix;
      if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
        const std::string base = name.substr(0, name.size() - s.size());
        const auto it = types_.find(base);
        if (it != types_.end() && it->second == "histogram") return base;
      }
    }
    return {};
  }

  void parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      if (line.rfind("# HELP ", 0) == 0) continue;
      if (line.rfind("# TYPE ", 0) == 0) {
        std::istringstream words(line.substr(7));
        std::string name;
        std::string type;
        words >> name >> type;
        if (!validName(name)) fail("bad family name in: " + line);
        if (type != "counter" && type != "gauge" && type != "histogram" && type != "summary" && type != "untyped") {
          fail("bad type in: " + line);
        }
        if (types_.count(name) != 0) fail("family announced twice: " + name);
        types_[name] = type;
        continue;
      }
      if (line[0] == '#') continue;
      MetricSample sample;
      std::size_t pos = 0;
      while (pos < line.size() && line[pos] != '{' && line[pos] != ' ') ++pos;
      sample.name = line.substr(0, pos);
      if (!validName(sample.name)) fail("bad sample name in: " + line);
      if (pos < line.size() && line[pos] == '{') {
        ++pos;
        while (pos < line.size() && line[pos] != '}') {
          std::size_t eq = line.find('=', pos);
          if (eq == std::string::npos || eq + 1 >= line.size() || line[eq + 1] != '"') {
            fail("bad label in: " + line);
            return;
          }
          const std::string key = line.substr(pos, eq - pos);
          std::string value;
          std::size_t i = eq + 2;
          bool closed = false;
          for (; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
              const char next = line[i + 1];
              value.push_back(next == 'n' ? '\n' : next);
              ++i;
            } else if (line[i] == '"') {
              closed = true;
              break;
            } else {
              value.push_back(line[i]);
            }
          }
          if (!closed) {
            fail("unterminated label value in: " + line);
            return;
          }
          sample.labels[key] = value;
          pos = i + 1;
          if (pos < line.size() && line[pos] == ',') ++pos;
        }
        if (pos >= line.size()) {
          fail("unterminated labels in: " + line);
          return;
        }
        ++pos;  // the closing brace
      }
      if (pos >= line.size() || line[pos] != ' ') {
        fail("no value in: " + line);
        return;
      }
      const std::string number = line.substr(pos + 1);
      if (number == "NaN") {
        sample.value = std::nan("");
      } else if (number == "+Inf") {
        sample.value = HUGE_VAL;
      } else if (number == "-Inf") {
        sample.value = -HUGE_VAL;
      } else {
        char* end = nullptr;
        sample.value = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0') fail("bad number in: " + line);
      }
      if (familyOf(sample.name).empty()) fail("sample without a # TYPE line: " + sample.name);
      samples_.push_back(std::move(sample));
    }
  }

  std::vector<MetricSample> samples_;
  std::map<std::string, std::string> types_;
  std::string error_;
};

}  // namespace edgeflow::testing
