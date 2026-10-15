#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/redis/RedisClient.hpp"
#include "support/RedisTestSupport.hpp"

namespace edgeflow::redis {
namespace {

using namespace std::chrono_literals;
using testing::FakeRedisServer;

using Action = FakeRedisServer::Action;
using Kind = Action::Kind;

RedisOptions fastOptions(std::uint16_t port) {
  RedisOptions options = testing::testRedisOptions("127.0.0.1", port);
  options.connect_timeout = 500ms;
  options.command_timeout = 300ms;
  options.retry_interval = 300ms;
  return options;
}

class RedisClientTest : public ::testing::Test {
 protected:
  testing::CapturedLogger log;
};

// ---------------------------------------------------------------- scripted server

TEST_F(RedisClientTest, RoundTripReturnsTheServersReply) {
  FakeRedisServer server([](const auto& command, auto, auto) {
    Action action;
    action.raw = command[0] == "PING" ? "+PONG\r\n" : "$5\r\nhello\r\n";
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  const auto pong = client->commandSync({"PING"});
  ASSERT_TRUE(pong.ok());
  EXPECT_EQ(pong.reply->text, "PONG");
  const auto get = client->commandSync({"GET", "k"});
  ASSERT_TRUE(get.ok());
  EXPECT_EQ(get.reply->text, "hello");
  EXPECT_EQ(server.connections(), 1U) << "the connection is reused";
  client->stop();
}

TEST_F(RedisClientTest, AnErrorReplyIsAReplyNotATransportFailure) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.raw = "-ERR unknown command\r\n";
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  const auto result = client->commandSync({"NOPE"});
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.reply->isError());
  EXPECT_TRUE(client->stats().available) << "an error reply does not mark Redis unavailable";
  EXPECT_EQ(client->stats().failures, 0U);
  client->stop();
}

TEST_F(RedisClientTest, CommandsRunConcurrentlyUpToThePoolSizeAndTheRestWait) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.delay = 100ms;
    return action;
  });
  RedisOptions options = fastOptions(server.port());
  options.pool_size = 3;
  options.command_timeout = 2000ms;
  auto client = RedisClient::create(options, log.logger());
  std::vector<std::future<RedisResult>> results;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 6; ++i) {
    auto promise = std::make_shared<std::promise<RedisResult>>();
    results.push_back(promise->get_future());
    client->command({"PING"}, [promise](RedisResult r) { promise->set_value(std::move(r)); });
  }
  for (auto& result : results) EXPECT_TRUE(result.get().ok());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(server.connections(), 3U) << "never more connections than pool_size";
  EXPECT_GE(elapsed, 190ms) << "6 commands of 100 ms over 3 connections take two rounds";
  client->stop();
}

TEST_F(RedisClientTest, ConnectionRefusedFailsThenFailsFastThenProbesAgain) {
  std::uint16_t dead_port = 0;
  {
    FakeRedisServer closed;
    dead_port = closed.port();
  }  // nothing listens there any more
  auto client = RedisClient::create(fastOptions(dead_port), log.logger());
  const auto first = client->commandSync({"PING"});
  ASSERT_FALSE(first.ok());
  EXPECT_EQ(first.error->kind, ErrorKind::ConnectFailed);
  EXPECT_FALSE(client->stats().available);

  // Inside the retry interval commands fail at once and never touch the network.
  const auto opened = client->stats().connections_opened;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    const auto fast = client->commandSync({"PING"});
    ASSERT_FALSE(fast.ok());
    EXPECT_EQ(fast.error->kind, ErrorKind::Unavailable);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 250ms);
  EXPECT_EQ(client->stats().connections_opened, opened);
  EXPECT_EQ(client->stats().fast_failures, 20U);

  // After the interval one command is let through to probe.
  std::this_thread::sleep_for(350ms);
  const auto probe = client->commandSync({"PING"});
  ASSERT_FALSE(probe.ok());
  EXPECT_EQ(probe.error->kind, ErrorKind::ConnectFailed);
  EXPECT_GT(client->stats().connections_opened, opened);
  EXPECT_TRUE(log.contains("is unavailable"));
  client->stop();
}

TEST_F(RedisClientTest, RedisComingBackIsNoticedAfterTheRetryInterval) {
  std::uint16_t port = 0;
  {
    FakeRedisServer first;
    port = first.port();
  }
  auto client = RedisClient::create(fastOptions(port), log.logger());
  ASSERT_FALSE(client->commandSync({"PING"}).ok());
  ASSERT_FALSE(client->stats().available);

  FakeRedisServer back([](const auto&, auto, auto) {
    Action action;
    action.raw = "+PONG\r\n";
    return action;
  }, port);
  std::this_thread::sleep_for(350ms);
  const auto result = client->commandSync({"PING"});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.reply->text, "PONG");
  EXPECT_TRUE(client->stats().available);
  EXPECT_TRUE(log.contains("is available again"));
  client->stop();
}

