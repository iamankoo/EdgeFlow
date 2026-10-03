#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/NetTestSupport.hpp"

namespace {

using namespace edgeflow::testing;  // NOLINT: test-only convenience
using edgeflow::network::HttpRequest;
using edgeflow::network::HttpResponse;
using edgeflow::network::HttpServer;
using edgeflow::network::RequestHandler;
using namespace std::chrono_literals;

nlohmann::json bodyOf(const HttpResponse& response) {
  return nlohmann::json::parse(response.body());
}

class ThrowingHandler final : public RequestHandler {
 public:
  HttpResponse handle(const HttpRequest&) override { throw std::runtime_error("handler boom"); }
};

class SlowHandler final : public RequestHandler {
 public:
  explicit SlowHandler(std::chrono::milliseconds delay) : delay_(delay) {}
  HttpResponse handle(const HttpRequest& request) override {
    entered.store(true);
    std::this_thread::sleep_for(delay_);
    return edgeflow::network::makeResponse(request, http::status::ok, "text/plain", "slow");
  }
  std::atomic<bool> entered{false};

 private:
  std::chrono::milliseconds delay_;
};

class BigBodyHandler final : public RequestHandler {
 public:
  HttpResponse handle(const HttpRequest& request) override {
    return edgeflow::network::makeResponse(request, http::status::ok, "application/octet-stream",
                                           std::string(32U * 1024U * 1024U, 'x'));
  }
};

// --- startup and shutdown ----------------------------------------------------------

TEST(HttpServerTest, StartsOnAnOsChosenPortAndStops) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  EXPECT_NE(harness.port(), 0);
  EXPECT_TRUE(harness.server.running());
  EXPECT_TRUE(harness.log.contains("HTTP server listening"));

  harness.server.stop(1s);
  EXPECT_FALSE(harness.server.running());
  EXPECT_TRUE(harness.log.contains("HTTP server stopped"));
}

TEST(HttpServerTest, StopIsIdempotentAndSafeBeforeStart) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  harness.server.stop(1s);
  EXPECT_NO_THROW(harness.server.stop(1s));

  edgeflow::testing::CapturedLogger log;
  HttpServer never_started(testServerConfig(), log.logger(),
                           std::make_shared<edgeflow::network::LocalRequestHandler>());
  EXPECT_NO_THROW(never_started.stop(1s));
  EXPECT_FALSE(never_started.start()) << "a stopped server cannot be started";
}

TEST(HttpServerTest, CannotStartTwice) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  EXPECT_FALSE(harness.server.start());
}

TEST(HttpServerTest, FailsToStartWhenPortIsTaken) {
  ServerHarness first;
  ASSERT_TRUE(first.started);

  auto config = testServerConfig();
  config.port = first.port();
  ServerHarness second(config);
  EXPECT_FALSE(second.started);
  EXPECT_TRUE(second.log.contains("failed to listen"));
}

TEST(HttpServerTest, StopsListeningAfterShutdown) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  const auto port = harness.port();
  harness.server.stop(1s);

  net::io_context io;
  net::ip::tcp::socket socket(io);
  boost::system::error_code ec;
  socket.connect({net::ip::make_address("127.0.0.1"), port}, ec);
  EXPECT_TRUE(ec) << "connections must be refused once the server has stopped";
}

// --- HTTP behavior -----------------------------------------------------------------

TEST(HttpServerTest, ServesRoot) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.get("/");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(response->version(), 11U);
  EXPECT_EQ((*response)[http::field::content_type], "application/json");
  EXPECT_EQ(response->at(http::field::content_length), std::to_string(response->body().size()));
  EXPECT_EQ((*response)[http::field::server].substr(0, 9), "EdgeFlow/");
  EXPECT_TRUE(response->keep_alive());
  EXPECT_EQ(bodyOf(*response)["service"], "edgeflow");
}

