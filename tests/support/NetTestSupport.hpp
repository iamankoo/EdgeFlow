#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include "edgeflow/config/Config.hpp"
#include "edgeflow/network/HttpServer.hpp"
#include "edgeflow/network/RequestHandler.hpp"
#include "support/TestLogger.hpp"

namespace edgeflow::testing {

namespace net = boost::asio;
namespace http = boost::beast::http;

inline constexpr std::chrono::seconds kClientReadTimeout{10};

// Polls `condition` until it holds or `timeout` passes.
inline bool waitFor(const std::function<bool()>& condition,
                    std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return condition();
}

// Loopback server configuration with an OS-chosen port, so tests never collide.
inline config::ServerConfig testServerConfig() {
  config::ServerConfig config;
  config.host = "127.0.0.1";
  config.port = 0;
  config.request_timeout = std::chrono::milliseconds{2000};
  config.keep_alive_timeout = std::chrono::milliseconds{5000};
  config.worker_threads = 2;
  return config;
}

// A started HttpServer plus a captured logger.
class ServerHarness {
 public:
  explicit ServerHarness(config::ServerConfig config = testServerConfig(),
                         std::shared_ptr<network::RequestHandler> handler =
                             std::make_shared<network::LocalRequestHandler>())
      : server(std::move(config), log.logger(), std::move(handler)) {
    started = server.start();
  }

  [[nodiscard]] std::uint16_t port() const { return server.port(); }

  CapturedLogger log;
  network::HttpServer server;
  bool started{false};
};

// Handler that records the last request it saw.
class RecordingHandler final : public network::RequestHandler {
 public:
  network::HttpResponse handle(const network::HttpRequest& request) override {
    {
      const std::lock_guard lock(mutex_);
      last_ = request;
      ++count_;
    }
    return network::makeResponse(request, http::status::ok, "text/plain", "recorded");
  }
  [[nodiscard]] network::HttpRequest last() {
    const std::lock_guard lock(mutex_);
    return last_;
  }
  [[nodiscard]] int count() {
    const std::lock_guard lock(mutex_);
    return count_;
  }

 private:
  std::mutex mutex_;
  network::HttpRequest last_;
  int count_{0};
};

// Blocking HTTP/1.1 test client over a single TCP connection. Reads time out after
// kClientReadTimeout so a server bug fails a test instead of hanging it.
class TestClient {
 public:
  explicit TestClient(std::uint16_t port) : socket_(io_context_) {
    socket_.connect({net::ip::make_address("127.0.0.1"), port});
#ifndef _WIN32
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(kClientReadTimeout.count());
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
  }

  void sendRaw(std::string_view data) { net::write(socket_, net::buffer(data.data(), data.size())); }

  std::optional<network::HttpResponse> readResponse() {
    network::HttpResponse response;
    last_error = {};
    http::read(socket_, buffer_, response, last_error);
    if (last_error) return std::nullopt;
    return response;
  }

  std::optional<network::HttpResponse> request(
      http::verb method, const std::string& target, const std::string& body = {},
      const std::vector<std::pair<std::string, std::string>>& headers = {},
      unsigned version = 11) {
    network::HttpRequest req{method, target, version};
    req.set(http::field::host, "localhost");
    for (const auto& [name, value] : headers) req.set(name, value);
    req.body() = body;
    req.prepare_payload();
    last_error = {};
    http::write(socket_, req, last_error);
    if (last_error) return std::nullopt;
    return readResponse();
  }

  std::optional<network::HttpResponse> get(const std::string& target) {
    return request(http::verb::get, target);
  }

  // True when the server has closed its side (EOF or reset) with no further data.
  bool closedByPeer() {
    std::array<char, 64> scratch{};
    boost::system::error_code ec;
    socket_.read_some(net::buffer(scratch), ec);
    return ec == net::error::eof || ec == net::error::connection_reset;
  }

  // Abortive close (TCP RST), as a crashed or hostile client would do.
  void reset() {
    boost::system::error_code ec;
    socket_.set_option(net::socket_base::linger(true, 0), ec);
    socket_.close(ec);
  }

  void close() {
    boost::system::error_code ec;
    socket_.close(ec);
  }

  boost::system::error_code last_error;

 private:
  net::io_context io_context_;
  net::ip::tcp::socket socket_;
  boost::beast::flat_buffer buffer_;
};

}  // namespace edgeflow::testing
