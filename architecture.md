# EdgeFlow — Architecture

> **Status:** Sections 1-16 describe the **target architecture**. Only what sections 17 (Phase 1: foundation), 18 (Phase 2: networking engine), 19 (Phase 3: service discovery and registry) and 20 (Phase 4: health checking) and 21 (Phase 5: load balancing) describe is implemented; everything else is introduced phase by phase according to [Phases.md](Phases.md).

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

Instances are registered, updated and deregistered. PostgreSQL is the persistent source of metadata. Phase 3 implements this against PostgreSQL directly (section 19). Phase 4 (section 20) adds health checking and the routable view on top of it. A cached, lock-light in-memory view for the request path is not implemented; PostgreSQL stays the only copy of registry state.

## 5. Health-Check Flow

`HealthChecker` periodically probes each instance (TCP connect or HTTP endpoint). Consecutive failures move an instance to unhealthy and exclude it from routing; consecutive successes restore it. Implemented in Phase 4 (section 20); transitions are logged. Counting them as metrics is part of Phase 9.

## 6. Load-Balancing Flow

`Router` holds a `LoadBalancingStrategy` (Round Robin, Least Connections, Weighted Routing, Consistent Hashing). It receives only healthy instances and returns a selection. Consistent Hashing uses a request key (e.g. client IP or header). Strategy is selected via configuration. Implemented in Phase 5 (section 21); the router is not yet in the request path because forwarding is Phase 6.

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

## 19. Phase 3 Implementation: Service Discovery & Registry

### Implemented in Phase 3

```text
include/edgeflow/discovery/  ServiceInstance  ServiceRegistry  PostgresServiceRegistry  Validation  Result
include/edgeflow/storage/    Postgres (PgConnection, PgPool, PgResult)  Migrator
include/edgeflow/network/    RegistryRequestHandler
src/discovery/ src/storage/ src/network/RegistryRequestHandler.cpp   matching implementations
db/migrations/               SQL schema, embedded into the binary at build time
```

| Component | Responsibility |
|-----------|----------------|
| `ServiceInstance` | One backend instance of a logical service: identity (`service`, `instance_id`, `host`, `port`; immutable) plus mutable metadata (`status`, `health`, `version`, `weight`, `connection_count`) and database-assigned timestamps. |
| `ServiceRegistry` | Abstract discovery interface: `registerInstance`, `deregisterInstance`, `lookupService`, `getInstance`, `listServices`, `updateInstance`, `adjustConnectionCount`. It knows nothing about HTTP, routing or health probing; Phases 4, 5 and 6 consume it. |
| `PostgresServiceRegistry` | The implementation. PostgreSQL is the only store: there is no in-memory copy, so reads always see what is persisted and a restart loses nothing. |
| `PgPool` / `PgConnection` / `PgResult` | A thin RAII wrapper over libpq: fixed-size pool of lazily opened connections, parameterised statements only (`PQexecParams`), SQLSTATE and constraint name exposed for error mapping. |
| `migrate()` | Applies the embedded migrations in order, each in a transaction, recorded in `schema_migrations`, serialised across processes by a PostgreSQL advisory lock. Idempotent. |
| `RegistryRequestHandler` | JSON API under `/services`. A decorator over the Phase 2 handler: everything else is passed through unchanged. |

### Model and semantics

- **Service vs instance.** A service is a logical name (`user-service`, lowercase `[a-z0-9._-]`, 1-64 chars). An instance is one concrete endpoint of it (`user-service @ 10.0.0.11:9001`). A service has any number of instances. Identity is `(service, instance_id)`; `instance_id` is supplied by the caller or assigned by the database.
- **Two different statuses.** `status` is the *registration* state set by whoever manages the deployment: `active` (default), `draining`, `disabled`. `health_status` is the *last known health*: `unknown` (default), `healthy`, `unhealthy`. Phase 3 never probes anything: health is stored metadata, written at registration or through `PATCH`. Phase 4 will own the transitions.
- **Duplicates are rejected, not overwritten.** Registering an `(service, instance_id)` or `(service, host, port)` that already exists returns `DuplicateInstance` (HTTP 409). Registration is not an idempotent upsert, so two deployments can never silently replace each other; changing metadata is an explicit update. The same `host:port` may exist under a different service.
- **Deregistration deletes the row.** A second deregistration returns not-found (404). The service name stays known with zero instances, so discovery of an emptied service returns an empty list (200) while a never-registered service is 404.
- **Mutable vs immutable.** `status`, `health_status`, `version`, `weight` (0-1000) and `connection_count` are mutable. `service`, `instance_id`, `host` and `port` are immutable; a moved instance is deregistered and registered again. `PATCH` rejects immutable and unknown fields.
- **`weight`** is stored metadata for Phase 5's weighted routing; nothing routes on it yet.
- **`connection_count`** is the number of in-flight requests to an instance. Phase 3 stores it and offers `updateInstance` (set) and `adjustConnectionCount` (atomic add, clamped at zero, safe under concurrency). Phase 6's proxy will maintain it and Phase 5's Least Connections will read it.

