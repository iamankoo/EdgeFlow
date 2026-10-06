#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using edgeflow::config::RoutingStrategy;
using edgeflow::discovery::HealthStatus;
using edgeflow::discovery::InstanceStatus;
namespace http = boost::beast::http;
namespace net = boost::asio;

// "name" out of a body shaped like "name|METHOD target|payload".
std::string whoAnswered(const network::HttpResponse& response) {
  return response.body().substr(0, response.body().find('|'));
}

class ProxyTest : public ProxyFixture {
 protected:
  // Service "orders" with a single instance "a" on `backend`, gateway started.
  void single(ScriptedBackend& backend, RoutingStrategy strategy = RoutingStrategy::RoundRobin) {
    addInstance("orders", "a", backend.port());
    startGateway(strategy);
  }

  static Action reply(unsigned code, std::string body = {}) {
    Action action;
    action.code = code;
    action.body = std::move(body);
    return action;
  }
  static Action ofKind(Kind kind, std::string body = {}) {
    Action action;
    action.kind = kind;
    action.body = std::move(body);
    return action;
  }
  static discovery::InstanceUpdate healthUpdate(HealthStatus health) {
    discovery::InstanceUpdate update;
    update.health = health;
    return update;
  }

  // Many clients, several backends, bodies echoed back: every answer must be the right one.
  void runConcurrentTraffic(RoutingStrategy strategy) {
    constexpr int kBackends = 4;
    constexpr int kThreads = 16;
    constexpr int kRequests = 25;
    std::vector<std::unique_ptr<ScriptedBackend>> backends;
    std::vector<std::string> ids;
    for (int i = 0; i < kBackends; ++i) {
      const std::string id(1, static_cast<char>('a' + i));
      backends.push_back(std::make_unique<ScriptedBackend>(nullptr, id));
      ids.push_back(id);
      addInstance("shop", id, backends.back()->port());
    }
    startGateway(strategy);

    std::atomic<int> good{0};
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        auto c = client();
        for (int i = 0; i < kRequests; ++i) {
          const std::string payload = "payload-" + std::to_string(t) + "-" + std::to_string(i);
          const std::string path = "/x/" + std::to_string(t) + "/" + std::to_string(i);
          const auto response = c->request(http::verb::post, "/proxy/shop" + path, payload);
          bool right = false;
          if (response && response->result() == http::status::ok) {
            const auto& body = response->body();
            const auto bar1 = body.find('|');
            const auto bar2 = bar1 == std::string::npos ? bar1 : body.find('|', bar1 + 1);
            right = bar1 == 1 && bar2 != std::string::npos &&
                    body.substr(bar1 + 1, bar2 - bar1 - 1) == "POST " + path &&
                    body.substr(bar2 + 1) == payload;
          }
          (right ? good : bad).fetch_add(1);
        }
      });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(good.load(), kThreads * kRequests);
    EXPECT_EQ(bad.load(), 0);
    std::size_t total = 0;
    for (const auto& backend : backends) total += backend->requests();
    EXPECT_EQ(total, static_cast<std::size_t>(kThreads * kRequests)) << "every request reached exactly one backend";
    const auto stats = proxy->stats();
    EXPECT_EQ(stats.responses, static_cast<std::uint64_t>(kThreads * kRequests));
    EXPECT_EQ(stats.gateway_errors, 0U);
    EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
    EXPECT_TRUE(countsReturnToZero("shop", ids)) << "no connection count leaked";
    EXPECT_GT(stats.pool.reused, 0U) << "connections were reused under load";
    EXPECT_LE(proxy->pool().idleCount(), static_cast<std::size_t>(kBackends * 8));
  }
};

// =====================================================================================
// Request and response forwarding
// =====================================================================================

TEST_F(ProxyTest, GetIsForwardedWithMethodTargetQueryAndHeaders) {
  ScriptedBackend backend;
  backend.always([] {
    Action action = reply(200, "hello from the backend");
    action.headers = {{"Content-Type", "text/plain"}, {"X-Backend", "yes"}};
    return action;
  }());
  single(backend);

  auto c = client();
  const auto response = c->request(http::verb::get, "/proxy/orders/api/users?id=42", {},
                                   {{"User-Agent", "probe/1"}, {"X-Custom", "v"}, {"Accept", "text/plain"}});
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(response->body(), "hello from the backend");
  EXPECT_EQ(header(*response, "Content-Type"), "text/plain");
  EXPECT_EQ(header(*response, "X-Backend"), "yes");

  ASSERT_EQ(backend.requests(), 1U);
  const auto seen = backend.last();
  EXPECT_EQ(seen.method(), http::verb::get);
  EXPECT_EQ(std::string{seen.target()}, "/api/users?id=42") << "prefix stripped, query kept";
  EXPECT_EQ(header(seen, "User-Agent"), "probe/1");
  EXPECT_EQ(header(seen, "X-Custom"), "v");
  EXPECT_EQ(header(seen, "Accept"), "text/plain");
  EXPECT_EQ(seen.version(), 11U);
}

TEST_F(ProxyTest, ServiceMappingExamples) {
  ScriptedBackend orders(nullptr, "orders");
  ScriptedBackend payment(nullptr, "payment");
  addInstance("orders", "o1", orders.port());
  addInstance("payment", "p1", payment.port());
  startGateway();

  auto c = client();
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"/proxy/orders/", "/"},
      {"/proxy/orders", "/"},
      {"/proxy/orders/api/users", "/api/users"},
      {"/proxy/orders/api/users?id=42", "/api/users?id=42"},
      {"/proxy/orders?x=1", "/?x=1"},
      {"/proxy/orders//a//b", "//a//b"},
      {"/proxy/orders/a%20b", "/a%20b"},
  };
  for (const auto& [target, expected] : cases) {
    SCOPED_TRACE(target);
    const auto response = c->get(target);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    EXPECT_EQ(whoAnswered(*response), "orders");
    EXPECT_EQ(std::string{orders.last().target()}, expected);
  }
  const auto charge = c->get("/proxy/payment/v1/charge");
  ASSERT_TRUE(charge);
  EXPECT_EQ(whoAnswered(*charge), "payment");
  EXPECT_EQ(std::string{payment.last().target()}, "/v1/charge");
}

TEST_F(ProxyTest, RequestBodiesAreForwardedExactly) {
  ScriptedBackend backend([](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    Action action = reply(200, request.body());
    action.headers = {{"Content-Type", "application/octet-stream"}};
    return action;
  });
  single(backend);

  std::string body;
  for (int i = 0; i < 300 * 1024; ++i) body.push_back(static_cast<char>((i * 31 + i / 7) & 0xFF));  // all byte values
  auto c = client();
  const auto response =
      c->request(http::verb::post, "/proxy/orders/upload", body, {{"Content-Type", "application/octet-stream"}});
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(response->body(), body) << "the body is neither modified nor truncated";

  const auto seen = backend.last();
  EXPECT_EQ(seen.method(), http::verb::post);
  EXPECT_EQ(seen.body(), body);
  EXPECT_EQ(header(seen, "Content-Length"), std::to_string(body.size()));
  EXPECT_EQ(header(seen, "Content-Type"), "application/octet-stream");
}

