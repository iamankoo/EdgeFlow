#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "edgeflow/logging/Logger.hpp"

namespace edgeflow::network {

// Asynchronous TCP listener. It only binds, listens and accepts; what happens to an
// accepted socket is up to the accept handler. It does not own an io_context or any
// threads: the caller runs the io_context.
//
// Each accepted socket gets its own strand, so per-connection handlers never run
// concurrently even when several threads run the io_context.
class TcpServer {
 public:
  using AcceptHandler = std::function<void(boost::asio::ip::tcp::socket)>;

  TcpServer(boost::asio::io_context& io_context, std::shared_ptr<logging::Logger> logger,
            AcceptHandler on_accept);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  // Resolves host, binds and listens. Returns false and sets lastError() on failure.
  // Port 0 asks the OS for a free port; port() reports the one chosen.
  [[nodiscard]] bool listen(const std::string& host, std::uint16_t port);

  // Begins the accept loop. The io_context must be (or soon be) running.
  void startAccepting();

  // Stops accepting and closes the listening socket. Synchronous, idempotent, and safe
  // to call from any thread other than an io_context worker. Established connections
  // are unaffected.
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::string& lastError() const noexcept { return last_error_; }

 private:
  void doAccept();
  void onAccept(const boost::system::error_code& error, boost::asio::ip::tcp::socket socket);
  void closeOnStrand();

  boost::asio::io_context& io_context_;
  std::shared_ptr<logging::Logger> logger_;
  AcceptHandler on_accept_;
  boost::asio::ip::tcp::acceptor acceptor_;      // bound to its own strand
  boost::asio::steady_timer retry_timer_;
  std::uint16_t port_{0};
  std::string last_error_;
  bool closed_{false};                           // touched on the acceptor strand only
  std::atomic<bool> accepting_{false};
};

}  // namespace edgeflow::network
