#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "support/NetTestSupport.hpp"

namespace {

using namespace edgeflow::testing;  // NOLINT: test-only convenience
using edgeflow::network::CancelFunction;
using edgeflow::network::HttpRequest;
using edgeflow::network::HttpResponse;
using edgeflow::network::RequestContext;
using edgeflow::network::RequestHandler;
using edgeflow::network::ResponseCallback;
using namespace std::chrono_literals;

// An asynchronous handler scripted by the request target.
//   /inline         answers before handleAsync returns
//   /later          answers from another thread after 300 ms
//   /never          never answers; records cancellation
//   /throw          throws from handleAsync
//   /twice          answers twice (the second answer must be ignored)
//   /fast           answers inline too (used to probe that workers are free)
class ScriptedAsyncHandler final : public RequestHandler {
 public:
  HttpResponse handle(const HttpRequest& request) override {
    return edgeflow::network::makeResponse(request, http::status::ok, "text/plain", "sync");
  }

  CancelFunction handleAsync(const HttpRequest& request, const RequestContext& context,
                             ResponseCallback done) override {
    {
      const std::lock_guard lock(mutex);
      last_context = context;
    }
    const std::string target{request.target()};
    const auto answer = [request](std::string body) {
      return edgeflow::network::makeResponse(request, http::status::ok, "text/plain", std::move(body));
    };
    if (target == "/inline" || target == "/fast") {
      done(answer(target));
      return {};
    }
    if (target == "/later") {
      std::lock_guard lock(mutex);
      threads.emplace_back([done = std::move(done), answer] {
        std::this_thread::sleep_for(300ms);
        done(answer("later"));
      });
      return {};
    }
    if (target == "/twice") {
      done(answer("first"));
      done(answer("second"));
      return {};
    }
    if (target == "/throw") throw std::runtime_error("handleAsync boom");
    if (target == "/never") {
      auto held = std::make_shared<ResponseCallback>(std::move(done));
      {
        const std::lock_guard lock(mutex);
        held_ = held;
      }
      entered.store(true);
      return [this] {
        ++cancelled;
        const std::lock_guard lock(mutex);
        held_.reset();  // release the callback (and with it the connection), as a real handler does
      };
    }
    done(answer("unknown"));
    return {};
  }

  ~ScriptedAsyncHandler() override {
    for (auto& thread : threads) {
      if (thread.joinable()) thread.join();
    }
  }

  std::mutex mutex;
  RequestContext last_context;
  std::vector<std::thread> threads;
  std::atomic<bool> entered{false};
  std::atomic<int> cancelled{0};

 private:
  std::shared_ptr<ResponseCallback> held_;
};

struct AsyncServer {
  explicit AsyncServer(unsigned workers = 2)
      : handler(std::make_shared<ScriptedAsyncHandler>()),
        harness(makeConfig(workers), handler) {}
  static edgeflow::config::ServerConfig makeConfig(unsigned workers) {
    auto config = testServerConfig();
    config.worker_threads = workers;
    return config;
  }
  std::shared_ptr<ScriptedAsyncHandler> handler;
  ServerHarness harness;
};

TEST(AsyncHandlerTest, AnInlineAnswerWorks) {
  AsyncServer server;
  ASSERT_TRUE(server.harness.started);
  TestClient client(server.harness.port());
  const auto response = client.get("/inline");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->body(), "/inline");
  const auto second = client.get("/inline");
  ASSERT_TRUE(second) << "the connection continues normally";
}

TEST(AsyncHandlerTest, TheHandlerSeesTheClientAddress) {
  AsyncServer server;
  ASSERT_TRUE(server.harness.started);
  TestClient client(server.harness.port());
  ASSERT_TRUE(client.get("/inline"));
  const std::lock_guard lock(server.handler->mutex);
  EXPECT_EQ(server.handler->last_context.client_address, "127.0.0.1");
  EXPECT_NE(server.handler->last_context.client_port, 0);
}

TEST(AsyncHandlerTest, AnAnswerFromAnotherThreadIsDeliveredAndDoesNotBlockTheOnlyWorker) {
  AsyncServer server(1);  // a single I/O worker thread
  ASSERT_TRUE(server.harness.started);

  std::promise<std::optional<HttpResponse>> slow;
  std::thread requester([&] {
    TestClient client(server.harness.port());
    slow.set_value(client.get("/later"));
  });
  std::this_thread::sleep_for(60ms);  // the slow request is now waiting for its answer

  const auto begin = std::chrono::steady_clock::now();
  TestClient other(server.harness.port());
  const auto fast = other.get("/fast");
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  ASSERT_TRUE(fast);
  EXPECT_EQ(fast->body(), "/fast");
  EXPECT_LT(elapsed, 200ms) << "the only worker thread was free to serve another connection";

  requester.join();
  const auto answer = slow.get_future().get();
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->body(), "later");
}

TEST(AsyncHandlerTest, AnExceptionFromHandleAsyncBecomes500AndTheConnectionSurvives) {
  AsyncServer server;
  ASSERT_TRUE(server.harness.started);
  TestClient client(server.harness.port());
  const auto response = client.get("/throw");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->result(), http::status::internal_server_error);
  EXPECT_TRUE(server.harness.log.contains("handleAsync boom"));
  const auto next = client.get("/inline");
  ASSERT_TRUE(next);
  EXPECT_EQ(next->body(), "/inline");
}

TEST(AsyncHandlerTest, OnlyTheFirstAnswerCounts) {
  AsyncServer server;
  ASSERT_TRUE(server.harness.started);
  TestClient client(server.harness.port());
  const auto first = client.get("/twice");
  ASSERT_TRUE(first);
  EXPECT_EQ(first->body(), "first");
  const auto next = client.get("/inline");
  ASSERT_TRUE(next);
  EXPECT_EQ(next->body(), "/inline") << "no stray second response is on the wire";
}

TEST(AsyncHandlerTest, ForcedCloseCancelsAHandlerThatNeverAnswers) {
  AsyncServer server;
  ASSERT_TRUE(server.harness.started);
  std::thread requester([&] {
    TestClient client(server.harness.port());
    EXPECT_FALSE(client.get("/never")) << "no response: the connection is closed";
  });
  ASSERT_TRUE(waitFor([&] { return server.handler->entered.load(); }));

  const auto begin = std::chrono::steady_clock::now();
  server.harness.server.stop(150ms);  // not enough time: the connection is closed by force
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  requester.join();
  EXPECT_EQ(server.handler->cancelled.load(), 1) << "the handler was told to abandon the request";
  EXPECT_LT(elapsed, 2000ms) << "shutdown did not hang";
  EXPECT_TRUE(server.harness.log.contains("still open after the grace period"));
  EXPECT_EQ(server.harness.server.activeConnections(), 0U);
}

TEST(AsyncHandlerTest, ARequestBeingHandledWhenDrainingStartsIsAnsweredWithConnectionClose) {
  AsyncServer server;
  ASSERT_TRUE(server.harness.started);
  std::promise<std::optional<HttpResponse>> result;
  std::thread requester([&] {
    TestClient client(server.harness.port());
    result.set_value(client.get("/later"));
  });
  std::this_thread::sleep_for(80ms);
  server.harness.server.stop(3s);  // waits for the request, which takes 300 ms
  requester.join();
  const auto response = result.get_future().get();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->body(), "later");
  EXPECT_EQ(response->find(http::field::connection)->value(), "close");
}

}  // namespace