TEST(HttpServerTest, ServesHealth) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.get("/health");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(bodyOf(*response), (nlohmann::json{{"status", "ok"}, {"service", "edgeflow"}}));
}

TEST(HttpServerTest, QueryStringDoesNotAffectRouting) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());
  const auto response = client.get("/health?verbose=1");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
}

TEST(HttpServerTest, UnknownPathIs404) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.get("/does-not-exist");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::not_found);
  EXPECT_EQ((*response)[http::field::content_type], "application/json");
  EXPECT_EQ(bodyOf(*response)["status"], 404);
  EXPECT_TRUE(response->keep_alive()) << "a 404 does not close the connection";
}

TEST(HttpServerTest, WrongMethodIs405WithAllowHeader) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::delete_, "/health");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::method_not_allowed);
  EXPECT_EQ((*response)[http::field::allow], "GET");
}

TEST(HttpServerTest, ParsesMethodTargetVersionHeadersAndBody) {
  auto recorder = std::make_shared<RecordingHandler>();
  ServerHarness harness(testServerConfig(), recorder);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::put, "/things/42?x=1", "hello body",
                                       {{"X-Custom", "abc"}, {"Content-Type", "text/plain"}});
  ASSERT_TRUE(response);
  EXPECT_EQ(response->body(), "recorded");

  const auto seen = recorder->last();
  EXPECT_EQ(seen.method(), http::verb::put);
  EXPECT_EQ(seen.target(), "/things/42?x=1");
  EXPECT_EQ(seen.version(), 11U);
  EXPECT_EQ(seen["X-Custom"], "abc");
  EXPECT_EQ(seen[http::field::content_type], "text/plain");
  EXPECT_EQ(seen.body(), "hello body");
}

TEST(HttpServerTest, PostBodyIsReceivedInFull) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::post, "/echo", std::string(100000, 'b'));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(bodyOf(*response)["received_bytes"], 100000);
}

TEST(HttpServerTest, ChunkedRequestBodyIsReassembled) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  const auto response = client.readResponse();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(bodyOf(*response)["received_bytes"], 11);
  EXPECT_TRUE(client.get("/health")) << "the connection stays usable after a chunked body";
}

TEST(HttpServerTest, Http10RequestIsAnsweredAndClosed) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::get, "/health", {}, {}, 10);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_FALSE(response->keep_alive()) << "HTTP/1.0 is not persistent by default";
  EXPECT_TRUE(client.closedByPeer());
}

TEST(HttpServerTest, MalformedRequestGets400AndClose) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("THIS IS NOT HTTP\r\n\r\n");
  const auto response = client.readResponse();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_request);
  EXPECT_FALSE(response->keep_alive());
  EXPECT_TRUE(client.closedByPeer());
}

TEST(HttpServerTest, UnsupportedHttpVersionGets400) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("GET / HTTP/9.9\r\nHost: x\r\n\r\n");
  const auto response = client.readResponse();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_request);
}

TEST(HttpServerTest, OversizedBodyGets413) {
  auto config = testServerConfig();
  config.max_request_body_bytes = 100;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::post, "/echo", std::string(1000, 'z'));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::payload_too_large);
  EXPECT_FALSE(response->keep_alive());
}

TEST(HttpServerTest, BodyWithinLimitIsAccepted) {
  auto config = testServerConfig();
  config.max_request_body_bytes = 100;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::post, "/echo", std::string(100, 'z'));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
}

TEST(HttpServerTest, OversizedHeadersGet431) {
  auto config = testServerConfig();
  config.max_header_bytes = 1024;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + std::string(5000, 'h') + "\r\n\r\n");
  const auto response = client.readResponse();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::request_header_fields_too_large);
}

