#include "edgeflow/network/HttpServer.hpp"

#include <exception>
#include <utility>

#include "edgeflow/network/HttpConnection.hpp"

namespace edgeflow::network {

namespace {
constexpr std::chrono::milliseconds kForceCloseWait{2000};
}

HttpServer::HttpServer(config::ServerConfig config, std::shared_ptr<logging::Logger> logger,
                       std::shared_ptr<RequestHandler> handler)
    : config_(std::move(config)),
      logger_(std::move(logger)),
      handler_(std::move(handler)),
      tracker_(std::make_shared<ConnectionTracker>()),
      io_context_(static_cast<int>(config_.worker_threads)),
      tcp_(io_context_, logger_, [this](boost::asio::ip::tcp::socket socket) {
        onAccept(std::move(socket));
      }) {}

HttpServer::~HttpServer() { stop(std::chrono::milliseconds{0}); }

bool HttpServer::start() {
  const std::lock_guard lock(lifecycle_mutex_);
  if (started_ || stopped_) {
    logger_->error("HTTP server cannot be started more than once");
    return false;
  }
  if (!tcp_.listen(config_.host, config_.port)) {
    logger_->error("HTTP server failed to listen: {}", tcp_.lastError());
    return false;
  }
  started_ = true;
  running_.store(true);

  work_guard_ = std::make_unique<WorkGuard>(io_context_.get_executor());
  workers_.reserve(config_.worker_threads);
  for (unsigned i = 0; i < config_.worker_threads; ++i) {
    workers_.emplace_back([this] { runWorker(); });
  }
  tcp_.startAccepting();
  logger_->info("HTTP server listening on {}:{} ({} worker thread(s))", config_.host,
                tcp_.port(), config_.worker_threads);
  return true;
}

void HttpServer::runWorker() {
  try {
    io_context_.run();
  } catch (const std::exception& e) {
    logger_->error("I/O worker terminated by an exception: {}", e.what());
  }
}

void HttpServer::onAccept(boost::asio::ip::tcp::socket socket) {
  if (tracker_->size() >= config_.max_connections) {
    rejected_.fetch_add(1);
    logger_->warn("connection limit ({}) reached; rejecting a new connection",
                  config_.max_connections);
    return;  // the socket is closed when it goes out of scope
  }
  accepted_.fetch_add(1);

  ConnectionSettings settings;
  settings.request_timeout = config_.request_timeout;
  settings.keep_alive_timeout = config_.keep_alive_timeout;
  settings.max_request_body_bytes = config_.max_request_body_bytes;
  settings.max_header_bytes = config_.max_header_bytes;

  std::make_shared<HttpConnection>(std::move(socket), next_connection_id_.fetch_add(1),
                                   settings, handler_, logger_, tracker_)
      ->start();
}

void HttpServer::stop(std::chrono::milliseconds grace) {
  const std::lock_guard lock(lifecycle_mutex_);
  if (stopped_) return;
  stopped_ = true;
  if (!started_) {
    tcp_.stop();
    return;
  }

  logger_->info("HTTP server stopping: no longer accepting connections");
  tcp_.stop();

  // Idle connections close at once; busy ones finish their current request first.
  for (const auto& connection : tracker_->snapshot()) connection->beginDrain();
  if (const auto open = tracker_->size(); open > 0) {
    logger_->info("waiting up to {}ms for {} connection(s) to finish", grace.count(), open);
  }
  if (!tracker_->waitUntilEmpty(grace)) {
    logger_->warn("{} connection(s) still open after the grace period; closing them",
                  tracker_->size());
    for (const auto& connection : tracker_->snapshot()) connection->forceClose();
    if (!tracker_->waitUntilEmpty(kForceCloseWait)) {
      logger_->error("{} connection(s) did not close", tracker_->size());
    }
  }

  work_guard_.reset();
  io_context_.stop();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  running_.store(false);
  logger_->info("HTTP server stopped ({} connection(s) served, {} rejected)", accepted_.load(),
                rejected_.load());
}

}  // namespace edgeflow::network
