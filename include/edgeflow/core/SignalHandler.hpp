#pragma once

#include <string_view>

namespace edgeflow::core {

// RAII installer for SIGINT/SIGTERM handlers. The handler only stores the signal
// number in a lock-free atomic (async-signal-safe); the application loop polls it.
// Only one instance can be installed at a time. Previous handlers are restored on
// destruction.
class SignalHandler {
 public:
  SignalHandler();
  ~SignalHandler();

  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;
  SignalHandler(SignalHandler&&) = delete;
  SignalHandler& operator=(SignalHandler&&) = delete;

  [[nodiscard]] bool installed() const noexcept { return installed_; }

  // 0 when no shutdown signal has been received since installation.
  [[nodiscard]] static int receivedSignal() noexcept;
  [[nodiscard]] static std::string_view signalName(int signal) noexcept;

 private:
  bool installed_{false};
  using Handler = void (*)(int);
  Handler previous_int_{nullptr};
  Handler previous_term_{nullptr};
};

}  // namespace edgeflow::core