### Schema (`db/migrations/001_service_registry.sql`)

`services(id, name UNIQUE, created_at)` and `service_instances(id, service_id -> services ON DELETE CASCADE, instance_id, host, port, status, health_status, version, weight, connection_count, registered_at, updated_at)`. Constraints: `UNIQUE (service_id, instance_id)` (identity), `UNIQUE (service_id, host, port)` (endpoint), and CHECKs for the name and id formats, port range, status and health vocabularies, version length, weight range and non-negative connection count. The application validates the same rules first; the CHECKs are the backstop. Normal discovery is one statement (`services LEFT JOIN service_instances`, served by the identity index through its `service_id` prefix), with no per-instance queries.

### Concurrency and failure handling

- Each operation is one parameterised statement, atomic on the server, run on a pooled connection; the registry holds no shared mutable state, so it needs no locks. PostgreSQL's constraints decide races: of N simultaneous registrations of one instance id exactly one wins and the rest get `DuplicateInstance`.
- A failed or unreachable database is never reported as success. Errors map to `DatabaseUnavailable` (HTTP 503), `InvalidArgument` (400), `ServiceNotFound`/`InstanceNotFound` (404), `DuplicateInstance` (409) or `Internal` (500).
- A pooled connection that the server closed while idle (restart, failover) is detected before use and replaced. If a connection dies mid-statement, reads (lookup, get, list) are retried once because they are idempotent; writes are not retried, because it is unknown whether they committed, so they report `DatabaseUnavailable` and the caller decides.
- Passwords are never logged and never appear in configuration: `database.password_env` names the environment variable that holds it.

### HTTP registry API

```text
GET    /services                                  list service names
POST   /services/{service}/instances              register       -> 201 + Location
GET    /services/{service}/instances              discover       -> 200 (404 unknown service)
GET    /services/{service}/instances/{instance}   one instance
PATCH  /services/{service}/instances/{instance}   update mutable metadata
DELETE /services/{service}/instances/{instance}   deregister     -> 204
```

This is registry management only. EdgeFlow does not forward client traffic to the registered instances in Phase 3 (that is Phase 6).

### Startup and the I/O workers

With `database.enabled: true`, `Application::initialize()` creates the pool, applies migrations and mounts the registry API; if PostgreSQL is unreachable or a migration fails, initialization fails (exit code 1) before the HTTP server starts. With `enabled: false` (the default) EdgeFlow behaves exactly as in Phase 2. `RequestHandler::handle` is synchronous, so a registry request occupies an I/O worker thread for the duration of its query (bounded by the 10 s server-side statement timeout and the pool size); size `server.worker_threads` and `database.pool_size` accordingly. Making request handling asynchronous belongs to a later phase.

### Planned in later phases

Periodic health checking that writes `health_status` (Phase 4), instance selection (Phase 5), request forwarding and `connection_count` maintenance (Phase 6).

## 20. Phase 4 Implementation: Health Checking & Dynamic Discovery

### Implemented in Phase 4

```text
include/edgeflow/discovery/  HealthChecker  HealthState (HealthTracker, HealthPolicy)  Prober  NameResolver
src/discovery/               matching implementations
ServiceRegistry (Phase 3) gained: listInstances  lookupRoutable  updateHealth
```

```text
registry.listInstances() --refresh--> one Monitor per instance to check
Monitor timer --> Prober (TCP or HTTP, bounded by timeout) --> ProbeResult
ProbeResult --> HealthTracker (thresholds) --on a transition--> registry.updateHealth()
registry.lookupRoutable()  =  status active  AND  health healthy
```

| Component | Responsibility |
|-----------|----------------|
| `Prober` (`TcpProber`, `HttpProber`) | Executes ONE probe asynchronously on an `io_context` and reports a `ProbeResult`. Both kinds share one operation (resolve, connect, timeout, cancellation); HTTP adds the request and the response head. Knows nothing about instances or health state. |
| `NameResolver` | Host-name lookups for probes, isolated per host (see below). |
| `HealthTracker` | The state machine for one instance: pure and deterministic, no I/O. |
| `HealthChecker` | Scheduling, discovery refresh and persistence. Owns a private `io_context` thread (timers and probes) and a one-thread pool for the blocking PostgreSQL calls. |
| `ServiceRegistry` | Still the only store. The checker keeps per-instance runtime state (streak counters, timers), not a copy of the registry. |

