#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/HealthTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using edgeflow::config::HealthCheckType;
using edgeflow::discovery::ProbeResult;
using edgeflow::discovery::ProbeTarget;
using edgeflow::discovery::makeProber;
using edgeflow::testing::closedPort;
using edgeflow::testing::probeOnce;
using edgeflow::testing::RawServer;

ProbeTarget target(std::uint16_t port, std::chrono::milliseconds timeout = 2000ms,
                   const std::string& path = "/health") {
  ProbeTarget t;
  t.host = "127.0.0.1";
  t.port = port;
  t.http_path = path;
  t.timeout = timeout;
  return t;
}

// --- TCP ----------------------------------------------------------------------------

TEST(TcpProberTest, ConnectionToAListeningPortIsHealthy) {
  RawServer server(RawServer::Mode::Close);
  auto prober = makeProber(HealthCheckType::Tcp);
  const auto result = probeOnce(*prober, target(server.port()));
  EXPECT_TRUE(result.healthy) << result.detail;
  EXPECT_EQ(result.detail, "connected");
  EXPECT_EQ(server.requests(), 0) << "a TCP probe sends nothing";
}

TEST(TcpProberTest, TcpSuccessSaysNothingAboutTheApplication) {
  // The listener answers every connection with garbage, i.e. there is no working HTTP
  // application behind the port. The TCP probe still (correctly) reports a reachable port.
  RawServer server(RawServer::Mode::Garbage);
  auto prober = makeProber(HealthCheckType::Tcp);
  EXPECT_TRUE(probeOnce(*prober, target(server.port())).healthy);
  auto http = makeProber(HealthCheckType::Http);
  EXPECT_FALSE(probeOnce(*http, target(server.port())).healthy);
}

TEST(TcpProberTest, RefusedConnectionIsUnhealthy) {
  auto prober = makeProber(HealthCheckType::Tcp);
  const auto result = probeOnce(*prober, target(closedPort()));
  EXPECT_FALSE(result.healthy);
  EXPECT_EQ(result.detail, "connection refused");
}

TEST(TcpProberTest, ServerThatDisappearsBecomesUnhealthy) {
  RawServer server(RawServer::Mode::Close);
  auto prober = makeProber(HealthCheckType::Tcp);
  ASSERT_TRUE(probeOnce(*prober, target(server.port())).healthy);
  server.stop();
  EXPECT_FALSE(probeOnce(*prober, target(server.port())).healthy);
  ASSERT_TRUE(server.restart());
  EXPECT_TRUE(probeOnce(*prober, target(server.port())).healthy);
}

TEST(TcpProberTest, UnresolvableHostIsUnhealthyWithinTheTimeout) {
  auto prober = makeProber(HealthCheckType::Tcp);
  ProbeTarget t = target(80, 1500ms);
  t.host = "no-such-host.invalid";
  std::chrono::milliseconds elapsed{0};
  const auto result = probeOnce(*prober, t, &elapsed);
  EXPECT_FALSE(result.healthy) << result.detail;
  EXPECT_LT(elapsed, 3000ms) << "bounded by the timeout, not by the resolver";
}

TEST(TcpProberTest, UnreachableAddressNeverBlocksBeyondTheTimeout) {
  // 10.255.255.1 is normally unroutable: depending on the network the connect either
  // fails fast or hangs. Either way the probe must end, unhealthy, within the timeout.
  auto prober = makeProber(HealthCheckType::Tcp);
  ProbeTarget t = target(9, 400ms);
  t.host = "10.255.255.1";
  std::chrono::milliseconds elapsed{0};
  const auto result = probeOnce(*prober, t, &elapsed);
  EXPECT_FALSE(result.healthy) << result.detail;
  EXPECT_LT(elapsed, 2000ms);
}

// --- HTTP ---------------------------------------------------------------------------

struct StatusCase {
  int status;
  bool healthy;
};

TEST(HttpProberTest, OnlyTwoHundredClassStatusesAreHealthy) {
  auto prober = makeProber(HealthCheckType::Http);
  for (const auto& c : {StatusCase{200, true}, StatusCase{204, true}, StatusCase{299, true},
                        StatusCase{100, false}, StatusCase{301, false}, StatusCase{302, false},
                        StatusCase{400, false}, StatusCase{404, false}, StatusCase{500, false},
                        StatusCase{503, false}}) {
    RawServer server(RawServer::Mode::Http, c.status);
    const auto result = probeOnce(*prober, target(server.port()));
    EXPECT_EQ(result.healthy, c.healthy) << c.status << ": " << result.detail;
    EXPECT_EQ(result.detail, "HTTP " + std::to_string(c.status));
  }
}