TEST(HttpServerTest, HandlerExceptionBecomes500) {
  ServerHarness harness(testServerConfig(), std::make_shared<ThrowingHandler>());
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.get("/");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::internal_server_error);
  EXPECT_EQ(bodyOf(*response)["status"], 500);
  EXPECT_TRUE(harness.log.contains("handler boom"));
  // The server must survive: a new connection still works.
  TestClient second(harness.port());
  EXPECT_TRUE(second.get("/"));
}

// --- keep-alive --------------------------------------------------------------------

TEST(HttpServerKeepAliveTest, ManySequentialRequestsReuseOneConnection) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  for (int i = 0; i < 10; ++i) {
    const auto response = client.get(i % 2 == 0 ? "/" : "/health");
    ASSERT_TRUE(response) << "request " << i << ": " << client.last_error.message();
    EXPECT_EQ(response->result(), http::status::ok);
    EXPECT_TRUE(response->keep_alive());
  }
  EXPECT_EQ(harness.server.acceptedConnections(), 1U) << "all requests must share one TCP connection";
  EXPECT_EQ(harness.server.activeConnections(), 1U);
}

TEST(HttpServerKeepAliveTest, PipelinedRequestsAreAnsweredInOrder) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("GET /health HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n"
                 "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
  const auto first = client.readResponse();
  const auto second = client.readResponse();
  const auto third = client.readResponse();
  ASSERT_TRUE(first && second && third);
  EXPECT_EQ(bodyOf(*first)["status"], "ok");
  EXPECT_EQ(bodyOf(*second)["service"], "edgeflow");
  EXPECT_EQ(third->result(), http::status::not_found);
  EXPECT_EQ(harness.server.acceptedConnections(), 1U);
}

TEST(HttpServerKeepAliveTest, ConnectionCloseIsHonored) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  const auto response = client.request(http::verb::get, "/health", {}, {{"Connection", "close"}});
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_FALSE(response->keep_alive());
  EXPECT_EQ((*response)[http::field::connection], "close");
  EXPECT_TRUE(client.closedByPeer());
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
}

TEST(HttpServerKeepAliveTest, ExplicitKeepAliveStaysOpen) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());
  ASSERT_TRUE(client.request(http::verb::get, "/", {}, {{"Connection", "keep-alive"}}));
  EXPECT_TRUE(client.get("/health"));
}

// --- timeouts ----------------------------------------------------------------------

TEST(HttpServerTimeoutTest, IdleConnectionWithNoRequestIsClosedSilently) {
  auto config = testServerConfig();
  config.keep_alive_timeout = 200ms;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  EXPECT_TRUE(client.closedByPeer()) << "idle timeout must close the connection";
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
  EXPECT_FALSE(harness.log.contains("[warning]"));
}

TEST(HttpServerTimeoutTest, KeepAliveConnectionClosesAfterIdleTimeout) {
  auto config = testServerConfig();
  config.keep_alive_timeout = 250ms;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  ASSERT_TRUE(client.get("/health"));
  EXPECT_TRUE(client.closedByPeer());
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
}

TEST(HttpServerTimeoutTest, IdleTimerIsRestartedByEachRequest) {
  auto config = testServerConfig();
  config.keep_alive_timeout = 400ms;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  // 5 x 150ms = 750ms in total, far beyond one idle period; each gap is within it.
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(client.get("/health")) << "request " << i;
    std::this_thread::sleep_for(150ms);
  }
  EXPECT_EQ(harness.server.acceptedConnections(), 1U);
}

TEST(HttpServerTimeoutTest, RequestTimerIsCancelledOnceTheRequestCompletes) {
  auto config = testServerConfig();
  config.request_timeout = 200ms;
  config.keep_alive_timeout = 5000ms;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  ASSERT_TRUE(client.get("/health"));
  std::this_thread::sleep_for(500ms);  // longer than request_timeout
  const auto response = client.get("/health");
  ASSERT_TRUE(response) << "the request timer must not fire on an idle connection";
  EXPECT_EQ(response->result(), http::status::ok);
}

