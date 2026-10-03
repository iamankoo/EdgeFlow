# EdgeFlow

EdgeFlow is a high-performance **API Gateway, Service Discovery system, and Load Balancer written in C++20**. It is being built as a production-style systems project covering TCP/HTTP networking, health-aware routing, reverse proxying, reliability engineering, caching, rate limiting, observability, and measured performance.

## Current Status

**Phases 1 and 2 are complete.** EdgeFlow is now a concurrent, asynchronous **HTTP/1.1 server** built on Boost.Asio and Boost.Beast, on top of the Phase 1 foundation (configuration, logging, lifecycle, graceful shutdown).

It is **not yet a gateway**: it answers requests locally. It has no service discovery, load balancing, reverse proxying, caching, or rate limiting; those arrive in Phases 3-10. See [Phases.md](Phases.md).

## Implemented

**Phase 1 - foundation**
- CMake (>= 3.25) project, C++20, strict per-target warnings, Debug and Release presets
- Pinned `FetchContent` dependencies (spdlog, yaml-cpp, nlohmann/json, GoogleTest)
- Strict YAML configuration with typed structures, defaults, and validation
- spdlog logging behind a small `Logger` abstraction
- Application lifecycle with SIGINT/SIGTERM graceful shutdown and an ordered `ShutdownCoordinator`

**Phase 2 - TCP/HTTP networking engine**
- Asynchronous TCP listener with configurable host and port
- HTTP/1.1 using Beast's parser: methods, targets, headers, bodies, chunked bodies
- Keep-alive (HTTP/1.1 default), `Connection: close`, HTTP/1.0 handling, pipelined requests
- Idle (keep-alive) timeout and request timeout, with `408` for stalled requests
- Connection limit, request body limit (`413`), header limit (`431`)
- Local endpoints: `GET /`, `GET /health`, `POST /echo`; `404`, `405`, `400`, `500` error responses
- Concurrent clients over a small `io_context` worker pool (no thread per connection)
- Graceful drain on shutdown: stop accepting, finish in-flight requests, then close
- Real container `HEALTHCHECK` (`edgeflow --healthcheck`) and Docker/Compose port publishing

## Planned Capabilities (not yet implemented)

- Service registry backed by PostgreSQL
- Active TCP/HTTP health checking of backends with automatic recovery
- Load balancing: Round Robin, Least Connections, Weighted Routing, Consistent Hashing
- Reverse proxy with connection pooling and request IDs
- Retries with exponential backoff, circuit breaker, failover
- Redis response caching and Token Bucket rate limiting
- Prometheus-compatible metrics

## Architecture Overview

See [architecture.md](architecture.md) for the target architecture and the Phase 1 and Phase 2 implementations. Current layout:

```text
.github/workflows/ci.yml   GitHub Actions CI
cmake/                     CMake modules (dependencies, warnings)
config/config.yaml         Default configuration (every key documented)
include/edgeflow/          Public headers: config, core, logging, network
src/                       Implementation and main.cpp
tests/                     GoogleTest suites (unit + real-socket integration) and fixtures
Dockerfile, docker-compose.yml, CMakePresets.json
```

## Technology Stack (Locked)

C++20, CMake, GCC/Clang, Boost.Asio, Boost.Beast, PostgreSQL, Redis, Docker, Docker Compose, GoogleTest, Google Benchmark, spdlog, nlohmann/json, YAML configuration, Prometheus-compatible metrics, perf, Valgrind, flamegraphs, wrk, GitHub Actions. Details, rationale, and per-technology status in [techstack.md](techstack.md).

## Prerequisites

- A C++20 compiler (GCC 11+ or Clang 14+)
- CMake 3.25+ and Ninja
- Boost 1.83+ headers (`libboost-dev` on Debian/Ubuntu); Asio and Beast are header-only
- Git and network access (other dependencies are fetched at configure time)
- Docker (optional, for container builds)

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Presets are also available: `cmake --preset debug && cmake --build --preset debug`. Add `-DEDGEFLOW_WARNINGS_AS_ERRORS=ON` to treat warnings as errors (the presets, CI and Docker do).

