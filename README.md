# EdgeFlow

EdgeFlow is a high-performance **API Gateway, Service Discovery system, and Load Balancer written in C++20**. It is being built as a production-style systems project covering TCP/HTTP networking, health-aware routing, reverse proxying, reliability engineering, caching, rate limiting, observability, and measured performance.

## Current Status

**Pre-Phase 1 — documentation and roadmap only.** No gateway functionality is implemented yet. See [Phases.md](Phases.md).

## Purpose

To demonstrate genuine backend and systems engineering: modular design, asynchronous networking, failure handling, and benchmark-backed performance claims.

## Architecture Overview

Modules under `src/` (planned): `gateway`, `network`, `discovery`, `routing`, `proxy`, `reliability`, `rate_limit`, `cache`, `storage`, `observability`, `config`. See [architecture.md](architecture.md).

## Core Capabilities (Planned — not yet implemented)

- Asynchronous HTTP/1.1 server with keep-alive and timeouts
- Service registry backed by PostgreSQL
- Active TCP/HTTP health checking with automatic recovery
- Load balancing: Round Robin, Least Connections, Weighted Routing, Consistent Hashing
- Reverse proxy with connection pooling and request IDs
- Retries with exponential backoff, circuit breaker, failover
- Redis response caching and Token Bucket rate limiting
- Prometheus-compatible metrics and structured logging

## Technology Stack (Locked)

C++20, CMake, GCC/Clang, Boost.Asio, Boost.Beast, PostgreSQL, Redis, Docker, Docker Compose, GoogleTest, Google Benchmark, spdlog, nlohmann/json, YAML configuration, Prometheus-compatible metrics, perf, Valgrind, flamegraphs, wrk, GitHub Actions. Details and rationale in [techstack.md](techstack.md).

## Roadmap

| Phase | Title |
|-------|-------|
| 1 | Foundation & Core Infrastructure |
| 2 | TCP/HTTP Networking Engine |
| 3 | Service Discovery & Registry |
| 4 | Health Checking & Dynamic Discovery |
| 5 | Load Balancing Engine |
| 6 | Reverse Proxy & Request Forwarding |
| 7 | Reliability Engineering |
| 8 | Redis Cache & Distributed Rate Limiting |
| 9 | Observability, Testing & Performance |
| 10 | Optimization, Production Hardening & Release |

Full scope and exit conditions: [Phases.md](Phases.md).

## Build and Run

Not available yet. Instructions will be added once Phase 1 produces a buildable application.

## Testing

Not available yet. Testing will use GoogleTest (unit/integration) and Google Benchmark (micro-benchmarks).

## Benchmarking

No benchmarks have been run, and no performance results exist yet. Load-testing targets (~10K requests/sec, p95 around ~20 ms, ~99.5% availability, ~10 service instances, ~100K+ requests) are **targets only**. Results will be published here only after being measured with reproducible tests.

## Repository

<https://github.com/iamankoo/EdgeFlow>