TEST(HttpServerTimeoutTest, StalledHeadersGet408) {
  auto config = testServerConfig();
  config.request_timeout = 200ms;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("GET /health HTTP/1.1\r\nHost: x\r\n");  // never finished
  const auto response = client.readResponse();
  ASSERT_TRUE(response) << client.last_error.message();
  EXPECT_EQ(response->result(), http::status::request_timeout);
  EXPECT_FALSE(response->keep_alive());
  EXPECT_TRUE(client.closedByPeer());
  EXPECT_TRUE(harness.log.contains("request timed out"));
}

TEST(HttpServerTimeoutTest, StalledBodyGet408) {
  auto config = testServerConfig();
  config.request_timeout = 200ms;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());

  client.sendRaw("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc");
  const auto response = client.readResponse();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::request_timeout);
}

// --- connection lifecycle ----------------------------------------------------------

TEST(HttpServerLifecycleTest, ClientDisconnectMidRequestIsHandled) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  {
    TestClient client(harness.port());
    client.sendRaw("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\npartial");
    ASSERT_TRUE(waitFor([&] { return harness.server.activeConnections() == 1; }));
    client.close();
  }
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
  TestClient again(harness.port());
  EXPECT_TRUE(again.get("/health")) << "the server must keep serving";
}

TEST(HttpServerLifecycleTest, ClientDisconnectWhileIdleIsHandled) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  {
    TestClient client(harness.port());
    ASSERT_TRUE(client.get("/health"));
  }  // closed by the client's destructor
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
}

TEST(HttpServerLifecycleTest, ConnectionResetIsHandled) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());
  ASSERT_TRUE(client.get("/health"));
  client.reset();
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
}

TEST(HttpServerLifecycleTest, WriteFailureReleasesTheConnection) {
  ServerHarness harness(testServerConfig(), std::make_shared<BigBodyHandler>());
  ASSERT_TRUE(harness.started);
  {
    TestClient client(harness.port());
    client.sendRaw("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(waitFor([&] { return harness.server.activeConnections() == 1; }));
    // Abort while the 32 MiB response is still being written.
    std::this_thread::sleep_for(100ms);
    client.reset();
  }
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
}

TEST(HttpServerLifecycleTest, ConnectionLimitRejectsExtraConnections) {
  auto config = testServerConfig();
  config.max_connections = 2;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);

  auto first = std::make_unique<TestClient>(harness.port());
  TestClient second(harness.port());
  ASSERT_TRUE(first->get("/health"));
  ASSERT_TRUE(second.get("/health"));

  TestClient third(harness.port());
  EXPECT_TRUE(third.closedByPeer());
  EXPECT_EQ(harness.server.rejectedConnections(), 1U);

  first.reset();
  ASSERT_TRUE(waitFor([&] { return harness.server.activeConnections() == 1; }));
  TestClient fourth(harness.port());
  EXPECT_TRUE(fourth.get("/health")) << "capacity is available again";
}

// --- shutdown ----------------------------------------------------------------------

TEST(HttpServerShutdownTest, ClosesIdleConnectionsPromptly) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < 3; ++i) {
    clients.push_back(std::make_unique<TestClient>(harness.port()));
    ASSERT_TRUE(clients.back()->get("/health"));
  }

  const auto begin = std::chrono::steady_clock::now();
  harness.server.stop(5s);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s) << "idle connections must not wait out the grace period";
  for (auto& client : clients) EXPECT_TRUE(client->closedByPeer());
  EXPECT_EQ(harness.server.activeConnections(), 0U);
}

TEST(HttpServerShutdownTest, InFlightRequestFinishesBeforeClose) {
  auto slow = std::make_shared<SlowHandler>(400ms);
  ServerHarness harness(testServerConfig(), slow);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());
  client.sendRaw("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(waitFor([&] { return slow->entered.load(); }));

  auto stopped = std::async(std::launch::async, [&] { harness.server.stop(5s); });
  const auto response = client.readResponse();
  ASSERT_TRUE(response) << client.last_error.message();
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(response->body(), "slow");
  EXPECT_FALSE(response->keep_alive()) << "a draining server asks the client to close";
  EXPECT_TRUE(client.closedByPeer());
  stopped.wait();
}

