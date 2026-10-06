#include "edgeflow/network/HttpConnection.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <utility>

namespace edgeflow::network {

namespace net = boost::asio;
namespace beast = boost::beast;

namespace {

constexpr std::chrono::milliseconds kMaxLingerTime{1000};

bool isClientGone(const boost::system::error_code& error) {
  return error == http::error::end_of_stream || error == http::error::partial_message ||
         error == net::error::eof || error == net::error::connection_reset ||
         error == net::error::connection_aborted || error == net::error::broken_pipe;
}

bool isHttpParseError(const boost::system::error_code& error) {
  return error.category() == http::make_error_code(http::error::bad_method).category();
}

}  // namespace

HttpConnection::HttpConnection(net::ip::tcp::socket socket, std::uint64_t id,
                               ConnectionSettings settings,
                               std::shared_ptr<RequestHandler> handler,
                               std::shared_ptr<logging::Logger> logger,
                               std::shared_ptr<ConnectionTracker> tracker)
    : socket_(std::move(socket)),
      timer_(socket_.get_executor()),
      id_(id),
      settings_(settings),
      handler_(std::move(handler)),
      logger_(std::move(logger)),
      tracker_(std::move(tracker)) {}

HttpConnection::~HttpConnection() { tracker_->remove(id_); }

void HttpConnection::start() {
  // Registered synchronously so the server can count and drain the connection even
  // before its first read is scheduled.
  tracker_->add(id_, weak_from_this());
  net::dispatch(socket_.get_executor(), [self = shared_from_this()] {
    boost::system::error_code ignored;
    const auto peer = self->socket_.remote_endpoint(ignored);
    if (!ignored) {
      auto address = peer.address();
      if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        address = net::ip::make_address_v4(net::ip::v4_mapped, address.to_v6());
      }
      self->peer_address_ = address.to_string();
      self->peer_port_ = peer.port();
    }
    self->logger_->debug("connection #{} accepted from {}:{}", self->id_, self->peer_address_,
                         self->peer_port_);
    self->doReadRequest();
  });
}

void HttpConnection::beginDrain() {
  draining_.store(true);
  net::dispatch(socket_.get_executor(), [self = shared_from_this()] {
    if (self->closed_) return;
    if (self->state_ == State::Idle || self->state_ == State::Closing) self->close();
  });
}

void HttpConnection::forceClose() {
  net::dispatch(socket_.get_executor(), [self = shared_from_this()] { self->close(); });
}

// --- reading ---------------------------------------------------------------------

void HttpConnection::doReadRequest() {
  if (closed_) return;
  parser_.emplace();
  parser_->body_limit(settings_.max_request_body_bytes);
  parser_->header_limit(static_cast<std::uint32_t>(settings_.max_header_bytes));

  if (buffer_.size() == 0) {
    // Nothing buffered (no pipelined data): wait for the first byte under the idle
    // limit. Waiting for readability consumes nothing, so the request timeout can start
    // exactly when the request does (Beast's async_read_some would instead keep reading
    // until the whole header is complete, leaving a stalled header under the idle limit).
    state_ = State::Idle;
    armTimer(settings_.keep_alive_timeout);
    socket_.async_wait(net::ip::tcp::socket::wait_read,
                       [self = shared_from_this()](const boost::system::error_code& error) {
                         self->onIdleReadable(error);
                       });
  } else {
    state_ = State::Reading;
    armTimer(settings_.request_timeout);
    http::async_read(socket_, buffer_, *parser_,
                     [self = shared_from_this()](const boost::system::error_code& error,
                                                 std::size_t) { self->onRead(error); });
  }
}

void HttpConnection::onIdleReadable(const boost::system::error_code& error) {
  if (closed_) return;
  if (error) {
    onReadError(error);
    return;
  }
  // Data (or EOF) is available: from here the whole request must complete within
  // request_timeout. A peer that closed shows up as end_of_stream in onRead().
  state_ = State::Reading;
  armTimer(settings_.request_timeout);
  http::async_read(socket_, buffer_, *parser_,
                   [self = shared_from_this()](const boost::system::error_code& read_error,
                                               std::size_t) { self->onRead(read_error); });
}