### Probe classification

- **TCP:** healthy if and only if a TCP connection is established within `timeout_ms`. This proves that something accepts connections on that port; it says nothing about the application behind it.
- **HTTP:** `GET <http_path>` with `Connection: close`. Healthy if and only if a complete status line and headers arrive within `timeout_ms` AND the status is **2xx**. Everything else is unhealthy: 1xx, 3xx (redirects are not followed), 4xx, 5xx, a malformed response, a connection closed before or during the response, a reset, an unresolvable name, a refusal, and a timeout. Only the response head is read, never the body.
- **Bounded:** the timeout covers the WHOLE probe (resolve + connect + request + response head). On expiry every pending operation is cancelled. A stalled backend therefore costs one timeout, never a stuck checker.
- **Name resolution is isolated.** Asio's own resolver runs `getaddrinfo()` one lookup at a time on a single background thread, so one lookup that hangs (for example the DNS name of a container that was just removed) would delay every other instance's lookup and make healthy instances time out. This was observed in Docker Compose during validation and is why `NameResolver` exists: each distinct host is looked up on its own thread, concurrent lookups of one host share a single lookup, at most 16 lookups run at once (beyond that a probe fails at once as unhealthy instead of queueing behind slow ones), and a cancelled or timed-out probe's lookup is never delivered, so a late result cannot reach a torn-down `io_context`. The lookup threads are detached because `getaddrinfo()` cannot be cancelled and shutdown must not wait for it. IP literals skip resolution.

### State machine

```text
Unknown   --success--> Healthy      the first probe decides at once (no prior state to flap from)
Unknown   --failure--> Unhealthy
Healthy   --failure_threshold consecutive failures--> Unhealthy
Unhealthy --success_threshold consecutive successes--> Healthy
```

A result in the opposite direction resets the streak, so a single transient failure cannot flip a settled instance (unless the threshold is 1). A transition starts a fresh streak. The state never returns to `unknown`; only a new registration starts there. Only transitions are written to PostgreSQL, so steady health causes no database writes. If a write fails (for example during an outage) the checker keeps its verdict and retries after the next probe; it never loops.

### Registration status vs health status

They stay separate. Registration status (`active`, `draining`, `disabled`) says whether an instance is meant to receive traffic; health says whether it works.

| Registration status | Probed? | Routable? |
|---|---|---|
| `active` | yes | only while `healthy` |
| `draining` | yes | never (its health is still tracked) |
| `disabled` | no (last known health is kept) | never, however healthy |

`lookupRoutable` (and `GET /services/{service}/routable`) is the neutral view Phase 5 will consume: `active AND healthy`, no ordering and no strategy. Plain discovery (`lookupService`) still returns everything. When health checking is on, the checker owns `health_status`: a value written by hand through `PATCH` is corrected by the next probe.

### Discovery refresh

Every `refresh_interval_ms` the checker re-reads the registry with one `listInstances()` query (not one per instance). A new instance is probed immediately, without an EdgeFlow restart; a deregistered or disabled instance is dropped and its pending probe cancelled; an instance whose `registered_at`, host or port changed (deregistered and registered again) starts over as a new instance and results computed for the old incarnation are discarded; `updateHealth` is additionally pinned to `registered_at` in the database, so a late result can never be written onto a newer instance. A failed refresh keeps the current instances and is logged once when it begins and once when it recovers.

### Concurrency and shutdown

