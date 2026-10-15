# EdgeFlow — Development Phases

**Current Phase: Phase 8 completed — awaiting Phase 9 requirements**

This document is the authoritative roadmap. Phases must not be skipped. A phase begins only after the previous phase has met its exit condition. Later-phase functionality must not be implemented early unless strictly required as a dependency and clearly documented.

Before implementing a phase, its detailed implementation prompt must be cross-checked against this file. Any mismatch must be reported, not silently resolved.

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Foundation & Core Infrastructure | Completed |
| 2 | TCP/HTTP Networking Engine | Completed |
| 3 | Service Discovery & Registry | Completed |
| 4 | Health Checking & Dynamic Discovery | Completed |
| 5 | Load Balancing Engine | Completed |
| 6 | Reverse Proxy & Request Forwarding | Completed |
| 7 | Reliability Engineering | Completed |
| 8 | Redis Cache & Distributed Rate Limiting | Completed |
| 9 | Observability, Testing & Performance | Not started |
| 10 | Optimization, Production Hardening & Release | Not started |

---

## Phase 1 — Foundation & Core Infrastructure

Build:

- CMake project
- C++20 configuration
- dependency management
- repository structure
- configuration system
- spdlog logging
- Docker setup
- Docker Compose
- GoogleTest
- basic GitHub Actions CI
- application startup
- application initialization
- graceful shutdown

**Exit condition:** EdgeFlow starts, loads configuration, initializes required infrastructure, logs lifecycle events, and shuts down cleanly.

**Completion summary:** Met. The CMake/C++20 project, pinned dependencies (spdlog, yaml-cpp, GoogleTest), strict YAML configuration, spdlog logging, application lifecycle with SIGINT/SIGTERM graceful shutdown, multi-stage Dockerfile, Docker Compose file, and GitHub Actions workflow are in place. 56 tests pass; the container starts, logs its lifecycle, and exits 0 on `docker stop` (SIGTERM) and SIGINT. See `summary.md` for details.

---

## Phase 2 — TCP/HTTP Networking Engine

Build:

- TCP listener
- asynchronous connections
- HTTP/1.1 handling
- request parsing
- response generation
- keep-alive
- connection lifecycle
- connection timeout
- concurrent clients

Technology: Boost.Asio + Boost.Beast.

**Exit condition:** EdgeFlow functions as a concurrent HTTP server.

**Completion summary:** Met. Asynchronous TCP listener and HTTP/1.1 server on Boost.Asio + Boost.Beast with request parsing, response generation, keep-alive, a per-connection lifecycle, idle and request timeouts (408), and concurrent clients, integrated with the Phase 1 graceful shutdown. 127 tests pass with GCC 13 and Clang 18 (`-Werror`), including real-socket tests with 1/10/50/100 concurrent clients. The real binary was exercised with curl and raw sockets, and in Docker and Docker Compose (healthcheck healthy, `docker stop` exits 0). The remote GitHub Actions run has not been confirmed. No performance figures were measured. See `summary.md`.

---

## Phase 3 — Service Discovery & Registry

Build:

- service registration
- deregistration
- service lookup
- service instance metadata
- instance status
- health status
- service version
- instance weight
- connection count

PostgreSQL is the persistent source of service metadata.

**Exit condition:** EdgeFlow can discover registered backend service instances dynamically.

**Completion summary:** Met. A PostgreSQL-backed registry (libpq, connection pool, embedded SQL migrations) stores service instances with identity, registration status, health status, version, weight and connection count; instances can be registered, deregistered, looked up, listed and updated through a `ServiceRegistry` interface and a JSON API under `/services`. Verified with real PostgreSQL in Docker Compose: three instances registered, discovered, EdgeFlow restarted and the whole stack taken down and up again with the instances still discoverable, one instance deregistered and no longer returned, and a database outage reported as `503` with recovery afterwards. 192 tests pass with GCC 13 and Clang 18 (`-Werror`) against PostgreSQL (30 are skipped when no database is available). Health status and weight are stored metadata only: probing is Phase 4, selection is Phase 5. The remote GitHub Actions run has not been confirmed. No performance figures were measured. See `summary.md`.

