#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "edgeflow/network/TcpServer.hpp"
#include "support/NetTestSupport.hpp"

namespace {

using namespace edgeflow::testing;  // NOLINT: test-only convenience
using edgeflow::network::TcpServer;

// Runs an io_context on a background thread for the lifetime of the object.
class IoThread {
 public:
  IoThread() : guard_(net::make_work_guard(io)), thread_([this] { io.run(); }) {}
  ~IoThread() {
    guard_.reset();
    io.stop();
    thread_.join();
  }
  net::io_context io;

 private:
  net::executor_work_guard<net::io_context::executor_type> guard_;
  std::thread thread_;
};

TEST(TcpServerTest, ListensOnAnOsChosenPort) {
  CapturedLogger log;
  net::io_context io;
  TcpServer server(io, log.logger(), [](net::ip::tcp::socket) {});
  ASSERT_TRUE(server.listen("127.0.0.1", 0)) << server.lastError();
  EXPECT_NE(server.port(), 0);
}

TEST(TcpServerTest, AcceptsConnectionsAndKeepsAccepting) {
  CapturedLogger log;
  IoThread io_thread;
  std::atomic<int> accepted{0};
  TcpServer server(io_thread.io, log.logger(), [&](net::ip::tcp::socket) { ++accepted; });
  ASSERT_TRUE(server.listen("127.0.0.1", 0)) << server.lastError();
  server.startAccepting();

  for (int i = 0; i < 5; ++i) {
    net::io_context client_io;
    net::ip::tcp::socket client(client_io);
    client.connect({net::ip::make_address("127.0.0.1"), server.port()});
  }
  EXPECT_TRUE(waitFor([&] { return accepted.load() == 5; }));
  server.stop();
}

TEST(TcpServerTest, StopRefusesNewConnectionsAndIsIdempotent) {
  CapturedLogger log;
  IoThread io_thread;
  TcpServer server(io_thread.io, log.logger(), [](net::ip::tcp::socket) {});
  ASSERT_TRUE(server.listen("127.0.0.1", 0));
  server.startAccepting();
  const auto port = server.port();

  server.stop();
  EXPECT_NO_THROW(server.stop());

  net::io_context client_io;
  net::ip::tcp::socket client(client_io);
  boost::system::error_code ec;
  client.connect({net::ip::make_address("127.0.0.1"), port}, ec);
  EXPECT_TRUE(ec);
}

TEST(TcpServerTest, StopWithoutStartingIsSafe) {
  CapturedLogger log;
  net::io_context io;
  TcpServer server(io, log.logger(), [](net::ip::tcp::socket) {});
  ASSERT_TRUE(server.listen("127.0.0.1", 0));
  EXPECT_NO_THROW(server.stop());
}

TEST(TcpServerTest, ListenFailsWhenAddressIsInUse) {
  CapturedLogger log;
  net::io_context io;
  TcpServer first(io, log.logger(), [](net::ip::tcp::socket) {});
  ASSERT_TRUE(first.listen("127.0.0.1", 0));

  TcpServer second(io, log.logger(), [](net::ip::tcp::socket) {});
  EXPECT_FALSE(second.listen("127.0.0.1", first.port()));
  EXPECT_NE(second.lastError().find("bind"), std::string::npos) << second.lastError();
}

TEST(TcpServerTest, ListenFailsForUnresolvableHost) {
  CapturedLogger log;
  net::io_context io;
  TcpServer server(io, log.logger(), [](net::ip::tcp::socket) {});
  EXPECT_FALSE(server.listen("host.invalid", 0));
  EXPECT_NE(server.lastError().find("cannot resolve"), std::string::npos) << server.lastError();
}

}  // namespace
