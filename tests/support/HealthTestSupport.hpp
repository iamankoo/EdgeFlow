#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include <sys/socket.h>

#include "edgeflow/discovery/Prober.hpp"
#include "support/NetTestSupport.hpp"

namespace edgeflow::testing {

// A real backend on loopback that behaves as scripted, for health-check tests. It can be
// stopped (connections are then refused) and restarted on the SAME port, which is how
// "backend goes away" and "backend recovers" are exercised over real sockets.
class RawServer {
 public:
  enum class Mode {
    Close,            // accept, then close at once (enough for a TCP probe)
    Http,             // read the request, answer with `status`
    Garbage,          // read the request, answer with bytes that are not HTTP
    Stall,            // read the request, then never answer
    PartialThenStall, // send half a status line, then stall
    HangUp            // read the request, then close without answering
  };

  explicit RawServer(Mode mode = Mode::Http, int status = 200) : mode_(mode), status_(status) {
    bind(0);
  }
  ~RawServer() { stop(); }

  RawServer(const RawServer&) = delete;
  RawServer& operator=(const RawServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  void setMode(Mode mode) { mode_.store(mode); }
  void setStatus(int status) { status_.store(status); }
  [[nodiscard]] int connections() const { return connections_.load(); }
  [[nodiscard]] int requests() const { return requests_.load(); }
  [[nodiscard]] std::string lastRequest() {
    const std::lock_guard lock(mutex_);
    return last_request_;
  }
  [[nodiscard]] bool running() const { return running_.load(); }

  // Closes the listener: new connections are refused until restart().
  void stop() {
    if (!running_.exchange(false)) return;
    wakeBlockedThread();
    if (thread_.joinable()) thread_.join();
    boost::system::error_code ignored;
    acceptor_->close(ignored);
  }

  // Listens again on the same port.
  bool restart() {
    stop();
    return bind(port_);
  }

 private:
  bool bind(std::uint16_t port) {
    try {
      acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
          io_, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), port});
    } catch (const std::exception&) {
      return false;
    }
    port_ = acceptor_->local_endpoint().port();
    running_.store(true);
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
  }

  // Closing a listening socket does not wake a thread blocked in accept() on Linux, so
  // connect to ourselves; and shut down the connection being served, if any.
  void wakeBlockedThread() {
    {
      boost::asio::io_context io;
      boost::asio::ip::tcp::socket waker(io);
      boost::system::error_code ignored;
      waker.connect({boost::asio::ip::make_address("127.0.0.1"), port_}, ignored);
    }
    const int fd = active_fd_.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
  }

  void acceptLoop() {
    while (running_.load()) {
      boost::asio::ip::tcp::socket socket(io_);
      boost::system::error_code error;
      acceptor_->accept(socket, error);
      if (error || !running_.load()) return;  // acceptor closed, or the wake-up connection
      ++connections_;
      active_fd_.store(socket.native_handle());
      if (running_.load()) serve(socket);
      active_fd_.store(-1);
    }
  }

  void serve(boost::asio::ip::tcp::socket& socket) {
    const Mode mode = mode_.load();
    if (mode == Mode::Close) return;

    // Read the request head.
    std::string head;
    std::array<char, 1024> chunk{};
    boost::system::error_code error;
    while (head.find("\r\n\r\n") == std::string::npos) {
      const auto n = socket.read_some(boost::asio::buffer(chunk), error);
      if (error) return;
      head.append(chunk.data(), n);
      if (head.size() > 16384) return;
    }
    ++requests_;
    {
      const std::lock_guard lock(mutex_);
      last_request_ = head;
    }

    switch (mode) {
      case Mode::Http: {
        const int status = status_.load();
        const std::string response = "HTTP/1.1 " + std::to_string(status) + " Test\r\n" +
                                     "Content-Length: 0\r\nConnection: close\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(response), error);
        break;
      }
      case Mode::Garbage: {
        const std::string junk = "THIS IS NOT HTTP AT ALL\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(junk), error);
        break;
      }
      case Mode::PartialThenStall: {
        const std::string part = "HTTP/1.1 20";
        boost::asio::write(socket, boost::asio::buffer(part), error);
        waitForPeerClose(socket);
        break;
      }
      case Mode::Stall:
        waitForPeerClose(socket);
        break;
      case Mode::HangUp:
      case Mode::Close:
        break;
    }
  }

  // Blocks until the peer closes (which is what a probe timeout does) or the server stops.
  void waitForPeerClose(boost::asio::ip::tcp::socket& socket) {
    std::array<char, 64> scratch{};
    boost::system::error_code error;
    while (running_.load()) {
      socket.read_some(boost::asio::buffer(scratch), error);
      if (error) return;
    }
  }

  boost::asio::io_context io_;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::thread thread_;
  std::atomic<Mode> mode_;
  std::atomic<int> status_;
  std::atomic<bool> running_{false};
  std::atomic<int> connections_{0};
  std::atomic<int> requests_{0};
  std::atomic<int> active_fd_{-1};
  std::uint16_t port_{0};
  std::mutex mutex_;
  std::string last_request_;
};

