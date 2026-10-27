# EdgeFlow — Development Phases

**Current Phase: Phase 10 completed — the roadmap is finished and the project is closed**

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
| 9 | Observability, Testing & Performance | Completed |
| 10 | Optimization, Production Hardening & Release | Completed |

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

**Completion summary:** Met. New `observability` module: a Prometheus-compatible `GET /metrics` endpoint (text format 0.0.4, off by default, `metrics.*`) answered by an outermost `MetricsHandler` that times every other request, with exact fixed-bucket latency histograms (78 buckets, 100 us to about 62 s) and a sliding window for requests/sec, error ratio and p50/p95/p99; per-component collectors read the existing counters at scrape time (active/accepted/rejected connections, request count by status class, 5xx error rate, backend latency measured around every exchange with a backend, cache hits/misses and hit ratio, retries and failovers, circuit-breaker trips and states, rejected requests by reason, Redis state). 53 tests were added (786 in total) covering the metrics at unit, integration, concurrency and full-stack level (the whole Application over real PostgreSQL and Redis, each metric compared with the behaviour that provokes it), and the test suite was run in every configuration of the project: GCC Debug and Release and Clang 18 (`-Werror`) 786/786 with PostgreSQL and Redis; the 499 Phase 8/9, reliability, proxy, pool, async, server, application and router tests passed 10 repeated runs, AddressSanitizer+UBSan reported no errors and ThreadSanitizer no warnings on them; the Docker image build (no database, no Redis) ran 786 tests with 667 passed, 119 skipped, 0 failed. A real Prometheus server scraped the Compose stack without errors and its totals matched what the clients observed (162 proxied requests, 64 refused with 429, 3 retries and failovers, 1 circuit trip, cache hit ratio 0.5). Benchmarks are reproducible: `bench/micro` (Google Benchmark, optional target) and `bench/load` (a Compose stack, wrk, raw outputs and the gateway's own `/metrics` saved per run, `bench/results/phase9-codespace-4cpu/`). **Measured on a 4 vCPU shared machine (wrk, 64 connections, 3 runs of 20 s):** the server alone 36K requests/s (p99 4.9 ms); a proxied request answered from the Redis cache 11.2K requests/s (p99 10.9 ms); a proxied request that reaches a backend (rate limiter, PostgreSQL routing, proxy) **1.8K requests/s** (p50 34.6 ms, p99 54.0 ms). The ~10K requests/s target is therefore met only for cached responses; improving the uncached path is Phase 10. Limitations: one machine type, closed-loop load, unauthenticated `/metrics`, no per-service labels, no availability (~99.5 %) or 10-instance measurement yet. The remote GitHub Actions run has not been confirmed. See `summary.md`.

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

**Completion summary:** Met. Profiled the Phase 9 gateway (perf + FlameGraph, pidstat, strace, `pg_stat_statements`, Redis `commandstats`, heaptrack, network counters) and changed only what the profiles named. The uncached proxied path (S2, wrk, 64 connections, 4 vCPU shared machine) went **1,935 -> 11,637 req/s (6.0x)**, p50 32.35 -> 5.36 ms, p99 45.98 -> 9.44 ms, in three measured steps: `proxy.connection_counting: auto` (no PostgreSQL row updates unless `least_connections`), `routing.snapshot_ttl_ms` (bounded-staleness routing snapshot, opt-in) and running attempts inline on the I/O thread when nothing can block. `TCP_NODELAY` was tried, showed no difference, and was reverted; memory, Redis and selection cost were measured and left alone. Hardening: opt-in bearer token for the registry API (`admin.token_env`, fail closed), log redaction of request targets, loopback-only Compose publishing, pids/memory limits, tmpfs, stripped binary, BuildKit cache; a PostgreSQL pool fail-fast window was added after the failure test showed an unreachable database multiplied latency. Verified on a running gateway: 29/29 malformed-input and resource-abuse checks, 18/18 dependency-failure and graceful-shutdown checks (exit code 0, in-flight request completed). Final benchmark (10 backends, 136K-215K requests per strategy, 0 failed requests): 10.2K-10.7K req/s for round_robin, weighted and consistent_hashing, **1.8K req/s and p95 45.8 ms for `least_connections` (target missed)**, p95 8.7-8.9 ms for round_robin and weighted, 20.6 ms for consistent_hashing; availability 100 % over 546,773 and 634,001 requests while backends were killed, hung and added (a hung backend costs latency, set by `upstream_timeout_ms`). Tests: 828 (Debug, Release and Clang `-Werror` pass 828/828 serially with PostgreSQL and Redis; database tests must run serially), ASan+UBSan and TSan clean on the 541 concurrency-relevant tests. Documentation: `docs/FLOWCHART.md` (architecture flowchart), `docs/PERFORMANCE.md`, `docs/SECURITY.md`, `docs/DEMO.md` (a reproducible terminal demo with real captured output, `demo/demo.sh`), final README. Limitations: one machine type and closed-loop load, no TLS, `/metrics` unauthenticated, the Mermaid diagrams were not rendered in a browser, the remote GitHub Actions run has not been confirmed. See `summary.md`.

---

## Resume Metrics Rule

The following are **targets, not achieved results**:

- ~10 service instances
- ~100K+ requests
- ~99.5% availability
- ~10K requests/sec
- p95 latency around ~20 ms

They must not be presented as achieved until actual tests produce them. When benchmarking is implemented, record the measured RPS, p50, p95, p99, error rate, availability, concurrency, and resource utilization. Only measured results may be used in the final resume.
