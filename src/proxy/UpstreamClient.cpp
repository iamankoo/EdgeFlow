#include "edgeflow/proxy/UpstreamClient.hpp"

#include <string>
#include <utility>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

namespace edgeflow::proxy {

namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;

namespace {

constexpr std::uint32_t kMaxResponseHeaderBytes = 64 * 1024;
constexpr int kMaxInterimResponses = 8;

std::string describe(const boost::system::error_code& error) {
  if (error == net::error::connection_refused) return "connection refused";
  if (error == net::error::connection_reset) return "connection reset by the backend";
  if (error == net::error::broken_pipe) return "connection closed by the backend";
  if (error == net::error::host_unreachable || error == net::error::network_unreachable) {
    return "host unreachable";
  }
  if (error == net::error::host_not_found || error == net::error::host_not_found_try_again ||
      error == net::error::service_not_found) {
    return "host name could not be resolved";
  }
  if (error == net::error::try_again) return "too many name lookups in progress";
  return error.message();
}

bool isHttpParseError(const boost::system::error_code& error) {
  return error.category() == http::make_error_code(http::error::bad_method).category();
}

// One request/response exchange. All members are touched only on `strand_`; the object lives
// as long as an asynchronous operation (or a timer) references it.
class Exchange final : public UpstreamCall, public std::enable_shared_from_this<Exchange> {
 public:
  Exchange(net::io_context& io, std::shared_ptr<UpstreamPool> pool,
           std::shared_ptr<discovery::NameResolver> names, UpstreamSettings settings,
           std::shared_ptr<logging::Logger> logger, UpstreamEndpoint endpoint,
           network::HttpRequest request, UpstreamClient::Callback done)
      : io_(io),
        strand_(net::make_strand(io)),
        pool_(std::move(pool)),
        names_(std::move(names)),
        settings_(settings),
        logger_(std::move(logger)),
        endpoint_(std::move(endpoint)),
        request_(std::move(request)),
        head_(request_.method() == http::verb::head),
        done_(std::move(done)),
        deadline_(strand_),
        connect_timer_(strand_) {}

  void start() {
    net::post(strand_, [self = shared_from_this()] { self->begin(); });
  }

  void cancel() noexcept override {
    try {
      net::post(strand_, [self = shared_from_this()] { self->fail(UpstreamError::Cancelled, "cancelled"); });
    } catch (...) {
      // Posting can only fail when memory is exhausted; the deadline still ends the exchange.
    }
  }

 private:
  using Strand = net::strand<net::io_context::executor_type>;

  void begin() {
    if (finished_) return;
    deadline_.expires_after(settings_.upstream_timeout);
    deadline_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
      if (error) return;  // cancelled: the exchange ended first
      self->fail(UpstreamError::Timeout, "no complete response within " +
                                             std::to_string(self->settings_.upstream_timeout.count()) +
                                             "ms");
    });

    conn_ = pool_->checkout(endpoint_);
    if (conn_) {
      reused_ = true;
      writeRequest();
    } else {
      connect();
    }
  }

  // --- connecting ---

