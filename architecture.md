# EdgeFlow — Architecture

> **Status:** Sections 1-16 describe the **target architecture**. Only what sections 17 (Phase 1: foundation) and 18 (Phase 2: networking engine) describe is implemented; everything else is introduced phase by phase according to [Phases.md](Phases.md).

## 1. High-Level Architecture

```text
            ┌──────────┐
            │  Client  │
            └────┬─────┘
                 │ HTTP/1.1
        ┌────────▼─────────────────────────────────────────┐
        │                    EdgeFlow                      │
        │  network → gateway → rate limit → cache → router │
        │                         │                        │
        │        reliability ◄────┴────► proxy             │
        │   discovery / health        observability        │
        └───────┬───────────────┬───────────────┬──────────┘
                │               │               │
          PostgreSQL         Redis          Backends
         (metadata)    (cache, rate state)  (service instances)
```

## 2. Source Layout and Component Responsibilities

```text
src/
├── gateway/        Gateway, RequestHandler, ResponseHandler
├── network/        TcpServer, HttpServer, Connection, ConnectionPool
├── discovery/      ServiceRegistry, ServiceInstance, HealthChecker
├── routing/        Router, RoundRobin, LeastConnections, WeightedRouting, ConsistentHashing
├── proxy/          RequestForwarder, ResponseForwarder
├── reliability/    RetryPolicy, TimeoutManager, CircuitBreaker, FailoverManager
├── rate_limit/     RateLimiter, TokenBucket
├── cache/          CacheManager
├── storage/        PostgreSQLClient, RedisClient
├── observability/  Logger, Metrics, Tracer
└── config/         ConfigManager
```

| Module | Responsibility |
|--------|----------------|
| gateway | Orchestrates the request pipeline; owns request/response handling policy. |
| network | Asynchronous TCP/HTTP server, connection lifecycle, keep-alive, timeouts, backend connection pooling. |
| discovery | Registry of service instances (metadata, status, health, version, weight, connection count); health checking. |
| routing | Selects a healthy instance through an exchangeable load-balancing strategy. |
| proxy | Forwards requests to backends and responses back to clients; header propagation; request IDs. |
| reliability | Timeouts, retries with backoff, circuit breaking, failover. |
| rate_limit | Token Bucket limiting per IP, API key, and route/service. |
| cache | Response caching with expiry, backed by Redis. |
| storage | PostgreSQL and Redis clients. |
| observability | Logging, Prometheus-compatible metrics, request tracing. |
| config | Loads and validates YAML configuration. |

## 3. Request Flow

1. Client opens a TCP connection; `TcpServer` accepts it and `Connection` is created.
2. `HttpServer` parses the HTTP/1.1 request (keep-alive supported).
3. `Gateway` assigns a request ID and applies rate limiting.
4. `CacheManager` is consulted; on a hit the cached response is returned.
5. `Router` resolves the target service and selects a healthy instance.
6. `RequestForwarder` sends the request upstream (pooled connection) under `TimeoutManager` limits.
7. `ResponseForwarder` returns the response; cacheable responses are stored.
8. Metrics and logs are recorded.

## 4. Service Discovery Flow

Instances are registered, updated and deregistered. PostgreSQL is the persistent source of metadata. `ServiceRegistry` loads instances and refreshes periodically, keeping an in-memory view for lock-light lookups on the request path.

## 5. Health-Check Flow

`HealthChecker` periodically probes each instance (TCP connect or HTTP endpoint). Consecutive failures move an instance to unhealthy and exclude it from routing; consecutive successes restore it. Transitions are logged and counted.

## 6. Load-Balancing Flow

`Router` holds a `LoadBalancingStrategy` (Round Robin, Least Connections, Weighted Routing, Consistent Hashing). It receives only healthy instances and returns a selection. Consistent Hashing uses a request key (e.g. client IP or header). Strategy is selected via configuration.

## 7. Reverse-Proxy Flow

