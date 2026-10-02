#include <gtest/gtest.h>

#include "support/TestLogger.hpp"

namespace {

using edgeflow::config::LogLevel;
using edgeflow::testing::CapturedLogger;

TEST(LoggerTest, EmitsAllSupportedLevels) {
  CapturedLogger log(LogLevel::Debug);
  auto logger = log.logger();
  logger->debug("debug message {}", 1);
  logger->info("info message");
  logger->warn("warn message");
  logger->error("error message");

  EXPECT_TRUE(log.contains("[debug]"));
  EXPECT_TRUE(log.contains("[info]"));
  EXPECT_TRUE(log.contains("[warning]"));
  EXPECT_TRUE(log.contains("[error]"));
  EXPECT_TRUE(log.contains("debug message 1"));
}

TEST(LoggerTest, FiltersBelowConfiguredLevel) {
  CapturedLogger log(LogLevel::Warn);
  auto logger = log.logger();
  logger->debug("hidden debug");
  logger->info("hidden info");
  logger->warn("visible warn");
  logger->error("visible error");

  EXPECT_FALSE(log.contains("hidden"));
  EXPECT_TRUE(log.contains("visible warn"));
  EXPECT_TRUE(log.contains("visible error"));
}

TEST(LoggerTest, LevelCanBeChangedAtRuntime) {
  CapturedLogger log(LogLevel::Error);
  auto logger = log.logger();
  EXPECT_EQ(logger->level(), LogLevel::Error);
  logger->info("before");
  logger->setLevel(LogLevel::Info);
  EXPECT_EQ(logger->level(), LogLevel::Info);
  logger->info("after");

  EXPECT_FALSE(log.contains("before"));
  EXPECT_TRUE(log.contains("after"));
}

TEST(LoggerTest, OffSuppressesEverything) {
  CapturedLogger log(LogLevel::Off);
  log.logger()->error("nothing should appear");
  EXPECT_FALSE(log.contains("nothing should appear"));
}

TEST(LoggerTest, DefaultConsoleLoggerConstructs) {
  edgeflow::logging::Logger logger;
  EXPECT_EQ(logger.level(), LogLevel::Info);
  EXPECT_NO_THROW(logger.flush());
}

}  // namespace
