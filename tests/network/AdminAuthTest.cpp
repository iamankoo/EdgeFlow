#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "edgeflow/config/ConfigManager.hpp"
#include "edgeflow/network/AdminAuthHandler.hpp"

namespace {

using namespace edgeflow;
using network::AdminAuthHandler;
using network::HttpRequest;
using network::HttpResponse;
namespace http = boost::beast::http;

// Answers 200 and counts how often it was reached.
class Reached final : public network::RequestHandler {
 public:
  HttpResponse handle(const HttpRequest& request) override {
    ++calls;
    return network::makeResponse(request, http::status::ok, "text/plain", "reached");
  }
  int calls{0};
};

HttpRequest request(http::verb method, const std::string& target, const std::string& authorization = "") {
  HttpRequest r{method, target, 11};
  if (!authorization.empty()) r.set(http::field::authorization, authorization);
  r.prepare_payload();
  return r;
}

class AdminAuthTest : public ::testing::Test {
 protected:
  std::shared_ptr<Reached> next = std::make_shared<Reached>();
  AdminAuthHandler handler{next, "s3cret-token"};
};

TEST_F(AdminAuthTest, TheRegistryApiNeedsTheBearerToken) {
  for (const auto verb : {http::verb::get, http::verb::post, http::verb::patch, http::verb::delete_}) {
    for (const char* target : {"/services", "/services/", "/services/a/instances", "/services/a/route?key=k"}) {
      const auto response = handler.handle(request(verb, target));
      EXPECT_EQ(response.result(), http::status::unauthorized) << target;
      EXPECT_EQ(response[http::field::www_authenticate], "Bearer");
    }
  }
  EXPECT_EQ(next->calls, 0) << "a rejected request never reaches the registry";
}

TEST_F(AdminAuthTest, ARightTokenIsPassedOnAndAWrongOneIsNot) {
  EXPECT_EQ(handler.handle(request(http::verb::post, "/services/a/instances", "Bearer s3cret-token")).result(),
            http::status::ok);
  for (const char* bad : {"Bearer s3cret-toke", "Bearer s3cret-tokenX", "Bearer ", "Bearer", "bearer s3cret-token",
                          "Basic s3cret-token", "s3cret-token", "Bearer S3CRET-TOKEN"}) {
    EXPECT_EQ(handler.handle(request(http::verb::get, "/services", bad)).result(), http::status::unauthorized) << bad;
  }
  EXPECT_EQ(next->calls, 1);
}

TEST_F(AdminAuthTest, EverythingElseStaysOpen) {
  for (const char* target : {"/health", "/", "/metrics", "/proxy/a/x", "/servicesX", "/service", "/echo"}) {
    EXPECT_EQ(handler.handle(request(http::verb::get, target)).result(), http::status::ok) << target;
  }
  EXPECT_EQ(next->calls, 7);
}

TEST(AdminAuthCompareTest, ComparesWholeValuesByContent) {
  EXPECT_TRUE(AdminAuthHandler::constantTimeEquals("abc", "abc"));
  EXPECT_FALSE(AdminAuthHandler::constantTimeEquals("abd", "abc"));
  EXPECT_FALSE(AdminAuthHandler::constantTimeEquals("ab", "abc"));
  EXPECT_FALSE(AdminAuthHandler::constantTimeEquals("abcd", "abc"));
  EXPECT_FALSE(AdminAuthHandler::constantTimeEquals("", "abc"));
  EXPECT_TRUE(AdminAuthHandler::constantTimeEquals("", ""));
}

TEST(AdminAuthConfigTest, TheKeyNamesAnEnvironmentVariableAndIsOffByDefault) {
  config::ConfigManager defaults;
  ASSERT_TRUE(defaults.loadFromString("server:\n  port: 8080\n"));
  EXPECT_TRUE(defaults.config().admin.token_env.empty());

  config::ConfigManager ok;
  ASSERT_TRUE(ok.loadFromString("admin:\n  token_env: EDGEFLOW_ADMIN_TOKEN\n"));
  EXPECT_EQ(ok.config().admin.token_env, "EDGEFLOW_ADMIN_TOKEN");

  for (const char* bad : {"admin:\n  token_env: \"not a name!\"\n", "admin:\n  token_env: 1abc\n",
                          "admin:\n  secret: x\n"}) {
    config::ConfigManager manager;
    EXPECT_FALSE(manager.loadFromString(bad)) << bad;
  }
}

}  // namespace
