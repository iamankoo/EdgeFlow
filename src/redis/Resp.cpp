#include "edgeflow/redis/Resp.hpp"

#include <charconv>
#include <limits>

namespace edgeflow::redis {

namespace {

constexpr unsigned kMaxDepth = 8;
constexpr std::size_t kMaxLineBytes = 64 * 1024;  // status, error and length lines

}  // namespace

std::string encodeCommand(const std::vector<std::string>& arguments) {
  std::string out;
  std::size_t total = 16;
  for (const auto& argument : arguments) total += argument.size() + 16;
  out.reserve(total);
  out.append("*").append(std::to_string(arguments.size())).append("\r\n");
  for (const auto& argument : arguments) {
    out.append("$").append(std::to_string(argument.size())).append("\r\n");
    out.append(argument).append("\r\n");
  }
  return out;
}

void RespParser::fail(std::string message) {
  invalid_ = true;
  error_ = std::move(message);
}

bool RespParser::readLine(std::size_t& pos, std::string_view& line) {
  const std::size_t end = buffer_.find("\r\n", pos);
  if (end == std::string::npos) {
    if (buffer_.size() - pos > kMaxLineBytes) fail("reply line too long");
    return false;
  }
  if (end - pos > kMaxLineBytes) {
    fail("reply line too long");
    return false;
  }
  line = std::string_view{buffer_}.substr(pos, end - pos);
  pos = end + 2;
  return true;
}

std::optional<Reply> RespParser::parse(std::size_t& pos, unsigned depth) {
  if (invalid_) return std::nullopt;
  if (depth > kMaxDepth) {
    fail("reply nested too deeply");
    return std::nullopt;
  }
  if (pos >= buffer_.size()) return std::nullopt;

  const char type = buffer_[pos];
  std::size_t cursor = pos + 1;
  std::string_view line;

  switch (type) {
    case '+':
    case '-': {
      if (!readLine(cursor, line)) return std::nullopt;
      Reply reply;
      reply.type = type == '+' ? Reply::Type::Status : Reply::Type::Error;
      reply.text.assign(line);
      pos = cursor;
      return reply;
    }
    case ':': {
      if (!readLine(cursor, line)) return std::nullopt;
      long long value = 0;
      const auto [end, ec] = std::from_chars(line.data(), line.data() + line.size(), value);
      if (ec != std::errc{} || end != line.data() + line.size()) {
        fail("invalid integer reply");
        return std::nullopt;
      }
      Reply reply;
      reply.type = Reply::Type::Integer;
      reply.integer = value;
      pos = cursor;
      return reply;
    }
    case '$': {
      if (!readLine(cursor, line)) return std::nullopt;
      long long length = 0;
      const auto [end, ec] = std::from_chars(line.data(), line.data() + line.size(), length);
      if (ec != std::errc{} || end != line.data() + line.size() || length < -1) {
        fail("invalid bulk string length");
        return std::nullopt;
      }
      if (length == -1) {
        pos = cursor;
        return Reply{};  // Nil
      }
      if (static_cast<unsigned long long>(length) > max_bulk_bytes_) {
        fail("bulk string larger than the allowed " + std::to_string(max_bulk_bytes_) + " bytes");
        return std::nullopt;
      }
      const auto size = static_cast<std::size_t>(length);
      if (buffer_.size() - cursor < size + 2) return std::nullopt;
      if (buffer_[cursor + size] != '\r' || buffer_[cursor + size + 1] != '\n') {
        fail("bulk string not terminated by CRLF");
        return std::nullopt;
      }
      Reply reply;
      reply.type = Reply::Type::Bulk;
      reply.text.assign(buffer_, cursor, size);
      pos = cursor + size + 2;
      return reply;
    }
    case '*': {
      if (!readLine(cursor, line)) return std::nullopt;
      long long count = 0;
      const auto [end, ec] = std::from_chars(line.data(), line.data() + line.size(), count);
      if (ec != std::errc{} || end != line.data() + line.size() || count < -1) {
        fail("invalid array length");
        return std::nullopt;
      }
      if (count == -1) {
        pos = cursor;
        return Reply{};  // Nil
      }
      if (static_cast<unsigned long long>(count) > max_array_elements_) {
        fail("array longer than the allowed " + std::to_string(max_array_elements_) + " elements");
        return std::nullopt;
      }
      Reply reply;
      reply.type = Reply::Type::Array;
      reply.elements.reserve(static_cast<std::size_t>(count));
      for (long long i = 0; i < count; ++i) {
        auto element = parse(cursor, depth + 1);
        if (!element) return std::nullopt;  // incomplete or invalid
        reply.elements.push_back(std::move(*element));
      }
      pos = cursor;
      return reply;
    }
    default:
      fail(std::string{"unexpected reply type byte '"} +
           (type >= 32 && type < 127 ? std::string(1, type) : std::string{"?"}) + "'");
      return std::nullopt;
  }
}

std::optional<Reply> RespParser::next() {
  if (invalid_) return std::nullopt;
  std::size_t pos = 0;
  auto reply = parse(pos, 0);
  if (!reply) return std::nullopt;
  buffer_.erase(0, pos);
  return reply;
}

}  // namespace edgeflow::redis
