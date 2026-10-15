#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edgeflow::redis {

// One reply of the Redis serialization protocol (RESP2), the part EdgeFlow needs.
struct Reply {
  enum class Type {
    Nil,      // null bulk string or null array
    Status,   // +OK
    Error,    // -ERR ... (a reply, not a transport failure)
    Integer,  // :42
    Bulk,     // $5\r\nhello\r\n
    Array     // *2\r\n...
  };
  Type type{Type::Nil};
  std::string text;          // Status, Error and Bulk
  long long integer{0};      // Integer
  std::vector<Reply> elements;  // Array

  [[nodiscard]] bool isError() const noexcept { return type == Type::Error; }
  [[nodiscard]] bool isNil() const noexcept { return type == Type::Nil; }
  // True for the status reply "+OK".
  [[nodiscard]] bool isOk() const noexcept { return type == Type::Status && text == "OK"; }
};

// Encodes a command as an array of bulk strings: ["GET", "k"] -> "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n".
// Arguments may contain any bytes.
[[nodiscard]] std::string encodeCommand(const std::vector<std::string>& arguments);

// Incremental RESP2 reply parser. Bytes arrive in arbitrary pieces (feed), complete replies
// are taken out in order (next). A protocol violation makes the parser invalid for good: the
// connection it belongs to must be dropped, nothing after the violation can be trusted.
//
// Bounded: bulk strings up to `max_bulk_bytes`, arrays up to `max_array_elements` elements
// and 8 levels of nesting, so a broken or hostile server cannot make EdgeFlow allocate
// without limit.
class RespParser {
 public:
  explicit RespParser(std::size_t max_bulk_bytes = 16 * 1024 * 1024,
                      std::size_t max_array_elements = 1024)
      : max_bulk_bytes_(max_bulk_bytes), max_array_elements_(max_array_elements) {}

  void feed(std::string_view bytes) { buffer_.append(bytes); }

  // The next complete reply, or nullopt when more bytes are needed (or the parser is invalid).
  [[nodiscard]] std::optional<Reply> next();

  [[nodiscard]] bool invalid() const noexcept { return invalid_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Bytes received but not yet part of a returned reply.
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }

 private:
  // Parses one reply starting at `pos`; on success advances `pos` past it. nullopt: incomplete
  // (pos untouched by the caller's copy) or invalid_ set.
  std::optional<Reply> parse(std::size_t& pos, unsigned depth);
  bool readLine(std::size_t& pos, std::string_view& line);
  void fail(std::string message);

  std::string buffer_;
  std::size_t max_bulk_bytes_;
  std::size_t max_array_elements_;
  bool invalid_{false};
  std::string error_;
};

}  // namespace edgeflow::redis