---

## Phase 4 — Health Checking & Dynamic Discovery

Build:

- periodic health checks
- TCP health checks
- HTTP health checks
- unhealthy-instance handling
- automatic recovery
- discovery refresh
- health state transitions

**Exit condition:** Routing automatically excludes unhealthy instances and reintroduces recovered instances.

**Completion summary:** Met. A health checker probes registered instances with timeout-bounded TCP or HTTP checks (HTTP healthy only on 2xx), drives an explicit state machine with failure and recovery thresholds, writes transitions to the PostgreSQL registry, and re-reads the registry periodically so added, removed, disabled and re-registered instances are handled without a restart. The registry exposes a neutral routable view (`active` AND `healthy`; `GET /services/{service}/routable`) that Phase 5 will consume. Verified with real backend containers in Docker Compose: a stopped backend and a backend returning HTTP 500 were excluded and, once they recovered, reintroduced; new instances were picked up and deregistered or disabled ones stopped being probed; health survived an EdgeFlow restart and a database outage; SIGTERM stopped the HTTP server then the checker. 272 tests pass with GCC 13 (Debug and Release) and Clang 18 (`-Werror`) against PostgreSQL (42 are skipped when no database is available). No selection or load-balancing logic exists yet (Phase 5). The remote GitHub Actions run has not been confirmed. No performance figures were measured. See `summary.md`.

---

## Phase 5 — Load Balancing Engine

Implement:

- Round Robin
- Least Connections
- Weighted Routing
- Consistent Hashing

The algorithms must operate through a clean routing abstraction.

**Exit condition:** Requests can be distributed among healthy backend instances using all four strategies.

**Completion summary:** Met. A `LoadBalancer` abstraction with Round Robin, Least Connections, Weighted Routing (smooth weighted round-robin) and Consistent Hashing (virtual-node ring) chooses among the routable (active AND healthy) instances that discovery supplies, selected by `routing.strategy`. Verified with real backend containers in Docker Compose against PostgreSQL and the real health checker: all four strategies distributed decisions only among healthy instances, an unhealthy instance with the most attractive numbers was never chosen, and failure, recovery and instance changes were followed. 379 tests pass with GCC 13 (Debug and Release) and Clang 18 (`-Werror`) against PostgreSQL (49 are skipped when no database is available). Requests are not yet forwarded to the selected instance (Phase 6) and `connection_count` is not yet maintained by a proxy. No performance figures were measured. See `summary.md`.

---

## Phase 6 — Reverse Proxy & Request Forwarding

Build:

- request forwarding
- response forwarding
- header propagation
- connection reuse
- backend connection pooling
- request IDs
- upstream timeout
- backend failure handling

**Exit condition:** Client → EdgeFlow → Backend → EdgeFlow → Client works reliably.

**Completion summary:** Met. `/proxy/{service}/...` is routed through the Phase 5 `Router` to a healthy instance (prefix stripped, query kept) by an asynchronous proxy (new `RequestHandler::handleAsync` boundary, its own upstream I/O threads, a per-backend keep-alive connection pool, a safeguard for stale pooled connections, connect and whole-exchange timeouts). Request and response bodies, status codes and headers are forwarded (hop-by-hop headers removed, `Via`/`X-Forwarded-*` added, `X-Request-Id` kept or generated); failures map to 502 (refused, reset, invalid or incomplete response), 503 (no healthy instance) and 504 (timeout), and `connection_count` is maintained around each request. Verified with real containers in Docker Compose (PostgreSQL, health checker, several Python backend containers): forwarding of GET/POST/PUT/DELETE/HEAD with query and body, header propagation, round robin over the healthy backends only, 40 sequential requests over one upstream connection, `connection_count` 1 and 3 during slow requests and 0 afterwards, backend status codes forwarded unchanged, 502 for a closed connection, reset, garbage and refused port, 504 at the 5 s upstream timeout, 503 with no healthy backend, a backend restart between requests answered 200 on a new connection, and `docker stop` with a request in flight let it finish (200) and exited 0. 507 tests pass with GCC 13 (Debug and Release) and Clang 18 (`-Werror`) against PostgreSQL (55 are skipped when no database is available); the proxy suites passed 12 repeated runs, ThreadSanitizer reported no warnings and AddressSanitizer+UBSan reported no errors on the 124 proxy-related tests. No retries, circuit breaker, failover, caching or TLS (Phase 7 and later). A hard kill (SIGKILL) cannot release in-flight `connection_count` values. The remote GitHub Actions run has not been confirmed. No performance figures were measured. See `summary.md`.