TEST_F(ProxyTest, ChunkedRequestBodiesArriveWithAContentLength) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  c->sendRaw("POST /proxy/orders/up HTTP/1.1\r\nHost: gw\r\nTransfer-Encoding: chunked\r\n\r\n"
             "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  const auto response = c->readResponse();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  const auto seen = backend.last();
  EXPECT_EQ(seen.body(), "hello world");
  EXPECT_EQ(header(seen, "Content-Length"), "11");
  EXPECT_EQ(header(seen, "Transfer-Encoding"), "<absent>");
}

TEST_F(ProxyTest, EveryHttpMethodIsForwarded) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  for (const auto method : {http::verb::get, http::verb::post, http::verb::put, http::verb::patch,
                            http::verb::delete_, http::verb::options}) {
    const std::string name{http::to_string(method)};
    SCOPED_TRACE(name);
    const auto response = c->request(method, "/proxy/orders/m", method == http::verb::get ? "" : "payload");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
    EXPECT_EQ(backend.last().method(), method);
    EXPECT_EQ(whoAnswered(*response), "backend");
  }
  EXPECT_EQ(backend.requests(), 6U);
}

TEST_F(ProxyTest, HeadResponsesKeepTheirLengthAndNoBodyAndTheConnectionStaysUsable) {
  ScriptedBackend backend;
  backend.always(reply(200, "twelve bytes"));
  single(backend);

  net::io_context io;
  net::ip::tcp::socket socket(io);
  socket.connect({net::ip::make_address("127.0.0.1"), server->port()});
  boost::beast::flat_buffer buffer;

  http::request<http::empty_body> head{http::verb::head, "/proxy/orders/doc", 11};
  head.set(http::field::host, "gw");
  http::write(socket, head);
  http::response_parser<http::string_body> parser;
  parser.skip(true);  // a response to HEAD has no body, whatever its Content-Length says
  http::read(socket, buffer, parser);
  const auto& response = parser.get();
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(header(response, "Content-Length"), "12") << "the length of the body a GET would return";
  EXPECT_TRUE(response.body().empty());
  EXPECT_EQ(backend.last().method(), http::verb::head);

  // Nothing is left over on the wire: the next request on the same connection works.
  http::request<http::empty_body> get{http::verb::get, "/proxy/orders/doc", 11};
  get.set(http::field::host, "gw");
  http::write(socket, get);
  http::response<http::string_body> second;
  http::read(socket, buffer, second);
  EXPECT_EQ(second.body(), "twelve bytes");
}

TEST_F(ProxyTest, BackendStatusCodesAreForwardedNotTurnedIntoGatewayErrors) {
  ScriptedBackend backend([](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    const auto code = static_cast<unsigned>(std::stoi(std::string{request.target().substr(1)}));
    Action action = reply(code, "body-" + std::to_string(code));
    if (code == 301 || code == 302) action.headers = {{"Location", "/elsewhere"}};
    return action;
  });
  single(backend);
  auto c = client();
  for (const unsigned code : {200U, 201U, 202U, 204U, 301U, 302U, 400U, 401U, 403U, 404U, 409U,
                              418U, 429U, 500U, 501U, 502U, 503U, 504U}) {
    SCOPED_TRACE(code);
    const auto response = c->get("/proxy/orders/" + std::to_string(code));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result_int(), code);
    if (code == 204) {
      EXPECT_TRUE(response->body().empty());
    } else {
      EXPECT_EQ(response->body(), "body-" + std::to_string(code));
    }
    if (code == 301 || code == 302) {
      EXPECT_EQ(header(*response, "Location"), "/elsewhere");
    }
  }
  EXPECT_EQ(proxy->stats().gateway_errors, 0U) << "only proxy failures are gateway errors";
  EXPECT_EQ(backend.requests(), 18U) << "and nothing is retried";
}

TEST_F(ProxyTest, ResponseHeadersAreForwardedAndHopByHopOnesAreNot) {
  ScriptedBackend backend;
  Action action = reply(200, "x");
  action.headers = {{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}, {"Keep-Alive", "timeout=5"},
                    {"X-Hop", "named"}, {"Connection", "X-Hop"}, {"X-Keep", "stays"},
                    {"Via", "1.1 backend-proxy"}, {"X-Request-Id", "backend-invention"}};
  backend.always(action);
  single(backend);

  auto c = client();
  const auto response = c->get("/proxy/orders/h");
  ASSERT_TRUE(response);
  std::size_t cookies = 0;
  for (const auto& field : *response) cookies += field.name() == http::field::set_cookie ? 1U : 0U;
  EXPECT_EQ(cookies, 2U);
  EXPECT_EQ(header(*response, "X-Keep"), "stays");
  EXPECT_EQ(header(*response, "Keep-Alive"), "<absent>");
  EXPECT_EQ(header(*response, "X-Hop"), "<absent>");
  EXPECT_EQ(header(*response, "Via"), "1.1 backend-proxy, 1.1 edgeflow");
  EXPECT_NE(header(*response, "X-Request-Id"), "backend-invention");
}

TEST_F(ProxyTest, ABackendThatClosesItsConnectionDoesNotCloseTheClientsConnection) {
  ScriptedBackend backend;
  Action action = reply(200, "bye");
  action.keep_alive = false;  // "Connection: close" towards EdgeFlow
  backend.always(action);
  single(backend);

  auto c = client();
  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response) << "request " << i << ": the client's connection must survive";
    EXPECT_EQ(response->body(), "bye");
    EXPECT_EQ(header(*response, "Connection"), "<absent>") << "the backend's Connection: close is its own business";
  }
  EXPECT_EQ(backend.connections(), 3U) << "but each backend connection was used once";
}

TEST_F(ProxyTest, ChunkedBackendResponsesAreDeliveredWithContentLength) {
  ScriptedBackend backend;
  Action action = reply(200, std::string(90, 'c') + std::string(30, 'd'));
  action.kind = Kind::Chunked;
  backend.always(action);
  single(backend);
  auto c = client();
  const auto response = c->get("/proxy/orders/chunks");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->body(), action.body);
  EXPECT_EQ(header(*response, "Transfer-Encoding"), "<absent>");
  EXPECT_EQ(header(*response, "Content-Length"), "120");
}

TEST_F(ProxyTest, ResponsesDelimitedByConnectionCloseAreForwardedWhole) {
  ScriptedBackend backend;
  Action action = reply(200, std::string(5000, 'e'));
  action.kind = Kind::UntilEof;
  backend.always(action);
  single(backend);
  auto c = client();
  for (int i = 0; i < 2; ++i) {
    const auto response = c->get("/proxy/orders/eof");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->body(), action.body);
  }
  EXPECT_EQ(backend.connections(), 2U) << "such a connection can never be reused";
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
}

TEST_F(ProxyTest, LargeResponsesAreForwarded) {
  ScriptedBackend backend;
  std::string big(5 * 1024 * 1024, '\0');
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 7) % 26);
  backend.always(reply(200, big));
  single(backend);
  auto c = client();
  const auto response = c->get("/proxy/orders/big");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->body().size(), big.size());
  EXPECT_TRUE(response->body() == big);
}

TEST_F(ProxyTest, AClientConnectionServesManyProxiedAndLocalRequests) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  for (int i = 0; i < 10; ++i) {
    const auto proxied = c->get("/proxy/orders/n" + std::to_string(i));
    ASSERT_TRUE(proxied);
    EXPECT_EQ(proxied->result(), http::status::ok);
    const auto local = c->get("/health");
    ASSERT_TRUE(local);
    EXPECT_EQ(local->result(), http::status::ok);
  }
  EXPECT_EQ(server->acceptedConnections(), 1U);
}

