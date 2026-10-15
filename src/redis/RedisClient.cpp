#include "edgeflow/redis/RedisClient.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <utility>

namespace edgeflow::redis {

namespace net = boost::asio;
using net::ip::tcp;

std::string_view toString(ErrorKind kind) noexcept {
  switch (kind) {
    case ErrorKind::Unavailable: return "unavailable";
    case ErrorKind::ConnectFailed: return "connect_failed";
    case ErrorKind::Timeout: return "timeout";
    case ErrorKind::Closed: return "closed";
    case ErrorKind::Protocol: return "protocol";
    case ErrorKind::Cancelled: return "cancelled";
  }
  return "unavailable";
}

RedisOptions RedisOptions::fromConfig(const config::RedisConfig& config, std::string password) {
  RedisOptions options;
  options.host = config.host;
  options.port = config.port;
  options.password = std::move(password);
  options.connect_timeout = config.connect_timeout;
  options.command_timeout = config.command_timeout;
  options.pool_size = config.pool_size;
  options.io_threads = config.io_threads;
  options.retry_interval = config.retry_interval;
  return options;
}

namespace {

RedisResult failed(ErrorKind kind, std::string message) {
  RedisResult result;
  result.error = RedisError{kind, std::move(message)};
  return result;
}

RedisResult succeeded(Reply reply) {
  RedisResult result;
  result.reply = std::move(reply);
  return result;
}

}  // namespace

// One TCP connection to Redis. Created, used and destroyed only on the client's strand.
struct RedisClient::Connection {
  explicit Connection(const net::strand<net::io_context::executor_type>& strand,
                      std::size_t max_bulk)
      : socket(strand), resolver(strand), timer(strand), parser(max_bulk) {}

