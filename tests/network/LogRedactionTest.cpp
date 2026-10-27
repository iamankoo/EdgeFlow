#include <gtest/gtest.h>

#include <string>

#include "edgeflow/network/Http.hpp"
#include "support/ProxyTestSupport.hpp"

namespace {

using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
namespace http = boost::beast::http;

TEST(LoggableTargetTest, TheQueryStringIsNeverKept) {
  EXPECT_EQ(network::loggableTarget("/proxy/shop/items"), "/proxy/shop/items");
  EXPECT_EQ(network::loggableTarget("/proxy/shop/items?token=abc&x=1"), "/proxy/shop/items?<query omitted>");
  EXPECT_EQ(network::loggableTarget("/a?"), "/a?<query omitted>");
  EXPECT_EQ(network::loggableTarget("?only=query"), "?<query omitted>");
  EXPECT_EQ(network::loggableTarget(""), "");
}

TEST(LoggableTargetTest, ALongPathIsCut) {
  const std::string path = "/" + std::string(5000, 'a');
  const auto text = network::loggableTarget(path + "?secret=1");
  EXPECT_EQ(text.size(), 200U + 3U + std::string{"?<query omitted>"}.size());
  EXPECT_EQ(text.find("secret"), std::string::npos);
}

// Nothing a client puts into a URL query or a credential header may reach the log, at any level (the fixture's
// logger is at debug level) and whatever happens to the request.
class LogRedactionTest : public ProxyFixture {};

TEST_F(LogRedactionTest, SecretsInTheQueryOrInCredentialHeadersAreNotLogged) {
  ScriptedBackend good;
  ScriptedBackend bad([](const auto&, auto, auto) {
    ScriptedBackend::Action action;
    action.code = 503;
    action.body = "down";
    return action;
  });
  addInstance("shop", "g", good.port());
  addInstance("down", "d", bad.port());
  startGateway();
  auto c = client();
  const std::vector<std::pair<std::string, std::string>> headers = {
      {"X-API-Key", "KEYSECRET-1"}, {"Authorization", "Bearer BEARERSECRET-2"}, {"Cookie", "session=COOKIESECRET-3"}};
  ASSERT_TRUE(c->request(http::verb::get, "/proxy/shop/ok?token=QUERYSECRET-4", {}, headers));        // forwarded, 200
  ASSERT_TRUE(c->request(http::verb::get, "/proxy/down/x?token=QUERYSECRET-5", {}, headers));         // backend 503
  ASSERT_TRUE(c->request(http::verb::get, "/proxy/nosuch/x?token=QUERYSECRET-6", {}, headers));       // unknown service
  ASSERT_TRUE(c->request(http::verb::get, "/proxy//x?token=QUERYSECRET-7", {}, headers));             // rejected target
  ASSERT_TRUE(c->request(http::verb::get, "/nosuch?token=QUERYSECRET-8", {}, headers));               // local 404
  c.reset();
  stopGateway();

  const auto text = log.output();
  EXPECT_NE(text.find("/proxy/shop/ok"), std::string::npos) << "the path is still logged";
  for (const char* secret : {"KEYSECRET", "BEARERSECRET", "COOKIESECRET", "QUERYSECRET"}) {
    EXPECT_EQ(text.find(secret), std::string::npos) << secret << " reached the log";
  }
}

}  // namespace
