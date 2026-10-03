#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/ConnectionTracker.hpp"
#include "edgeflow/network/Http.hpp"
#include "edgeflow/network/RequestHandler.hpp"

namespace edgeflow::network {

struct ConnectionSettings {
  std::chrono::milliseconds request_timeout{5000};
  std::chrono::milliseconds keep_alive_timeout{10000};
  std::size_t max_request_body_bytes{1024 * 1024};
  std::size_t max_header_bytes{8192};
};

// One accepted TCP connection speaking HTTP/1.1.
//
// Lifecycle:  Idle -> Reading -> Handling -> Writing -> (Idle again | Closing) -> closed
//   Idle      waiting for the first byte of the next request (keep-alive timeout); no
//             data is consumed, so the request timeout starts only once a byte arrives
//   Reading   request partially received (request timeout; expiry answers 408)
//   Handling  request complete, RequestHandler running
//   Writing   response being sent (request timeout bounds a stalled client)
//   Closing   final response sent, write side shut down, briefly draining the peer
//
// Ownership: the object is kept alive by the shared_ptr captured in each pending
// asynchronous operation (and the timer); when the last one completes it is destroyed
// and deregisters itself from the tracker. All state is confined to the socket's strand;
// only beginDrain() and forceClose() may be called from other threads.
class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
 public:
  HttpConnection(boost::asio::ip::tcp::socket socket, std::uint64_t id,
                 ConnectionSettings settings, std::shared_ptr<RequestHandler> handler,
                 std::shared_ptr<logging::Logger> logger,
                 std::shared_ptr<ConnectionTracker> tracker);
  ~HttpConnection();

  HttpConnection(const HttpConnection&) = delete;
  HttpConnection& operator=(const HttpConnection&) = delete;

  // Registers with the tracker and begins reading. Must be called on a connection
  // owned by a shared_ptr.
  void start();

  // Graceful: closes immediately if idle, otherwise lets the current request finish,
  // answers it with "Connection: close", then closes. Thread-safe.
  void beginDrain();

  // Immediate close; aborts any in-flight operation. Thread-safe.
  void forceClose();

 private:
  enum class State { Idle, Reading, Handling, Writing, Closing };

  void doReadRequest();
  void onIdleReadable(const boost::system::error_code& error);
  void onRead(const boost::system::error_code& error);
  void onReadError(const boost::system::error_code& error);
  void onRequestComplete();
  void sendError(http::status status, std::string_view detail);
  void writeResponse(HttpResponse response, bool keep_alive);
  void onWrite(const boost::system::error_code& error, bool keep_alive);
  void beginLingeringClose();
  void doLingeringRead();
  void close();

  void armTimer(std::chrono::milliseconds duration);
  void cancelTimer();
  void onTimeout();

  boost::asio::ip::tcp::socket socket_;
  boost::asio::steady_timer timer_;
  const std::uint64_t id_;
  const ConnectionSettings settings_;
  std::shared_ptr<RequestHandler> handler_;
  std::shared_ptr<logging::Logger> logger_;
  std::shared_ptr<ConnectionTracker> tracker_;

  boost::beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  std::array<char, 1024> discard_{};

  State state_{State::Idle};
  std::uint64_t timer_generation_{0};
  bool closed_{false};
  // Set from the thread calling beginDrain(), so a request being handled right now
  // (which blocks this connection's strand) still sees it when its response is built.
  std::atomic<bool> draining_{false};
  bool timed_out_{false};
  std::size_t requests_served_{0};
};

}  // namespace edgeflow::network
