# EdgeFlow

EdgeFlow is a high-performance **API Gateway, Service Discovery system, and Load Balancer written in C++20**. It is being built as a production-style systems project covering TCP/HTTP networking, health-aware routing, reverse proxying, reliability engineering, caching, rate limiting, observability, and measured performance.

## Current Status

**Phases 1 to 4 are complete.** EdgeFlow is a concurrent, asynchronous **HTTP/1.1 server** built on Boost.Asio and Boost.Beast, on top of the Phase 1 foundation (configuration, logging, lifecycle, graceful shutdown), with a **PostgreSQL-backed service registry** that discovers registered backend instances (Phase 3) and an **active health checker** that excludes unhealthy instances from the routable view and reintroduces recovered ones (Phase 4).

It is **not yet a gateway**: it answers requests locally and manages the registry, but it does not forward traffic. It has no load balancing, reverse proxying, caching, or rate limiting; those arrive in Phases 5-10. See [Phases.md](Phases.md).

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

**Phase 3 - service discovery and registry**
- Register, deregister, look up and update backend service instances (service name, host, port, instance id, registration status, health status, version, weight, connection count)
- PostgreSQL is the persistent source of truth: parameterised statements through libpq, a connection pool, versioned SQL migrations applied at startup, constraints that repeat the validation
- JSON registry API under `/services` (see below); duplicates rejected, repeated deregistration reported, database outages reported as `503` instead of false success
- Docker Compose stack with PostgreSQL (health-gated startup, named volume)

**Phase 4 - health checking and dynamic discovery**
- Periodic, timeout-bounded TCP and HTTP health checks of registered instances (HTTP healthy only on 2xx)
- Explicit health state machine with failure and recovery thresholds; transitions persisted in PostgreSQL
- Discovery refresh: new, deregistered, disabled and re-registered instances are handled without a restart
- A neutral routable view (`active` AND `healthy`) via `GET /services/{service}/routable`, for the load balancer (Phase 5)

## Planned Capabilities (not yet implemented)

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
db/migrations/             PostgreSQL schema (SQL), embedded into the binary at build time
include/edgeflow/          Public headers: config, core, discovery, logging, network, storage
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
- The PostgreSQL client library (`libpq-dev` on Debian/Ubuntu); a PostgreSQL server (16 tested) is needed only when the registry is enabled
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

The service registry is off by default, so this runs without a database. To use the registry, set `database.enabled: true` and point it at a PostgreSQL server (export the password in the variable named by `database.password_env`, `EDGEFLOW_DB_PASSWORD` by default); startup then applies the schema and fails if the database is unreachable.

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
| `database.enabled` | `false` | true/false | turn the PostgreSQL-backed service registry on; when true, startup fails if the database is unreachable |
| `database.host` | `127.0.0.1` | IP or name | PostgreSQL address |
| `database.port` | `5432` | 1-65535 | PostgreSQL port |
| `database.name` | `edgeflow` | name | database name |
| `database.user` | `edgeflow` | name | database user |
| `database.password_env` | `EDGEFLOW_DB_PASSWORD` | env var name | NAME of the environment variable holding the password. The password itself can never be put in the configuration, and is never logged |
| `database.pool_size` | `4` | 1-64 | database connections; each in-flight registry request holds one |
| `database.connect_timeout_seconds` | `5` | 1-60 | time allowed to open a database connection |
| `health_check.enabled` | `false` | true/false | turn active health checking on; requires `database.enabled` |
| `health_check.type` | `tcp` | `tcp`, `http` | kind of probe |
| `health_check.interval_ms` | `5000` | 100-3600000 | pause between the end of one probe of an instance and the start of the next |
| `health_check.timeout_ms` | `2000` | 10-60000 | bound for one whole probe; must not exceed `interval_ms` |
| `health_check.http_path` | `/health` | path | request path for `type: http` |
| `health_check.failure_threshold` | `3` | 1-100 | consecutive failures before a healthy instance becomes unhealthy |
| `health_check.success_threshold` | `2` | 1-100 | consecutive successes before an unhealthy instance becomes healthy |
| `health_check.refresh_interval_ms` | `5000` | 100-3600000 | how often the registry is re-read for new, removed and disabled instances |
| `health_check.max_concurrent_checks` | `32` | 1-1024 | probes running at the same time |
| `shutdown.grace_period_seconds` | `5` | 0-300 | time in-flight requests get to finish on shutdown |

## Service Registry (Phase 3)

EdgeFlow keeps a registry of backend service instances. **PostgreSQL is the persistent source of truth**: registrations survive restarts of EdgeFlow and of the database container (the Compose stack stores them in a named volume). The registry is managed through a small JSON API; EdgeFlow does **not** forward client traffic to registered instances yet (that is the reverse proxy, Phase 6).