TEST(HttpProberTest, SendsTheConfiguredPathAndAProperRequest) {
  RawServer server(RawServer::Mode::Http, 200);
  auto prober = makeProber(HealthCheckType::Http);
  ASSERT_TRUE(probeOnce(*prober, target(server.port(), 2000ms, "/internal/ready?deep=1")).healthy);
  const auto request = server.lastRequest();
  EXPECT_EQ(request.rfind("GET /internal/ready?deep=1 HTTP/1.1\r\n", 0), 0U) << request;
  EXPECT_NE(request.find("Host: 127.0.0.1:" + std::to_string(server.port())), std::string::npos);
  EXPECT_NE(request.find("Connection: close"), std::string::npos);
}

TEST(HttpProberTest, AConnectionWithoutAValidResponseIsUnhealthy) {
  auto prober = makeProber(HealthCheckType::Http);
  RawServer garbage(RawServer::Mode::Garbage);
  const auto malformed = probeOnce(*prober, target(garbage.port()));
  EXPECT_FALSE(malformed.healthy);
  EXPECT_EQ(malformed.detail, "malformed HTTP response");

  RawServer hangup(RawServer::Mode::HangUp);
  const auto closed = probeOnce(*prober, target(hangup.port()));
  EXPECT_FALSE(closed.healthy);
  EXPECT_EQ(closed.detail, "connection closed before a response");

  RawServer plain(RawServer::Mode::Close);  // accepts and closes before reading anything
  EXPECT_FALSE(probeOnce(*prober, target(plain.port())).healthy);
}

TEST(HttpProberTest, StalledBackendTimesOutInsteadOfBlocking) {
  RawServer server(RawServer::Mode::Stall);
  auto prober = makeProber(HealthCheckType::Http);
  std::chrono::milliseconds elapsed{0};
  const auto result = probeOnce(*prober, target(server.port(), 250ms), &elapsed);
  EXPECT_FALSE(result.healthy);
  EXPECT_EQ(result.detail, "timed out after 250ms");
  EXPECT_GE(elapsed, 240ms);
  EXPECT_LT(elapsed, 1500ms);
}

TEST(HttpProberTest, ResponseThatNeverCompletesTimesOut) {
  RawServer server(RawServer::Mode::PartialThenStall);
  auto prober = makeProber(HealthCheckType::Http);
  std::chrono::milliseconds elapsed{0};
  const auto result = probeOnce(*prober, target(server.port(), 250ms), &elapsed);
  EXPECT_FALSE(result.healthy);
  EXPECT_EQ(result.detail, "timed out after 250ms");
  EXPECT_LT(elapsed, 1500ms);
}

TEST(HttpProberTest, RefusedAndRecoveredBackend) {
  RawServer server(RawServer::Mode::Http, 200);
  auto prober = makeProber(HealthCheckType::Http);
  ASSERT_TRUE(probeOnce(*prober, target(server.port())).healthy);
  server.stop();
  EXPECT_EQ(probeOnce(*prober, target(server.port())).detail, "connection refused");
  ASSERT_TRUE(server.restart());
  EXPECT_TRUE(probeOnce(*prober, target(server.port())).healthy);
}

// --- cancellation -----------------------------------------------------------------------

TEST(ProberCancelTest, CancelledProbeNeverCallsBack) {
  RawServer server(RawServer::Mode::Stall);
  auto prober = makeProber(HealthCheckType::Http);
  boost::asio::io_context io;
  bool called = false;
  auto handle = prober->start(io, target(server.port(), 5000ms), [&](ProbeResult) { called = true; });
  // Let the probe connect and send, then cancel it.
  io.run_for(100ms);
  handle->cancel();
  io.restart();
  io.run_for(300ms);
  EXPECT_FALSE(called);
}

TEST(ProberCancelTest, CancellingAfterCompletionIsHarmless) {
  RawServer server(RawServer::Mode::Close);
  auto prober = makeProber(HealthCheckType::Tcp);
  boost::asio::io_context io;
  bool called = false;
  auto handle = prober->start(io, target(server.port()), [&](ProbeResult) { called = true; });
  io.run();
  EXPECT_TRUE(called);
  handle->cancel();
  handle->cancel();
}

}  // namespace
