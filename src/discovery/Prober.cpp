#include "edgeflow/discovery/Prober.hpp"

#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

namespace edgeflow::discovery {

namespace {

namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;

constexpr std::uint32_t kMaxResponseHeaderBytes = 16 * 1024;

std::string describe(const boost::system::error_code& error) {
  if (error == net::error::connection_refused) return "connection refused";
  if (error == net::error::connection_reset) return "connection reset";
  if (error == net::error::host_unreachable || error == net::error::network_unreachable) {
    return "host unreachable";
  }
  if (error == net::error::host_not_found || error == net::error::host_not_found_try_again ||
      error == net::error::service_not_found) {
    return "host name could not be resolved";
  }
  if (error == http::error::end_of_stream || error == net::error::eof) {
    return "connection closed before a response";
  }
  if (error == http::error::partial_message) return "connection closed in the middle of a response";
  if (error == net::error::try_again) return "too many name lookups in progress";
  return error.message();
}

// One probe. Lives as long as an asynchronous operation (or its timer) references it.
// All members are touched only from the single thread running the io_context.
class ProbeOperation final : public ProbeHandle,
                             public std::enable_shared_from_this<ProbeOperation> {
 public:
  ProbeOperation(net::io_context& io, std::shared_ptr<NameResolver> names, ProbeTarget target,
                 bool http_check, std::function<void(ProbeResult)> done)
      : io_(io),
        names_(std::move(names)),
        target_(std::move(target)),
        http_check_(http_check),
        done_(std::move(done)),
        socket_(io),
        timer_(io) {}

  void run() {
    timer_.expires_after(target_.timeout);
    timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
      if (error) return;  // cancelled: the probe finished (or was cancelled) first
      self->finish(false, "timed out after " + std::to_string(self->target_.timeout.count()) + "ms");
    });

    boost::system::error_code ec;
    const auto address = net::ip::make_address(target_.host, ec);
    if (!ec) {
      // IP literal: no name resolution needed.
      connect(tcp::resolver::results_type::create(tcp::endpoint{address, target_.port},
                                                  target_.host, std::to_string(target_.port)));
      return;
    }
    resolving_ = true;
    ticket_ = names_->lookup(
        io_, target_.host, std::to_string(target_.port),
        [self = shared_from_this()](const boost::system::error_code& error,
                                    const tcp::resolver::results_type& results) {
          self->resolving_ = false;
          if (self->finished_) return;
          if (error) {
            self->finish(false, describe(error));
            return;
          }
          self->connect(results);
        });
  }

  void cancel() noexcept override {
    if (finished_) return;
    finished_ = true;  // suppress the callback
    close();
  }

 private:
  void connect(const tcp::resolver::results_type& results) {
    net::async_connect(socket_, results,
                       [self = shared_from_this()](const boost::system::error_code& error,
                                                   const tcp::endpoint&) {
                         if (self->finished_) return;
                         if (error) {
                           self->finish(false, describe(error));
                           return;
                         }
                         if (!self->http_check_) {
                           self->finish(true, "connected");
                           return;
                         }
                         self->sendRequest();
                       });
  }

  void sendRequest() {
    request_.method(http::verb::get);
    request_.target(target_.http_path);
    request_.version(11);
    request_.set(http::field::host, target_.host + ":" + std::to_string(target_.port));
    request_.set(http::field::user_agent, "EdgeFlow-HealthCheck");
    request_.set(http::field::accept, "*/*");
    request_.set(http::field::connection, "close");
    http::async_write(socket_, request_,
                      [self = shared_from_this()](const boost::system::error_code& error,
                                                  std::size_t) {
                        if (self->finished_) return;
                        if (error) {
                          self->finish(false, describe(error));
                          return;
                        }
                        self->readResponse();
                      });
  }

  void readResponse() {
    parser_.header_limit(kMaxResponseHeaderBytes);
    // Only the status line and headers matter; the body is never read.
    http::async_read_header(socket_, buffer_, parser_,
                            [self = shared_from_this()](const boost::system::error_code& error,
                                                        std::size_t) {
                              if (self->finished_) return;
                              if (error) {
                                const bool closed = error == http::error::end_of_stream ||
                                                    error == http::error::partial_message ||
                                                    error == net::error::eof;
                                const bool malformed =
                                    !closed && error.category() ==
                                                   http::make_error_code(http::error::bad_method).category();
                                self->finish(false, malformed ? "malformed HTTP response"
                                                              : describe(error));
                                return;
                              }
                              const unsigned status = self->parser_.get().result_int();
                              const bool ok = status >= 200 && status < 300;
                              self->finish(ok, "HTTP " + std::to_string(status));
                            });
  }

  void finish(bool healthy, std::string detail) {
    if (finished_) return;
    finished_ = true;
    close();
    done_(ProbeResult{healthy, std::move(detail)});
  }

  void close() noexcept {
    boost::system::error_code ignored;
    timer_.cancel();
    if (resolving_) {
      resolving_ = false;
      names_->cancel(ticket_);  // the lookup may keep running, but it will not be delivered
    }
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

  net::io_context& io_;
  const std::shared_ptr<NameResolver> names_;
  const ProbeTarget target_;
  const bool http_check_;
  std::function<void(ProbeResult)> done_;
  NameResolver::Ticket ticket_{0};
  bool resolving_{false};
  tcp::socket socket_;
  net::steady_timer timer_;
  boost::beast::flat_buffer buffer_;
  http::request<http::empty_body> request_;
  http::response_parser<http::empty_body> parser_;
  bool finished_{false};
};

// The two probers differ only in whether a request is sent after connecting.
class SocketProber final : public Prober {
 public:
  SocketProber(bool http_check, std::shared_ptr<NameResolver> names)
      : http_check_(http_check), names_(std::move(names)) {}

  std::shared_ptr<ProbeHandle> start(net::io_context& io, ProbeTarget target,
                                     std::function<void(ProbeResult)> done) override {
    auto operation = std::make_shared<ProbeOperation>(io, names_, std::move(target), http_check_,
                                                      std::move(done));
    operation->run();
    return operation;
  }

 private:
  const bool http_check_;
  const std::shared_ptr<NameResolver> names_;
};

}  // namespace

std::shared_ptr<Prober> makeProber(config::HealthCheckType type,
                                   std::shared_ptr<NameResolver> names) {
  if (!names) names = std::make_shared<NameResolver>();
  return std::make_shared<SocketProber>(type == config::HealthCheckType::Http, std::move(names));
}

}  // namespace edgeflow::discovery
