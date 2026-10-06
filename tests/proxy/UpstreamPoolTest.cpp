#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "edgeflow/proxy/UpstreamPool.hpp"
#include "support/NetTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using edgeflow::proxy::UpstreamConnection;
using edgeflow::proxy::UpstreamEndpoint;
using edgeflow::proxy::UpstreamPool;
using edgeflow::testing::waitFor;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// A loopback listener that hands out connected socket pairs: the pool's side (an
// UpstreamConnection, as the proxy would have opened it) and the "backend" side.
class Loopback {
 private:
  // Declared first, destroyed last: the sockets in backend_side belong to this io_context.
  net::io_context io_;
  tcp::acceptor acceptor_;

 public:
  Loopback() : acceptor_(io_, tcp::endpoint{net::ip::make_address("127.0.0.1"), 0}) {}

  [[nodiscard]] UpstreamEndpoint endpoint() const {
    return UpstreamEndpoint{"127.0.0.1", acceptor_.local_endpoint().port()};
  }

  // Opens a connection; the accepted (backend-side) socket is kept in `backend_side`.
  std::unique_ptr<UpstreamConnection> open(net::io_context& pool_io) {
    auto connection = std::make_unique<UpstreamConnection>(pool_io, endpoint());
    connection->socket.connect(tcp::endpoint{net::ip::make_address("127.0.0.1"), endpoint().port});
    auto server = std::make_unique<tcp::socket>(io_);
    acceptor_.accept(*server);
    backend_side.push_back(std::move(server));
    return connection;
  }

  std::vector<std::unique_ptr<tcp::socket>> backend_side;
};

class UpstreamPoolTest : public ::testing::Test {
 protected:
  static UpstreamPool::Settings settings(std::size_t max_idle = 4,
                                         std::chrono::milliseconds ttl = 30s) {
    return UpstreamPool::Settings{max_idle, ttl};
  }

  net::io_context io;  // owns the pooled sockets; never run: the pool only peeks at them
  Loopback backend;
};

TEST_F(UpstreamPoolTest, CheckoutOfAnUnknownBackendFindsNothing) {
  UpstreamPool pool(settings());
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr);
  EXPECT_EQ(pool.idleCount(), 0U);
  EXPECT_EQ(pool.stats().reused, 0U);
}

TEST_F(UpstreamPoolTest, ACheckedInConnectionIsReusedAndNotShared) {
  UpstreamPool pool(settings());
  auto connection = backend.open(io);
  auto* raw = connection.get();
  pool.checkin(std::move(connection));
  EXPECT_EQ(pool.idleCount(), 1U);
  EXPECT_EQ(pool.idleCount(backend.endpoint()), 1U);

  auto again = pool.checkout(backend.endpoint());
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again.get(), raw) << "the very same connection comes back";
  EXPECT_EQ(pool.idleCount(), 0U) << "checked out: no longer in the pool";
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr) << "and it is not handed out twice";
  EXPECT_EQ(pool.stats().reused, 1U);
  EXPECT_EQ(pool.stats().returned, 1U);
}

TEST_F(UpstreamPoolTest, ConnectionsAreKeyedByHostAndPort) {
  UpstreamPool pool(settings());
  Loopback other;
  pool.checkin(backend.open(io));
  pool.checkin(other.open(io));
  EXPECT_EQ(pool.idleCount(), 2U);
  EXPECT_NE(pool.checkout(other.endpoint()), nullptr);
  EXPECT_EQ(pool.checkout(other.endpoint()), nullptr);
  UpstreamEndpoint elsewhere{"localhost", backend.endpoint().port};
  EXPECT_EQ(pool.checkout(elsewhere), nullptr) << "a different host string is a different backend";
  EXPECT_NE(pool.checkout(backend.endpoint()), nullptr);
}

TEST_F(UpstreamPoolTest, TheMostRecentlyUsedConnectionIsReusedFirst) {
  UpstreamPool pool(settings());
  auto first = backend.open(io);
  auto second = backend.open(io);
  auto* second_raw = second.get();
  pool.checkin(std::move(first));
  pool.checkin(std::move(second));
  auto got = pool.checkout(backend.endpoint());
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got.get(), second_raw);
}

TEST_F(UpstreamPoolTest, AtMostMaxIdleConnectionsAreKeptPerBackend) {
  UpstreamPool pool(settings(2));
  for (int i = 0; i < 5; ++i) pool.checkin(backend.open(io));
  EXPECT_EQ(pool.idleCount(backend.endpoint()), 2U);
  const auto stats = pool.stats();
  EXPECT_EQ(stats.returned, 2U);
  EXPECT_EQ(stats.discarded, 3U) << "the surplus was closed, not leaked";

  // The surplus really is closed: the backend sees end-of-stream on three connections.
  int closed = 0;
  for (auto& server : backend.backend_side) {
    char byte = 0;
    boost::system::error_code ec;
    server->non_blocking(true);
    server->receive(net::buffer(&byte, 1), 0, ec);
    closed += ec == net::error::eof ? 1 : 0;
  }
  EXPECT_EQ(closed, 3);
}