// =====================================================================================
// Mapping errors and the routes that existed before
// =====================================================================================

TEST_F(ProxyTest, MalformedProxyUrlsAreClientErrorsAndReachNoBackend) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  for (const char* target : {"/proxy", "/proxy/", "/proxy//x", "/proxy?x=1", "/proxy/Orders/x",
                             "/proxy/ord%65rs/x", "/proxy/-x/y", "/proxy/%2e%2e/x", "/proxy/../x"}) {
    SCOPED_TRACE(target);
    const auto response = c->get(target);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::bad_request);
    EXPECT_NE(header(*response, "X-Request-Id"), "<absent>");
    const auto body = nlohmann::json::parse(response->body());
    EXPECT_EQ(body["status"], 400);
    EXPECT_FALSE(body["detail"].get<std::string>().empty());
  }
  EXPECT_EQ(backend.requests(), 0U);
}

TEST_F(ProxyTest, AnUnknownServiceIs404) {
  ScriptedBackend backend;
  single(backend);
  const auto response = client()->get("/proxy/nobody/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::not_found);
  EXPECT_EQ(backend.requests(), 0U);
}

TEST_F(ProxyTest, ExistingRoutesBehaveAsBefore) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();

  const auto banner = c->get("/");
  ASSERT_TRUE(banner);
  EXPECT_EQ(banner->result(), http::status::ok);
  const auto health = c->get("/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(nlohmann::json::parse(health->body())["status"], "ok");
  const auto echo = c->request(http::verb::post, "/echo", "abcd");
  ASSERT_TRUE(echo);
  EXPECT_EQ(echo->result(), http::status::ok);
  EXPECT_EQ(c->get("/nope")->result(), http::status::not_found);
  EXPECT_EQ(c->get("/proxyfoo")->result(), http::status::not_found) << "only /proxy and /proxy/... are proxied";
  EXPECT_EQ(c->get("/Proxy/orders/x")->result(), http::status::not_found);
  EXPECT_EQ(c->request(http::verb::post, "/health")->result(), http::status::method_not_allowed);

  const auto services = c->get("/services");
  ASSERT_TRUE(services);
  EXPECT_EQ(services->result(), http::status::ok);
  EXPECT_EQ(c->get("/services/orders/instances")->result(), http::status::ok);
  EXPECT_EQ(c->get("/services/orders/instances/a")->result(), http::status::ok);
  EXPECT_EQ(c->get("/services/orders/routable")->result(), http::status::ok);
  EXPECT_EQ(c->get("/services/missing/routable")->result(), http::status::not_found);
  EXPECT_EQ(backend.requests(), 0U) << "none of these touches a backend";
}

TEST_F(ProxyTest, TheRouteDiagnosticStillOnlyReportsADecision) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  const auto response = c->get("/services/orders/route?key=k1");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  const auto body = nlohmann::json::parse(response->body());
  EXPECT_EQ(body["selected"]["instance_id"], "a");
  EXPECT_EQ(c->request(http::verb::post, "/services/orders/route")->result(), http::status::method_not_allowed);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(backend.requests(), 0U) << "a routing decision forwards nothing";
  EXPECT_EQ(backend.connections(), 0U);
  EXPECT_EQ(connectionCount("orders", "a"), 0U) << "and is not a connection";
  EXPECT_EQ(registry->adjustments.load(), 0);
}

// =====================================================================================
// Header propagation
// =====================================================================================

TEST_F(ProxyTest, HostIsRewrittenForTheBackend) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  ASSERT_TRUE(c->request(http::verb::get, "/proxy/orders/h", {}, {{"Host", "gateway.example.com:8080"}}));
  EXPECT_EQ(header(backend.last(), "Host"), "127.0.0.1:" + std::to_string(backend.port()))
      << "the gateway's own host and port must not leak to the backend";
}

TEST_F(ProxyTest, ForwardedHeadersAndVia) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();

  ASSERT_TRUE(c->get("/proxy/orders/a"));
  auto seen = backend.last();
  EXPECT_EQ(header(seen, "X-Forwarded-For"), "127.0.0.1");
  EXPECT_EQ(header(seen, "X-Forwarded-Proto"), "http");
  EXPECT_EQ(header(seen, "Via"), "1.1 edgeflow");

  ASSERT_TRUE(c->request(http::verb::get, "/proxy/orders/b", {},
                         {{"X-Forwarded-For", "198.51.100.7, 10.0.0.2"}, {"X-Forwarded-Proto", "https"},
                          {"Via", "1.0 edge-cdn"}}));
  seen = backend.last();
  EXPECT_EQ(header(seen, "X-Forwarded-For"), "198.51.100.7, 10.0.0.2, 127.0.0.1")
      << "the chain is extended, not replaced";
  EXPECT_EQ(header(seen, "X-Forwarded-Proto"), "http");
  EXPECT_EQ(header(seen, "Via"), "1.0 edge-cdn, 1.1 edgeflow");
}

TEST_F(ProxyTest, HopByHopRequestHeadersNeverReachTheBackend) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  ASSERT_TRUE(c->request(http::verb::post, "/proxy/orders/hop", "data",
                         {{"Connection", "keep-alive, X-Secret-Hop"}, {"Keep-Alive", "timeout=9"},
                          {"TE", "trailers"}, {"Trailer", "X-T"}, {"Upgrade", "websocket"},
                          {"Proxy-Authorization", "Basic Zm9v"}, {"Proxy-Connection", "keep-alive"},
                          {"X-Secret-Hop", "named by the Connection header"}, {"Expect", "100-continue"},
                          {"X-Keep", "stays"}}));
  const auto seen = backend.last();
  for (const char* name : {"Connection", "Keep-Alive", "TE", "Trailer", "Upgrade", "Proxy-Authorization",
                           "Proxy-Connection", "X-Secret-Hop", "Expect", "Transfer-Encoding"}) {
    EXPECT_EQ(header(seen, name), "<absent>") << name;
  }
  EXPECT_EQ(header(seen, "X-Keep"), "stays");
  EXPECT_EQ(seen.body(), "data");
}

// =====================================================================================
// Request ids
// =====================================================================================

TEST_F(ProxyTest, ASuppliedRequestIdIsPreservedEverywhere) {
  ScriptedBackend backend;
  single(backend);
  const auto response = client()->request(http::verb::get, "/proxy/orders/r", {}, {{"X-Request-Id", "abc123"}});
  ASSERT_TRUE(response);
  EXPECT_EQ(header(*response, "X-Request-Id"), "abc123");
  EXPECT_EQ(header(backend.last(), "X-Request-Id"), "abc123");
  EXPECT_TRUE(log.contains("proxy [abc123]")) << "the id is in EdgeFlow's log";
}

TEST_F(ProxyTest, AMissingRequestIdIsGeneratedAndTheSameOnBothSides) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  std::set<std::string> ids;
  for (int i = 0; i < 20; ++i) {
    const auto response = c->get("/proxy/orders/r");
    ASSERT_TRUE(response);
    const auto id = header(*response, "X-Request-Id");
    ASSERT_NE(id, "<absent>");
    EXPECT_EQ(id.size(), 36U);
    EXPECT_EQ(header(backend.last(), "X-Request-Id"), id) << "the backend saw the id the client got";
    EXPECT_TRUE(log.contains("proxy [" + id + "]"));
    ids.insert(id);
  }
  EXPECT_EQ(ids.size(), 20U) << "every request gets its own id";
}