---

## Phase 7 — Reliability Engineering

Build:

- timeouts
- retry policy
- retry limits
- exponential backoff
- circuit breaker
- circuit states
- failover
- retryable-status configuration
- graceful degradation

Circuit breaker lifecycle:

```text
CLOSED → OPEN → HALF-OPEN → CLOSED
```

**Exit condition:** Backend failures do not unnecessarily cascade into gateway failures.

**Completion summary:** Met. `ReliabilityManager` (new `reliability` module) wraps every proxied request in a configurable policy: a total request budget (`reliability.timeout.total_timeout_ms`; each attempt gets at most `min(proxy.upstream_timeout_ms, what is left)`), a retry policy (`max_attempts` counts every attempt, exponential backoff `base_delay_ms` doubling up to `max_delay_ms` with downward jitter, `retryable_statuses`, `retryable_methods`; a request that provably never reached a backend is retried whatever its method), a circuit breaker per backend instance (CLOSED -> OPEN -> HALF-OPEN -> CLOSED; one failure per request per backend; `failure_threshold`, `recovery_timeout_ms`, `half_open_max_requests`), failover (a retry prefers a backend the request has not tried; backends with an open circuit are skipped by the router filter) and graceful degradation (when nothing is eligible the client gets a fast `503` or the last backend answer; the gateway itself, `/health` and other services stay up). Verified with real containers in Docker Compose (PostgreSQL, health checker, Python backend containers): with one of two backends answering `503`, 8 of 8 requests succeeded through the other, the circuit opened after 3 failed requests and the failing backend received no traffic while it was open, then went OPEN -> HALF-OPEN -> CLOSED and served again; a single backend that always fails was tried exactly 3 times (backoff pauses 99 ms and 191 ms), then answered from the open circuit in about 1 ms; a hanging backend ended at 12.0 s (the total budget); stopping a backend container left 9 of 9 requests (GET and POST) successful; POST/DELETE with a `503` were not retried (one backend hit), a `500` was forwarded without retry, HEAD was retried. **Exit condition ("Backend failures do not unnecessarily cascade into gateway failures") is met in these runs:** no client received a gateway error while a healthy backend existed, and the gateway and unrelated services stayed available while every backend of one service failed. 630 tests pass with GCC 13 (Debug and Release) and Clang 18 (`-Werror`) against PostgreSQL (58 are skipped when no database is available: 572 pass); the 330 reliability/proxy/pool/async/server/application/router tests passed 10 repeated runs, AddressSanitizer+UBSan reported no errors and ThreadSanitizer no warnings on them. Limitations: circuit state is per process and in memory; no metrics (Phase 9); no cache or rate limiting (Phase 8). No performance figures were measured. The remote GitHub Actions run has not been confirmed. See `summary.md`.

---

## Phase 8 — Redis Cache & Distributed Rate Limiting

Build:

- Redis integration
- response caching
- cache expiration
- cache hit/miss handling
- Token Bucket rate limiting
- per-IP limits
- per-API-key limits
- per-route/service limits
- shared Redis-backed rate-limit state

**Exit condition:** Caching and rate limiting operate correctly under concurrent traffic.

