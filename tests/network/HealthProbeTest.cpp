#include <gtest/gtest.h>

#include "edgeflow/network/HealthProbe.hpp"
#include "support/NetTestSupport.hpp"

namespace {

using namespace edgeflow::testing;  // NOLINT: test-only convenience
using edgeflow::network::probeHealth;
using namespace std::chrono_literals;

class NotFoundHandler final : public edgeflow::network::RequestHandler {
 public:
  edgeflow::network::HttpResponse handle(const edgeflow::network::HttpRequest& request) override {
    return edgeflow::network::makeErrorResponse(request.version(), request.keep_alive(),
                                                http::status::not_found, "nope");
  }
};

TEST(HealthProbeTest, SucceedsAgainstRunningServer) {
  ServerHarness harness;
  ASSERT_TRUE(harness.started);
  std::string detail;
  EXPECT_TRUE(probeHealth("127.0.0.1", harness.port(), 2s, detail)) << detail;
}

TEST(HealthProbeTest, FailsWhenNothingIsListening) {
  std::uint16_t port = 0;
  {
    ServerHarness harness;
    ASSERT_TRUE(harness.started);
    port = harness.port();
  }  // stopped: the port is now closed
  std::string detail;
  EXPECT_FALSE(probeHealth("127.0.0.1", port, 2s, detail));
  EXPECT_NE(detail.find("connect"), std::string::npos) << detail;
}

TEST(HealthProbeTest, FailsOnNon200Status) {
  ServerHarness harness(testServerConfig(), std::make_shared<NotFoundHandler>());
  ASSERT_TRUE(harness.started);
  std::string detail;
  EXPECT_FALSE(probeHealth("127.0.0.1", harness.port(), 2s, detail));
  EXPECT_NE(detail.find("404"), std::string::npos) << detail;
}

}  // namespace