void HttpConnection::onRead(const boost::system::error_code& error) {
  if (closed_) return;
  if (error) {
    onReadError(error);
    return;
  }
  timed_out_ = false;
  onRequestComplete();
}

void HttpConnection::onReadError(const boost::system::error_code& error) {
  cancelTimer();

  if (error == net::error::operation_aborted) {
    if (timed_out_) {
      timed_out_ = false;
      logger_->info("connection #{}: request timed out after {}ms", id_,
                    settings_.request_timeout.count());
      sendError(http::status::request_timeout, "the request was not received in time");
    } else {
      close();  // shutdown or forced close
    }
    return;
  }
  if (isClientGone(error)) {
    logger_->debug("connection #{}: client disconnected ({})", id_, error.message());
    close();
    return;
  }
  if (error == http::error::body_limit) {
    logger_->info("connection #{}: request body exceeds {} bytes", id_,
                  settings_.max_request_body_bytes);
    sendError(http::status::payload_too_large, "request body exceeds the configured limit");
    return;
  }
  if (error == http::error::header_limit) {
    logger_->info("connection #{}: request headers exceed {} bytes", id_,
                  settings_.max_header_bytes);
    sendError(http::status::request_header_fields_too_large,
              "request headers exceed the configured limit");
    return;
  }
  if (isHttpParseError(error)) {
    logger_->info("connection #{}: malformed request ({})", id_, error.message());
    sendError(http::status::bad_request, "malformed HTTP request");
    return;
  }
  logger_->warn("connection #{}: read failed: {}", id_, error.message());
  close();
}

// --- handling & writing ----------------------------------------------------------

void HttpConnection::onRequestComplete() {
  cancelTimer();
  state_ = State::Handling;
  const HttpRequest request = parser_->release();
  parser_.reset();
  ++requests_served_;

  RequestInfo info;
  info.version = request.version();
  info.keep_alive = request.keep_alive();
  info.head = request.method() == http::verb::head;
  info.method = std::string{request.method_string()};
  info.target = std::string{request.target()};
  const std::uint64_t sequence = ++handling_sequence_;

  // The handler may answer from any thread, at any time (even before handleAsync returns):
  // the response is always re-posted onto this connection's strand, and only the first
  // answer counts.
  const auto answered = std::make_shared<std::atomic<bool>>(false);
  ResponseCallback done = [self = shared_from_this(), answered, info,
                           sequence](HttpResponse response) {
    if (answered->exchange(true)) return;
    net::post(self->socket_.get_executor(),
              [self, info, sequence, response = std::move(response)]() mutable {
                self->onHandled(sequence, info, std::move(response));
              });
  };

  try {
    cancel_ = handler_->handleAsync(request, RequestContext{peer_address_, peer_port_},
                                    std::move(done));
  } catch (const std::exception& e) {
    logger_->error("connection #{}: handler failed for {} {}: {}", id_, info.method, info.target,
                   e.what());
    if (!answered->exchange(true)) {
      onHandled(sequence, info,
                makeErrorResponse(info.version, info.keep_alive,
                                  http::status::internal_server_error,
                                  "the server failed to process the request"));
    }
  }
}

void HttpConnection::onHandled(std::uint64_t sequence, const RequestInfo& info,
                               HttpResponse response) {
  if (closed_ || state_ != State::Handling || sequence != handling_sequence_) return;
  cancel_ = nullptr;
  logger_->debug("connection #{}: {} {} -> {}", id_, info.method, info.target,
                 response.result_int());

  const bool keep_alive = info.keep_alive && !draining_.load();
  writeResponse(std::move(response), keep_alive, info.head);
}

void HttpConnection::sendError(http::status status, std::string_view detail) {
  writeResponse(makeErrorResponse(11, false, status, detail), false);
}