Enable it with `database.enabled: true` (see Configuration). The easiest way is Docker Compose, which starts PostgreSQL, applies the schema, and starts EdgeFlow with the registry on:

```bash
docker compose up --build
```

```bash
# register two instances of user-service
curl -i -X POST http://127.0.0.1:8080/services/user-service/instances \
  -H 'Content-Type: application/json' \
  -d '{"instance_id":"user-1","host":"10.0.0.11","port":9001,"version":"1.4.2","weight":10}'
curl -X POST http://127.0.0.1:8080/services/user-service/instances \
  -H 'Content-Type: application/json' -d '{"host":"10.0.0.12","port":9001}'   # id is generated

curl http://127.0.0.1:8080/services/user-service/instances            # discover all instances
curl http://127.0.0.1:8080/services/user-service/instances/user-1     # one instance
curl -X PATCH http://127.0.0.1:8080/services/user-service/instances/user-1 \
  -H 'Content-Type: application/json' -d '{"health_status":"healthy","weight":20}'
curl -X DELETE http://127.0.0.1:8080/services/user-service/instances/user-1   # 204
curl http://127.0.0.1:8080/services                                   # known service names
```

| Method and path | Result |
|-----------------|--------|
| `POST /services/{service}/instances` | `201` + `Location`; body fields: `host`, `port` (required), `instance_id`, `version`, `weight` (0-1000, default 1), `status`, `health_status`, `connection_count` |
| `GET /services/{service}/instances` | `200` `{"service","count","instances":[...]}`; `404` for a service that was never registered |
| `GET /services/{service}/routable` | `200`, same shape as discovery but only instances that are `active` AND `healthy` (Phase 4); no ordering or selection |
| `GET /services/{service}/instances/{id}` | `200` or `404` |
| `PATCH /services/{service}/instances/{id}` | `200`; mutable fields only (`status`, `health_status`, `version`, `weight`, `connection_count`); `400` for identity fields |
| `DELETE /services/{service}/instances/{id}` | `204`; `404` if already gone |
| `GET /services` | `200` `{"services":[...]}` |

Errors are JSON (`{"error","status","detail"}`): `400` invalid input, `404` unknown service or instance, `405` wrong method (with `Allow`), `409` duplicate, `503` database unavailable.

Instance metadata: `status` is the registration state (`active`, `draining`, `disabled`); `health_status` is the last known health (`unknown`, `healthy`, `unhealthy`). They are separate things. In Phase 3 nothing probes instances, so health is whatever was registered or last written; active health checking is Phase 4. `weight` and `connection_count` are stored for the load balancer (Phase 5) and proxy (Phase 6) and are not acted on yet. Registering an id or `host:port` that already exists for the service is rejected with `409` rather than overwriting it. Design details, schema and failure behavior: [architecture.md](architecture.md) section 19.

## Health Checking (Phase 4)

EdgeFlow can actively probe the registered instances and keep their health up to date, so that discovery for routing automatically **excludes unhealthy instances and reintroduces recovered ones**. It is off by default; enable it with `health_check.enabled: true` (it needs `database.enabled`, because it reads instances from, and writes health to, the PostgreSQL registry). `docker compose` enables it.

- **TCP check** (`type: tcp`, the default): healthy when a TCP connection is established within `timeout_ms`. This proves the port accepts connections, not that the application works.
- **HTTP check** (`type: http`): `GET <http_path>` (default `/health`); healthy only on a **2xx** response within `timeout_ms`. Redirects are not followed; 1xx, 3xx, 4xx, 5xx, a malformed response, a closed or reset connection, an unresolvable name and a timeout are all unhealthy.
- **State machine**: the first probe of a new instance decides at once (`unknown` to `healthy` or `unhealthy`). After that a healthy instance becomes unhealthy after `failure_threshold` consecutive failures, and an unhealthy one becomes healthy after `success_threshold` consecutive successes; a result in the other direction resets the streak, so one transient failure does not flap an instance.
- **Registration vs health**: `active` and `draining` instances are probed, `disabled` ones are not. Only `active` AND `healthy` instances are routable.
- **Discovery refresh**: every `refresh_interval_ms` the registry is re-read, so instances registered, deregistered, disabled or re-registered while EdgeFlow runs are picked up or dropped without a restart.
- Health transitions are logged (`is now unhealthy` is a warning); each probe is bounded by `timeout_ms`; shutdown cancels everything.

```bash
curl http://127.0.0.1:8080/services/shop/routable       # only active AND healthy instances
curl http://127.0.0.1:8080/services/shop/instances      # everything registered, with health_status
```