TEST_F(UpstreamPoolTest, ZeroMaxIdleDisablesPooling) {
  UpstreamPool pool(settings(0));
  pool.checkin(backend.open(io));
  EXPECT_EQ(pool.idleCount(), 0U);
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr);
  EXPECT_EQ(pool.stats().discarded, 1U);
}

TEST_F(UpstreamPoolTest, ConnectionsOlderThanTheIdleTimeoutAreNotReused) {
  auto now = std::make_shared<std::atomic<long long>>(0);
  const auto base = std::chrono::steady_clock::now();
  UpstreamPool pool(settings(4, 1000ms), [now, base] { return base + std::chrono::milliseconds{now->load()}; });

  pool.checkin(backend.open(io));
  now->store(999);
  auto fresh_enough = pool.checkout(backend.endpoint());
  ASSERT_NE(fresh_enough, nullptr) << "999ms < 1000ms";

  pool.checkin(std::move(fresh_enough));  // idle again from t=999
  now->store(999 + 1000);
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr) << "exactly the timeout old: expired";
  EXPECT_EQ(pool.stats().expired_dropped, 1U);
  EXPECT_EQ(pool.idleCount(), 0U);
}

TEST_F(UpstreamPoolTest, ExpiredConnectionsAreEvictedWhenAnyBackendIsUsed) {
  auto now = std::make_shared<std::atomic<long long>>(0);
  const auto base = std::chrono::steady_clock::now();
  UpstreamPool pool(settings(4, 500ms), [now, base] { return base + std::chrono::milliseconds{now->load()}; });
  Loopback other;
  pool.checkin(backend.open(io));
  pool.checkin(backend.open(io));
  EXPECT_EQ(pool.idleCount(), 2U);
  now->store(600);
  pool.checkin(other.open(io));  // use of the pool evicts the stale ones of ANY backend
  EXPECT_EQ(pool.idleCount(), 1U);
  EXPECT_EQ(pool.idleCount(other.endpoint()), 1U);

  now->store(2000);
  pool.evictExpired();
  EXPECT_EQ(pool.idleCount(), 0U);
  EXPECT_EQ(pool.stats().expired_dropped, 3U);
}

TEST_F(UpstreamPoolTest, ABackendThatClosedAnIdleConnectionIsDetectedAndSkipped) {
  UpstreamPool pool(settings());
  pool.checkin(backend.open(io));  // will die
  pool.checkin(backend.open(io));  // stays alive
  // The most recently returned one is tried first; kill that one (the second accepted).
  backend.backend_side[1]->shutdown(tcp::socket::shutdown_both);
  backend.backend_side[1]->close();
  // The FIN needs a moment to reach the pooled socket.
  std::this_thread::sleep_for(100ms);

  auto got = pool.checkout(backend.endpoint());
  ASSERT_NE(got, nullptr) << "the live connection is found after the dead one is dropped";
  EXPECT_TRUE(UpstreamPool::isUsable(got->socket));
  EXPECT_EQ(pool.stats().stale_dropped, 1U);
  EXPECT_EQ(pool.stats().reused, 1U);
  EXPECT_EQ(pool.idleCount(), 0U);
}

TEST_F(UpstreamPoolTest, AllDeadConnectionsMeanACheckoutMiss) {
  UpstreamPool pool(settings());
  pool.checkin(backend.open(io));
  pool.checkin(backend.open(io));
  for (auto& server : backend.backend_side) server->close();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr);
  EXPECT_EQ(pool.stats().stale_dropped, 2U);
  EXPECT_EQ(pool.stats().reused, 0U);
  EXPECT_EQ(pool.idleCount(), 0U);
}

TEST_F(UpstreamPoolTest, AResetConnectionIsDetected) {
  UpstreamPool pool(settings());
  pool.checkin(backend.open(io));
  boost::system::error_code ec;
  backend.backend_side[0]->set_option(net::socket_base::linger(true, 0), ec);
  backend.backend_side[0]->close();  // RST
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr);
  EXPECT_EQ(pool.stats().stale_dropped, 1U);
}

TEST_F(UpstreamPoolTest, UnsolicitedBytesMakeAConnectionUnusable) {
  UpstreamPool pool(settings());
  pool.checkin(backend.open(io));
  net::write(*backend.backend_side[0], net::buffer(std::string{"HTTP/1.1 200 stray response"}));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr)
      << "bytes that arrived with no request pending would be mistaken for the next response";
  EXPECT_EQ(pool.stats().stale_dropped, 1U);
}