- All checker state is confined to one thread, so it needs no locks. Blocking PostgreSQL calls run on a separate single-thread pool, so database latency never delays probing and no lock is held across I/O. Probes are limited to `max_concurrent_checks`; the rest wait in a queue.
- Shutdown order: the HTTP server stops first, then `HealthChecker::stop()` posts a cancel-everything step onto its thread (timers, probes, queue), stops that thread, waits for an in-flight database call (bounded by the registry's own timeouts) and joins the pool. After it returns nothing runs; `stop()` is idempotent and concurrent callers all wait for completion.

### Limitations

One probe type applies to all instances (`health_check.type`); the instance model has no per-instance check settings. The routable view is a database query on each call: it is correct and always current but is not the cached, lock-light view that request-path routing may want (a Phase 5 concern, to be measured first).

## 21. Phase 5 Implementation: Load Balancing Engine

### Implemented in Phase 5

```text
include/edgeflow/routing/  LoadBalancer  Strategies (RoundRobin, LeastConnections, WeightedRouting, ConsistentHashing)  Router
src/routing/               matching implementations
```

```text
ServiceRegistry::lookupRoutable(service)    active AND healthy instances (Phase 4), one snapshot
        |
        v
LoadBalancer::select(snapshot, {key})       the configured strategy picks ONE
        |
        v
selected instance                           [Phase 6 will forward the request to it]
```

| Component | Responsibility |
|-----------|----------------|
| `LoadBalancer` | The abstraction all four strategies implement: `select(instances, context)` returns a pointer to one element of the given set, or null when it is empty. Non-virtual `select` + virtual `doSelect` (the empty set is handled once, centrally). |
| `RoundRobin`, `LeastConnections`, `WeightedRouting`, `ConsistentHashing` | The four strategies. They are pure: no registry, no database, no health logic, no HTTP. |
| `Router` | The glue for one request: reads the routable set from the registry, then asks the strategy. Returns the chosen instance or `ServiceNotFound` / `NoRoutableInstance` / the registry's own error. It does not forward anything. |
| `makeLoadBalancer(config::RoutingStrategy)` | The factory behind `routing.strategy`. |

### Responsibility boundary

Health and registration eligibility belong to discovery (Phase 4). The strategies receive the routable set and choose among it; none of them inspects `health` or `status`, so an excluded instance cannot be chosen by any of them, and a recovered one is chosen again as soon as discovery lists it. Strategies never see PostgreSQL: the router takes one snapshot from `lookupRoutable` and no registry access or lock is held while choosing.

### Instance identity and ordering

An instance is identified by `(service, instance_id)`: never by its position in the input, its address in memory, or mutable metadata. Every strategy first puts the input into canonical order of that identity (an instance listed twice counts once), so the order in which discovery returns instances has no effect on any strategy. The hash input for consistent hashing is the text `service/instance_id` (neither part can contain `/`).

### The strategies

- **Round Robin** cycles `a, b, c, a, ...` in canonical order. Its only state is the identity of the instance chosen last; the next choice is the first instance with a greater identity, wrapping to the smallest. A removed instance is skipped and a new one joins where its position falls, so the cycle stays correct when the set changes between calls. One mutex guards that value, so concurrent callers share one strict cycle (tested: 24 000 concurrent selections over 3 instances give exactly 8 000 each).
- **Least Connections** picks the smallest `connection_count`, ties to the smallest identity. It is stateless and reads the count from the snapshot it is given. **Semantics:** Phase 5 only READS this field. It is whatever was written through the registry (`updateInstance`, `adjustConnectionCount`). The proxy that opens and closes backend connections and keeps the count current is Phase 6, so a routing decision does not change the count (it is not a connection) and, until Phase 6, the count is only as current as the writes made to it.
- **Weighted** uses smooth weighted round-robin (as nginx): over any window of (sum of weights) selections of an unchanged set each instance is chosen exactly `weight` times, interleaved rather than in bursts, with no randomness. Weights 5/3/2 give the sequence `abcaabacba`. Weight 0 means no share while any instance has a positive weight; if every weight is 0 the instances are treated as equal rather than refusing to route. Weights are summed in 64 bits. State is a running score per instance, pruned to the current set on every call.
- **Consistent Hashing** places each instance at 160 virtual points on a 64-bit ring (`stableHash("service/instance_id#replica")`; FNV-1a plus a murmur3 finaliser, identical on every platform) and serves a key from the first point at or after `stableHash(key)`. The ring is immutable and cached; it is rebuilt only when the set of identities differs from the cached one, and selection works on a shared snapshot. Adding an instance moves keys only to it; removing one moves only its keys (both verified by tests, with about 1/N of the keys moving). Weights are not used. **Key semantics:** the key is supplied by the caller (`RoutingContext::key`); an empty key is hashed like any other, so keyless requests all land on one instance. The other strategies ignore the key.

### Configuration and the decision endpoint

`routing.strategy` is one of `round_robin` (default), `least_connections`, `weighted`, `consistent_hashing`; anything else, or an unknown key in the section, is a startup error. With the registry enabled the application builds a `Router` over it. `GET /services/{service}/route[?key=K]` returns the instance the strategy picks right now, together with the strategy name: `200`, `404` unknown service, `503` when the service has no routable instance. **It is a routing decision only**: nothing is forwarded and no state changes. It exists to observe and test routing in a running system until Phase 6 uses `Router::route` for every proxied request.

### Limitations before Phase 6

No request is forwarded to the selected instance. `connection_count` is not maintained by EdgeFlow yet. The routable set is read from PostgreSQL for each decision (correct and current, but not the cached view a high-rate proxy may want; to be measured, not assumed, in Phase 9). One strategy applies to all services.
