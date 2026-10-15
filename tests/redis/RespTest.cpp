#include <gtest/gtest.h>

#include <string>

#include "edgeflow/redis/Resp.hpp"
#include "edgeflow/redis/Sha256.hpp"

namespace edgeflow::redis {
namespace {

TEST(EncodeCommandTest, EncodesAnArrayOfBulkStrings) {
  EXPECT_EQ(encodeCommand({"GET", "k"}), "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  EXPECT_EQ(encodeCommand({}), "*0\r\n");
}

TEST(EncodeCommandTest, ArgumentsMayContainAnyBytes) {
  const std::string binary{"a\r\nb\0c", 6};
  EXPECT_EQ(encodeCommand({"SET", binary}), "*2\r\n$3\r\nSET\r\n$6\r\n" + binary + "\r\n");
}

TEST(RespParserTest, ParsesEveryReplyType) {
  RespParser parser;
  parser.feed("+OK\r\n-ERR boom\r\n:42\r\n:-7\r\n$5\r\nhello\r\n$0\r\n\r\n$-1\r\n*-1\r\n");
  auto status = parser.next();
  ASSERT_TRUE(status);
  EXPECT_TRUE(status->isOk());
  auto error = parser.next();
  ASSERT_TRUE(error);
  EXPECT_TRUE(error->isError());
  EXPECT_EQ(error->text, "ERR boom");
  EXPECT_EQ(parser.next()->integer, 42);
  EXPECT_EQ(parser.next()->integer, -7);
  EXPECT_EQ(parser.next()->text, "hello");
  const auto empty = parser.next();
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->type, Reply::Type::Bulk);
  EXPECT_TRUE(empty->text.empty());
  EXPECT_TRUE(parser.next()->isNil());
  EXPECT_TRUE(parser.next()->isNil());
  EXPECT_FALSE(parser.next());
  EXPECT_FALSE(parser.invalid());
  EXPECT_EQ(parser.buffered(), 0U);
}

TEST(RespParserTest, ParsesNestedArrays) {
  RespParser parser;
  parser.feed("*3\r\n:1\r\n*2\r\n$1\r\na\r\n$-1\r\n+x\r\n");
  const auto reply = parser.next();
  ASSERT_TRUE(reply);
  ASSERT_EQ(reply->type, Reply::Type::Array);
  ASSERT_EQ(reply->elements.size(), 3U);
  EXPECT_EQ(reply->elements[0].integer, 1);
  ASSERT_EQ(reply->elements[1].elements.size(), 2U);
  EXPECT_EQ(reply->elements[1].elements[0].text, "a");
  EXPECT_TRUE(reply->elements[1].elements[1].isNil());
  EXPECT_EQ(reply->elements[2].text, "x");
}

TEST(RespParserTest, BulkStringsMayContainCrlfAndNul) {
  RespParser parser;
  const std::string payload{"a\r\nb\0c\r\n", 8};
  parser.feed("$8\r\n" + payload + "\r\n");
  const auto reply = parser.next();
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->text, payload);
}

TEST(RespParserTest, ReplyArrivingOneByteAtATimeIsParsedOnlyWhenComplete) {
  const std::string wire = "*2\r\n$5\r\nhello\r\n:99\r\n";
  RespParser parser;
  for (std::size_t i = 0; i < wire.size(); ++i) {
    EXPECT_FALSE(parser.next()) << "complete after " << i << " bytes";
    parser.feed(wire.substr(i, 1));
  }
  const auto reply = parser.next();
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->elements[0].text, "hello");
  EXPECT_EQ(reply->elements[1].integer, 99);
  EXPECT_FALSE(parser.invalid());
}

TEST(RespParserTest, TwoRepliesInOneChunkComeOutInOrder) {
  RespParser parser;
  parser.feed("+A\r\n+B\r\n+C");
  EXPECT_EQ(parser.next()->text, "A");
  EXPECT_EQ(parser.next()->text, "B");
  EXPECT_FALSE(parser.next());  // "+C" is incomplete
  parser.feed("\r\n");
  EXPECT_EQ(parser.next()->text, "C");
}

TEST(RespParserTest, ProtocolViolationsMakeTheParserInvalidForGood) {
  for (const std::string bad : {"?what\r\n", ":12x\r\n", "$-2\r\n", "$3\r\nabcXY", "*-5\r\n", "$abc\r\n"}) {
    RespParser parser;
    parser.feed(bad);
    EXPECT_FALSE(parser.next()) << bad;
    EXPECT_TRUE(parser.invalid()) << bad;
    EXPECT_FALSE(parser.error().empty());
    parser.feed("+OK\r\n");
    EXPECT_FALSE(parser.next()) << "nothing after a violation can be trusted";
  }
}

TEST(RespParserTest, LimitsProtectAgainstHostileLengths) {
  {
    RespParser parser(/*max_bulk_bytes=*/10);
    parser.feed("$11\r\n");
    EXPECT_FALSE(parser.next());
    EXPECT_TRUE(parser.invalid());
  }
  {
    RespParser parser(1024, /*max_array_elements=*/2);
    parser.feed("*3\r\n");
    EXPECT_FALSE(parser.next());
    EXPECT_TRUE(parser.invalid());
  }
  {
    std::string nested;
    for (int i = 0; i < 12; ++i) nested += "*1\r\n";
    nested += ":1\r\n";
    RespParser deep;
    deep.feed(nested);
    EXPECT_FALSE(deep.next());
    EXPECT_TRUE(deep.invalid());
  }
  {
    RespParser parser;
    parser.feed("+" + std::string(70 * 1024, 'x'));  // a line that never ends
    EXPECT_FALSE(parser.next());
    EXPECT_TRUE(parser.invalid());
  }
}

TEST(Sha256Test, MatchesKnownVectors) {
  EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha256Hex(std::string(1000000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, PaddingBoundariesAreCorrect) {
  // 55, 56 and 64 bytes straddle the single-block / two-block padding cases.
  EXPECT_EQ(sha256Hex(std::string(55, 'a')),
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_EQ(sha256Hex(std::string(56, 'a')),
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_EQ(sha256Hex(std::string(64, 'a')),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

}  // namespace
}  // namespace edgeflow::redis