  tcp::socket socket;
  tcp::resolver resolver;
  net::steady_timer timer;
  RespParser parser;
  std::array<char, 8192> buffer{};
  std::string out;
  bool busy{false};    // connecting, or an exchange is in flight
  bool ready{false};   // connected (and authenticated): usable for the next command
  bool closed{false};  // dropped; late handlers must do nothing
  std::uint64_t exchange_id{0};
  std::function<void(RedisResult)> on_done;  // completion of the exchange in flight
  std::optional<Pending> first;              // the command that caused this connection to open
};

std::shared_ptr<RedisClient> RedisClient::create(RedisOptions options,
                                                 std::shared_ptr<logging::Logger> logger) {
  return std::shared_ptr<RedisClient>(new RedisClient(std::move(options), std::move(logger)));
}

RedisClient::RedisClient(RedisOptions options, std::shared_ptr<logging::Logger> logger)
    : options_(std::move(options)),
      logger_(std::move(logger)),
      io_(static_cast<int>(std::max(1U, options_.io_threads))),
      work_guard_(std::make_unique<WorkGuard>(io_.get_executor())),
      strand_(net::make_strand(io_)) {
  const unsigned threads = std::max(1U, options_.io_threads);
  threads_.reserve(threads);
  for (unsigned i = 0; i < threads; ++i) {
    threads_.emplace_back([this] {
      while (true) {
        try {
          io_.run();
          return;
        } catch (const std::exception& e) {
          logger_->error("redis I/O thread: unexpected exception: {}", e.what());
        }
      }
    });
  }
}

RedisClient::~RedisClient() {
  try {
    stop();
  } catch (...) {
    // Destructors must not throw.
  }
}

void RedisClient::deliver(Callback callback, RedisResult result) {
  if (!callback) return;
  net::post(io_, [this, callback = std::move(callback), result = std::move(result)]() mutable {
    try {
      callback(std::move(result));
    } catch (const std::exception& e) {
      logger_->error("redis: command callback threw: {}", e.what());
    }
  });
}

void RedisClient::command(std::vector<std::string> arguments, Callback callback) {
  commands_.fetch_add(1);
  if (stop_requested_.load()) {
    // The I/O threads are going away (or gone): answer on the caller's thread.
    if (callback) callback(failed(ErrorKind::Cancelled, "the Redis client is stopped"));
    return;
  }
  Pending pending;
  pending.arguments = std::move(arguments);
  pending.callback = std::move(callback);
  net::post(strand_, [self = shared_from_this(), pending = std::move(pending)]() mutable {
    self->submit(std::move(pending));
  });
}

RedisResult RedisClient::commandSync(std::vector<std::string> arguments) {
  auto promise = std::make_shared<std::promise<RedisResult>>();
  auto future = promise->get_future();
  command(std::move(arguments), [promise](RedisResult result) { promise->set_value(std::move(result)); });
  const auto limit = options_.connect_timeout + options_.command_timeout + std::chrono::seconds{2};
  if (future.wait_for(limit) != std::future_status::ready) {
    return failed(ErrorKind::Timeout, "no answer from the Redis client in time");
  }
  return future.get();
}

// --- strand: queueing -----------------------------------------------------------------

void RedisClient::submit(Pending pending) {
  const auto now = std::chrono::steady_clock::now();
  if (stopped_) {
    deliver(std::move(pending.callback),
            failed(ErrorKind::Cancelled, "the Redis client is stopped"));
    return;
  }
  if (now < unavailable_until_) {
    failures_.fetch_add(1);
    fast_failures_.fetch_add(1);
    deliver(std::move(pending.callback),
            failed(ErrorKind::Unavailable, "Redis was unreachable a moment ago"));
    return;
  }
  if (pending_.size() >= options_.max_pending) {
    failures_.fetch_add(1);
    fast_failures_.fetch_add(1);
    deliver(std::move(pending.callback),
            failed(ErrorKind::Unavailable, "too many commands are waiting for Redis"));
    return;
  }
  pending.deadline = now + options_.connect_timeout + options_.command_timeout;
  pending_.push_back(std::move(pending));
  pump();
}

bool RedisClient::isDead(tcp::socket& socket) {
  if (!socket.is_open()) return true;
  boost::system::error_code error;
  // Non-blocking so the peek can never wait (asynchronous operations are unaffected).
  socket.non_blocking(true, error);
  if (error) return true;
  char byte = 0;
  socket.receive(net::buffer(&byte, 1), tcp::socket::message_peek, error);
  // Nothing to read and no error: an idle, healthy connection. End of stream (the server
  // closed it), a reset, or bytes nobody asked for all make it unusable.
  return !(error == net::error::would_block || error == net::error::try_again);
}

void RedisClient::pump() {
  while (!pending_.empty()) {
    const auto now = std::chrono::steady_clock::now();
    if (now < unavailable_until_) {
      // Redis was just found unreachable: nothing that waits may wait for it.
      while (!pending_.empty()) {
        auto pending = std::move(pending_.front());
        pending_.pop_front();
        failures_.fetch_add(1);
        fast_failures_.fetch_add(1);
        deliver(std::move(pending.callback),
                failed(ErrorKind::Unavailable, "Redis was unreachable a moment ago"));
      }
      return;
    }
    if (pending_.front().deadline <= now) {
      auto pending = std::move(pending_.front());
      pending_.pop_front();
      failures_.fetch_add(1);
      deliver(std::move(pending.callback),
              failed(ErrorKind::Timeout, "waited too long for a free Redis connection"));
      continue;
    }

    std::shared_ptr<Connection> idle;
    for (std::size_t i = 0; i < connections_.size(); ++i) {
      const auto& connection = connections_[i];
      if (!connection->ready || connection->busy) continue;
      if (isDead(connection->socket)) {
        dropConnection(connection);  // erases from connections_
        --i;
        continue;
      }
      idle = connection;
      break;
    }
    if (idle) {
      auto pending = std::move(pending_.front());
      pending_.pop_front();
      runOnConnection(idle, std::move(pending));
      continue;
    }
    if (connections_.size() < options_.pool_size) {
      auto connection = std::make_shared<Connection>(strand_, 16 * 1024 * 1024);
      connection->first = std::move(pending_.front());
      pending_.pop_front();
      connection->busy = true;
      connections_.push_back(connection);
      opened_.fetch_add(1);
      openConnection(connection);
      continue;
    }
    break;  // every connection is busy: the command waits for one to finish
  }
}

// --- strand: connecting ----------------------------------------------------------------

void RedisClient::openConnection(const std::shared_ptr<Connection>& connection) {
  const std::weak_ptr<RedisClient> weak = shared_from_this();
  auto fail = [weak, connection](ErrorKind kind, const std::string& message) {
    const auto self = weak.lock();
    if (!self || connection->closed) return;
    RedisError error{kind, message};
    self->noteFailure(error);
    auto first = std::move(connection->first);
    connection->first.reset();
    self->dropConnection(connection);
    if (first) self->deliver(std::move(first->callback), failed(kind, message));
    self->pump();
  };

  connection->timer.expires_after(options_.connect_timeout);
  connection->timer.async_wait([weak, connection, fail](const boost::system::error_code& ec) {
    if (ec || connection->closed || connection->ready) return;
    const auto self = weak.lock();
    if (!self) return;
    boost::system::error_code ignored;
    connection->socket.close(ignored);
    connection->resolver.cancel();
    fail(ErrorKind::Timeout, "connecting to Redis timed out");
  });

  connection->resolver.async_resolve(
      options_.host, std::to_string(options_.port),
      [weak, connection, fail](const boost::system::error_code& ec,
                               const tcp::resolver::results_type& endpoints) {
        if (connection->closed) return;
        if (ec) {
          if (ec != net::error::operation_aborted) {
            connection->timer.cancel();
            fail(ErrorKind::ConnectFailed, "cannot resolve the Redis host: " + ec.message());
          }
          return;
        }
        net::async_connect(
            connection->socket, endpoints,
            [weak, connection, fail](const boost::system::error_code& connect_ec,
                                     const tcp::endpoint&) {
              if (connection->closed) return;
              if (connect_ec) {
                if (connect_ec != net::error::operation_aborted) {
                  connection->timer.cancel();
                  fail(ErrorKind::ConnectFailed, "cannot connect to Redis: " + connect_ec.message());
                }
                return;
              }
              const auto self = weak.lock();
              if (!self) return;
              connection->timer.cancel();
              boost::system::error_code ignored;
              connection->socket.set_option(tcp::no_delay(true), ignored);
              self->authenticate(connection);
            });
      });
}

void RedisClient::authenticate(const std::shared_ptr<Connection>& connection) {
  const std::weak_ptr<RedisClient> weak = shared_from_this();
  auto ready = [weak, connection] {
    const auto self = weak.lock();
    if (!self) return;
    auto first = std::move(connection->first);
    connection->first.reset();
    if (connection->closed) {  // dropped while becoming ready: the command never ran
      if (first) {
        self->deliver(std::move(first->callback),
                      failed(ErrorKind::Closed, "the Redis connection was dropped"));
      }
      self->pump();
      return;
    }
    connection->ready = true;
    if (first) {
      self->runOnConnection(connection, std::move(*first));
    } else {
      connection->busy = false;
      self->pump();
    }
  };
  if (options_.password.empty()) {
    ready();
    return;
  }
  exchange(connection, {"AUTH", options_.password}, options_.connect_timeout,
           [weak, connection, ready](RedisResult result) {
             const auto self = weak.lock();
             if (!self) return;
             std::string problem;
             ErrorKind kind = ErrorKind::ConnectFailed;
             if (!result.ok()) {
               problem = "AUTH failed: " + result.error->message;
               kind = result.error->kind;
             } else if (result.reply->isError()) {
               problem = "Redis refused AUTH: " + result.reply->text;
             }
             if (problem.empty()) {
               ready();
               return;
             }
             self->noteFailure(RedisError{kind, problem});
             auto first = std::move(connection->first);
             connection->first.reset();
             self->dropConnection(connection);
             if (first) self->deliver(std::move(first->callback), failed(kind, problem));
             self->pump();
           });
}

// --- strand: one command on one connection ---------------------------------------------

void RedisClient::runOnConnection(const std::shared_ptr<Connection>& connection, Pending pending) {
  const std::weak_ptr<RedisClient> weak = shared_from_this();
  auto callback = std::move(pending.callback);
  exchange(connection, std::move(pending.arguments), options_.command_timeout,
           [weak, callback = std::move(callback)](RedisResult result) mutable {
             const auto self = weak.lock();
             if (!self) {
               if (callback) callback(std::move(result));
               return;
             }
             if (result.ok()) {
               self->noteSuccess();
             } else {
               self->noteFailure(*result.error);
             }
             self->deliver(std::move(callback), std::move(result));
             self->pump();
           });
}

void RedisClient::exchange(const std::shared_ptr<Connection>& connection,
                           std::vector<std::string> arguments, std::chrono::milliseconds timeout,
                           std::function<void(RedisResult)> done) {
  const std::weak_ptr<RedisClient> weak = shared_from_this();
  connection->busy = true;
  const std::uint64_t id = ++connection->exchange_id;
  connection->on_done = std::move(done);
  connection->out = encodeCommand(arguments);

  connection->timer.expires_after(timeout);
  connection->timer.async_wait([weak, connection, id](const boost::system::error_code& ec) {
    if (ec || connection->closed || connection->exchange_id != id) return;
    const auto self = weak.lock();
    if (!self) return;
    boost::system::error_code ignored;
    connection->socket.close(ignored);
    self->finishExchange(connection, failed(ErrorKind::Timeout, "the Redis command timed out"));
  });

  net::async_write(connection->socket, net::buffer(connection->out),
                   [weak, connection, id](const boost::system::error_code& ec, std::size_t) {
                     if (connection->closed || connection->exchange_id != id) return;
                     const auto self = weak.lock();
                     if (!self) return;
                     if (ec) {
                       self->finishExchange(connection, failed(ErrorKind::Closed,
                                                               "writing to Redis failed: " + ec.message()));
                       return;
                     }
                     self->readReply(connection);
                   });
}

void RedisClient::readReply(const std::shared_ptr<Connection>& connection) {
  const std::weak_ptr<RedisClient> weak = shared_from_this();
  const std::uint64_t id = connection->exchange_id;
  connection->socket.async_read_some(
      net::buffer(connection->buffer),
      [weak, connection, id](const boost::system::error_code& ec, std::size_t size) {
        if (connection->closed || connection->exchange_id != id) return;
        const auto self = weak.lock();
        if (!self) return;
        if (ec) {
          self->finishExchange(connection, failed(ErrorKind::Closed, "reading from Redis failed: " +
                                                                         ec.message()));
          return;
        }
        connection->parser.feed(std::string_view{connection->buffer.data(), size});
        auto reply = connection->parser.next();
        if (connection->parser.invalid()) {
          self->finishExchange(connection, failed(ErrorKind::Protocol, "invalid Redis reply: " +
                                                                           connection->parser.error()));
          return;
        }
        if (!reply) {
          self->readReply(connection);
          return;
        }
        self->finishExchange(connection, succeeded(std::move(*reply)));
      });
}

void RedisClient::finishExchange(const std::shared_ptr<Connection>& connection, RedisResult result) {
  auto done = std::move(connection->on_done);
  connection->on_done = nullptr;
  ++connection->exchange_id;  // late handlers of this exchange now do nothing
  connection->timer.cancel();
  connection->busy = false;
  // A connection that failed, or that has bytes nobody asked for, is never reused.
  if (!result.ok() || connection->parser.buffered() > 0 || connection->parser.invalid()) {
    dropConnection(connection);
  }
  if (done) done(std::move(result));
}

void RedisClient::dropConnection(const std::shared_ptr<Connection>& connection) {
  connection->closed = true;
  connection->ready = false;
  boost::system::error_code ignored;
  connection->socket.close(ignored);
  connection->resolver.cancel();
  connection->timer.cancel();
  for (auto it = connections_.begin(); it != connections_.end(); ++it) {
    if (*it == connection) {
      connections_.erase(it);
      break;
    }
  }
}

// --- strand: availability bookkeeping --------------------------------------------------

void RedisClient::noteFailure(const RedisError& error) {
  failures_.fetch_add(1);
  unavailable_until_ = std::chrono::steady_clock::now() + options_.retry_interval;
  if (available_) {
    available_ = false;
    available_flag_.store(false);
    logger_->warn("redis {}:{} is unavailable ({}: {}); commands fail at once for {}ms and then one "
                  "is let through to probe it",
                  options_.host, options_.port, toString(error.kind), error.message,
                  options_.retry_interval.count());
  }
}

void RedisClient::noteSuccess() {
  unavailable_until_ = {};
  if (!available_) {
    available_ = true;
    available_flag_.store(true);
    logger_->info("redis {}:{} is available again", options_.host, options_.port);
  }
}

// --- stop ------------------------------------------------------------------------------

void RedisClient::failAll(const RedisError& error) {
  while (!pending_.empty()) {
    auto pending = std::move(pending_.front());
    pending_.pop_front();
    deliver(std::move(pending.callback), failed(error.kind, error.message));
  }
  const auto connections = connections_;  // finishExchange / dropConnection edit the list
  for (const auto& connection : connections) {
    if (connection->closed) continue;
    if (connection->on_done) {
      boost::system::error_code ignored;
      connection->socket.close(ignored);
      finishExchange(connection, failed(error.kind, error.message));
      continue;
    }
    auto first = std::move(connection->first);
    connection->first.reset();
    dropConnection(connection);
    if (first) deliver(std::move(first->callback), failed(error.kind, error.message));
  }
}

void RedisClient::stop() {
  const std::lock_guard stop_lock(stop_mutex_);
  if (stop_done_) return;
  stop_requested_.store(true);

  std::promise<void> closed;
  auto closed_future = closed.get_future();
  net::post(strand_, [this, &closed] {
    stopped_ = true;
    failAll(RedisError{ErrorKind::Cancelled, "the Redis client is stopped"});
    closed.set_value();
  });
  closed_future.wait();

  work_guard_.reset();  // the threads leave run() once every posted callback has run
  for (auto& thread : threads_) {
    if (!thread.joinable()) continue;
    if (thread.get_id() == std::this_thread::get_id()) {
      thread.detach();  // stopped from inside a callback: cannot join itself
    } else {
      thread.join();
    }
  }
  threads_.clear();
  stop_done_ = true;
}

RedisClient::Stats RedisClient::stats() const {
  Stats stats;
  stats.commands = commands_.load();
  stats.failures = failures_.load();
  stats.fast_failures = fast_failures_.load();
  stats.connections_opened = opened_.load();
  stats.available = available_flag_.load();
  return stats;
}

}  // namespace edgeflow::redis