// A port nothing listens on (bound once, then released).
inline std::uint16_t closedPort() {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor(
      io, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});
  return acceptor.local_endpoint().port();
}

// Runs one probe to completion on a private io_context.
inline discovery::ProbeResult probeOnce(discovery::Prober& prober, discovery::ProbeTarget target,
                                        std::chrono::milliseconds* elapsed = nullptr) {
  boost::asio::io_context io;
  discovery::ProbeResult result;
  bool done = false;
  const auto begin = std::chrono::steady_clock::now();
  auto handle = prober.start(io, std::move(target), [&](discovery::ProbeResult r) {
    result = std::move(r);
    done = true;
  });
  io.run();
  if (elapsed != nullptr) {
    *elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
  }
  if (!done) result.detail = "probe never completed";
  return result;
}

// A prober whose results are decided by the test, one probe at a time. Lets threshold and
// race tests run without any real network and without sleeps: the test sees each probe
// the moment it is started, and answers it when it chooses.
class ManualProber final : public discovery::Prober {
 public:
  struct Pending {
    discovery::ProbeTarget target;
    std::function<void(discovery::ProbeResult)> done;
    boost::asio::io_context* io{nullptr};
    std::shared_ptr<std::atomic<bool>> cancelled;
    bool answered{false};
  };

  std::shared_ptr<discovery::ProbeHandle> start(boost::asio::io_context& io,
                                                discovery::ProbeTarget target,
                                                std::function<void(discovery::ProbeResult)> done) override {
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
      const std::lock_guard lock(mutex_);
      pending_.push_back(Pending{std::move(target), std::move(done), &io, cancelled, false});
      ++started_;
      const auto open = openLocked();
      if (open > max_open_) max_open_ = open;
    }
    return std::make_shared<Handle>(cancelled);
  }

  // Number of probes started and not yet answered or cancelled.
  [[nodiscard]] int open() {
    const std::lock_guard lock(mutex_);
    return openLocked();
  }
  [[nodiscard]] int maxOpen() {
    const std::lock_guard lock(mutex_);
    return max_open_;
  }
  [[nodiscard]] int started() {
    const std::lock_guard lock(mutex_);
    return started_;
  }
  // Open probes for one port.
  [[nodiscard]] int openFor(std::uint16_t port) {
    const std::lock_guard lock(mutex_);
    int n = 0;
    for (const auto& p : pending_) {
      if (!p.answered && !p.cancelled->load() && p.target.port == port) ++n;
    }
    return n;
  }
  [[nodiscard]] int cancelledCount() {
    const std::lock_guard lock(mutex_);
    int n = 0;
    for (const auto& p : pending_) n += (!p.answered && p.cancelled->load()) ? 1 : 0;
    return n;
  }

  // Answers the oldest open probe for `port`. Returns false if there is none.
  bool answer(std::uint16_t port, bool healthy, const std::string& detail = "scripted") {
    std::function<void(discovery::ProbeResult)> done;
    boost::asio::io_context* io = nullptr;
    {
      const std::lock_guard lock(mutex_);
      for (auto& p : pending_) {
        if (p.answered || p.cancelled->load() || p.target.port != port) continue;
        p.answered = true;
        done = p.done;
        io = p.io;
        break;
      }
    }
    if (!done) return false;
    boost::asio::post(*io, [done, healthy, detail] { done(discovery::ProbeResult{healthy, detail}); });
    return true;
  }

  // Answers every open probe.
  void answerAll(bool healthy) {
    for (;;) {
      std::uint16_t port = 0;
      {
        const std::lock_guard lock(mutex_);
        bool found = false;
        for (const auto& p : pending_) {
          if (!p.answered && !p.cancelled->load()) {
            port = p.target.port;
            found = true;
            break;
          }
        }
        if (!found) return;
      }
      if (!answer(port, healthy)) return;
    }
  }

  // The target of the most recent probe started (for assertions on host/path/timeout).
  [[nodiscard]] discovery::ProbeTarget lastTarget() {
    const std::lock_guard lock(mutex_);
    return pending_.empty() ? discovery::ProbeTarget{} : pending_.back().target;
  }

 private:
  class Handle final : public discovery::ProbeHandle {
   public:
    explicit Handle(std::shared_ptr<std::atomic<bool>> cancelled) : cancelled_(std::move(cancelled)) {}
    void cancel() noexcept override { cancelled_->store(true); }

   private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
  };

  int openLocked() const {
    int n = 0;
    for (const auto& p : pending_) n += (!p.answered && !p.cancelled->load()) ? 1 : 0;
    return n;
  }

  std::mutex mutex_;
  std::vector<Pending> pending_;
  int started_{0};
  int max_open_{0};
};

}  // namespace edgeflow::testing