Client → EdgeFlow → Backend → EdgeFlow → Client. The proxy rewrites hop-by-hop headers, propagates the request ID and forwarding headers, reuses pooled backend connections, and enforces upstream timeouts. Backend failures are surfaced to the reliability layer.

## 8. Failure and Retry Flow

On timeout, connection failure, or a configured retryable status, `RetryPolicy` decides whether to retry (limits, exponential backoff). `FailoverManager` selects a different healthy instance. When retries are exhausted, a gateway error (e.g. 502/503/504) is returned.

## 9. Circuit-Breaker Flow

Per-instance breaker: `CLOSED → OPEN → HALF-OPEN → CLOSED`. Failures beyond a threshold open the circuit; requests are short-circuited. After a cool-down it becomes half-open and admits limited probes; success closes it, failure reopens it.

## 10. Redis Cache Flow

Cacheable requests are keyed by method, route and relevant headers. Hit: return from Redis. Miss: forward upstream, store with TTL. Redis failure degrades to pass-through rather than failing requests.

## 11. Rate-Limiting Flow

Token Bucket per key (IP, API key, route/service). State is shared in Redis so limits hold across gateway instances, with atomic updates. Rejected requests receive HTTP 429 and are counted.

## 12. Observability Architecture

- **Logging:** spdlog, with lifecycle and request logs.
- **Metrics:** Prometheus-compatible endpoint covering requests/sec, active connections, request count, error rate, p50/p95/p99 latency, backend latency, cache hit ratio, retries, circuit-breaker trips, rejected requests.
- **Tracing:** request IDs propagated end to end.

## 13. Persistence Responsibilities

| Store | Holds |
|-------|-------|
| PostgreSQL | Durable service/instance metadata. |
| Redis | Cached responses; distributed rate-limit state. |
| Process memory | Hot-path registry view, health state, circuit state, connection counts. |

## 14. Concurrency Model

Boost.Asio `io_context` driven by a pool of worker threads; connections are asynchronous and strand-protected where state is shared. Shared hot-path state favors read-mostly structures and atomics. The detailed model is finalized in Phase 2 and tuned in Phase 10 using lock-contention analysis.

## 15. Docker Deployment Architecture

Docker Compose runs: EdgeFlow, multiple backend service instances, PostgreSQL, and Redis, on a shared network, with configuration mounted from YAML and environment variables for secrets.

## 16. Key Architectural Decisions

- Modular components with strictly separated responsibilities.
- Load-balancing strategies behind one interface, swappable at configuration time.
- PostgreSQL for durable metadata; Redis for shared ephemeral state.
- Asynchronous I/O (Boost.Asio/Beast) rather than thread-per-connection.
- Graceful degradation: cache or rate-limit store failure must not take down the gateway.
- Performance claims are made only from measured, reproducible benchmarks.

## 17. Phase 1 Implementation

### Implemented in Phase 1

```text
include/edgeflow/  config/{Config, ConfigManager}   logging/Logger
                   core/{Application, ShutdownCoordinator, SignalHandler, CommandLine}
src/               matching implementations + main.cpp
```

Built as a static library `edgeflow_core` (alias `edgeflow::core`), a thin `edgeflow` executable, and the `edgeflow_tests` GoogleTest binary.

| Component | Responsibility |
|-----------|----------------|
| `ConfigManager` / `Config` | Reads YAML into typed structs. Defaults for missing fields; rejects unknown keys, wrong types, out-of-range values; reports all errors; keeps the previous config on failure. Files over 1 MiB are refused. |
| `Logger` | Wraps a private (non-global) spdlog logger. Levels debug/info/warn/error (plus trace/critical/off). Sinks are injectable for tests. |
| `Application` | Lifecycle: construct, `initialize()`, `run()`, `shutdown()`. Receives config and logger by injection. |
| `ShutdownCoordinator` | Ordered, idempotent, thread-safe shutdown. Components register stop callbacks; they run once in reverse registration order; a throwing callback is logged and does not stop the rest. |
| `SignalHandler` | RAII SIGINT/SIGTERM handlers that only set a lock-free atomic. One instance at a time. |
| `CommandLine` | `--config`, `--version`, `--help`; config path precedence flag > `$EDGEFLOW_CONFIG` > default. |

