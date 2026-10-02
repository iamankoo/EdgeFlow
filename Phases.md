# EdgeFlow — Development Phases

**Current Phase: Initialization / Pre-Phase 1**

This document is the authoritative roadmap. Phases must not be skipped. A phase begins only after the previous phase has met its exit condition. Later-phase functionality must not be implemented early unless strictly required as a dependency and clearly documented.

Before implementing a phase, its detailed implementation prompt must be cross-checked against this file. Any mismatch must be reported, not silently resolved.

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Foundation & Core Infrastructure | Not started |
| 2 | TCP/HTTP Networking Engine | Not started |
| 3 | Service Discovery & Registry | Not started |
| 4 | Health Checking & Dynamic Discovery | Not started |
| 5 | Load Balancing Engine | Not started |
| 6 | Reverse Proxy & Request Forwarding | Not started |
| 7 | Reliability Engineering | Not started |
| 8 | Redis Cache & Distributed Rate Limiting | Not started |
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

---

## Phase 5 — Load Balancing Engine

Implement:

- Round Robin
- Least Connections
- Weighted Routing
- Consistent Hashing

The algorithms must operate through a clean routing abstraction.

**Exit condition:** Requests can be distributed among healthy backend instances using all four strategies.

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