TEST(HttpServerShutdownTest, ForceClosesStalledConnectionsAfterGracePeriod) {
  auto config = testServerConfig();
  config.request_timeout = 30000ms;  // so only the grace period can end this
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());
  client.sendRaw("GET / HTTP/1.1\r\nHost: x\r\n");  // incomplete request, in flight
  ASSERT_TRUE(waitFor([&] { return harness.server.activeConnections() == 1; }));
  std::this_thread::sleep_for(100ms);

  const auto begin = std::chrono::steady_clock::now();
  harness.server.stop(300ms);
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_GE(elapsed, 250ms);
  EXPECT_LT(elapsed, 5s);
  EXPECT_TRUE(client.closedByPeer());
  EXPECT_TRUE(harness.log.contains("closing them"));
}

TEST(HttpServerShutdownTest, ConcurrentStopCallsAreSafe) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  TestClient client(harness.port());
  ASSERT_TRUE(client.get("/health"));

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { harness.server.stop(1s); });
  for (auto& t : threads) t.join();
  EXPECT_FALSE(harness.server.running());
}

// --- concurrency -------------------------------------------------------------------

class ConcurrentClientsTest : public ::testing::TestWithParam<int> {};

TEST_P(ConcurrentClientsTest, AllClientsGetCorrectResponses) {
  constexpr int kRequestsPerClient = 5;
  const int clients = GetParam();

  auto config = testServerConfig();
  config.worker_threads = 4;
  ServerHarness harness(config);
  ASSERT_TRUE(harness.started);

  std::atomic<int> failures{0};
  std::atomic<int> responses{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(clients));
  for (int c = 0; c < clients; ++c) {
    threads.emplace_back([&, c] {
      try {
        TestClient client(harness.port());
        for (int r = 0; r < kRequestsPerClient; ++r) {
          // Distinct body sizes per client/request detect any cross-connection mix-up.
          const auto size = static_cast<std::size_t>(c * 10 + r + 1);
          const auto response = (r % 3 == 0)   ? client.get("/health")
                                : (r % 3 == 1) ? client.request(http::verb::post, "/echo",
                                                                 std::string(size, 'q'))
                                               : client.get("/");
          if (!response || response->result() != http::status::ok) {
            ++failures;
            return;
          }
          const auto json = nlohmann::json::parse(response->body());
          const bool correct = (r % 3 == 0)   ? json["status"] == "ok"
                               : (r % 3 == 1) ? json["received_bytes"] == size
                                              : json["service"] == "edgeflow";
          if (!correct) ++failures;
          ++responses;
        }
      } catch (const std::exception&) {
        ++failures;
      }
    });
  }
  for (auto& t : threads) t.join();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(responses.load(), clients * kRequestsPerClient);
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }))
      << harness.server.activeConnections() << " connection(s) leaked";
  EXPECT_EQ(harness.server.acceptedConnections(), static_cast<std::uint64_t>(clients));
}

INSTANTIATE_TEST_SUITE_P(Clients, ConcurrentClientsTest, ::testing::Values(1, 10, 50, 100));

TEST(HttpServerConcurrencyTest, RepeatedConnectChurnLeavesNoConnections) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 25; ++i) {
        try {
          TestClient client(harness.port());
          if (!client.request(http::verb::get, "/health", {}, {{"Connection", "close"}})) ++failures;
        } catch (const std::exception&) {
          ++failures;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_TRUE(waitFor([&] { return harness.server.activeConnections() == 0; }));
  EXPECT_EQ(harness.server.acceptedConnections(), 200U);
}

}  // namespace
