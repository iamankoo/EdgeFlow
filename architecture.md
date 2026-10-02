# EdgeFlow — Architecture

> **Status:** Sections 1-16 describe the **target architecture**; only the foundation in the "Phase 1 Implementation" section at the end is implemented. All other components are introduced phase by phase according to [Phases.md](Phases.md).

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

Everything in sections 1-16 other than the above: networking, gateway pipeline, discovery, health checks, routing, proxy, reliability, rate limiting, caching, storage clients, metrics and tracing. The `server` configuration section is validated in Phase 1 but nothing listens on it.