TEST_F(ProxyTest, GatewayErrorsCarryTheRequestIdToo) {
  ScriptedBackend backend;
  single(backend);
  registry->inner->updateInstance("orders", "a", healthUpdate(HealthStatus::Unhealthy));
  auto c = client();
  const auto unavailable = c->request(http::verb::get, "/proxy/orders/x", {}, {{"X-Request-Id", "id-503"}});
  ASSERT_TRUE(unavailable);
  EXPECT_EQ(unavailable->result(), http::status::service_unavailable);
  EXPECT_EQ(header(*unavailable, "X-Request-Id"), "id-503");
  const auto missing = c->request(http::verb::get, "/proxy/ghost/x", {}, {{"X-Request-Id", "id-404"}});
  EXPECT_EQ(header(*missing, "X-Request-Id"), "id-404");
  const auto bad = c->request(http::verb::get, "/proxy/", {}, {{"X-Request-Id", "id-400"}});
  EXPECT_EQ(header(*bad, "X-Request-Id"), "id-400");
  const auto generated = c->get("/proxy/");
  EXPECT_EQ(header(*generated, "X-Request-Id").size(), 36U);
}

// =====================================================================================
// Connection reuse and the pool
// =====================================================================================

TEST_F(ProxyTest, SequentialRequestsReuseOneBackendConnection) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  for (int i = 0; i < 12; ++i) {
    const auto response = c->get("/proxy/orders/seq" + std::to_string(i));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
  }
  EXPECT_EQ(backend.requests(), 12U);
  EXPECT_EQ(backend.connections(), 1U) << "twelve requests over ONE upstream connection";
  const auto stats = proxy->stats().pool;
  EXPECT_EQ(stats.created, 1U);
  EXPECT_EQ(stats.reused, 11U);
  EXPECT_EQ(proxy->pool().idleCount(), 1U);
  const auto received = backend.received();
  for (const auto& r : received) EXPECT_EQ(r.connection, 1U);
}

TEST_F(ProxyTest, DifferentClientsShareThePool) {
  ScriptedBackend backend;
  single(backend);
  for (int i = 0; i < 5; ++i) {
    auto c = client();  // a new client connection every time
    ASSERT_TRUE(c->get("/proxy/orders/x"));
  }
  EXPECT_EQ(backend.connections(), 1U);
}

TEST_F(ProxyTest, ConnectionCloseFromTheBackendMeansNoReuse) {
  ScriptedBackend backend;
  Action action = reply(200, "ok");
  action.keep_alive = false;
  backend.always(action);
  single(backend);
  auto c = client();
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_EQ(backend.connections(), 4U);
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
  EXPECT_EQ(proxy->stats().pool.reused, 0U);
  EXPECT_TRUE(waitFor([&] { return backend.openConnections() == 0; })) << "and none is left open";
}

TEST_F(ProxyTest, Http10BackendsAreNotReused) {
  ScriptedBackend backend;
  backend.setHandler([](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    Action action = reply(200, "old");
    action.kind = Kind::UntilEof;
    return action;
  });
  single(backend);
  auto c = client();
  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->body(), "old");
  }
  EXPECT_EQ(backend.connections(), 3U);
}

TEST_F(ProxyTest, AtMostMaxIdleConnectionsAreKeptAfterABurst) {
  proxy_config.max_idle_connections = 3;
  ScriptedBackend backend;
  auto gate = std::make_shared<Gate>();
  backend.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    Action action = reply(200, "ok");
    action.gate = gate;
    return action;
  });
  single(backend);

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      auto c = client();
      (void)c->get("/proxy/orders/x");
    });
  }
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 8; })) << "eight requests in flight at once";
  gate->open();
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(backend.connections(), 8U);
  ASSERT_TRUE(waitFor([&] { return proxy->pool().idleCount() == 3; }));
  EXPECT_EQ(proxy->pool().idleCount(), 3U) << "the surplus was closed";
  EXPECT_TRUE(waitFor([&] { return backend.openConnections() == 3; }));
}

TEST_F(ProxyTest, APooledConnectionThatDiedWhileIdleIsReplacedBeforeAnyByteIsSent) {
  ScriptedBackend backend([](const ScriptedBackend::Request&, std::uint64_t connection, std::size_t) {
    Action action = reply(200, "answer");
    action.close_after = connection == 1;  // answers, then silently closes the (pooled) connection
    return action;
  });
  single(backend);
  auto c = client();

  const auto first = c->get("/proxy/orders/one");
  ASSERT_TRUE(first);
  EXPECT_EQ(first->result(), http::status::ok);
  ASSERT_TRUE(waitFor([&] { return proxy->pool().idleCount() == 1; })) << "pooled after a keep-alive answer";
  ASSERT_TRUE(waitFor([&] { return backend.peerClosed() == 0 && backend.openConnections() == 0; }))
      << "the backend closed it";
  std::this_thread::sleep_for(100ms);  // let the FIN arrive

  const auto second = c->get("/proxy/orders/two");
  ASSERT_TRUE(second);
  EXPECT_EQ(second->result(), http::status::ok) << "the request succeeds on a fresh connection";
  EXPECT_EQ(second->body(), "answer");

  EXPECT_EQ(backend.connections(), 2U) << "a new connection was opened";
  EXPECT_EQ(backend.requests(), 2U) << "and the request was sent exactly once";
  const auto received = backend.received();
  ASSERT_EQ(received.size(), 2U);
  EXPECT_EQ(received[0].connection, 1U);
  EXPECT_EQ(received[1].connection, 2U);
  const auto stats = proxy->stats().pool;
  EXPECT_EQ(stats.stale_dropped, 1U) << "the dead connection was detected and discarded";
  EXPECT_EQ(stats.created, 2U);
}

TEST_F(ProxyTest, ARequestThatWasAlreadySentIsNeverRepeated) {
  ScriptedBackend backend([](const ScriptedBackend::Request&, std::uint64_t connection, std::size_t n) {
    if (connection == 1 && n == 2) return ofKind(Kind::Close);  // reads the request, hangs up
    return reply(200, "fine");
  });
  single(backend);
  auto c = client();

  ASSERT_TRUE(c->get("/proxy/orders/one"));
  ASSERT_EQ(backend.connections(), 1U);

  const auto failed = c->request(http::verb::post, "/proxy/orders/two", "important payload");
  ASSERT_TRUE(failed) << "the client connection is not closed by a backend failure";
  EXPECT_EQ(failed->result(), http::status::bad_gateway);
  EXPECT_EQ(backend.requests(), 2U) << "the second request reached the backend ONCE and was not retried";
  EXPECT_EQ(backend.connections(), 1U) << "no new connection was opened for a retry";

  // The broken connection was discarded, not pooled: the next request opens a fresh one.
  const auto third = c->get("/proxy/orders/three");
  ASSERT_TRUE(third);
  EXPECT_EQ(third->result(), http::status::ok);
  EXPECT_EQ(backend.connections(), 2U);
  EXPECT_EQ(backend.requests(), 3U);
}

TEST_F(ProxyTest, NoRetryOnAnHttpStatusEither) {
  ScriptedBackend backend;
  backend.always(reply(503, "busy"));
  single(backend);
  const auto response = client()->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(response->body(), "busy");
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(backend.requests(), 1U);
}

