#include "edgeflow/network/TcpServer.hpp"

#include <chrono>
#include <future>
#include <utility>

namespace edgeflow::network {

namespace net = boost::asio;
using boost::asio::ip::tcp;

namespace {
constexpr std::chrono::milliseconds kAcceptRetryDelay{100};
}

TcpServer::TcpServer(net::io_context& io_context, std::shared_ptr<logging::Logger> logger,
                     AcceptHandler on_accept)
    : io_context_(io_context),
      logger_(std::move(logger)),
      on_accept_(std::move(on_accept)),
      acceptor_(net::make_strand(io_context)),
      retry_timer_(acceptor_.get_executor()) {}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::listen(const std::string& host, std::uint16_t port) {
  boost::system::error_code ec;

  tcp::resolver resolver(io_context_);
  const auto endpoints =
      resolver.resolve(host, std::to_string(port), tcp::resolver::passive, ec);
  if (ec || endpoints.empty()) {
    last_error_ = "cannot resolve '" + host + "': " + (ec ? ec.message() : "no addresses");
    return false;
  }
  const tcp::endpoint endpoint = endpoints.begin()->endpoint();

  const auto fail = [&](const char* step) {
    last_error_ = std::string{step} + " " + endpoint.address().to_string() + ":" +
                  std::to_string(port) + " failed: " + ec.message();
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    return false;
  };

  if (acceptor_.open(endpoint.protocol(), ec); ec) return fail("open");
  if (acceptor_.set_option(net::socket_base::reuse_address(true), ec); ec) {
    return fail("set_option(reuse_address)");
  }
  if (acceptor_.bind(endpoint, ec); ec) return fail("bind");
  if (acceptor_.listen(net::socket_base::max_listen_connections, ec); ec) return fail("listen");

  port_ = acceptor_.local_endpoint(ec).port();
  closed_ = false;
  return true;
}

void TcpServer::startAccepting() {
  accepting_.store(true);
  net::post(acceptor_.get_executor(), [this] { doAccept(); });
}

void TcpServer::doAccept() {
  if (closed_ || !acceptor_.is_open()) return;
  acceptor_.async_accept(net::make_strand(io_context_),
                         [this](const boost::system::error_code& error, tcp::socket socket) {
                           onAccept(error, std::move(socket));
                         });
}

void TcpServer::onAccept(const boost::system::error_code& error, tcp::socket socket) {
  if (closed_ || error == net::error::operation_aborted) return;  // expected during stop()

  if (error) {
    if (error == net::error::connection_aborted) {
      logger_->debug("accept: client aborted before the connection was established");
      doAccept();
      return;
    }
    // Typically resource exhaustion (descriptors, memory). Back off briefly instead of
    // spinning, then keep accepting.
    logger_->warn("accept failed: {}; retrying in {}ms", error.message(),
                  kAcceptRetryDelay.count());
    retry_timer_.expires_after(kAcceptRetryDelay);
    retry_timer_.async_wait([this](const boost::system::error_code& wait_error) {
      if (!wait_error) doAccept();
    });
    return;
  }

  on_accept_(std::move(socket));
  doAccept();
}

void TcpServer::closeOnStrand() {
  closed_ = true;
  boost::system::error_code ignored;
  retry_timer_.cancel();
  acceptor_.close(ignored);
}

void TcpServer::stop() {
  if (!accepting_.exchange(false)) {
    // The accept loop never ran, so nothing else touches the acceptor.
    closeOnStrand();
    return;
  }
  std::promise<void> done;
  auto finished = done.get_future();
  net::dispatch(acceptor_.get_executor(), [this, &done] {
    closeOnStrand();
    done.set_value();
  });
  finished.wait();
}

}  // namespace edgeflow::network
