#include "edgeflow/network/HealthProbe.hpp"

#include <memory>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace edgeflow::network {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;

namespace {

// Everything one probe needs, kept alive by the asynchronous handlers.
struct Probe : std::enable_shared_from_this<Probe> {
  explicit Probe(net::io_context& io_context)
      : resolver(io_context), stream(io_context) {}

  tcp::resolver resolver;
  beast::tcp_stream stream;
  beast::flat_buffer buffer;
  http::request<http::empty_body> request;
  http::response<http::string_body> response;
  std::chrono::milliseconds timeout{0};
  bool ok{false};
  std::string detail;

  void fail(const char* step, const boost::system::error_code& error) {
    detail = std::string{step} + " failed: " + error.message();
  }

  void run(const std::string& host, const std::string& port) {
    request.method(http::verb::get);
    request.target("/health");
    request.version(11);
    request.set(http::field::host, host);
    request.set(http::field::connection, "close");

    stream.expires_after(timeout);
    resolver.async_resolve(host, port,
                           [self = shared_from_this()](const boost::system::error_code& error,
                                                       const tcp::resolver::results_type& results) {
                             if (error) return self->fail("resolve", error);
                             self->stream.async_connect(
                                 results, [self](const boost::system::error_code& connect_error,
                                                 const tcp::endpoint&) {
                                   if (connect_error) return self->fail("connect", connect_error);
                                   self->write();
                                 });
                           });
  }

  void write() {
    http::async_write(stream, request,
                      [self = shared_from_this()](const boost::system::error_code& error,
                                                  std::size_t) {
                        if (error) return self->fail("write", error);
                        self->read();
                      });
  }

  void read() {
    http::async_read(stream, buffer, response,
                     [self = shared_from_this()](const boost::system::error_code& error,
                                                 std::size_t) {
                       if (error) return self->fail("read", error);
                       if (self->response.result() == http::status::ok) {
                         self->ok = true;
                       } else {
                         self->detail = "unexpected status " +
                                        std::to_string(self->response.result_int());
                       }
                     });
  }
};

}  // namespace

bool probeHealth(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout,
                 std::string& detail) {
  net::io_context io_context;
  auto probe = std::make_shared<Probe>(io_context);
  probe->timeout = timeout;
  probe->run(host, std::to_string(port));
  io_context.run();

  detail = probe->detail;
  return probe->ok;
}

}  // namespace edgeflow::network
