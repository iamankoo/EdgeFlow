#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/discovery/NameResolver.hpp"
#include "support/HealthTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using edgeflow::discovery::NameResolver;
using edgeflow::discovery::ProbeResult;
using edgeflow::discovery::ProbeTarget;
using edgeflow::testing::RawServer;
using edgeflow::testing::waitFor;
namespace net = boost::asio;
using tcp = net::ip::tcp;

NameResolver::Results loopback(const std::string& host, const std::string& port) {
  return NameResolver::Results::create(
      tcp::endpoint{net::ip::make_address("127.0.0.1"), static_cast<std::uint16_t>(std::stoi(port))}, host, port);
}

// A lookup function where chosen hosts block until released, like a hung DNS query.
class ScriptedDns {
 public:
  NameResolver::LookupFunction function() {
    return [this](const std::string& host, const std::string& port, boost::system::error_code& error) {
      ++calls_;
      {
        std::unique_lock lock(mutex_);
        ++blocked_now_;
        released_cv_.wait(lock, [&] { return !blocked_hosts_.count(host) || released_all_; });
        --blocked_now_;
      }
      if (host == "throws.test") throw std::runtime_error("resolver crashed");
      if (host == "missing.test") {
        error = net::error::make_error_code(net::error::host_not_found);
        return NameResolver::Results{};
      }
      return loopback(host, port);
    };
  }
  void block(const std::string& host) {
    const std::lock_guard lock(mutex_);
    blocked_hosts_.insert(host);
  }
  void release(const std::string& host) {
    {
      const std::lock_guard lock(mutex_);
      blocked_hosts_.erase(host);
    }
    released_cv_.notify_all();
  }
  void releaseAll() {
    {
      const std::lock_guard lock(mutex_);
      released_all_ = true;
    }
    released_cv_.notify_all();
  }
  int calls() const { return calls_.load(); }
  int blockedNow() {
    const std::lock_guard lock(mutex_);
    return blocked_now_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable released_cv_;
  std::set<std::string> blocked_hosts_;
  bool released_all_{false};
  int blocked_now_{0};
  std::atomic<int> calls_{0};
};

// Runs the io_context on a helper thread so handlers can be awaited.
class LoopThread {
 public:
  LoopThread() : work_(net::make_work_guard(io)), thread_([this] { io.run(); }) {}
  ~LoopThread() {
    work_.reset();
    io.stop();
    thread_.join();
  }
  net::io_context io;

