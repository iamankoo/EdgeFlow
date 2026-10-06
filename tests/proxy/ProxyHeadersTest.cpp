#include <gtest/gtest.h>

#include <set>
#include <string>

#include "edgeflow/proxy/ProxyHeaders.hpp"

namespace {

using namespace edgeflow::proxy;  // NOLINT: test-only convenience
using edgeflow::network::HttpRequest;
using edgeflow::network::HttpResponse;
namespace http = edgeflow::network::http;

HttpRequest makeRequest(http::verb method = http::verb::get, const std::string& target = "/proxy/orders/x",
                        unsigned version = 11) {
  HttpRequest request{method, target, version};
  request.set(http::field::host, "gateway.example:8080");
  return request;
}

ForwardInfo info() {
  ForwardInfo forward;
  forward.client_address = "203.0.113.9";
  forward.upstream_host = "10.0.0.5:9001";
  forward.request_id = "rid-1";
  return forward;
}

std::string header(const HttpRequest& request, const std::string& name) {
  const auto it = request.find(name);
  return it == request.end() ? "<absent>" : std::string{it->value()};
}

std::string header(const HttpResponse& response, const std::string& name) {
  const auto it = response.find(name);
  return it == response.end() ? "<absent>" : std::string{it->value()};
}

std::size_t count(const HttpRequest& request, const std::string& name) {
  std::size_t n = 0;
  for (const auto& field : request) {
    if (boost::beast::iequals(field.name_string(), name)) ++n;
  }
  return n;
}

// --- service mapping ---------------------------------------------------------------

TEST(ProxyTargetTest, RecognisesOnlyTheProxyPrefix) {
  EXPECT_TRUE(isProxyTarget("/proxy"));
  EXPECT_TRUE(isProxyTarget("/proxy/"));
  EXPECT_TRUE(isProxyTarget("/proxy/orders"));
  EXPECT_TRUE(isProxyTarget("/proxy/orders/api?x=1"));
  EXPECT_TRUE(isProxyTarget("/proxy?x=1"));

  EXPECT_FALSE(isProxyTarget("/"));
  EXPECT_FALSE(isProxyTarget("/health"));
  EXPECT_FALSE(isProxyTarget("/echo"));
  EXPECT_FALSE(isProxyTarget("/services"));
  EXPECT_FALSE(isProxyTarget("/services/orders/route"));
  EXPECT_FALSE(isProxyTarget("/proxyfoo"));
  EXPECT_FALSE(isProxyTarget("/Proxy/orders")) << "paths are case sensitive";
  EXPECT_FALSE(isProxyTarget("/x/proxy/orders"));
  EXPECT_FALSE(isProxyTarget("http://example.com/proxy/orders")) << "absolute-form is not a proxy URL";
  EXPECT_FALSE(isProxyTarget("*"));
  EXPECT_FALSE(isProxyTarget("/services?next=/proxy/orders"));
}

struct MappingCase {
  const char* target;
  const char* service;
  const char* upstream;
};

TEST(ProxyTargetTest, StripsTheServicePrefix) {
  const MappingCase cases[] = {
      {"/proxy/orders/", "orders", "/"},
      {"/proxy/orders", "orders", "/"},
      {"/proxy/orders/api/users", "orders", "/api/users"},
      {"/proxy/payment/v1/charge", "payment", "/v1/charge"},
      {"/proxy/orders/api/users?id=42", "orders", "/api/users?id=42"},
      {"/proxy/orders?x=1", "orders", "/?x=1"},
      {"/proxy/orders/?x=1&y=2", "orders", "/?x=1&y=2"},
      {"/proxy/orders//double//slash", "orders", "//double//slash"},
      {"/proxy/orders/a%20b/c%2Fd", "orders", "/a%20b/c%2Fd"},
      {"/proxy/orders/proxy/orders", "orders", "/proxy/orders"},
      {"/proxy/a.b-c_d/x", "a.b-c_d", "/x"},
      {"/proxy/orders/x?redirect=/proxy/other", "orders", "/x?redirect=/proxy/other"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.target);
    const auto mapped = mapProxyTarget(c.target);
    ASSERT_TRUE(mapped.route.has_value()) << mapped.detail;
    EXPECT_EQ(mapped.route->service, c.service);
    EXPECT_EQ(mapped.route->upstream_target, c.upstream);
  }
}

TEST(ProxyTargetTest, MalformedProxyUrlsAreClientErrorsNotGuesses) {
  for (const char* target : {"/proxy", "/proxy/", "/proxy//x", "/proxy?x=1", "/proxy/?x=1"}) {
    SCOPED_TRACE(target);
    const auto mapped = mapProxyTarget(target);
    EXPECT_FALSE(mapped.route.has_value());
    ASSERT_TRUE(mapped.error.has_value());
    EXPECT_EQ(*mapped.error, ProxyTargetError::MissingService);
    EXPECT_FALSE(mapped.detail.empty());
  }
  for (const char* target : {"/proxy/Orders/x", "/proxy/ord%65rs/x", "/proxy/-orders/x",
                             "/proxy/orders-/x", "/proxy/or ders/x", "/proxy/%2e%2e/x",
                             "/proxy/..", "/proxy/./x", "/proxy/ord;ers/x",
                             "/proxy/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/x"}) {
    SCOPED_TRACE(target);
    const auto mapped = mapProxyTarget(target);
    EXPECT_FALSE(mapped.route.has_value());
    ASSERT_TRUE(mapped.error.has_value());
    EXPECT_EQ(*mapped.error, ProxyTargetError::InvalidService);
  }
}

// --- request ids -------------------------------------------------------------------

TEST(RequestIdTest, GeneratedIdsAreUuidShapedAndUnique) {
  std::set<std::string> seen;
  for (int i = 0; i < 2000; ++i) {
    const auto id = generateRequestId();
    ASSERT_EQ(id.size(), 36U) << id;
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');
    EXPECT_EQ(id[14], '4') << "version nibble: " << id;
    EXPECT_NE(std::string{"89ab"}.find(id[19]), std::string::npos) << "variant nibble: " << id;
    for (std::size_t k = 0; k < id.size(); ++k) {
      if (k == 8 || k == 13 || k == 18 || k == 23) continue;
      EXPECT_NE(std::string{"0123456789abcdef"}.find(id[k]), std::string::npos) << id;
    }
    seen.insert(id);
  }
  EXPECT_EQ(seen.size(), 2000U) << "ids must not repeat";
}

TEST(RequestIdTest, ASuppliedIdIsPreserved) {
  auto request = makeRequest();
  request.set("X-Request-Id", "abc123");
  EXPECT_EQ(chooseRequestId(request), "abc123");
  request.set("x-request-id", "Trace/42:a.b_c-d");  // header names are case-insensitive
  EXPECT_EQ(chooseRequestId(request), "Trace/42:a.b_c-d");
}

TEST(RequestIdTest, AMissingOrUnusableIdIsReplaced) {
  auto request = makeRequest();
  const auto generated = chooseRequestId(request);
  EXPECT_EQ(generated.size(), 36U);

  request.set("X-Request-Id", "");
  EXPECT_EQ(chooseRequestId(request).size(), 36U) << "empty";
  request.set("X-Request-Id", std::string(kMaxRequestIdLength + 1, 'a'));
  EXPECT_EQ(chooseRequestId(request).size(), 36U) << "too long";
  request.set("X-Request-Id", std::string(kMaxRequestIdLength, 'a'));
  EXPECT_EQ(chooseRequestId(request), std::string(kMaxRequestIdLength, 'a')) << "the limit itself is fine";
  request.set("X-Request-Id", "has space");
  EXPECT_EQ(chooseRequestId(request).size(), 36U) << "whitespace inside";
  request.set("X-Request-Id", std::string{"bad\x01id"});
  EXPECT_EQ(chooseRequestId(request).size(), 36U) << "control character";
  request.set("X-Request-Id", std::string{"caf\xc3\xa9"});
  EXPECT_EQ(chooseRequestId(request).size(), 36U) << "non-ASCII";
}

// --- upstream request --------------------------------------------------------------

TEST(UpstreamRequestTest, KeepsMethodBodyAndRewritesTheTarget) {
  auto request = makeRequest(http::verb::post, "/proxy/orders/api/users?id=42");
  request.set(http::field::content_type, "application/json");
  request.body() = "{\"a\":1}";
  request.prepare_payload();

  const auto out = makeUpstreamRequest(request, "/api/users?id=42", info());
  EXPECT_EQ(out.method(), http::verb::post);
  EXPECT_EQ(std::string{out.target()}, "/api/users?id=42");
  EXPECT_EQ(out.version(), 11U);
  EXPECT_EQ(out.body(), "{\"a\":1}");
  EXPECT_EQ(header(out, "Content-Type"), "application/json");
  EXPECT_EQ(header(out, "Content-Length"), "7");
}

TEST(UpstreamRequestTest, BodyIsForwardedByteForByte) {
  auto request = makeRequest(http::verb::put, "/proxy/orders/blob");
  std::string body;
  for (int i = 0; i < 256; ++i) body.push_back(static_cast<char>(i));
  body += std::string{"\r\n\r\nHTTP/1.1 200 OK\r\n"};
  request.body() = body;
  request.prepare_payload();
  const auto out = makeUpstreamRequest(request, "/blob", info());
  EXPECT_EQ(out.body(), body);
  EXPECT_EQ(header(out, "Content-Length"), std::to_string(body.size()));
}

TEST(UpstreamRequestTest, UnknownMethodsAreForwarded) {
  HttpRequest request{http::verb::unknown, "/proxy/orders/x", 11};
  request.method_string("PURGE");
  const auto out = makeUpstreamRequest(request, "/x", info());
  EXPECT_EQ(std::string{out.method_string()}, "PURGE");
}

TEST(UpstreamRequestTest, RewritesHostForTheBackend) {
  const auto out = makeUpstreamRequest(makeRequest(), "/x", info());
  EXPECT_EQ(header(out, "Host"), "10.0.0.5:9001");
  EXPECT_EQ(count(out, "Host"), 1U);

  EXPECT_EQ(hostHeaderFor("backend", 9001), "backend:9001");
  EXPECT_EQ(hostHeaderFor("backend", 80), "backend") << "the default port is implied";
  EXPECT_EQ(hostHeaderFor("::1", 9001), "[::1]:9001") << "IPv6 literals are bracketed";
  EXPECT_EQ(hostHeaderFor("::1", 80), "[::1]");
}

TEST(UpstreamRequestTest, DropsHopByHopHeaders) {
  auto request = makeRequest(http::verb::post);
  request.set(http::field::connection, "keep-alive, X-Custom-Hop, Upgrade");
  request.set("Keep-Alive", "timeout=5");
  request.set(http::field::te, "trailers");
  request.set(http::field::trailer, "X-Trailer");
  request.set(http::field::upgrade, "websocket");
  request.set("Proxy-Authorization", "Basic abc");
  request.set("Proxy-Connection", "keep-alive");
  request.set("Proxy-Whatever", "1");
  request.set("X-Custom-Hop", "named by Connection");
  request.set("X-Keep", "stays");
  request.set(http::field::expect, "100-continue");
  request.body() = "data";
  request.prepare_payload();
  request.set(http::field::transfer_encoding, "chunked");

  const auto out = makeUpstreamRequest(request, "/x", info());
  for (const char* name : {"Connection", "Keep-Alive", "TE", "Trailer", "Upgrade",
                           "Proxy-Authorization", "Proxy-Connection", "Proxy-Whatever",
                           "X-Custom-Hop", "Expect"}) {
    EXPECT_EQ(header(out, name), "<absent>") << name;
  }
  EXPECT_EQ(header(out, "Transfer-Encoding"), "<absent>");
  EXPECT_EQ(header(out, "Content-Length"), "4") << "the body is sent with a Content-Length";
  EXPECT_EQ(header(out, "X-Keep"), "stays");
}

TEST(UpstreamRequestTest, ConnectionTokensAreMatchedAcrossRepeatedHeadersAndCase) {
  auto request = makeRequest();
  request.insert(http::field::connection, "close");
  request.insert(http::field::connection, "x-one ,  X-TWO");
  request.set("x-one", "1");
  request.set("X-Two", "2");
  request.set("X-Three", "3");
  const auto out = makeUpstreamRequest(request, "/x", info());
  EXPECT_EQ(header(out, "X-One"), "<absent>");
  EXPECT_EQ(header(out, "X-Two"), "<absent>");
  EXPECT_EQ(header(out, "X-Three"), "3");
}

TEST(UpstreamRequestTest, ForwardedForKeepsTheChainAndAppendsTheClient) {
  auto request = makeRequest();
  EXPECT_EQ(header(makeUpstreamRequest(request, "/x", info()), "X-Forwarded-For"), "203.0.113.9")
      << "no chain yet: just the client";

  request.set("X-Forwarded-For", "198.51.100.1, 192.0.2.7");
  EXPECT_EQ(header(makeUpstreamRequest(request, "/x", info()), "X-Forwarded-For"),
            "198.51.100.1, 192.0.2.7, 203.0.113.9")
      << "an existing chain is preserved, not overwritten";

  HttpRequest repeated = makeRequest();
  repeated.insert("X-Forwarded-For", "1.1.1.1");
  repeated.insert("x-forwarded-for", "2.2.2.2");
  const auto out = makeUpstreamRequest(repeated, "/x", info());
  EXPECT_EQ(header(out, "X-Forwarded-For"), "1.1.1.1, 2.2.2.2, 203.0.113.9");
  EXPECT_EQ(count(out, "X-Forwarded-For"), 1U) << "one combined header";

  auto unknown_client = info();
  unknown_client.client_address.clear();
  EXPECT_EQ(header(makeUpstreamRequest(request, "/x", unknown_client), "X-Forwarded-For"),
            "198.51.100.1, 192.0.2.7")
      << "without a client address the chain is passed on untouched";
  EXPECT_EQ(header(makeUpstreamRequest(makeRequest(), "/x", unknown_client), "X-Forwarded-For"),
            "<absent>");
}

TEST(UpstreamRequestTest, ForwardedProtoDescribesTheListener) {
  auto request = makeRequest();
  EXPECT_EQ(header(makeUpstreamRequest(request, "/x", info()), "X-Forwarded-Proto"), "http");
  request.set("X-Forwarded-Proto", "https");  // not believed: the listener speaks plain HTTP
  const auto out = makeUpstreamRequest(request, "/x", info());
  EXPECT_EQ(header(out, "X-Forwarded-Proto"), "http");
  EXPECT_EQ(count(out, "X-Forwarded-Proto"), 1U);
}

TEST(UpstreamRequestTest, ViaAppendsEdgeFlowToTheChain) {
  auto request = makeRequest();
  EXPECT_EQ(header(makeUpstreamRequest(request, "/x", info()), "Via"), "1.1 edgeflow");
  request.set(http::field::via, "1.0 fred, 1.1 p.example.net");
  EXPECT_EQ(header(makeUpstreamRequest(request, "/x", info()), "Via"),
            "1.0 fred, 1.1 p.example.net, 1.1 edgeflow");
  const auto http10 = makeRequest(http::verb::get, "/proxy/orders/x", 10);
  EXPECT_EQ(header(makeUpstreamRequest(http10, "/x", info()), "Via"), "1.0 edgeflow")
      << "the received protocol version is recorded";
  EXPECT_EQ(makeUpstreamRequest(http10, "/x", info()).version(), 11U) << "upstream is always HTTP/1.1";
}

TEST(UpstreamRequestTest, RequestIdIsSetAndReplacesAnyOtherValue) {
  auto request = makeRequest();
  request.set("X-Request-Id", "from-client");
  request.insert("X-Request-Id", "second");
  const auto out = makeUpstreamRequest(request, "/x", info());
  EXPECT_EQ(header(out, "X-Request-Id"), "rid-1");
  EXPECT_EQ(count(out, "X-Request-Id"), 1U);
}

TEST(UpstreamRequestTest, OtherHeadersPassThroughIncludingRepeats) {
  auto request = makeRequest();
  request.set(http::field::user_agent, "curl/8");
  request.set(http::field::accept, "text/html");
  request.set(http::field::authorization, "Bearer t");
  request.insert(http::field::cookie, "a=1");
  request.insert(http::field::cookie, "b=2");
  request.set("X-Custom", "v");
  const auto out = makeUpstreamRequest(request, "/x", info());
  EXPECT_EQ(header(out, "User-Agent"), "curl/8");
  EXPECT_EQ(header(out, "Accept"), "text/html");
  EXPECT_EQ(header(out, "Authorization"), "Bearer t");
  EXPECT_EQ(header(out, "X-Custom"), "v");
  EXPECT_EQ(count(out, "Cookie"), 2U);
}

TEST(UpstreamRequestTest, AGetWithoutBodyHasNoBodyHeaders) {
  const auto out = makeUpstreamRequest(makeRequest(), "/x", info());
  EXPECT_EQ(header(out, "Content-Length"), "<absent>");
  EXPECT_EQ(header(out, "Transfer-Encoding"), "<absent>");
  EXPECT_TRUE(out.body().empty());
}

// --- client response ---------------------------------------------------------------

HttpResponse backendResponse(http::status status = http::status::ok, const std::string& body = "hello") {
  HttpResponse response{status, 11};
  response.set(http::field::content_type, "text/plain");
  response.set(http::field::server, "backend/1");
  response.body() = body;
  response.prepare_payload();
  return response;
}

TEST(ClientResponseTest, StatusReasonAndBodyAreForwarded) {
  for (const auto status : {http::status::ok, http::status::created, http::status::not_found,
                            http::status::internal_server_error, http::status::bad_gateway}) {
    SCOPED_TRACE(static_cast<unsigned>(status));
    const auto out = makeClientResponse(backendResponse(status, "body"), makeRequest(), "rid");
    EXPECT_EQ(out.result(), status);
    EXPECT_EQ(out.body(), "body");
    EXPECT_EQ(header(out, "Content-Type"), "text/plain");
    EXPECT_EQ(header(out, "Server"), "backend/1");
  }
  HttpResponse custom{http::status::ok, 11};
  custom.result(299);
  custom.reason("Custom Reason");
  const auto out = makeClientResponse(std::move(custom), makeRequest(), "rid");
  EXPECT_EQ(out.result_int(), 299U);
  EXPECT_EQ(std::string{out.reason()}, "Custom Reason");
}

TEST(ClientResponseTest, DropsHopByHopHeadersAndWhatTheBackendConnectionNames) {
  auto upstream = backendResponse();
  upstream.set(http::field::connection, "keep-alive, X-Hop");
  upstream.set("Keep-Alive", "timeout=5");
  upstream.set(http::field::upgrade, "h2c");
  upstream.set(http::field::trailer, "X-Late");
  upstream.set("Proxy-Authenticate", "Basic");
  upstream.set("X-Hop", "named by Connection");
  upstream.set("X-Late", "a trailer field");
  upstream.set("X-Keep", "stays");
  const auto out = makeClientResponse(std::move(upstream), makeRequest(), "rid");
  for (const char* name : {"Connection", "Keep-Alive", "Upgrade", "Trailer", "Proxy-Authenticate",
                           "X-Hop", "X-Late", "Transfer-Encoding"}) {
    EXPECT_EQ(header(out, name), "<absent>") << name;
  }
  EXPECT_EQ(header(out, "X-Keep"), "stays");
}

TEST(ClientResponseTest, RepeatedHeadersSurvive) {
  auto upstream = backendResponse();
  upstream.insert(http::field::set_cookie, "a=1");
  upstream.insert(http::field::set_cookie, "b=2");
  const auto out = makeClientResponse(std::move(upstream), makeRequest(), "rid");
  std::size_t cookies = 0;
  for (const auto& field : out) cookies += field.name() == http::field::set_cookie ? 1U : 0U;
  EXPECT_EQ(cookies, 2U);
}

TEST(ClientResponseTest, LengthIsRecomputedFromTheBodyButKeptForHead) {
  auto upstream = backendResponse(http::status::ok, "twelve bytes");
  upstream.set(http::field::content_length, "999");  // would be wrong for the body we hold
  auto out = makeClientResponse(std::move(upstream), makeRequest(), "rid");
  out.prepare_payload();
  EXPECT_EQ(header(out, "Content-Length"), "12");

  HttpResponse head_response{http::status::ok, 11};
  head_response.set(http::field::content_length, "4096");
  const auto head_out =
      makeClientResponse(std::move(head_response), makeRequest(http::verb::head), "rid");
  EXPECT_EQ(header(head_out, "Content-Length"), "4096")
      << "the backend's length describes the body a HEAD response does not carry";
  EXPECT_TRUE(head_out.body().empty());
}

TEST(ClientResponseTest, ViaAndRequestIdAreSet) {
  auto upstream = backendResponse();
  upstream.set(http::field::via, "1.1 backend-proxy");
  upstream.set("X-Request-Id", "backend-made-this-up");
  const auto out = makeClientResponse(std::move(upstream), makeRequest(), "rid-9");
  EXPECT_EQ(header(out, "Via"), "1.1 backend-proxy, 1.1 edgeflow");
  EXPECT_EQ(header(out, "X-Request-Id"), "rid-9");

  EXPECT_EQ(header(makeClientResponse(backendResponse(), makeRequest(), "r"), "Via"), "1.1 edgeflow");
}

TEST(ClientResponseTest, UsesTheClientsHttpVersion) {
  const auto out = makeClientResponse(backendResponse(), makeRequest(http::verb::get, "/p", 10), "r");
  EXPECT_EQ(out.version(), 10U);
}

TEST(GatewayErrorTest, CarriesStatusJsonBodyAndRequestId) {
  const auto out = makeGatewayError(makeRequest(), http::status::bad_gateway, "the backend could not be reached", "rid-3");
  EXPECT_EQ(out.result(), http::status::bad_gateway);
  EXPECT_EQ(header(out, "X-Request-Id"), "rid-3");
  EXPECT_EQ(header(out, "Content-Type"), "application/json");
  EXPECT_NE(out.body().find("\"status\":502"), std::string::npos) << out.body();
  EXPECT_NE(out.body().find("the backend could not be reached"), std::string::npos);
}

}  // namespace
