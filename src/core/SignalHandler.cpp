#include "edgeflow/core/SignalHandler.hpp"

#include <atomic>
#include <csignal>

namespace edgeflow::core {

namespace {

// A signal handler cannot carry state, so the received signal lives in a
// translation-unit-local atomic. It is the only global state in the project.
std::atomic<int> g_received_signal{0};
std::atomic<bool> g_installed{false};
static_assert(std::atomic<int>::is_always_lock_free,
              "signal handler state must be lock-free");

extern "C" void onShutdownSignal(int signal) { g_received_signal.store(signal); }

}  // namespace

SignalHandler::SignalHandler() {
  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true)) return;  // already installed

  g_received_signal.store(0);
  previous_int_ = std::signal(SIGINT, onShutdownSignal);
  previous_term_ = std::signal(SIGTERM, onShutdownSignal);
  if (previous_int_ == SIG_ERR || previous_term_ == SIG_ERR) {
    if (previous_int_ != SIG_ERR) std::signal(SIGINT, previous_int_);
    if (previous_term_ != SIG_ERR) std::signal(SIGTERM, previous_term_);
    g_installed.store(false);
    return;
  }
  installed_ = true;
}

SignalHandler::~SignalHandler() {
  if (!installed_) return;
  std::signal(SIGINT, previous_int_);
  std::signal(SIGTERM, previous_term_);
  g_installed.store(false);
}

int SignalHandler::receivedSignal() noexcept { return g_received_signal.load(); }

std::string_view SignalHandler::signalName(int signal) noexcept {
  switch (signal) {
    case SIGINT: return "SIGINT";
    case SIGTERM: return "SIGTERM";
    default: return "unknown signal";
  }
}

}  // namespace edgeflow::core
