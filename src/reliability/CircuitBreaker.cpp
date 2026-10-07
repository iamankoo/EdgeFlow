#include "edgeflow/reliability/CircuitBreaker.hpp"

#include <algorithm>
#include <utility>

namespace edgeflow::reliability {

const char* toString(CircuitState state) noexcept {
  switch (state) {
    case CircuitState::Closed: return "closed";
    case CircuitState::Open: return "open";
    case CircuitState::HalfOpen: return "half-open";
  }
  return "unknown";
}

CircuitBreaker::CircuitBreaker(CircuitBreakerSettings settings, SteadyClock clock, Listener listener)
    : settings_(settings), clock_(std::move(clock)), listener_(std::move(listener)) {}

void CircuitBreaker::moveTo(CircuitState next, Transition& transition) {
  transition.happened = true;
  transition.from = state_;
  transition.to = next;
  state_ = next;
  ++epoch_;  // tickets issued before this moment are now stale
  probes_in_flight_ = 0;
  probe_successes_ = 0;
  if (next == CircuitState::Open) opened_at_ = clock_();
  if (next == CircuitState::Closed) consecutive_failures_ = 0;
}

void CircuitBreaker::notify(const Transition& transition) const {
  if (transition.happened && listener_) listener_(transition.from, transition.to);
}

Admission CircuitBreaker::tryAcquire() {
  Transition transition;
  Admission admission;
  {
    const std::lock_guard lock(mutex_);
    if (state_ == CircuitState::Open && clock_() - opened_at_ >= settings_.recovery_timeout) {
      moveTo(CircuitState::HalfOpen, transition);
    }
    switch (state_) {
      case CircuitState::Closed:
        admission = {true, false, epoch_};
        break;
      case CircuitState::Open:
        break;
      case CircuitState::HalfOpen:
        if (probes_in_flight_ + probe_successes_ < settings_.half_open_max_requests) {
          ++probes_in_flight_;
          admission = {true, true, epoch_};
        }
        break;
    }
  }
  notify(transition);
  return admission;
}

bool CircuitBreaker::isAvailable() const {
  const std::lock_guard lock(mutex_);
  switch (state_) {
    case CircuitState::Closed: return true;
    case CircuitState::Open: return clock_() - opened_at_ >= settings_.recovery_timeout;
    case CircuitState::HalfOpen:
      return probes_in_flight_ + probe_successes_ < settings_.half_open_max_requests;
  }
  return false;
}

void CircuitBreaker::recordSuccess(const Admission& admission) {
  if (!admission.admitted) return;
  Transition transition;
  {
    const std::lock_guard lock(mutex_);
    if (admission.epoch != epoch_) return;
    if (state_ == CircuitState::Closed) {
      consecutive_failures_ = 0;
    } else if (state_ == CircuitState::HalfOpen && admission.probe) {
      if (probes_in_flight_ > 0) --probes_in_flight_;
      ++probe_successes_;
      if (probe_successes_ >= settings_.half_open_max_requests) {
        moveTo(CircuitState::Closed, transition);
      }
    }
  }
  notify(transition);
}

void CircuitBreaker::recordFailure(const Admission& admission) {
  if (!admission.admitted) return;
  Transition transition;
  {
    const std::lock_guard lock(mutex_);
    if (admission.epoch != epoch_) return;
    if (state_ == CircuitState::Closed) {
      ++consecutive_failures_;
      if (consecutive_failures_ >= settings_.failure_threshold) {
        moveTo(CircuitState::Open, transition);
      }
    } else if (state_ == CircuitState::HalfOpen && admission.probe) {
      moveTo(CircuitState::Open, transition);
    }
  }
  notify(transition);
}

void CircuitBreaker::release(const Admission& admission) {
  if (!admission.admitted || !admission.probe) return;
  const std::lock_guard lock(mutex_);
  if (admission.epoch == epoch_ && state_ == CircuitState::HalfOpen && probes_in_flight_ > 0) {
    --probes_in_flight_;
  }
}

CircuitState CircuitBreaker::state() const {
  const std::lock_guard lock(mutex_);
  return state_;
}

unsigned CircuitBreaker::consecutiveFailures() const {
  const std::lock_guard lock(mutex_);
  return consecutive_failures_;
}

CircuitBreakerRegistry::CircuitBreakerRegistry(CircuitBreakerSettings settings, SteadyClock clock,
                                               Listener listener, std::size_t max_entries)
    : settings_(settings),
      clock_(std::move(clock)),
      listener_(std::move(listener)),
      max_entries_(std::max<std::size_t>(1, max_entries)) {}

std::shared_ptr<CircuitBreaker> CircuitBreakerRegistry::get(const std::string& key) {
  const std::lock_guard lock(mutex_);
  if (const auto found = breakers_.find(key); found != breakers_.end()) return found->second;

  if (breakers_.size() >= max_entries_) {
    // Soft cap: forget breakers that carry no information (closed, no failures streak).
    // Open or recovering circuits are never forgotten, so the cap can be exceeded.
    for (auto it = breakers_.begin(); it != breakers_.end();) {
      if (it->second->state() == CircuitState::Closed && it->second->consecutiveFailures() == 0) {
        it = breakers_.erase(it);
      } else {
        ++it;
      }
    }
  }

  CircuitBreaker::Listener forward;
  if (listener_) {
    forward = [listener = listener_, key](CircuitState from, CircuitState to) {
      listener(key, from, to);
    };
  }
  auto breaker = std::make_shared<CircuitBreaker>(settings_, clock_, std::move(forward));
  breakers_.emplace(key, breaker);
  return breaker;
}

std::shared_ptr<CircuitBreaker> CircuitBreakerRegistry::find(const std::string& key) const {
  const std::lock_guard lock(mutex_);
  const auto found = breakers_.find(key);
  return found == breakers_.end() ? nullptr : found->second;
}

std::size_t CircuitBreakerRegistry::size() const {
  const std::lock_guard lock(mutex_);
  return breakers_.size();
}

}  // namespace edgeflow::reliability