TEST_F(ProxyTest, AFailedExchangeNeverReturnsItsConnectionToThePool) {
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  ASSERT_TRUE(c->get("/proxy/orders/ok"));
  EXPECT_EQ(proxy->pool().idleCount(), 1U);

  backend.always(ofKind(Kind::Garbage));
  const auto garbage = c->get("/proxy/orders/bad");
  ASSERT_TRUE(garbage);
  EXPECT_EQ(garbage->result(), http::status::bad_gateway);
  EXPECT_EQ(proxy->pool().idleCount(), 0U) << "the pooled connection used for the failed request is gone";
}

// =====================================================================================
// Routing: all strategies, health, counts
// =====================================================================================

struct Trio {
  ScriptedBackend a{nullptr, "a"};
  ScriptedBackend b{nullptr, "b"};
  ScriptedBackend c{nullptr, "c"};
};

TEST_F(ProxyTest, RoundRobinDrivesTheProxy) {
  Trio backends;
  addInstance("shop", "a", backends.a.port());
  addInstance("shop", "b", backends.b.port());
  addInstance("shop", "c", backends.c.port());
  startGateway(RoutingStrategy::RoundRobin);
  auto c = client();
  std::string order;
  for (int i = 0; i < 9; ++i) {
    const auto response = c->get("/proxy/shop/x");
    ASSERT_TRUE(response);
    order += whoAnswered(*response);
  }
  EXPECT_EQ(order, "abcabcabc");
}

TEST_F(ProxyTest, LeastConnectionsDrivesTheProxy) {
  Trio backends;
  addInstance("shop", "a", backends.a.port(), HealthStatus::Healthy, InstanceStatus::Active, 1, 10);
  addInstance("shop", "b", backends.b.port(), HealthStatus::Healthy, InstanceStatus::Active, 1, 2);
  addInstance("shop", "c", backends.c.port(), HealthStatus::Healthy, InstanceStatus::Active, 1, 20);
  startGateway(RoutingStrategy::LeastConnections);
  auto c = client();
  for (int i = 0; i < 10; ++i) {
    const auto response = c->get("/proxy/shop/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(whoAnswered(*response), "b") << "b has the fewest connections";
  }
  EXPECT_TRUE(waitFor([&] { return connectionCount("shop", "b") == 2; })) << "and its count is back to what it was";
  EXPECT_EQ(connectionCount("shop", "a"), 10U);
  EXPECT_EQ(connectionCount("shop", "c"), 20U);
}

TEST_F(ProxyTest, WeightedRoutingDrivesTheProxy) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  addInstance("shop", "a", a.port(), HealthStatus::Healthy, InstanceStatus::Active, 3);
  addInstance("shop", "b", b.port(), HealthStatus::Healthy, InstanceStatus::Active, 1);
  startGateway(RoutingStrategy::Weighted);
  auto c = client();
  std::map<std::string, int> counts;
  for (int i = 0; i < 40; ++i) {
    const auto response = c->get("/proxy/shop/x");
    ASSERT_TRUE(response);
    ++counts[whoAnswered(*response)];
  }
  EXPECT_EQ(counts["a"], 30);
  EXPECT_EQ(counts["b"], 10);
}

TEST_F(ProxyTest, ConsistentHashingDrivesTheProxyAndIsStickyPerClientAddress) {
  Trio backends;
  addInstance("shop", "a", backends.a.port());
  addInstance("shop", "b", backends.b.port());
  addInstance("shop", "c", backends.c.port());
  startGateway(RoutingStrategy::ConsistentHashing);
  std::set<std::string> answered;
  for (int i = 0; i < 12; ++i) {
    auto c = client();
    const auto response = c->get("/proxy/shop/x");
    ASSERT_TRUE(response);
    answered.insert(whoAnswered(*response));
  }
  EXPECT_EQ(answered.size(), 1U) << "the key is the client address, so one client always lands on one backend";
}

TEST_F(ProxyTest, UnhealthyDrainingAndDisabledInstancesAreNeverUsedByAnyStrategy) {
  for (const auto strategy : {RoutingStrategy::RoundRobin, RoutingStrategy::LeastConnections,
                              RoutingStrategy::Weighted, RoutingStrategy::ConsistentHashing}) {
    SCOPED_TRACE(std::string{config::toString(strategy)});
    // fresh gateway and registry for each strategy
    registry = std::make_shared<FlakyRegistry>();
    ScriptedBackend good(nullptr, "good");
    ScriptedBackend unhealthy(nullptr, "unhealthy");
    ScriptedBackend draining(nullptr, "draining");
    ScriptedBackend disabled(nullptr, "disabled");
    ScriptedBackend unknown(nullptr, "unknown");
    addInstance("svc", "good", good.port(), HealthStatus::Healthy, InstanceStatus::Active, 1, 50);
    // all the others look more attractive: huge weight, no connections
    addInstance("svc", "u", unhealthy.port(), HealthStatus::Unhealthy, InstanceStatus::Active, 1000, 0);
    addInstance("svc", "d", draining.port(), HealthStatus::Healthy, InstanceStatus::Draining, 1000, 0);
    addInstance("svc", "x", disabled.port(), HealthStatus::Healthy, InstanceStatus::Disabled, 1000, 0);
    addInstance("svc", "k", unknown.port(), HealthStatus::Unknown, InstanceStatus::Active, 1000, 0);
    startGateway(strategy);
    for (int i = 0; i < 20; ++i) {
      auto c = client();
      const auto response = c->get("/proxy/svc/x");
      ASSERT_TRUE(response);
      ASSERT_EQ(response->result(), http::status::ok);
      ASSERT_EQ(whoAnswered(*response), "good");
    }
    EXPECT_EQ(unhealthy.requests() + draining.requests() + disabled.requests() + unknown.requests(), 0U);
    stopGateway();
    server.reset();
    proxy.reset();
  }
}

TEST_F(ProxyTest, AnInstanceThatBecomesUnhealthyStopsReceivingRequests) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  addInstance("shop", "a", a.port());
  addInstance("shop", "b", b.port());
  startGateway(RoutingStrategy::RoundRobin);
  auto c = client();
  std::set<std::string> before;
  for (int i = 0; i < 6; ++i) before.insert(whoAnswered(*c->get("/proxy/shop/x")));
  EXPECT_EQ(before, (std::set<std::string>{"a", "b"}));

  ASSERT_TRUE(registry->inner->updateInstance("shop", "a", healthUpdate(HealthStatus::Unhealthy)).ok());
  const auto a_before = a.requests();
  for (int i = 0; i < 10; ++i) EXPECT_EQ(whoAnswered(*c->get("/proxy/shop/x")), "b");
  EXPECT_EQ(a.requests(), a_before);

  ASSERT_TRUE(registry->inner->updateInstance("shop", "a", healthUpdate(HealthStatus::Healthy)).ok());
  std::set<std::string> after;
  for (int i = 0; i < 6; ++i) after.insert(whoAnswered(*c->get("/proxy/shop/x")));
  EXPECT_EQ(after, (std::set<std::string>{"a", "b"})) << "and a recovered one is used again";
}