### Lifecycle flow

```text
main
 ├─ parse command line
 ├─ ConfigManager::load        (failure: errors to stderr, exit 2)
 ├─ create Logger from config  → "EdgeFlow starting", "configuration loaded"
 ├─ Application::initialize    → install signal handlers, register them with the
 │                               ShutdownCoordinator → "application initialized"
 │                               (failure: exit 1)
 └─ Application::run           → "EdgeFlow ready", wait for signal / requestShutdown()
      └─ "shutdown requested" → ShutdownCoordinator::shutdown
            → "shutdown sequence started" → stop components (reverse order)
            → flush → "shutdown completed" → exit 0
```

Configuration is loaded before logging exists (the log level comes from it), so configuration failures are written to stderr.

### Shutdown behavior

- A signal handler never logs or locks; it stores the signal number. `run()` polls it every 50 ms and also wakes immediately on `requestShutdown()`.
- Repeated or concurrent `shutdown()` calls run cleanup exactly once; later callers block until it has completed.
- There are no connections to drain yet. Future networking components register a stop callback with `Application::shutdownCoordinator()`.
- `shutdown.grace_period_seconds` is logged and a warning is emitted if shutdown exceeds it. Callbacks are not forcibly interrupted.

### Planned in later phases

Everything in sections 1-16 other than sections 17 and 18.

## 18. Phase 2 Implementation: TCP/HTTP Networking Engine

### Implemented in Phase 2

```text
include/edgeflow/network/  TcpServer  HttpServer  HttpConnection  ConnectionTracker
                           RequestHandler (+ LocalRequestHandler)  Http  HealthProbe
src/network/               matching implementations
```

| Component | Responsibility |
|-----------|----------------|
| `TcpServer` | Resolves host/port from configuration, binds, listens, and accepts asynchronously on its own strand. Each accepted socket is created on a fresh strand. Accept errors are logged and retried (with a short back-off for resource exhaustion); `operation_aborted` during stop is not an error. Does not own the `io_context` or any threads. |
| `HttpServer` | Owns the `io_context`, `worker_threads` worker threads, the `TcpServer`, and the `ConnectionTracker`. Enforces `max_connections`. `start()` / `stop(grace)` implement startup and graceful shutdown. |
| `HttpConnection` | One TCP connection: Beast request parser, state machine, timers, keep-alive loop, error responses. |
| `ConnectionTracker` | Mutex-protected registry of weak references to live connections: counts them and lets shutdown drain or force-close them. |
| `RequestHandler` | Interface `HttpRequest -> HttpResponse`. `LocalRequestHandler` answers locally (`GET /`, `GET /health`, `POST /echo`, otherwise 404/405). This is **not** the load-balancing router, which arrives in Phase 5. |
| `Http` | Beast type aliases and response builders (JSON bodies via nlohmann/json). |
| `HealthProbe` | One-shot client behind `edgeflow --healthcheck` and the container `HEALTHCHECK`. |

### Concurrency model

One `io_context` run by `server.worker_threads` threads (default 2). Everything is asynchronous: there is no thread per connection or per request. Each connection's socket, timer and handlers share one strand, so a connection's state is touched by one thread at a time and needs no locks. The only cross-thread state is the `ConnectionTracker` (a mutex), atomic counters, and each connection's atomic `draining` flag (set by `beginDrain()` from the stopping thread so that a request being handled when shutdown starts still gets `Connection: close`). `RequestHandler::handle` runs on a worker thread and must be thread-safe and short; it is synchronous in Phase 2.

### Connection lifecycle

```text
accepted ──► Idle ──readable──► Reading ──complete──► Handling ──► Writing ─┬─ keep-alive ─► Idle
              │                      │                                  │     └─ close ─► Closing ─► closed
              │ keep_alive_timeout   │ request_timeout                  │ request_timeout
              ▼                      ▼                                  ▼
           closed (silent)       408 + Closing                       closed
```

