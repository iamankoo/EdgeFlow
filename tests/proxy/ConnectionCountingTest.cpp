#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <optional>
#include <thread>

#include "edgeflow/config/ConfigManager.hpp"
#include "support/ProxyTestSupport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace edgeflow;           // NOLINT: test-only convenience
using namespace edgeflow::testing;  // NOLINT
using config::ConnectionCounting;
using config::RoutingStrategy;
using Action = ScriptedBackend::Action;
namespace http = boost::beast::http;

// proxy.connection_counting decides whether the proxy writes connection_count to the registry for every
// request (two writes: +1 when it starts, -1 when it ends). Phase 10 profiling found those writes to be
// 86.7 % of the PostgreSQL time of a proxied request; only Least Connections reads the count.
class ConnectionCountingTest : public ProxyFixture {
 protected:
  // Holds a request in flight on `a` and returns the count seen meanwhile, the answer and the final count.
  struct Observed {
    std::uint64_t during{0};
    std::uint64_t after{0};
    http::status status{http::status::internal_server_error};
  };

  Observed holdOneRequest(ConnectionCounting counting, RoutingStrategy strategy) {
    ScriptedBackend backend;
    auto gate = std::make_shared<Gate>();
    backend.setHandler([&](const ScriptedBackend::Request&, std::uint64_t, std::size_t) {
      Action action;
      action.body = "held";
      action.gate = gate;
      return action;
    });
    proxy_config.connection_counting = counting;
    addInstance("orders", "a", backend.port());
    startGateway(strategy);

    std::promise<std::optional<network::HttpResponse>> result;
    std::thread requester([&] { result.set_value(client()->get("/proxy/orders/x")); });
    EXPECT_TRUE(waitFor([&] { return backend.requests() == 1; }));
    Observed observed;
    observed.during = connectionCount("orders", "a");
    gate->open();
    requester.join();
    const auto response = result.get_future().get();
    if (response) observed.status = response->result();
    observed.after = connectionCount("orders", "a");
    return observed;
  }
};

TEST_F(ConnectionCountingTest, AlwaysCountsEveryRequestWhateverTheStrategy) {
  const auto observed = holdOneRequest(ConnectionCounting::Always, RoutingStrategy::RoundRobin);
  EXPECT_EQ(observed.status, http::status::ok);
  EXPECT_EQ(observed.during, 1U);
  EXPECT_EQ(observed.after, 0U);
}

TEST_F(ConnectionCountingTest, AutoDoesNotCountWhenTheStrategyIgnoresTheCount) {
  for (const auto strategy : {RoutingStrategy::RoundRobin, RoutingStrategy::Weighted, RoutingStrategy::ConsistentHashing}) {
    SCOPED_TRACE(static_cast<int>(strategy));
    stopGateway();
    registry = std::make_shared<FlakyRegistry>();
    const auto observed = holdOneRequest(ConnectionCounting::Auto, strategy);
    EXPECT_EQ(observed.status, http::status::ok);
    EXPECT_EQ(observed.during, 0U) << "no registry write when nothing reads the count";
    EXPECT_EQ(observed.after, 0U);
    EXPECT_NE(log.output().find("connection counting auto (no registry writes per request)"), std::string::npos);
  }
}

TEST_F(ConnectionCountingTest, AutoStillCountsForLeastConnections) {
  const auto observed = holdOneRequest(ConnectionCounting::Auto, RoutingStrategy::LeastConnections);
  EXPECT_EQ(observed.status, http::status::ok);
  EXPECT_EQ(observed.during, 1U) << "Least Connections needs the count, so it is kept";
  EXPECT_EQ(observed.after, 0U);
  EXPECT_NE(log.output().find("connection counting auto (registry writes per request)"), std::string::npos);
}

TEST_F(ConnectionCountingTest, NeverNeverCountsEvenForLeastConnectionsAndRoutingStillWorks) {
  const auto observed = holdOneRequest(ConnectionCounting::Never, RoutingStrategy::LeastConnections);
  EXPECT_EQ(observed.status, http::status::ok) << "requests are still routed and answered";
  EXPECT_EQ(observed.during, 0U);
  EXPECT_EQ(observed.after, 0U);
}

TEST_F(ConnectionCountingTest, ACountThatWasNotTouchedIsLeftAsItWas) {
  ScriptedBackend backend;
  proxy_config.connection_counting = ConnectionCounting::Never;
  addInstance("orders", "a", backend.port(), discovery::HealthStatus::Healthy, discovery::InstanceStatus::Active, 1,
              /*connections=*/7);
  startGateway();
  for (int i = 0; i < 5; ++i) ASSERT_EQ(client()->get("/proxy/orders/x")->result(), http::status::ok);
  EXPECT_EQ(connectionCount("orders", "a"), 7U) << "no request adds to, or subtracts from, a count it does not keep";
}

TEST(ConnectionCountingConfigTest, ParsesTheThreeValuesAndRejectsOthers) {
  const std::string base = "database:\n  enabled: true\nproxy:\n  enabled: true\n";
  EXPECT_EQ(config::ProxyConfig{}.connection_counting, ConnectionCounting::Always) << "the default keeps Phase 6-9 behaviour";
  for (const auto& [text, expected] :
       std::vector<std::pair<std::string, ConnectionCounting>>{{"always", ConnectionCounting::Always},
                                                              {"auto", ConnectionCounting::Auto},
                                                              {"never", ConnectionCounting::Never}}) {
    config::ConfigManager manager;
    ASSERT_TRUE(manager.loadFromString(base + "  connection_counting: " + text + "\n")) << text;
    EXPECT_EQ(manager.config().proxy.connection_counting, expected);
    EXPECT_EQ(config::toString(expected), text);
  }
  config::ConfigManager manager;
  EXPECT_FALSE(manager.loadFromString(base + "  connection_counting: sometimes\n"));
  ASSERT_FALSE(manager.errors().empty());
  EXPECT_NE(manager.errors().front().find("proxy.connection_counting"), std::string::npos);
}

}  // namespace