TEST_F(UpstreamPoolTest, AHealthyIdleConnectionIsUsableAndNothingIsConsumed) {
  auto connection = backend.open(io);
  EXPECT_TRUE(UpstreamPool::isUsable(connection->socket));
  EXPECT_TRUE(UpstreamPool::isUsable(connection->socket)) << "checking twice changes nothing";
  net::write(*backend.backend_side[0], net::buffer(std::string{"x"}));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(UpstreamPool::isUsable(connection->socket));
  char byte = 0;
  boost::system::error_code ec;
  EXPECT_EQ(connection->socket.receive(net::buffer(&byte, 1), 0, ec), 1U) << "the byte was only peeked";
  connection->close();
  EXPECT_FALSE(UpstreamPool::isUsable(connection->socket)) << "a closed socket is not usable";
}

TEST_F(UpstreamPoolTest, DiscardClosesTheConnection) {
  UpstreamPool pool(settings());
  pool.discard(backend.open(io));
  EXPECT_EQ(pool.stats().discarded, 1U);
  EXPECT_EQ(pool.idleCount(), 0U);
  char byte = 0;
  boost::system::error_code ec;
  backend.backend_side[0]->receive(net::buffer(&byte, 1), 0, ec);
  EXPECT_EQ(ec, net::error::eof) << "the backend sees the close";
  pool.discard(nullptr);  // harmless
}

TEST_F(UpstreamPoolTest, CloseReleasesEveryIdleConnectionAndRefusesNewOnes) {
  UpstreamPool pool(settings());
  pool.checkin(backend.open(io));
  pool.checkin(backend.open(io));
  pool.checkin(backend.open(io));
  EXPECT_FALSE(pool.closed());
  pool.close();
  EXPECT_TRUE(pool.closed());
  EXPECT_EQ(pool.idleCount(), 0U);
  EXPECT_EQ(pool.checkout(backend.endpoint()), nullptr);

  pool.checkin(backend.open(io));  // a request that finishes after close(): closed, not pooled
  EXPECT_EQ(pool.idleCount(), 0U);
  EXPECT_EQ(pool.stats().discarded, 4U);

  for (auto& server : backend.backend_side) {
    char byte = 0;
    boost::system::error_code ec;
    server->receive(net::buffer(&byte, 1), 0, ec);
    EXPECT_EQ(ec, net::error::eof) << "every connection was really closed";
  }
  EXPECT_NO_THROW(pool.close()) << "idempotent";
}

TEST_F(UpstreamPoolTest, DestructionClosesIdleConnections) {
  {
    UpstreamPool pool(settings());
    pool.checkin(backend.open(io));
  }
  char byte = 0;
  boost::system::error_code ec;
  backend.backend_side[0]->receive(net::buffer(&byte, 1), 0, ec);
  EXPECT_EQ(ec, net::error::eof);
}

TEST_F(UpstreamPoolTest, ConcurrentUseNeverSharesOrLosesAConnection) {
  UpstreamPool pool(settings(8));
  constexpr int kThreads = 8;
  constexpr int kPrefilled = 6;
  std::vector<UpstreamConnection*> prefilled;
  for (int i = 0; i < kPrefilled; ++i) {
    auto connection = backend.open(io);
    prefilled.push_back(connection.get());
    pool.checkin(std::move(connection));
  }

  std::mutex in_use_mutex;
  std::set<UpstreamConnection*> in_use;
  std::atomic<int> violations{0};
  std::atomic<int> hits{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 400; ++i) {
        auto connection = pool.checkout(backend.endpoint());
        if (!connection) continue;
        ++hits;
        {
          const std::lock_guard lock(in_use_mutex);
          if (!in_use.insert(connection.get()).second) ++violations;  // handed out twice
        }
        std::this_thread::yield();
        {
          const std::lock_guard lock(in_use_mutex);
          in_use.erase(connection.get());
        }
        pool.checkin(std::move(connection));
      }
    });
  }
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(violations.load(), 0);
  EXPECT_GT(hits.load(), 0);
  EXPECT_EQ(pool.idleCount(), static_cast<std::size_t>(kPrefilled)) << "nothing leaked, nothing duplicated";
  const auto stats = pool.stats();
  EXPECT_EQ(stats.reused, static_cast<std::uint64_t>(hits.load()));
  EXPECT_EQ(stats.discarded, 0U);
}

TEST_F(UpstreamPoolTest, ConcurrentCloseDuringUseIsSafe) {
  UpstreamPool pool(settings(8));
  for (int i = 0; i < 6; ++i) pool.checkin(backend.open(io));
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      while (!stop.load()) {
        auto connection = pool.checkout(backend.endpoint());
        if (connection) pool.checkin(std::move(connection));
        std::this_thread::yield();
      }
    });
  }
  std::this_thread::sleep_for(50ms);
  pool.close();
  stop.store(true);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(pool.idleCount(), 0U);
  EXPECT_TRUE(pool.closed());
}

}  // namespace