- **Idle** waits for the socket to become readable (`async_wait`, so no data is consumed) under `keep_alive_timeout_ms`. Waiting without reading matters: Beast's `async_read_some` keeps reading until the whole header is complete, which would leave a stalled partial header under the idle limit instead of the request timeout. On expiry the connection is closed without a response, which HTTP permits.
- **Reading** begins when the first byte (or EOF) arrives; the whole request must arrive within `request_timeout_ms`. On expiry the pending read is cancelled and the client gets `408` with `Connection: close`.
- **Writing** is bounded by `request_timeout_ms` so a client that stops reading cannot hold a connection forever.
- **Closing** (after a final response) shuts down the write side so the peer sees EOF after the response, then discards incoming data until the peer closes or about one second passes. Closing the socket outright could reset the connection and destroy the response before the client reads it.
- Keep-alive follows HTTP/1.1: persistent unless `Connection: close` (or HTTP/1.0 without keep-alive). Pipelined requests already in the buffer are served in order, one at a time.

### Timers

Each connection owns one `steady_timer` on its strand. Every arm or cancel increments a generation number captured by the wait handler, so a handler that was already queued when the timer was reset recognises itself as stale and does nothing. Because timer and socket handlers share a strand, a timeout cannot race with an in-flight completion, and a timer handler cannot touch a destroyed connection (it holds a `shared_ptr`).

### Ownership and lifetime

A `HttpConnection` is owned by the `shared_ptr` captured in each pending asynchronous operation and its timer; when the last one completes it destroys itself and deregisters from the tracker. The tracker holds only `weak_ptr`s and is itself shared-owned by every connection, so connections still queued in the `io_context` at destruction can safely deregister. Responses are heap-allocated and held by the write handler until the write completes. Shared ownership is used only for connections, not elsewhere.

### Request limits and error responses

| Setting | Default | Effect when exceeded |
|---------|---------|----------------------|
| `request_timeout_ms` | 5000 | 408 (reading) / connection closed (writing) |
| `keep_alive_timeout_ms` | 10000 | idle connection closed silently |
| `max_request_body_bytes` | 1 MiB | 413 |
| `max_header_bytes` | 8192 | 431 |
| `max_connections` | 1024 | new connection closed immediately (counted as rejected) |
| `worker_threads` | 2 | n/a |

Errors produced by the connection itself (400, 408, 413, 431, 500) always close the connection. 404 and 405 (with `Allow`) come from the handler and keep it open. A handler exception becomes a logged 500. Status codes that need an upstream (such as 502/504) do not exist yet. `Expect: 100-continue` is not implemented; clients that send it simply proceed after their own timeout.

### Shutdown integration

`Application` registers `http-server` with the `ShutdownCoordinator` after the signal handlers, so on shutdown it stops first:

```text
SIGINT/SIGTERM -> Application::run() leaves its loop -> ShutdownCoordinator
  -> HttpServer::stop(grace_period)
       1. TcpServer::stop()          stop accepting (acceptor closed on its strand)
       2. beginDrain() every connection
            idle -> closed now;  busy -> finish the response with "Connection: close", then close
       3. wait up to the grace period for the tracker to empty
          still open -> forceClose(), wait briefly
       4. release the work guard, stop the io_context, join the workers
  -> signal handlers released -> flush -> "shutdown completed"
```

`stop()` is idempotent and thread-safe. A failure to bind makes `Application::initialize()` fail (exit code 1), releasing the signal handlers it had installed.

### Docker

The image exposes the configured port (default 8080) and has a real `HEALTHCHECK` that runs `edgeflow --healthcheck` (a `GET /health` against the configured port, success only on 200), so no curl is needed. Compose publishes `${EDGEFLOW_HTTP_PORT:-8080}` to the container port, which must equal `server.port` in the mounted configuration.

### Planned in later phases

Service discovery, health checking of backends, load balancing, reverse proxying, reliability features, Redis caching and rate limiting, metrics and tracing.