**Completion summary:** Met. Redis (`redis:7`, through a new asynchronous RESP2 client on Boost.Asio: connection pool, connect/command timeouts, a fail-fast window after an outage, stale-connection replacement, AUTH from the environment) holds the response cache and the rate-limit state. `PolicyHandler` sits in front of the Phase 6/7 proxy: a distributed **Token Bucket** (one atomic Lua script in Redis, Redis's own clock, all-or-nothing across the buckets of a request) limits **per client address, per API key (stored only as a hash) and per service** (with per-service overrides) and answers `429` with `Retry-After` and `X-RateLimit-Scope`; a **response cache** (key = SHA-256 of method, service and upstream target; lifetime from `s-maxage`/`max-age` or the default, capped, enforced by Redis; hit/miss reported in `X-Cache`; bypass rules for Authorization, Cookie, Range, If-* and no-cache/no-store requests; Set-Cookie, Vary, private, no-store and gateway-generated responses never stored; invalidation by a successful POST/PUT/PATCH/DELETE) serves repeated GETs without contacting a backend. Redis failure never fails a request: the cache is bypassed and the rate limiter follows `rate_limit.on_redis_failure` (`allow` by default, or `reject` = 503). Verified under concurrent traffic: 160 concurrent requests are granted exactly the bucket's 40 (two or three limiter instances with their own connections, 600 concurrent requests, exactly 100), and on real containers (PostgreSQL, Redis, health checker, backends, up to four EdgeFlow instances sharing one Redis): bursts of 40/100/20 concurrent requests were granted exactly 10/30/5 (API key, address, service), **two EdgeFlow instances alternating over one bucket granted 10 in total (not 20)**, 200 concurrent GETs of a cold URL produced 12 misses and 188 hits and a second round 200 hits with 0 backend requests, a hit is served by a second instance from the first one's entry, an entry expires after its lifetime, a POST invalidates it, with Redis stopped 150 of 150 concurrent requests succeeded without limiting (fail open) while the fail-closed instance answered 503 and `/health` stayed 200, and a `docker restart` of Redis under 6000 concurrent requests produced 6000 x 200. 733 tests pass with GCC 13 (Debug and Release) and Clang 18 (`-Werror`) against PostgreSQL and Redis (111 are skipped without them: 622 pass); the 446 Phase 8, reliability, proxy, pool, async, server, application and router tests passed 10 repeated runs, AddressSanitizer+UBSan reported no errors and ThreadSanitizer no warnings on them. Limitations: Redis is a single shared dependency (no cluster), clients are identified by TCP peer address, no request coalescing, revalidation, `Vary` or `Expires`; an unreachable Redis costs one request per retry interval up to the connect timeout. No performance figures were measured. The remote GitHub Actions run has not been confirmed. See `summary.md`.

---

## Phase 9 — Observability, Testing & Performance

Implement metrics for:

- requests/sec
- active connections
- request count
- error rate
- p50 latency
- p95 latency
- p99 latency
- backend latency
- cache hit ratio
- retry count
- circuit-breaker trips
- rejected requests

Build:

- unit tests
- integration tests
- concurrency tests
- routing tests
- database tests
- Redis tests
- failure tests
- end-to-end tests

Load testing target: **~10K requests/sec**. The final resume metric MUST be based on actual measured results.

**Exit condition:** EdgeFlow has reproducible benchmark and test results.

---

## Phase 10 — Optimization, Production Hardening & Release

Perform:

- CPU profiling
- memory profiling
- flamegraph analysis
- lock-contention analysis
- network-performance analysis
- database-performance analysis
- Redis-performance analysis

Optimize:

- connection handling
- thread utilization
- memory allocation
- connection pooling
- routing
- caching
- synchronization

Then perform:

- security review
- configuration hardening
- Docker optimization
- graceful-shutdown testing
- failure testing
- documentation
- architecture diagrams
- benchmark report
- final README

**Exit condition:** EdgeFlow is a reproducible, documented, tested, benchmarked, Dockerized systems project suitable for a serious backend/SDE portfolio.

---

## Resume Metrics Rule

The following are **targets, not achieved results**:

- ~10 service instances
- ~100K+ requests
- ~99.5% availability
- ~10K requests/sec
- p95 latency around ~20 ms

They must not be presented as achieved until actual tests produce them. When benchmarking is implemented, record the measured RPS, p50, p95, p99, error rate, availability, concurrency, and resource utilization. Only measured results may be used in the final resume.