  void connect() {
    reused_ = false;
    conn_ = std::make_unique<UpstreamConnection>(io_, endpoint_);
    pool_->noteCreated();

    connect_timer_.expires_after(settings_.connect_timeout);
    connect_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
      if (error) return;
      self->fail(UpstreamError::ConnectTimeout,
                 "connecting took longer than " +
                     std::to_string(self->settings_.connect_timeout.count()) + "ms");
    });

    boost::system::error_code ec;
    const auto address = net::ip::make_address(endpoint_.host, ec);
    if (!ec) {  // IP literal: nothing to resolve
      doConnect(tcp::resolver::results_type::create(tcp::endpoint{address, endpoint_.port},
                                                    endpoint_.host, std::to_string(endpoint_.port)));
      return;
    }
    resolving_ = true;
    ticket_ = names_->lookup(
        io_, endpoint_.host, std::to_string(endpoint_.port),
        [self = shared_from_this()](const boost::system::error_code& error,
                                    const tcp::resolver::results_type& results) {
          // Delivered on an arbitrary I/O thread: continue on this exchange's strand.
          net::post(self->strand_, [self, error, results] { self->onResolved(error, results); });
        });
  }

  void onResolved(const boost::system::error_code& error,
                  const tcp::resolver::results_type& results) {
    resolving_ = false;
    if (finished_) return;
    if (error) {
      fail(UpstreamError::Resolve, describe(error));
      return;
    }
    doConnect(results);
  }

  void doConnect(const tcp::resolver::results_type& results) {
    net::async_connect(conn_->socket, results,
                       net::bind_executor(strand_, [self = shared_from_this()](
                                                       const boost::system::error_code& error,
                                                       const tcp::endpoint&) {
                         self->onConnected(error);
                       }));
  }

  void onConnected(const boost::system::error_code& error) {
    if (finished_) return;
    connect_timer_.cancel();
    if (error) {
      fail(UpstreamError::Connect, describe(error));
      return;
    }
    boost::system::error_code ignored;
    conn_->socket.set_option(tcp::no_delay(true), ignored);
    writeRequest();
  }

  // --- request ---

  void writeRequest() {
    http::async_write(conn_->socket, request_,
                      net::bind_executor(strand_, [self = shared_from_this()](
                                                      const boost::system::error_code& error,
                                                      std::size_t written) {
                        self->onWritten(error, written);
                      }));
  }

  void onWritten(const boost::system::error_code& error, std::size_t written) {
    if (finished_) return;
    if (error) {
      if (reused_ && written == 0 && !replaced_) {
        // A pooled connection that died while idle, discovered before any request byte left:
        // nothing reached the backend, so the request simply goes out on a fresh connection.
        replaced_ = true;
        logger_->debug("upstream {}:{}: pooled connection was dead ({}); using a new one",
                       endpoint_.host, endpoint_.port, describe(error));
        pool_->discard(std::move(conn_));
        connect();
        return;
      }
      fail(UpstreamError::Closed, "sending the request failed: " + describe(error));
      return;
    }
    readResponse();
  }

  // --- response ---

  void readResponse() {
    parser_.emplace();
    parser_->body_limit(static_cast<std::uint64_t>(settings_.max_response_bytes));
    parser_->header_limit(kMaxResponseHeaderBytes);
    if (head_) parser_->skip(true);  // a HEAD response carries headers only
    http::async_read(conn_->socket, buffer_, *parser_,
                     net::bind_executor(strand_, [self = shared_from_this()](
                                                     const boost::system::error_code& error,
                                                     std::size_t) { self->onResponse(error); }));
  }

  void onResponse(const boost::system::error_code& error) {
    if (finished_) return;
    if (error) {
      if (error == http::error::body_limit) {
        fail(UpstreamError::TooLarge, "the response body exceeds " +
                                          std::to_string(settings_.max_response_bytes) + " bytes");
      } else if (error == http::error::header_limit) {
        fail(UpstreamError::Malformed, "the response headers are too large");
      } else if (error == http::error::end_of_stream || error == net::error::eof) {
        fail(UpstreamError::Closed, "the backend closed the connection before sending a response");
      } else if (error == http::error::partial_message) {
        fail(UpstreamError::Closed, "the backend closed the connection in the middle of a response");
      } else if (isHttpParseError(error)) {
        fail(UpstreamError::Malformed, "the backend sent an invalid HTTP response (" + error.message() + ")");
      } else {
        fail(UpstreamError::Closed, "reading the response failed: " + describe(error));
      }
      return;
    }

    const unsigned status = parser_->get().result_int();
    if (status >= 100 && status < 200) {
      // Interim response (e.g. 100 Continue, 103 Early Hints): not the answer; wait for the
      // final one. 101 switches protocols, which EdgeFlow never asks for.
      if (status == 101 || ++interim_responses_ > kMaxInterimResponses) {
        fail(UpstreamError::Malformed, "unexpected informational response " + std::to_string(status));
        return;
      }
      readResponse();
      return;
    }
    succeed();
  }

  void succeed() {
    // Reusable only when the exchange ended cleanly at a message boundary: the backend
    // agreed to keep the connection open, the body was not delimited by closing it, and not
    // a single byte is left over.
    const bool reusable = parser_->keep_alive() && !parser_->need_eof() && buffer_.size() == 0;

    UpstreamResult result;
    result.response = parser_->release();
    result.reused_connection = reused_;
    result.stale_connection_replaced = replaced_;

    finished_ = true;
    cancelTimers();
    if (reusable) {
      pool_->checkin(std::move(conn_));
      result.connection_pooled = true;
    } else {
      pool_->discard(std::move(conn_));
    }
    deliver(std::move(result));
  }

  // --- ending ---

  void fail(UpstreamError error, std::string detail) {
    if (finished_) return;
    finished_ = true;
    cancelTimers();
    if (resolving_) {
      resolving_ = false;
      names_->cancel(ticket_);  // the lookup may keep running, but it is never delivered
    }
    if (conn_) pool_->discard(std::move(conn_));  // may be mid-operation: never reusable

    UpstreamResult result;
    result.failure = UpstreamFailure{error, std::move(detail)};
    result.reused_connection = reused_;
    result.stale_connection_replaced = replaced_;
    deliver(std::move(result));
  }

  void cancelTimers() {
    deadline_.cancel();
    connect_timer_.cancel();
  }

  void deliver(UpstreamResult result) {
    auto done = std::move(done_);
    done_ = nullptr;
    if (done) done(std::move(result));
  }

  net::io_context& io_;
  Strand strand_;
  const std::shared_ptr<UpstreamPool> pool_;
  const std::shared_ptr<discovery::NameResolver> names_;
  const UpstreamSettings settings_;
  const std::shared_ptr<logging::Logger> logger_;
  const UpstreamEndpoint endpoint_;
  network::HttpRequest request_;
  const bool head_;
  UpstreamClient::Callback done_;

  net::steady_timer deadline_;
  net::steady_timer connect_timer_;
  std::unique_ptr<UpstreamConnection> conn_;
  boost::beast::flat_buffer buffer_;
  std::optional<http::response_parser<http::string_body>> parser_;
  discovery::NameResolver::Ticket ticket_{0};
  bool resolving_{false};
  bool reused_{false};
  bool replaced_{false};
  bool finished_{false};
  int interim_responses_{0};
};

}  // namespace