TEST_F(ProxyTest, NothingRoutableIs503AndTouchesNoBackend) {
  ScriptedBackend backend;
  addInstance("orders", "a", backend.port(), HealthStatus::Unhealthy);
  startGateway();
  const auto response = client()->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::service_unavailable);
  EXPECT_EQ(nlohmann::json::parse(response->body())["status"], 503);
  EXPECT_EQ(backend.requests(), 0U);
  EXPECT_EQ(backend.connections(), 0U);
  EXPECT_EQ(proxy->stats().active, 0U);
}

TEST_F(ProxyTest, ARegistryOutageIs503) {
  ScriptedBackend backend;
  single(backend);
  registry->routable_unavailable.store(true);
  auto c = client();
  const auto down = c->get("/proxy/orders/x");
  ASSERT_TRUE(down);
  EXPECT_EQ(down->result(), http::status::service_unavailable);
  EXPECT_EQ(backend.requests(), 0U);
  EXPECT_EQ(c->get("/health")->result(), http::status::ok) << "the gateway itself stays up";
  registry->routable_unavailable.store(false);
  EXPECT_EQ(c->get("/proxy/orders/x")->result(), http::status::ok) << "and recovers";
}

TEST_F(ProxyTest, TheConnectionCountRisesDuringARequestAndReturnsAfterIt) {
  ScriptedBackend backend;
  auto gate = std::make_shared<Gate>();
  backend.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    Action action = reply(200, "slow");
    action.gate = gate;
    return action;
  });
  single(backend);
  EXPECT_EQ(connectionCount("orders", "a"), 0U);

  std::promise<std::optional<network::HttpResponse>> result;
  std::thread requester([&] { result.set_value(client()->get("/proxy/orders/x")); });
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 1; }));
  EXPECT_EQ(connectionCount("orders", "a"), 1U) << "one request in flight";
  EXPECT_EQ(proxy->stats().active, 1U);

  gate->open();
  requester.join();
  const auto response = result.get_future().get();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(connectionCount("orders", "a"), 0U) << "already released when the client holds its response";
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

TEST_F(ProxyTest, LeastConnectionsSeesRequestsInFlight) {
  ScriptedBackend a(nullptr, "a");
  ScriptedBackend b(nullptr, "b");
  auto gate = std::make_shared<Gate>();
  a.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    Action action = reply(200, "a|held|");
    action.gate = gate;
    return action;
  });
  addInstance("shop", "a", a.port());
  addInstance("shop", "b", b.port());
  startGateway(RoutingStrategy::LeastConnections);

  // Both idle: the tie goes to "a", which holds the request.
  std::promise<std::optional<network::HttpResponse>> first;
  std::thread holder([&] { first.set_value(client()->get("/proxy/shop/x")); });
  ASSERT_TRUE(waitFor([&] { return a.requests() == 1; }));
  ASSERT_EQ(connectionCount("shop", "a"), 1U);

  // While "a" is busy, new requests go to "b".
  auto c = client();
  for (int i = 0; i < 5; ++i) EXPECT_EQ(whoAnswered(*c->get("/proxy/shop/x")), "b");
  EXPECT_EQ(a.requests(), 1U);

  gate->open();
  holder.join();
  EXPECT_TRUE(first.get_future().get().has_value());
  EXPECT_TRUE(countsReturnToZero("shop", {"a", "b"}));
}

// =====================================================================================
// Backend failures
// =====================================================================================

TEST_F(ProxyTest, ConnectionRefusedIs502AndTheClientConnectionSurvives) {
  addInstance("orders", "a", closedPort());
  startGateway();
  auto c = client();
  const auto response = c->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_gateway);
  const auto body = nlohmann::json::parse(response->body());
  EXPECT_EQ(body["status"], 502);
  EXPECT_EQ(body["detail"], "the backend could not be reached");
  EXPECT_EQ(response->body().find("127.0.0.1"), std::string::npos) << "no internal addresses in the answer";
  ASSERT_EQ(c->get("/health")->result(), http::status::ok) << "the client's connection is still open and usable";
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  EXPECT_TRUE(log.contains("connect"));
}

TEST_F(ProxyTest, EveryTransportFailureIs502) {
  struct Scenario {
    const char* name;
    Action action;
  };
  std::vector<Scenario> scenarios;
  scenarios.push_back({"closed without a response", ofKind(Kind::Close)});
  scenarios.push_back({"connection reset", ofKind(Kind::Reset)});
  scenarios.push_back({"not HTTP", ofKind(Kind::Garbage)});
  Action truncated = reply(200, "only the beginning");
  truncated.kind = Kind::Truncated;
  scenarios.push_back({"body cut short", truncated});

  ScriptedBackend backend;
  single(backend);
  auto c = client();
  for (const auto& scenario : scenarios) {
    SCOPED_TRACE(scenario.name);
    backend.always(scenario.action);
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response) << "the client always gets an answer";
    EXPECT_EQ(response->result(), http::status::bad_gateway);
    EXPECT_NE(header(*response, "X-Request-Id"), "<absent>");
  }
  EXPECT_EQ(c->get("/health")->result(), http::status::ok);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  EXPECT_EQ(proxy->pool().idleCount(), 0U) << "no failed connection was pooled";
}

TEST_F(ProxyTest, ANameThatCannotBeResolvedIs502) {
  auto resolver = std::make_shared<discovery::NameResolver>(
      [](const std::string&, const std::string&, boost::system::error_code& error) {
        error = net::error::host_not_found;
        return net::ip::tcp::resolver::results_type{};
      });
  addInstance("orders", "a", 9000, HealthStatus::Healthy, InstanceStatus::Active, 1, 0, "no-such-host.test");
  startGateway(RoutingStrategy::RoundRobin, resolver);
  const auto response = client()->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::bad_gateway);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(ProxyTest, BackendsAreReachedByNameToo) {
  ScriptedBackend backend;
  const auto port = backend.port();
  auto resolver = std::make_shared<discovery::NameResolver>(
      [port](const std::string& host, const std::string& service, boost::system::error_code& error) {
        error = {};
        return net::ip::tcp::resolver::results_type::create(
            net::ip::tcp::endpoint{net::ip::make_address("127.0.0.1"), port}, host, service);
      });
  addInstance("orders", "a", port, HealthStatus::Healthy, InstanceStatus::Active, 1, 0, "backend.test");
  startGateway(RoutingStrategy::RoundRobin, resolver);
  auto c = client();
  for (int i = 0; i < 3; ++i) {
    const auto response = c->get("/proxy/orders/x");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->result(), http::status::ok);
  }
  EXPECT_EQ(header(backend.last(), "Host"), "backend.test:" + std::to_string(port));
  EXPECT_EQ(backend.connections(), 1U) << "and its connection is pooled under that name";
}

TEST_F(ProxyTest, ASilentBackendTimesOutWith504) {
  proxy_config.upstream_timeout = 300ms;
  ScriptedBackend backend;
  backend.always(ofKind(Kind::Stall));
  single(backend);
  auto c = client();

  const auto begin = std::chrono::steady_clock::now();
  const auto response = c->get("/proxy/orders/slow");
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::gateway_timeout);
  EXPECT_GE(elapsed, 280ms);
  EXPECT_LT(elapsed, 3000ms) << "the unresponsive backend did not hold the request";
  EXPECT_NE(header(*response, "X-Request-Id"), "<absent>");

  EXPECT_TRUE(waitFor([&] { return backend.peerClosed() == 1; })) << "the timed-out connection was closed";
  EXPECT_EQ(proxy->pool().idleCount(), 0U) << "and not returned to the pool";
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  ASSERT_EQ(c->get("/health")->result(), http::status::ok) << "the client connection survived";
}

