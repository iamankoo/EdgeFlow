#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "edgeflow/network/RequestHandler.hpp"

namespace {

using edgeflow::network::HttpRequest;
using edgeflow::network::LocalRequestHandler;
namespace http = edgeflow::network::http;

HttpRequest makeRequest(http::verb method, const std::string& target, std::string body = {},
                        bool keep_alive = true) {
  HttpRequest request{method, target, 11};
  request.keep_alive(keep_alive);
  request.body() = std::move(body);
  request.prepare_payload();
  return request;
}

TEST(LocalRequestHandlerTest, RootReturnsBanner) {
  LocalRequestHandler handler;
  const auto response = handler.handle(makeRequest(http::verb::get, "/"));
  EXPECT_EQ(response.result(), http::status::ok);
  const auto json = nlohmann::json::parse(response.body());
  EXPECT_EQ(json["service"], "edgeflow");
  EXPECT_EQ(json["message"], "EdgeFlow is running");
}

TEST(LocalRequestHandlerTest, HealthReturnsOk) {
  LocalRequestHandler handler;
  const auto response = handler.handle(makeRequest(http::verb::get, "/health"));
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(nlohmann::json::parse(response.body()),
            (nlohmann::json{{"status", "ok"}, {"service", "edgeflow"}}));
}

TEST(LocalRequestHandlerTest, ResponseMirrorsRequestKeepAlive) {
  LocalRequestHandler handler;
  EXPECT_TRUE(handler.handle(makeRequest(http::verb::get, "/", {}, true)).keep_alive());
  EXPECT_FALSE(handler.handle(makeRequest(http::verb::get, "/", {}, false)).keep_alive());
}

TEST(LocalRequestHandlerTest, EchoReportsBodySize) {
  LocalRequestHandler handler;
  const auto response = handler.handle(makeRequest(http::verb::post, "/echo", "12345"));
  EXPECT_EQ(nlohmann::json::parse(response.body())["received_bytes"], 5);
}

TEST(LocalRequestHandlerTest, UnknownPathIsNotFound) {
  LocalRequestHandler handler;
  const auto response = handler.handle(makeRequest(http::verb::get, "/missing"));
  EXPECT_EQ(response.result(), http::status::not_found);
}

TEST(LocalRequestHandlerTest, WrongMethodIsRejectedWithAllow) {
  LocalRequestHandler handler;
  const auto post_health = handler.handle(makeRequest(http::verb::post, "/health"));
  EXPECT_EQ(post_health.result(), http::status::method_not_allowed);
  EXPECT_EQ(post_health[http::field::allow], "GET");

  const auto get_echo = handler.handle(makeRequest(http::verb::get, "/echo"));
  EXPECT_EQ(get_echo.result(), http::status::method_not_allowed);
  EXPECT_EQ(get_echo[http::field::allow], "POST");
}

TEST(LocalRequestHandlerTest, InvalidUtf8InTargetStillProducesValidJson) {
  LocalRequestHandler handler;
  const auto response = handler.handle(makeRequest(http::verb::get, "/\xff\xfe"));
  EXPECT_EQ(response.result(), http::status::not_found);
  nlohmann::json body;
  EXPECT_NO_THROW(body = nlohmann::json::parse(response.body()));
  EXPECT_TRUE(body.is_object());
}

}  // namespace