void HttpConnection::writeResponse(HttpResponse response, bool keep_alive, bool head_request) {
  response.keep_alive(keep_alive);
  const bool keeps_own_length = head_request && response.body().empty() &&
                                response.find(http::field::content_length) != response.end();
  if (!keeps_own_length) response.prepare_payload();
  state_ = State::Writing;
  armTimer(settings_.request_timeout);

  // The response must outlive the asynchronous write, so the handler owns it.
  auto owned = std::make_shared<HttpResponse>(std::move(response));
  http::async_write(socket_, *owned,
                    [self = shared_from_this(), owned, keep_alive](
                        const boost::system::error_code& error, std::size_t) {
                      self->onWrite(error, keep_alive);
                    });
}

void HttpConnection::onWrite(const boost::system::error_code& error, bool keep_alive) {
  if (closed_) return;
  cancelTimer();
  if (error) {
    if (error == net::error::operation_aborted) {
      logger_->debug("connection #{}: write aborted", id_);
    } else if (isClientGone(error)) {
      logger_->debug("connection #{}: client disconnected during write ({})", id_,
                     error.message());
    } else {
      logger_->warn("connection #{}: write failed: {}", id_, error.message());
    }
    close();
    return;
  }
  if (keep_alive && !draining_.load()) {
    doReadRequest();
  } else {
    beginLingeringClose();
  }
}

// --- closing ---------------------------------------------------------------------

// After a final response, closing the socket outright while the peer may still be
// sending can reset the connection and destroy the response before it is read. So the
// write side is shut down (the peer sees EOF after the response) and incoming data is
// discarded until the peer closes or a short deadline passes.
void HttpConnection::beginLingeringClose() {
  state_ = State::Closing;
  boost::system::error_code ignored;
  socket_.shutdown(net::ip::tcp::socket::shutdown_send, ignored);
  armTimer(std::min(settings_.request_timeout, kMaxLingerTime));
  doLingeringRead();
}

void HttpConnection::doLingeringRead() {
  socket_.async_read_some(net::buffer(discard_),
                          [self = shared_from_this()](const boost::system::error_code& error,
                                                      std::size_t) {
                            if (self->closed_) return;
                            if (error) {
                              self->close();
                            } else {
                              self->doLingeringRead();
                            }
                          });
}

void HttpConnection::close() {
  if (closed_) return;
  closed_ = true;
  cancelTimer();
  boost::system::error_code ignored;
  socket_.shutdown(net::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
  if (cancel_) {
    // The client is gone (or the server is closing it) while the handler still works on
    // its request: let the handler abandon the work and release what it holds.
    const auto cancel = std::move(cancel_);
    cancel_ = nullptr;
    cancel();
  }
  logger_->debug("connection #{} closed after {} request(s)", id_, requests_served_);
}

// --- timers ----------------------------------------------------------------------

// Every timer wait carries a generation number. Re-arming or cancelling bumps it, so a
// wait handler that was already queued when the timer was reset recognises itself as
// stale and does nothing. All of this runs on the connection's strand, so there is no
// race between the timer and the socket handlers.
void HttpConnection::armTimer(std::chrono::milliseconds duration) {
  const std::uint64_t generation = ++timer_generation_;
  timer_.expires_after(duration);
  timer_.async_wait([self = shared_from_this(), generation](const boost::system::error_code& error) {
    if (error || self->closed_ || generation != self->timer_generation_) return;
    self->onTimeout();
  });
}

void HttpConnection::cancelTimer() {
  ++timer_generation_;
  timer_.cancel();
}

void HttpConnection::onTimeout() {
  switch (state_) {
    case State::Idle:
      logger_->debug("connection #{}: idle for {}ms, closing", id_,
                     settings_.keep_alive_timeout.count());
      close();
      break;
    case State::Reading: {
      // Abort the pending read; onReadError() sees timed_out_ and answers 408.
      timed_out_ = true;
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      break;
    }
    case State::Writing:
      logger_->warn("connection #{}: response not written within {}ms, closing", id_,
                    settings_.request_timeout.count());
      close();
      break;
    case State::Closing:
      close();
      break;
    case State::Handling:
      break;  // no timer runs while a handler works (the handler bounds its own work)
  }
}

}  // namespace edgeflow::network