Compiling Beast is memory-hungry: on a machine with little RAM, limit parallelism (`cmake --build build -j2`).

## Run

```bash
./build/src/edgeflow --config config/config.yaml
```

The config path is taken from `--config`, then `$EDGEFLOW_CONFIG`, then `config/config.yaml`. Ctrl+C or SIGTERM stops it gracefully. Exit codes: `0` clean stop, `1` initialization failure (for example the port is in use), `2` bad arguments or configuration.

Then, from another terminal (default port 8080):

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/health          # {"service":"edgeflow","status":"ok"}
curl -i http://127.0.0.1:8080/does-not-exist  # 404
curl -i -H "Connection: close" http://127.0.0.1:8080/health
curl -i -X POST --data-binary @file http://127.0.0.1:8080/echo   # {"received_bytes":N}
```

`./build/src/edgeflow --healthcheck` probes `GET /health` on the configured port and exits 0 only on a 200.

## Configuration

`config/config.yaml` documents every key. Unknown keys and out-of-range values are rejected at startup.

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `server.host` | `0.0.0.0` | IP or name | bind address |
| `server.port` | `8080` | 1-65535 | listen port |
| `server.request_timeout_ms` | `5000` | 10-600000 | time to receive a request (408) and to write a response |
| `server.keep_alive_timeout_ms` | `10000` | 10-600000 | idle time before a connection is closed |
| `server.max_request_body_bytes` | `1048576` | 0-67108864 | larger bodies get 413 |
| `server.max_header_bytes` | `8192` | 1024-65536 | larger headers get 431 |
| `server.max_connections` | `1024` | 1-100000 | extra connections are closed immediately |
| `server.worker_threads` | `2` | 1-64 | threads running the I/O loop |
| `shutdown.grace_period_seconds` | `5` | 0-300 | time in-flight requests get to finish on shutdown |

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The suite covers configuration, logging, the shutdown coordinator, application lifecycle (including real SIGINT/SIGTERM), and the networking engine. Network tests are real integration tests: they start the server on an OS-assigned loopback port and talk to it over TCP, covering endpoints, parsing errors, limits, keep-alive and connection reuse, pipelining, idle and request timeouts, timer cancellation, client disconnects and resets, write failure, connection limits, graceful and forced shutdown, and 1/10/50/100 concurrent clients. These are correctness tests, not benchmarks.

## Docker

```bash
docker build -t edgeflow:phase2 .            # builds, runs the tests, produces the runtime image
docker run --rm -p 8080:8080 edgeflow:phase2 # stop with Ctrl+C or `docker stop`
docker compose up --build                    # publishes ${EDGEFLOW_HTTP_PORT:-8080}
docker compose down
```

The image build is bounded to 2 parallel compile jobs (`--build-arg BUILD_JOBS=N` to change it) because Beast is memory-hungry. The image runs as a non-root user and includes a `HEALTHCHECK` that runs `edgeflow --healthcheck` (so no curl is required). The container port must equal `server.port` in the configuration (8080 by default).

## CI

`.github/workflows/ci.yml` installs dependencies, then configures, builds (warnings as errors), and runs CTest (unit and integration tests) on Ubuntu for GCC Debug, GCC Release, and Clang Debug on every push and pull request to `main`. The workflow has not yet been confirmed by a remote GitHub Actions run; the same GCC Debug/Release and Clang Debug configurations were run locally in Linux containers.

## Benchmarking

No benchmarks have been run, and no performance results exist yet. Load-testing targets (~10K requests/sec, p95 around ~20 ms, ~99.5% availability, ~10 service instances, ~100K+ requests) are **targets only**. Results will be published here only after being measured with reproducible tests.

## Roadmap

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Foundation & Core Infrastructure | Completed |
| 2 | TCP/HTTP Networking Engine | Completed |
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