const char* toString(UpstreamError error) noexcept {
  switch (error) {
    case UpstreamError::Resolve: return "resolve";
    case UpstreamError::Connect: return "connect";
    case UpstreamError::ConnectTimeout: return "connect_timeout";
    case UpstreamError::Timeout: return "timeout";
    case UpstreamError::Closed: return "closed";
    case UpstreamError::Malformed: return "malformed";
    case UpstreamError::TooLarge: return "too_large";
    case UpstreamError::Cancelled: return "cancelled";
    case UpstreamError::Internal: return "internal";
  }
  return "internal";
}

UpstreamClient::UpstreamClient(net::io_context& io, std::shared_ptr<UpstreamPool> pool,
                               std::shared_ptr<discovery::NameResolver> names,
                               UpstreamSettings settings, std::shared_ptr<logging::Logger> logger)
    : io_(io),
      pool_(std::move(pool)),
      names_(std::move(names)),
      settings_(settings),
      logger_(std::move(logger)) {}

std::shared_ptr<UpstreamCall> UpstreamClient::send(UpstreamEndpoint endpoint,
                                                   network::HttpRequest request, Callback done) {
  auto exchange = std::make_shared<Exchange>(io_, pool_, names_, settings_, logger_,
                                             std::move(endpoint), std::move(request),
                                             std::move(done));
  exchange->start();
  return exchange;
}

}  // namespace edgeflow::proxy
