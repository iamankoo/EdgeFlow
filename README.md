# EdgeFlow

EdgeFlow is a high-performance **API Gateway, Service Discovery system, and Load Balancer written in C++20**. It is being built as a production-style systems project covering TCP/HTTP networking, health-aware routing, reverse proxying, reliability engineering, caching, rate limiting, observability, and measured performance.

## Current Status

**Phase 1 (Foundation & Core Infrastructure) is complete.** EdgeFlow starts, loads and validates its configuration, initializes its infrastructure, logs lifecycle events, and shuts down cleanly on SIGINT/SIGTERM.

EdgeFlow is **not yet a gateway**: it has no HTTP server, service discovery, load balancing, or proxying. Those arrive in Phases 2-10. See [Phases.md](Phases.md).

## Implemented in Phase 1

- CMake (>= 3.25) project, C++20, strict per-target warnings, Debug and Release presets
- Reproducible dependency management (pinned `FetchContent` tags: spdlog, yaml-cpp, GoogleTest)
- Strict YAML configuration with typed structures, defaults, and validation (unknown keys rejected, ranges checked, all errors reported)
- spdlog-based logging behind a small `Logger` abstraction
- Application lifecycle (construct, initialize, run, shutdown)
- Graceful, idempotent shutdown: SIGINT/SIGTERM handling and an ordered shutdown coordinator that future components register with
- GoogleTest suite (56 tests)
- Multi-stage Dockerfile (non-root runtime user) and Docker Compose file
- GitHub Actions workflow (GCC and Clang, Debug and Release)

## Planned Capabilities (not yet implemented)

- Asynchronous HTTP/1.1 server with keep-alive and timeouts
- Service registry backed by PostgreSQL
- Active TCP/HTTP health checking with automatic recovery
- Load balancing: Round Robin, Least Connections, Weighted Routing, Consistent Hashing
- Reverse proxy with connection pooling and request IDs
- Retries with exponential backoff, circuit breaker, failover
- Redis response caching and Token Bucket rate limiting
- Prometheus-compatible metrics

## Architecture Overview

See [architecture.md](architecture.md) for the full target architecture and the Phase 1 implementation. Current layout:

```text
.github/workflows/ci.yml   GitHub Actions CI
cmake/                     CMake modules (dependencies, warnings)
config/config.yaml         Default configuration
include/edgeflow/          Public headers (config, core, logging)
src/                       Implementation and main.cpp
tests/                     GoogleTest suites and fixtures
Dockerfile, docker-compose.yml, CMakePresets.json
```

## Technology Stack (Locked)

C++20, CMake, GCC/Clang, Boost.Asio, Boost.Beast, PostgreSQL, Redis, Docker, Docker Compose, GoogleTest, Google Benchmark, spdlog, nlohmann/json, YAML configuration, Prometheus-compatible metrics, perf, Valgrind, flamegraphs, wrk, GitHub Actions. Details, rationale, and per-technology status in [techstack.md](techstack.md).

## Prerequisites

- A C++20 compiler (GCC 11+ or Clang 14+)
- CMake 3.25+ and Ninja
- Git and network access (dependencies are fetched at configure time)
- Docker (optional, for container builds)

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Presets are also available: `cmake --preset debug && cmake --build --preset debug`.
Add `-DEDGEFLOW_WARNINGS_AS_ERRORS=ON` to treat warnings as errors (the presets, CI and Docker do).

## Run

```bash
./build/edgeflow --config config/config.yaml
```

The config path is taken from `--config`, then `$EDGEFLOW_CONFIG`, then `config/config.yaml`. Press Ctrl+C (or send SIGTERM) to stop. Exit codes: `0` clean stop, `1` initialization failure, `2` bad arguments or configuration.

Example output:

```text
[info] [edgeflow] EdgeFlow starting (version 0.1.0)
[info] [edgeflow] configuration loaded from /etc/edgeflow/config.yaml
[info] [edgeflow] application initialized (name=edgeflow, environment=development)
[info] [edgeflow] EdgeFlow ready
[info] [edgeflow] shutdown requested (SIGTERM)
[info] [edgeflow] shutdown sequence started (grace period 5s)
[info] [edgeflow] stopping component 'signal-handlers'
[info] [edgeflow] shutdown completed
```

The `server` section of the configuration is validated but nothing listens on it yet.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The suite covers configuration loading and validation, logging, the shutdown coordinator, application lifecycle (including real SIGINT/SIGTERM), and command-line behavior.

## Docker

```bash
docker build -t edgeflow:phase1 .      # builds, runs the tests, produces the runtime image
docker run --rm edgeflow:phase1        # stop with Ctrl+C or `docker stop`
docker compose up --build              # same, using docker-compose.yml
docker compose down
```

The image runs as a non-root user and has no health check yet because there is no network endpoint to probe.

## CI

`.github/workflows/ci.yml` configures, builds (warnings as errors), and runs CTest on Ubuntu for GCC Debug, GCC Release, and Clang Debug on every push and pull request to `main`.

## Benchmarking

No benchmarks have been run, and no performance results exist yet. Load-testing targets (~10K requests/sec, p95 around ~20 ms, ~99.5% availability, ~10 service instances, ~100K+ requests) are **targets only**. Results will be published here only after being measured with reproducible tests.

## Roadmap

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Foundation & Core Infrastructure | Completed |
| 2 | TCP/HTTP Networking Engine | Not started |
| 3 | Service Discovery & Registry | Not started |
| 4 | Health Checking & Dynamic Discovery | Not started |
| 5 | Load Balancing Engine | Not started |
| 6 | Reverse Proxy & Request Forwarding | Not started |
| 7 | Reliability Engineering | Not started |
| 8 | Redis Cache & Distributed Rate Limiting | Not started |
| 9 | Observability, Testing & Performance | Not started |
| 10 | Optimization, Production Hardening & Release | Not started |

Full scope and exit conditions: [Phases.md](Phases.md). Progress log: [summary.md](summary.md).

## Repository

<https://github.com/iamankoo/EdgeFlow>