TEST_F(ProxyTest, ABackendThatStallsMidBodyTimesOutToo) {
  proxy_config.upstream_timeout = 300ms;
  ScriptedBackend backend;
  backend.always(ofKind(Kind::PartialStall, "partial"));
  single(backend);
  const auto response = client()->get("/proxy/orders/slow");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::gateway_timeout);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(ProxyTest, TheDeadlineCoversASlowButEventuallyAnsweringBackend) {
  proxy_config.upstream_timeout = 300ms;
  ScriptedBackend backend;
  Action slow = reply(200, "too late");
  slow.delay = 1000ms;
  backend.always(slow);
  single(backend);
  const auto begin = std::chrono::steady_clock::now();
  const auto response = client()->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::gateway_timeout);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 900ms);
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
}

TEST_F(ProxyTest, ConnectTimeoutIs504) {
  proxy_config.connect_timeout = 200ms;
  proxy_config.upstream_timeout = 5000ms;
  auto resolver = std::make_shared<discovery::NameResolver>(
      [](const std::string&, const std::string&, boost::system::error_code& error) {
        std::this_thread::sleep_for(1500ms);  // name resolution that does not finish in time
        error = net::error::host_not_found;
        return net::ip::tcp::resolver::results_type{};
      });
  addInstance("orders", "a", 9000, HealthStatus::Healthy, InstanceStatus::Active, 1, 0, "slow-dns.test");
  startGateway(RoutingStrategy::RoundRobin, resolver);
  const auto begin = std::chrono::steady_clock::now();
  const auto response = client()->get("/proxy/orders/x");
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::gateway_timeout);
  EXPECT_GE(elapsed, 180ms);
  EXPECT_LT(elapsed, 1200ms);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  std::this_thread::sleep_for(1500ms);  // let the detached lookup thread finish before teardown
}

TEST_F(ProxyTest, ATooLargeResponseIs502AndLeavesNothingBehind) {
  proxy_config.max_response_bytes = 64 * 1024;
  ScriptedBackend backend;
  backend.setHandler([](const ScriptedBackend::Request& request, std::uint64_t, std::size_t) {
    return reply(200, request.target() == "/big" ? std::string(1024 * 1024, 'z') : "small");
  });
  single(backend);
  auto c = client();
  const auto big = c->get("/proxy/orders/big");
  ASSERT_TRUE(big);
  EXPECT_EQ(big->result(), http::status::bad_gateway);
  EXPECT_LT(big->body().size(), 1024U) << "the oversize body was not passed on";
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));

  const auto small = c->get("/proxy/orders/small");
  ASSERT_TRUE(small);
  EXPECT_EQ(small->result(), http::status::ok);
  EXPECT_EQ(small->body(), "small") << "a response exactly within the limit side is fine";
}

TEST_F(ProxyTest, ABackendError500IsForwardedNot502) {
  ScriptedBackend backend;
  backend.always(reply(500, "the application failed"));
  single(backend);
  const auto response = client()->get("/proxy/orders/x");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::internal_server_error);
  EXPECT_EQ(response->body(), "the application failed");
}

TEST_F(ProxyTest, ConnectionCountsAreReleasedOnEveryOutcome) {
  proxy_config.upstream_timeout = 300ms;
  proxy_config.max_response_bytes = 4096;
  ScriptedBackend backend;
  single(backend);
  auto c = client();
  const std::vector<Action> outcomes = [&] {
    std::vector<Action> list;
    list.push_back(reply(200, "ok"));
    list.push_back(reply(500, "app error"));
    list.push_back(reply(404, "missing"));
    list.push_back(ofKind(Kind::Close));
    list.push_back(ofKind(Kind::Reset));
    list.push_back(ofKind(Kind::Garbage));
    list.push_back(ofKind(Kind::Stall));
    list.push_back(reply(200, std::string(100000, 'x')));
    return list;
  }();
  for (const auto& outcome : outcomes) {
    backend.always(outcome);
    ASSERT_TRUE(c->get("/proxy/orders/x"));
    EXPECT_TRUE(countsReturnToZero("orders", {"a"})) << "after outcome kind " << static_cast<int>(outcome.kind);
  }
  // Refused connection and unresolvable name leave nothing behind either.
  registry->inner->deregisterInstance("orders", "a");
  addInstance("orders", "dead", closedPort());
  ASSERT_TRUE(c->get("/proxy/orders/x"));
  EXPECT_TRUE(countsReturnToZero("orders", {"dead"}));
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
}

TEST_F(ProxyTest, AClientThatLeavesMidRequestLeaksNothing) {
  proxy_config.upstream_timeout = 500ms;
  ScriptedBackend backend;
  backend.always(ofKind(Kind::Stall));
  single(backend);
  {
    auto gone = client();
    gone->sendRaw("GET /proxy/orders/x HTTP/1.1\r\nHost: gw\r\n\r\n");
    ASSERT_TRUE(waitFor([&] { return backend.requests() == 1; }));
    gone->reset();  // RST while the proxy waits for the backend
  }
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; })) << "the exchange still ends (at the deadline)";
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
  EXPECT_EQ(client()->get("/health")->result(), http::status::ok) << "and the server is unharmed";
}

// =====================================================================================
// The asynchronous design
// =====================================================================================

TEST_F(ProxyTest, SlowBackendsDoNotBlockTheServersIoWorkers) {
  ScriptedBackend backend;
  auto gate = std::make_shared<Gate>();
  backend.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    Action action = reply(200, "released");
    action.gate = gate;
    return action;
  });
  single(backend);  // the server has only 2 I/O worker threads

  constexpr int kSlow = 10;
  std::vector<std::future<std::optional<network::HttpResponse>>> slow;
  std::vector<std::thread> threads;
  for (int i = 0; i < kSlow; ++i) {
    auto promise = std::make_shared<std::promise<std::optional<network::HttpResponse>>>();
    slow.push_back(promise->get_future());
    threads.emplace_back([this, promise] { promise->set_value(client()->get("/proxy/orders/x")); });
  }
  ASSERT_TRUE(waitFor([&] { return backend.requests() == static_cast<std::size_t>(kSlow); }))
      << "ten proxied requests are waiting for the backend, more than there are I/O workers";

  // The server still answers promptly: no worker is tied up by a waiting request.
  for (int i = 0; i < 20; ++i) {
    const auto begin = std::chrono::steady_clock::now();
    const auto health = client()->get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->result(), http::status::ok);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 1000ms);
  }
  EXPECT_EQ(proxy->stats().active, static_cast<std::uint64_t>(kSlow));

  gate->open();
  for (auto& thread : threads) thread.join();
  for (auto& future : slow) {
    const auto response = future.get();
    ASSERT_TRUE(response);
    EXPECT_EQ(response->body(), "released");
  }
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
}

TEST_F(ProxyTest, TheSynchronousInterfaceStillWorks) {
  ScriptedBackend backend;
  single(backend);
  network::HttpRequest request{http::verb::get, "/proxy/orders/sync", 11};
  request.set(http::field::host, "x");
  const auto proxied = proxy->handle(request);
  EXPECT_EQ(proxied.result(), http::status::ok);
  EXPECT_EQ(whoAnswered(proxied), "backend");
  network::HttpRequest local{http::verb::get, "/health", 11};
  EXPECT_EQ(proxy->handle(local).result(), http::status::ok);
}