TEST_F(RedisClientTest, AServerThatNeverAnswersTimesOut) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.kind = Kind::Stall;
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  const auto start = std::chrono::steady_clock::now();
  const auto result = client->commandSync({"GET", "k"});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->kind, ErrorKind::Timeout);
  EXPECT_GE(elapsed, 250ms);
  EXPECT_LT(elapsed, 1500ms);
  EXPECT_FALSE(client->stats().available);
  client->stop();
}

TEST_F(RedisClientTest, GarbageFromTheServerIsAProtocolError) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.kind = Kind::Garbage;
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  const auto result = client->commandSync({"GET", "k"});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->kind, ErrorKind::Protocol);
  client->stop();
}

TEST_F(RedisClientTest, ConnectionClosedWhileACommandIsInFlight) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.kind = Kind::Close;
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  const auto result = client->commandSync({"GET", "k"});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->kind, ErrorKind::Closed);
  client->stop();
}

TEST_F(RedisClientTest, ExtraBytesAfterAReplyRetireTheConnection) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.raw = "+OK\r\n+UNSOLICITED\r\n";
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  ASSERT_TRUE(client->commandSync({"SET", "a", "b"}).ok());
  ASSERT_TRUE(client->commandSync({"SET", "a", "b"}).ok());
  EXPECT_EQ(server.connections(), 2U) << "a connection with unexplained bytes is never reused";
  client->stop();
}

TEST_F(RedisClientTest, PasswordIsSentWithAuthBeforeTheFirstCommand) {
  FakeRedisServer server([](const auto& command, auto, auto) {
    Action action;
    if (command[0] == "AUTH") {
      action.raw = command[1] == "s3cret" ? "+OK\r\n" : "-ERR invalid password\r\n";
    } else {
      action.raw = "+PONG\r\n";
    }
    return action;
  });
  RedisOptions options = fastOptions(server.port());
  options.password = "s3cret";
  auto client = RedisClient::create(options, log.logger());
  const auto result = client->commandSync({"PING"});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.reply->text, "PONG");
  const auto commands = server.commands();
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0][0], "AUTH");
  EXPECT_EQ(commands[1][0], "PING");
  ASSERT_TRUE(client->commandSync({"PING"}).ok());
  EXPECT_EQ(server.commands().size(), 3U) << "AUTH once per connection, not per command";
  client->stop();
  EXPECT_FALSE(log.contains("s3cret")) << "the password is never logged";
}

TEST_F(RedisClientTest, ARefusedPasswordFailsTheCommandAndNamesTheReason) {
  FakeRedisServer server([](const auto& command, auto, auto) {
    Action action;
    action.raw = command[0] == "AUTH" ? "-WRONGPASS invalid username-password pair\r\n" : "+PONG\r\n";
    return action;
  });
  RedisOptions options = fastOptions(server.port());
  options.password = "wrong";
  auto client = RedisClient::create(options, log.logger());
  const auto result = client->commandSync({"PING"});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->kind, ErrorKind::ConnectFailed);
  EXPECT_NE(result.error->message.find("AUTH"), std::string::npos);
  client->stop();
}

TEST_F(RedisClientTest, AnIdleConnectionTheServerClosedIsReplacedWithoutLosingTheCommand) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.raw = "+PONG\r\n";
    return action;
  });
  auto client = RedisClient::create(fastOptions(server.port()), log.logger());
  ASSERT_TRUE(client->commandSync({"PING"}).ok());
  server.dropConnections();  // what a Redis restart does to idle pooled connections
  std::this_thread::sleep_for(50ms);
  const auto result = client->commandSync({"PING"});
  ASSERT_TRUE(result.ok()) << (result.error ? result.error->message : "");
  EXPECT_EQ(server.connections(), 2U);
  EXPECT_EQ(client->stats().failures, 0U) << "detected before the command was written";
  EXPECT_TRUE(client->stats().available);
  client->stop();
}

TEST_F(RedisClientTest, TooManyWaitingCommandsFailAtOnce) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.delay = 300ms;
    return action;
  });
  RedisOptions options = fastOptions(server.port());
  options.pool_size = 1;
  options.max_pending = 2;
  options.command_timeout = 2000ms;
  auto client = RedisClient::create(options, log.logger());
  std::vector<std::future<RedisResult>> results;
  for (int i = 0; i < 8; ++i) {
    auto promise = std::make_shared<std::promise<RedisResult>>();
    results.push_back(promise->get_future());
    client->command({"PING"}, [promise](RedisResult r) { promise->set_value(std::move(r)); });
  }
  int ok = 0;
  int refused = 0;
  for (auto& result : results) {
    const auto value = result.get();
    if (value.ok()) {
      ++ok;
    } else if (value.error->kind == ErrorKind::Unavailable) {
      ++refused;
    }
  }
  EXPECT_GE(refused, 1);
  EXPECT_GE(ok, 1);
  EXPECT_EQ(ok + refused, 8);
  client->stop();
}