To try it locally with Docker, use two EdgeFlow containers as throwaway backends (the image answers `GET /health` with 200 on port 8080; its shipped configuration has no database):

```bash
docker compose up -d --build
for n in a b; do
  docker run -d --name backend-$n --network edgeflow_default edgeflow:phase4
  curl -X POST http://127.0.0.1:8080/services/shop/instances -H 'Content-Type: application/json'     -d "{\"instance_id\":\"$n\",\"host\":\"backend-$n\",\"port\":8080}"
done
curl http://127.0.0.1:8080/services/shop/routable      # within a few seconds: a and b
docker stop backend-b                                  # after the failure threshold: only a
docker start backend-b                                 # after the success threshold: a and b again
docker rm -f backend-a backend-b; docker compose down -v
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The suite covers configuration, logging, the shutdown coordinator, application lifecycle (including real SIGINT/SIGTERM), and the networking engine. Network tests are real integration tests: they start the server on an OS-assigned loopback port and talk to it over TCP, covering endpoints, parsing errors, limits, keep-alive and connection reuse, pipelining, idle and request timeouts, timer cancellation, client disconnects and resets, write failure, connection limits, graceful and forced shutdown, and 1/10/50/100 concurrent clients. These are correctness tests, not benchmarks.

The registry tests run against a **real PostgreSQL**. Point them at one with `EDGEFLOW_TEST_DB_HOST` (plus optional `EDGEFLOW_TEST_DB_PORT`, `EDGEFLOW_TEST_DB_NAME`, `EDGEFLOW_TEST_DB_USER`, `EDGEFLOW_TEST_DB_PASSWORD`); CI does this with a PostgreSQL service container. Without it, the 42 tests that need a database are reported by CTest as **skipped** (with the reason) and everything else still runs, including the database-unavailable tests. They cover registration, deregistration, lookup, every metadata field, duplicates, validation, SQL metacharacters stored as data, persistence after dropping all in-memory state, concurrent registration, lookup and deregistration, recovery after the server kills a connection, and the HTTP API including a server restart.

## Docker

```bash
docker build -t edgeflow:phase2 .            # builds, runs the tests, produces the runtime image
docker run --rm -p 8080:8080 edgeflow:phase2 # HTTP server only (registry off); stop with Ctrl+C or `docker stop`
docker compose up --build                    # PostgreSQL + EdgeFlow with the registry on; publishes ${EDGEFLOW_HTTP_PORT:-8080}
docker compose down
```

Compose starts `postgres:16` with a health check and a named volume (`docker compose down` keeps your registrations, `down -v` deletes them) and starts EdgeFlow only once the database is healthy, using `config/config.compose.yaml`, which also enables health checking (HTTP `GET /health`, 2 s interval). The database password defaults to a development-only value; override it with `EDGEFLOW_DB_PASSWORD=... docker compose up` or an untracked `.env` file. The image build is bounded to 2 parallel compile jobs (`--build-arg BUILD_JOBS=N` to change it) because Beast is memory-hungry. The image runs as a non-root user and includes a `HEALTHCHECK` that runs `edgeflow --healthcheck` (so no curl is required). The container port must equal `server.port` in the configuration (8080 by default).

## CI

`.github/workflows/ci.yml` starts a PostgreSQL service container, installs dependencies, then configures, builds (warnings as errors), and runs CTest (unit and integration tests) on Ubuntu for GCC Debug, GCC Release, and Clang Debug on every push and pull request to `main`. The workflow has not yet been confirmed by a remote GitHub Actions run; the same GCC Debug/Release and Clang Debug configurations were run locally in Linux containers.

## Benchmarking

No benchmarks have been run, and no performance results exist yet. Load-testing targets (~10K requests/sec, p95 around ~20 ms, ~99.5% availability, ~10 service instances, ~100K+ requests) are **targets only**. Results will be published here only after being measured with reproducible tests.

## Roadmap

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Foundation & Core Infrastructure | Completed |
| 2 | TCP/HTTP Networking Engine | Completed |
| 3 | Service Discovery & Registry | Completed |
| 4 | Health Checking & Dynamic Discovery | Completed |
| 5 | Load Balancing Engine | Not started |
| 6 | Reverse Proxy & Request Forwarding | Not started |
| 7 | Reliability Engineering | Not started |
| 8 | Redis Cache & Distributed Rate Limiting | Not started |
| 9 | Observability, Testing & Performance | Not started |
| 10 | Optimization, Production Hardening & Release | Not started |

Full scope and exit conditions: [Phases.md](Phases.md). Progress log: [summary.md](summary.md).

## Repository

<https://github.com/iamankoo/EdgeFlow>