// =====================================================================================
// Concurrency
// =====================================================================================

TEST_F(ProxyTest, ConcurrentClientsWithRoundRobin) { runConcurrentTraffic(RoutingStrategy::RoundRobin); }
TEST_F(ProxyTest, ConcurrentClientsWithLeastConnections) { runConcurrentTraffic(RoutingStrategy::LeastConnections); }
TEST_F(ProxyTest, ConcurrentClientsWithWeightedRouting) { runConcurrentTraffic(RoutingStrategy::Weighted); }

TEST_F(ProxyTest, ConcurrentClientsWhileABackendFails) {
  ScriptedBackend good1(nullptr, "a");
  ScriptedBackend good2(nullptr, "b");
  ScriptedBackend broken(nullptr, "c");
  broken.always(ofKind(Kind::Reset));
  addInstance("shop", "a", good1.port());
  addInstance("shop", "b", good2.port());
  addInstance("shop", "c", broken.port());
  startGateway(RoutingStrategy::RoundRobin);

  std::atomic<int> ok{0};
  std::atomic<int> gateway{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      auto c = client();
      for (int i = 0; i < 30; ++i) {
        const auto response = c->get("/proxy/shop/x");
        if (!response) {
          other.fetch_add(1);
        } else if (response->result() == http::status::ok) {
          ok.fetch_add(1);
        } else if (response->result() == http::status::bad_gateway) {
          gateway.fetch_add(1);
        } else {
          other.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(ok.load() + gateway.load(), 240) << "every client got a proper answer, none was dropped";
  EXPECT_EQ(other.load(), 0);
  EXPECT_GT(ok.load(), 0);
  EXPECT_GT(gateway.load(), 0) << "no failover or retry exists in this phase: the broken backend's share fails";
  EXPECT_TRUE(waitFor([&] { return proxy->stats().active == 0; }));
  EXPECT_TRUE(countsReturnToZero("shop", {"a", "b", "c"}));
}

// =====================================================================================
// Shutdown
// =====================================================================================

TEST_F(ProxyTest, ARequestInFlightDuringShutdownCompletesWithinTheGracePeriod) {
  ScriptedBackend backend;
  Action slow = reply(200, "finished");
  slow.delay = 400ms;
  backend.always(slow);
  single(backend);

  std::promise<std::optional<network::HttpResponse>> result;
  std::thread requester([&] { result.set_value(client()->get("/proxy/orders/x")); });
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 1; }));

  const auto begin = std::chrono::steady_clock::now();
  stopGateway(3000ms);  // the server drains: the request may finish
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  requester.join();
  const auto response = result.get_future().get();
  ASSERT_TRUE(response) << "the client was not abandoned";
  EXPECT_EQ(response->result(), http::status::ok);
  EXPECT_EQ(response->body(), "finished");
  EXPECT_EQ(header(*response, "Connection"), "close") << "and was told the connection ends";
  EXPECT_LT(elapsed, 2500ms) << "shutdown did not wait for the whole grace period";
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  EXPECT_EQ(proxy->stats().active, 0U);
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
}

TEST_F(ProxyTest, RequestsStillRunningWhenTheGracePeriodEndsAreCancelled) {
  ScriptedBackend backend;
  backend.always(ofKind(Kind::Stall));
  single(backend);

  std::promise<std::optional<network::HttpResponse>> result;
  std::thread requester([&] { result.set_value(client()->get("/proxy/orders/x")); });
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 1; }));
  ASSERT_EQ(connectionCount("orders", "a"), 1U);

  const auto begin = std::chrono::steady_clock::now();
  stopGateway(300ms);
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  requester.join();
  EXPECT_FALSE(result.get_future().get().has_value()) << "the client connection was closed, not left hanging";
  EXPECT_LT(elapsed, 4000ms);
  EXPECT_GE(elapsed, 250ms) << "the grace period was honoured first";

  EXPECT_TRUE(countsReturnToZero("orders", {"a"})) << "the connection count was released";
  EXPECT_EQ(proxy->stats().active, 0U);
  EXPECT_TRUE(waitFor([&] { return backend.peerClosed() == 1; })) << "the upstream connection was closed";
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
  EXPECT_TRUE(log.contains("reverse proxy stopped"));
}

TEST_F(ProxyTest, ManyRequestsInFlightAtShutdownAreAllReleased) {
  ScriptedBackend backend;
  auto gate = std::make_shared<Gate>();
  backend.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
    Action action = reply(200, "never");
    action.gate = gate;
    return action;
  });
  single(backend);
  std::vector<std::thread> threads;
  std::atomic<int> answered{0};
  for (int i = 0; i < 12; ++i) {
    threads.emplace_back([&] {
      if (client()->get("/proxy/orders/x")) ++answered;
    });
  }
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 12; }));
  ASSERT_EQ(connectionCount("orders", "a"), 12U);
  stopGateway(200ms);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(answered.load(), 0);
  EXPECT_TRUE(countsReturnToZero("orders", {"a"}));
  EXPECT_EQ(proxy->stats().active, 0U);
  gate->open();
}

TEST_F(ProxyTest, StoppingTheProxyAnswersRunningRequestsAndReleasesEverything) {
  ScriptedBackend backend;
  backend.always(ofKind(Kind::Stall));
  single(backend);

  network::HttpRequest request{http::verb::get, "/proxy/orders/x", 11};
  request.set(http::field::host, "x");
  std::promise<network::HttpResponse> answer;
  (void)proxy->handleAsync(request, network::RequestContext{"127.0.0.1", 1},
                           [&answer](network::HttpResponse response) { answer.set_value(std::move(response)); });
  ASSERT_TRUE(waitFor([&] { return backend.requests() == 1; }));
  ASSERT_EQ(connectionCount("orders", "a"), 1U);

  proxy->stop();
  auto future = answer.get_future();
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready) << "the waiting caller was answered";
  EXPECT_EQ(future.get().result(), http::status::service_unavailable);
  EXPECT_EQ(connectionCount("orders", "a"), 0U) << "stop() returns only after the counts were released";
  EXPECT_EQ(proxy->stats().active, 0U);
}

TEST_F(ProxyTest, AStoppedProxyRefusesNewProxyRequestsButNotOthers) {
  ScriptedBackend backend;
  single(backend);
  proxy->stop();
  EXPECT_NO_THROW(proxy->stop()) << "idempotent";
  auto c = client();
  const auto refused = c->get("/proxy/orders/x");
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->result(), http::status::service_unavailable);
  EXPECT_EQ(backend.requests(), 0U);
  EXPECT_EQ(c->get("/health")->result(), http::status::ok) << "everything else is unaffected";
}

TEST_F(ProxyTest, StoppingReleasesPooledConnections) {
  ScriptedBackend backend;
  single(backend);
  ASSERT_TRUE(client()->get("/proxy/orders/x"));
  ASSERT_EQ(proxy->pool().idleCount(), 1U);
  ASSERT_EQ(backend.openConnections(), 1U);
  proxy->stop();
  EXPECT_EQ(proxy->pool().idleCount(), 0U);
  EXPECT_TRUE(waitFor([&] { return backend.openConnections() == 0; })) << "the backend sees the connection closed";
}

}  // namespace