TEST_F(RedisClientTest, StopCancelsEverythingInFlightAndWaiting) {
  FakeRedisServer server([](const auto&, auto, auto) {
    Action action;
    action.kind = Kind::Stall;
    return action;
  });
  RedisOptions options = fastOptions(server.port());
  options.pool_size = 1;
  options.command_timeout = 5000ms;
  auto client = RedisClient::create(options, log.logger());
  std::vector<std::future<RedisResult>> results;
  for (int i = 0; i < 4; ++i) {
    auto promise = std::make_shared<std::promise<RedisResult>>();
    results.push_back(promise->get_future());
    client->command({"GET", "k"}, [promise](RedisResult r) { promise->set_value(std::move(r)); });
  }
  std::this_thread::sleep_for(100ms);
  client->stop();
  for (auto& result : results) {
    const auto value = result.get();
    ASSERT_FALSE(value.ok());
    EXPECT_EQ(value.error->kind, ErrorKind::Cancelled);
  }
  const auto after = client->commandSync({"PING"});
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.error->kind, ErrorKind::Cancelled);
  client->stop();  // idempotent
}

TEST_F(RedisClientTest, ManyThreadsGetTheirOwnRepliesBack) {
  FakeRedisServer server([](const auto& command, auto, auto) {
    Action action;
    action.raw = "$" + std::to_string(command[1].size()) + "\r\n" + command[1] + "\r\n";  // echo
    return action;
  });
  RedisOptions options = fastOptions(server.port());
  options.command_timeout = 3000ms;
  auto client = RedisClient::create(options, log.logger());
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 100; ++i) {
        const std::string value = "t" + std::to_string(t) + "-" + std::to_string(i);
        const auto result = client->commandSync({"ECHO", value});
        if (!result.ok() || result.reply->text != value) mismatches.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_LE(server.connections(), 4U);
  client->stop();
}

// ---------------------------------------------------------------- real Redis

TEST_F(RedisClientTest, RealRedisSetGetDelAndExpiry) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  auto client = testing::makeRedisClient(*test_redis, log.logger());
  const std::string key = testing::uniqueName("client");
  ASSERT_TRUE(client->commandSync({"SET", key, "value"}).reply->isOk());
  EXPECT_EQ(client->commandSync({"GET", key}).reply->text, "value");
  EXPECT_EQ(client->commandSync({"DEL", key}).reply->integer, 1);
  EXPECT_TRUE(client->commandSync({"GET", key}).reply->isNil());

  ASSERT_TRUE(client->commandSync({"SET", key, "short-lived", "PX", "100"}).reply->isOk());
  EXPECT_EQ(client->commandSync({"GET", key}).reply->text, "short-lived");
  std::this_thread::sleep_for(250ms);
  EXPECT_TRUE(client->commandSync({"GET", key}).reply->isNil()) << "an expired key is gone";
  client->stop();
}

TEST_F(RedisClientTest, RealRedisBinaryAndLargeValuesSurvive) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  auto client = testing::makeRedisClient(*test_redis, log.logger());
  const std::string key = testing::uniqueName("binary");
  std::string value;
  for (int i = 0; i < 256; ++i) value.push_back(static_cast<char>(i));
  value += std::string(1024 * 1024, 'z');
  ASSERT_TRUE(client->commandSync({"SET", key, value, "PX", "10000"}).reply->isOk());
  const auto got = client->commandSync({"GET", key});
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.reply->text.size(), value.size());
  EXPECT_EQ(got.reply->text, value);
  (void)client->commandSync({"DEL", key});
  client->stop();
}

TEST_F(RedisClientTest, RealRedisConcurrentIncrementsAreAllCounted) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  auto client = testing::makeRedisClient(*test_redis, log.logger());
  const std::string key = testing::uniqueName("counter");
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        if (!client->commandSync({"INCR", key}).ok()) failures.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(client->commandSync({"GET", key}).reply->text, "1600");
  (void)client->commandSync({"DEL", key});
  client->stop();
}

TEST_F(RedisClientTest, RealRedisOutageAndRecoveryThroughARelay) {
  EDGEFLOW_REQUIRE_TEST_REDIS();
  testing::RedisRelay relay(test_redis->host, test_redis->port);
  RedisOptions options = fastOptions(relay.port());
  auto client = RedisClient::create(options, log.logger());
  const std::string key = testing::uniqueName("outage");
  ASSERT_TRUE(client->commandSync({"SET", key, "1", "PX", "30000"}).reply->isOk());

  relay.refuse();  // Redis dies
  const auto down = client->commandSync({"GET", key});
  ASSERT_FALSE(down.ok());
  EXPECT_FALSE(client->stats().available);

  relay.resume();  // Redis returns
  ASSERT_TRUE(testing::eventually([&] { return client->commandSync({"GET", key}).ok(); }, 3s));
  EXPECT_EQ(client->commandSync({"GET", key}).reply->text, "1") << "the data survived (it is the real Redis)";
  EXPECT_TRUE(client->stats().available);

  relay.hang();  // Redis answers nothing
  const auto start = std::chrono::steady_clock::now();
  const auto hung = client->commandSync({"GET", key});
  ASSERT_FALSE(hung.ok());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "a hung Redis costs one command timeout";
  relay.resume();
  ASSERT_TRUE(testing::eventually([&] { return client->commandSync({"GET", key}).ok(); }, 3s));
  (void)client->commandSync({"DEL", key});
  client->stop();
}

}  // namespace
}  // namespace edgeflow::redis
