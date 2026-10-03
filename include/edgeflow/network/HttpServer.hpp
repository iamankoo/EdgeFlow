#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "edgeflow/config/Config.hpp"
#include "edgeflow/logging/Logger.hpp"
#include "edgeflow/network/ConnectionTracker.hpp"
#include "edgeflow/network/RequestHandler.hpp"
#include "edgeflow/network/TcpServer.hpp"

namespace edgeflow::network {

// Concurrent HTTP/1.1 server.
//
// Concurrency model: one io_context run by `worker_threads` threads. The listener and
// every connection are asynchronous; there is no thread per connection or per request.
// Each connection's handlers are serialized by its own strand, so connection state needs
// no locks; the only shared state is the ConnectionTracker (mutex) and atomic counters.
//
// Request handlers run on the worker threads and must not block for long.
class HttpServer {
 public:
  HttpServer(config::ServerConfig config, std::shared_ptr<logging::Logger> logger,
             std::shared_ptr<RequestHandler> handler);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Binds, listens and starts the worker threads. Returns false (after logging why)
  // if the address cannot be bound. May only be called once.
  [[nodiscard]] bool start();

  // Graceful shutdown, idempotent and thread-safe (not callable from a worker thread):
  //   1. stop accepting,  2. close idle connections, let in-flight requests finish,
  //   3. wait up to `grace` for connections to drain, force-close what is left,
  //   4. stop the io_context and join the workers.
  void stop(std::chrono::milliseconds grace);

  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] std::uint16_t port() const noexcept { return tcp_.port(); }
  [[nodiscard]] std::size_t activeConnections() const { return tracker_->size(); }
  [[nodiscard]] std::uint64_t acceptedConnections() const noexcept { return accepted_.load(); }
  [[nodiscard]] std::uint64_t rejectedConnections() const noexcept { return rejected_.load(); }

 private:
  void onAccept(boost::asio::ip::tcp::socket socket);
  void runWorker();

  config::ServerConfig config_;
  std::shared_ptr<logging::Logger> logger_;
  std::shared_ptr<RequestHandler> handler_;

  // Declaration order matters for destruction: connections still queued in the
  // io_context deregister themselves from the tracker (which they also share-own),
  // and the acceptor must go before the io_context it was created from.
  std::shared_ptr<ConnectionTracker> tracker_;
  boost::asio::io_context io_context_;
  TcpServer tcp_;
  using WorkGuard =
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
  std::unique_ptr<WorkGuard> work_guard_;
  std::vector<std::thread> workers_;

  std::mutex lifecycle_mutex_;
  bool started_{false};
  bool stopped_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> next_connection_id_{1};
  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace edgeflow::network