 private:
  net::executor_work_guard<net::io_context::executor_type> work_;
  std::thread thread_;
};

TEST(NameResolverTest, ResolvesWithTheRealResolver) {
  auto resolver = std::make_shared<NameResolver>();
  LoopThread loop;
  std::promise<boost::system::error_code> done;
  std::atomic<std::size_t> endpoints{0};
  net::post(loop.io, [&] {
    (void)resolver->lookup(loop.io, "localhost", "80", [&](const boost::system::error_code& error,
                                                           const NameResolver::Results& results) {
      endpoints = results.size();
      done.set_value(error);
    });
  });
  EXPECT_FALSE(done.get_future().get());
  EXPECT_GE(endpoints.load(), 1U);
}

TEST(NameResolverTest, ASlowHostDoesNotDelayAnotherHost) {
  ScriptedDns dns;
  dns.block("slow.test");
  auto resolver = std::make_shared<NameResolver>(dns.function());
  LoopThread loop;

  std::atomic<bool> slow_done{false};
  std::atomic<bool> fast_done{false};
  net::post(loop.io, [&] {
    (void)resolver->lookup(loop.io, "slow.test", "80", [&](auto&&, auto&&) { slow_done = true; });
  });
  ASSERT_TRUE(waitFor([&] { return dns.blockedNow() == 1; })) << "the slow lookup is stuck";
  net::post(loop.io, [&] {
    (void)resolver->lookup(loop.io, "fast.test", "80", [&](auto&&, auto&&) { fast_done = true; });
  });
  EXPECT_TRUE(waitFor([&] { return fast_done.load(); }, 2s)) << "a hung lookup must not hold up others";
  EXPECT_FALSE(slow_done.load());

  dns.release("slow.test");
  EXPECT_TRUE(waitFor([&] { return slow_done.load(); }));
}

TEST(NameResolverTest, ConcurrentLookupsOfOneHostShareASingleLookup) {
  ScriptedDns dns;
  dns.block("shared.test");
  auto resolver = std::make_shared<NameResolver>(dns.function());
  LoopThread loop;
  std::atomic<int> delivered{0};
  net::post(loop.io, [&] {
    for (int i = 0; i < 5; ++i) {
      (void)resolver->lookup(loop.io, "shared.test", "80", [&](const boost::system::error_code& e, auto&&) {
        if (!e) ++delivered;
      });
    }
  });
  ASSERT_TRUE(waitFor([&] { return dns.blockedNow() == 1; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(dns.calls(), 1) << "five waiters, one lookup, one thread";
  EXPECT_EQ(resolver->activeLookups(), 1U);
  dns.release("shared.test");
  EXPECT_TRUE(waitFor([&] { return delivered.load() == 5; }));
  EXPECT_TRUE(waitFor([&] { return resolver->activeLookups() == 0; }));
}

TEST(NameResolverTest, ACancelledLookupIsNeverDelivered) {
  ScriptedDns dns;
  dns.block("cancel.test");
  auto resolver = std::make_shared<NameResolver>(dns.function());
  std::atomic<bool> delivered{false};
  {
    LoopThread loop;
    std::promise<NameResolver::Ticket> ticket;
    net::post(loop.io, [&] {
      ticket.set_value(resolver->lookup(loop.io, "cancel.test", "80", [&](auto&&, auto&&) { delivered = true; }));
    });
    const auto id = ticket.get_future().get();
    ASSERT_TRUE(waitFor([&] { return dns.blockedNow() == 1; }));
    resolver->cancel(id);
  }  // the io_context is destroyed while the lookup is still blocked
  dns.release("cancel.test");  // the lookup now finishes: it must not touch the dead io_context
  ASSERT_TRUE(waitFor([&] { return resolver->activeLookups() == 0; }));
  EXPECT_FALSE(delivered.load());
  resolver->cancel(12345);  // unknown ticket: harmless
}

TEST(NameResolverTest, TooManySimultaneousLookupsFailFastInsteadOfQueueing) {
  ScriptedDns dns;
  auto resolver = std::make_shared<NameResolver>(dns.function());
  std::vector<std::string> hosts;
  for (unsigned i = 0; i < NameResolver::kMaxConcurrentLookups; ++i) {
    hosts.push_back("stuck" + std::to_string(i) + ".test");
    dns.block(hosts.back());
  }
  LoopThread loop;
  net::post(loop.io, [&] {
    for (const auto& host : hosts) (void)resolver->lookup(loop.io, host, "80", [](auto&&, auto&&) {});
  });
  ASSERT_TRUE(waitFor([&] { return dns.blockedNow() == static_cast<int>(NameResolver::kMaxConcurrentLookups); }));

  std::promise<boost::system::error_code> overflow;
  net::post(loop.io, [&] {
    (void)resolver->lookup(loop.io, "one-too-many.test", "80", [&](const boost::system::error_code& e, auto&&) {
      overflow.set_value(e);
    });
  });
  auto result = overflow.get_future();
  ASSERT_EQ(result.wait_for(2s), std::future_status::ready) << "must not wait behind the stuck lookups";
  EXPECT_EQ(result.get(), net::error::try_again);
  dns.releaseAll();
  EXPECT_TRUE(waitFor([&] { return resolver->activeLookups() == 0; }));
}

TEST(NameResolverTest, FailuresAndExceptionsAreDeliveredAsErrors) {
  ScriptedDns dns;
  auto resolver = std::make_shared<NameResolver>(dns.function());
  LoopThread loop;
  for (const char* host : {"missing.test", "throws.test"}) {
    std::promise<boost::system::error_code> done;
    net::post(loop.io, [&] {
      (void)resolver->lookup(loop.io, host, "80", [&](const boost::system::error_code& e, auto&&) {
        done.set_value(e);
      });
    });
    EXPECT_TRUE(done.get_future().get()) << host;
  }
}

// --- the regression: one hung name must not make OTHER instances fail their probes ---------

TEST(ProberIsolationTest, AHungDnsLookupDoesNotFailAnotherHostsProbe) {
  RawServer healthy_backend(RawServer::Mode::Close);
  ScriptedDns dns;
  dns.block("deleted-container.test");  // like the DNS name of a container that was removed
  auto names = std::make_shared<NameResolver>(dns.function());
  auto prober = edgeflow::discovery::makeProber(edgeflow::config::HealthCheckType::Tcp, names);

  net::io_context io;
  ProbeResult slow_result;
  ProbeResult healthy_result;
  bool slow_done = false;
  bool healthy_done = false;
  ProbeTarget slow;
  slow.host = "deleted-container.test";
  slow.port = healthy_backend.port();
  slow.timeout = 600ms;
  ProbeTarget good;
  good.host = "healthy-backend.test";  // a name, so it goes through the resolver too
  good.port = healthy_backend.port();
  good.timeout = 600ms;

  auto first = prober->start(io, slow, [&](ProbeResult r) { slow_result = r; slow_done = true; });
  auto second = prober->start(io, good, [&](ProbeResult r) { healthy_result = r; healthy_done = true; });
  const auto begin = std::chrono::steady_clock::now();
  io.run();
  EXPECT_TRUE(slow_done);
  EXPECT_FALSE(slow_result.healthy);
  EXPECT_EQ(slow_result.detail, "timed out after 600ms");
  EXPECT_TRUE(healthy_done);
  EXPECT_TRUE(healthy_result.healthy) << "the healthy backend must stay healthy: " << healthy_result.detail;
  EXPECT_GE(std::chrono::steady_clock::now() - begin, 550ms);
  dns.releaseAll();
  EXPECT_TRUE(waitFor([&] { return names->activeLookups() == 0; }));
}

TEST(ProberIsolationTest, AProbeThatTimedOutDuringDnsNeverCallsBackLater) {
  ScriptedDns dns;
  dns.block("slow.test");
  auto names = std::make_shared<NameResolver>(dns.function());
  auto prober = edgeflow::discovery::makeProber(edgeflow::config::HealthCheckType::Tcp, names);
  int callbacks = 0;
  {
    net::io_context io;
    ProbeTarget slow;
    slow.host = "slow.test";
    slow.port = 9;
    slow.timeout = 100ms;
    auto handle = prober->start(io, slow, [&](ProbeResult) { ++callbacks; });
    io.run();  // ends when the probe times out
    EXPECT_EQ(callbacks, 1);
  }  // io_context destroyed while the lookup thread is still blocked
  dns.release("slow.test");
  ASSERT_TRUE(waitFor([&] { return names->activeLookups() == 0; }));
  EXPECT_EQ(callbacks, 1) << "the late lookup result was discarded";
}

}  // namespace
