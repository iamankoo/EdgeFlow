# EdgeFlow

EdgeFlow is a high-performance **API Gateway, Service Discovery system, and Load Balancer written in C++20**. It is being built as a production-style systems project covering TCP/HTTP networking, health-aware routing, reverse proxying, reliability engineering, caching, rate limiting, observability, and measured performance.

## Current Status

**Phases 1 to 9 are complete.** EdgeFlow is a concurrent, asynchronous **HTTP/1.1 server** built on Boost.Asio and Boost.Beast, on top of the Phase 1 foundation (configuration, logging, lifecycle, graceful shutdown), with a **PostgreSQL-backed service registry** that discovers registered backend instances (Phase 3) and an **active health checker** that excludes unhealthy instances from the routable view and reintroduces recovered ones (Phase 4), and **four load-balancing strategies** that choose among the routable instances (Phase 5). Phase 6 turns it into a **reverse proxy**: `/proxy/{service}/...` is forwarded to a healthy instance chosen by the routing strategy, and the response is returned to the client. Phase 7 makes that forwarding **reliable**: retries with exponential backoff, a circuit breaker per backend instance, failover to another instance, a total request budget and graceful degradation. Phase 8 adds **Redis**: a response cache and distributed Token Bucket rate limiting (per client address, per API key, per service) whose state is shared by every EdgeFlow instance. Phase 9 adds **observability and measurement**: a Prometheus-compatible `/metrics` endpoint (requests/sec, latency percentiles, backend latency, error rate, cache hit ratio, retries, circuit-breaker trips, rejected requests, connections) and reproducible benchmarks.

It is **not yet production-hardened** (profiling-driven optimisation, a security review and release hardening are Phase 10), and its measured performance is modest on the uncached proxied path (see [Benchmarking](#benchmarking)). See [Phases.md](Phases.md).

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

**Phase 5 - load balancing**
- One `LoadBalancer` abstraction, four strategies: Round Robin, Least Connections, Weighted Routing, Consistent Hashing
- Strategies choose only among the routable (active AND healthy) instances supplied by discovery; none contains health logic
- `routing.strategy` selects the strategy; `GET /services/{service}/route[?key=K]` shows the decision (a diagnostic: it forwards nothing)

**Phase 6 - reverse proxy and request forwarding**
- `/proxy/{service}/...` forwards method, path (prefix stripped), query, headers and body to an instance chosen by the Phase 5 router from the routable (active AND healthy) set, and returns the backend's status, headers and body
- Header propagation: hop-by-hop headers removed, `Host` rewritten, `X-Forwarded-For` / `X-Forwarded-Proto` / `Via` added, `X-Request-Id` preserved or generated and returned
- Upstream keep-alive connection reuse through a per-backend pool (idle cap, idle TTL, liveness check before reuse, stale pooled connections replaced before any byte is sent)
- Connect and whole-exchange upstream timeouts (`504`), backend failures (`502`), nothing routable (`503`); a backend's own 4xx/5xx is forwarded, not turned into a gateway error
- `connection_count` maintained for the duration of each proxied request, so Least Connections now sees real load
- Asynchronous: a slow backend never blocks a client-side I/O worker; graceful shutdown completes or cancels in-flight proxied requests and releases their counts

**Phase 7 - reliability engineering** (`reliability.*`, see [Reliability](#reliability-phase-7))
- Total request budget and per-attempt timeouts; retry policy with a limit on attempts, exponential backoff with jitter, retryable statuses and retryable methods
- Circuit breaker per backend instance (CLOSED -> OPEN -> HALF-OPEN -> CLOSED) that stops sending traffic to an instance whose real requests keep failing
- Failover: a retry prefers a backend the request has not tried; backends with an open circuit are skipped
- Graceful degradation: with nothing eligible the client gets a fast `503` (or the last backend answer); the gateway stays up

**Phase 8 - Redis cache and distributed rate limiting** (`redis.*`, `cache.*`, `rate_limit.*`, see [Redis, Cache and Rate Limiting](#redis-response-cache-and-rate-limiting-phase-8))
- Redis through an own asynchronous client (pool, timeouts, fail-fast window after an outage, stale-connection replacement)
- Response cache for successful public GET responses with expiry, hit/miss handling (`X-Cache`) and invalidation by writes
- Token Bucket rate limiting per client address, per API key and per service, atomic and shared through Redis; `429` with `Retry-After`
- Redis trouble never fails a request: the cache is bypassed, the limiter fails open (default) or closed

**Phase 9 - observability, testing and performance** (`metrics.*`, see [Metrics](#metrics-phase-9), [bench/README.md](bench/README.md))
- `GET /metrics` in the Prometheus text format: requests/sec, active connections, request count, error rate, p50/p95/p99 latency, backend latency, cache hit ratio, retry count, circuit-breaker trips, rejected requests, Redis state
- Exact latency histograms plus a sliding window; every component's counters read at scrape time, a few atomic increments per request on the request path
- Google Benchmark micro-benchmarks and a wrk load test (Docker Compose stack, raw results kept in `bench/results`)

## Planned Capabilities (not yet implemented)

- Profiling-driven optimisation, a security review and production hardening (Phase 10)

## Architecture Overview

See [architecture.md](architecture.md) for the target architecture and the Phase 1 and Phase 2 implementations. Current layout:

```text
.github/workflows/ci.yml   GitHub Actions CI
cmake/                     CMake modules (dependencies, warnings)
config/config.yaml         Default configuration (every key documented)
db/migrations/             PostgreSQL schema (SQL), embedded into the binary at build time
include/edgeflow/          Public headers: config, core, discovery, logging, network, proxy, routing, storage
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
| `routing.strategy` | `round_robin` | `round_robin`, `least_connections`, `weighted`, `consistent_hashing` | how an instance is chosen among the routable ones |
| `proxy.enabled` | `false` | true/false | turn the reverse proxy on; requires `database.enabled` |
| `proxy.connect_timeout_ms` | `2000` | 10-60000 | time to resolve and connect to a backend (`504`) |
| `proxy.upstream_timeout_ms` | `30000` | 10-600000 | time for the whole upstream exchange: connect, request, complete response (`504`); not below `connect_timeout_ms` |
| `proxy.io_threads` | `2` | 1-64 | threads running upstream I/O (separate from `server.worker_threads`) |
| `proxy.max_idle_connections` | `32` | 0-1024 | idle keep-alive connections kept per backend; 0 disables connection reuse |
| `proxy.idle_timeout_ms` | `30000` | 10-3600000 | an idle pooled connection older than this is closed instead of reused |
| `proxy.max_response_bytes` | `16777216` | 1024-268435456 | a larger backend response body is refused with `502` |
| `reliability.enabled` | `false` | true/false | turn the reliability layer on; requires `proxy.enabled` |
| `reliability.timeout.total_timeout_ms` | `30000` | 10-3600000 | bound for the WHOLE proxied request, every attempt and backoff pause included; one attempt gets at most `min(proxy.upstream_timeout_ms, what is left)` |
| `reliability.retry.enabled` | `true` | true/false | retry failed attempts |
| `reliability.retry.max_attempts` | `3` | 1-10 | TOTAL attempts including the first (1 = never retry) |
| `reliability.retry.base_delay_ms` | `100` | 1-60000 | pause before the first retry; doubles for each further one |
| `reliability.retry.max_delay_ms` | `2000` | 1-600000 | cap of the pause; not below `base_delay_ms` |
| `reliability.retry.jitter_percent` | `20` | 0-100 | each pause is shortened by up to this percent at random |
| `reliability.retry.retryable_statuses` | `[502, 503, 504]` | each 400-599 | a backend answer with one of these statuses counts as a failed attempt; any other status is forwarded |
| `reliability.retry.retryable_methods` | `[GET, HEAD, OPTIONS]` | HTTP methods | methods retried after the backend MAY have processed the request (timeout, reset, retryable status); a request that never reached a backend is retried whatever its method |
| `reliability.circuit_breaker.enabled` | `true` | true/false | one circuit per backend instance |
| `reliability.circuit_breaker.failure_threshold` | `5` | 1-1000 | consecutive failed requests that open the circuit |
| `reliability.circuit_breaker.recovery_timeout_ms` | `30000` | 10-3600000 | time open before probe request(s) are let through |
| `reliability.circuit_breaker.half_open_max_requests` | `1` | 1-100 | probes allowed in flight; that many successes close the circuit |
| `redis.enabled` | `false` | true/false | turn Redis on; used by `cache` and `rate_limit` |
| `redis.host` / `redis.port` | `127.0.0.1` / `6379` | name / 1-65535 | Redis address |
| `redis.password_env` | empty | env var name | NAME of the environment variable holding the Redis password (empty: no AUTH) |
| `redis.key_prefix` | `edgeflow` | 1-64 of `A-Za-z0-9_-.` | prefix of every key EdgeFlow writes |
| `redis.connect_timeout_ms` | `1000` | 10-60000 | resolve + connect (and AUTH); while Redis is unreachable one request per `retry_interval_ms` waits this long |
| `redis.command_timeout_ms` | `250` | 10-60000 | one command, from write to complete reply |
| `redis.pool_size` / `redis.io_threads` | `4` / `1` | 1-64 | connections to Redis / threads running Redis I/O |
| `redis.retry_interval_ms` | `1000` | 10-3600000 | after a Redis failure commands fail at once for this long, then one probes |
| `cache.enabled` | `false` | true/false | response cache; requires `redis.enabled` and `proxy.enabled` |
| `cache.default_ttl_seconds` / `cache.max_ttl_seconds` | `60` / `3600` | 1-2592000 | lifetime without max-age/s-maxage / upper bound for every lifetime |
| `cache.max_entry_bytes` | `1048576` | 1-67108864 | a response with a larger body is not stored |
| `cache.cacheable_statuses` | `[200]` | each 200-599 | statuses that may be stored |
| `rate_limit.enabled` | `false` | true/false | Token Bucket rate limiting of `/proxy/...`; requires `redis.enabled` and `proxy.enabled` |
| `rate_limit.on_redis_failure` | `allow` | `allow`, `reject` | when Redis cannot be asked: let the request through, or answer 503 |
| `rate_limit.ip.enabled` / `.capacity` / `.refill_per_second` | `true` / `100` / `50` | capacity 1-1000000, refill >0-1000000 (fractions allowed) | one bucket per client address |
| `rate_limit.api_key.enabled` / `.header` / `.capacity` / `.refill_per_second` | `false` / `X-API-Key` / `1000` / `500` | as above | one bucket per API key; requests without the header are not limited by this scope |
| `rate_limit.service.enabled` / `.capacity` / `.refill_per_second` / `.overrides` | `false` / `5000` / `2500` / none | as above | one bucket per proxied service; `overrides` gives named services their own `capacity` and `refill_per_second` |
| `metrics.enabled` | `false` | true/false | serve the Prometheus endpoint and record request metrics; unauthenticated, enable only on a trusted network |
| `metrics.path` | `/metrics` | `/...` (not `/`, `/proxy`, `/services`, `/health`, `/echo`) | URL of the endpoint (GET/HEAD) |
| `metrics.window_seconds` | `10` | 1-300 | sliding window of the requests/sec, error-ratio and p50/p95/p99 gauges |
| `shutdown.grace_period_seconds` | `5` | 0-300 | time in-flight requests get to finish on shutdown |

## Service Registry (Phase 3)

EdgeFlow keeps a registry of backend service instances. **PostgreSQL is the persistent source of truth**: registrations survive restarts of EdgeFlow and of the database container (the Compose stack stores them in a named volume). The registry is managed through a small JSON API; Client traffic is forwarded to registered instances by the reverse proxy (Phase 6, below).

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
| `GET /services/{service}/route[?key=K]` | `200` with the instance the configured strategy picks right now and the strategy name; `404` unknown service; `503` nothing routable. A routing **decision only**: nothing is forwarded and no state changes. `key` feeds consistent hashing |
| `GET /services/{service}/routable` | `200`, same shape as discovery but only instances that are `active` AND `healthy` (Phase 4); no ordering or selection |
| `GET /services/{service}/instances/{id}` | `200` or `404` |
| `PATCH /services/{service}/instances/{id}` | `200`; mutable fields only (`status`, `health_status`, `version`, `weight`, `connection_count`); `400` for identity fields |
| `DELETE /services/{service}/instances/{id}` | `204`; `404` if already gone |
| `GET /services` | `200` `{"services":[...]}` |

Errors are JSON (`{"error","status","detail"}`): `400` invalid input, `404` unknown service or instance, `405` wrong method (with `Allow`), `409` duplicate, `503` database unavailable.

Instance metadata: `status` is the registration state (`active`, `draining`, `disabled`); `health_status` is the last known health (`unknown`, `healthy`, `unhealthy`). They are separate things. In Phase 3 nothing probes instances, so health is whatever was registered or last written; active health checking is Phase 4. `weight` is used by weighted routing (Phase 5); `connection_count` is maintained by the proxy for each request in flight (Phase 6) and read by Least Connections. Registering an id or `host:port` that already exists for the service is rejected with `409` rather than overwriting it. Design details, schema and failure behavior: [architecture.md](architecture.md) section 19.

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
  docker run -d --name backend-$n --network edgeflow_default edgeflow:phase9
  curl -X POST http://127.0.0.1:8080/services/shop/instances -H 'Content-Type: application/json'     -d "{\"instance_id\":\"$n\",\"host\":\"backend-$n\",\"port\":8080}"
done
curl http://127.0.0.1:8080/services/shop/routable      # within a few seconds: a and b
docker stop backend-b                                  # after the failure threshold: only a
docker start backend-b                                 # after the success threshold: a and b again
docker rm -f backend-a backend-b; docker compose down -v
```

## Load Balancing (Phase 5)

Set `routing.strategy` and EdgeFlow picks among the **routable** instances (`active` AND `healthy`, see Health Checking). The strategies never look at health themselves: discovery hands them the routable set.

| Strategy | Behaviour |
|----------|-----------|
| `round_robin` | `a, b, c, a, ...` in `(service, instance_id)` order; correct when instances come and go between requests |
| `least_connections` | the instance with the smallest `connection_count` (ties: smallest id). The proxy keeps the count equal to the proxied requests in flight (Phase 6); the strategy only reads it |
| `weighted` | exactly `weight` of every `sum(weights)` requests per instance, smoothly interleaved (weights 5/3/2 give `a b c a a b a c b a`); weight 0 gets no share unless all are 0 |
| `consistent_hashing` | a request `key` maps to a stable instance; adding or removing an instance moves only the keys that must move (virtual-node ring) |

```bash
curl 'http://127.0.0.1:8080/services/shop/route'                 # round robin / least connections / weighted
curl 'http://127.0.0.1:8080/services/shop/route?key=client-42'   # consistent hashing: same key, same instance
```

This reports the decision; it does not forward the request (that is `/proxy/...`, below). To compare strategies locally, start the Compose stack with two or three backends as in Health Checking, change `strategy:` in `config/config.compose.yaml` and recreate the `edgeflow` container.

## Reverse Proxy (Phase 6)

With `proxy.enabled: true` (needs `database.enabled`; `docker compose` enables it) a request to `/proxy/{service}/...` is forwarded to a healthy instance of `{service}`: **Client -> EdgeFlow -> backend -> EdgeFlow -> Client**.

```bash
curl -i http://127.0.0.1:8080/proxy/shop/api/items?id=42     # backend receives GET /api/items?id=42
curl -i -X POST http://127.0.0.1:8080/proxy/shop/orders -H 'X-Request-Id: my-id' -d '{"sku":1}'
```

- **Mapping:** `/proxy/orders/` -> `/`, `/proxy/orders/api/users?id=42` -> `/api/users?id=42`. The prefix is stripped; path and query are forwarded verbatim. `/proxy` and `/proxy/`, or an invalid service name, are `400`; an unknown service is `404`. `/`, `/health`, `/echo` and `/services/...` are unchanged (`GET /services/{service}/route` still only reports a decision).
- **Routing:** the instance comes from the configured strategy over the routable (active AND healthy) instances; unhealthy, draining and disabled instances are never used. Consistent hashing is keyed by the client address.
- **Headers:** hop-by-hop headers (`Connection`, `Keep-Alive`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`, `Proxy-*`, anything named by `Connection`) are not forwarded either way; `Host` is rewritten to the instance; `X-Forwarded-For` extends the client's chain; `X-Forwarded-Proto: http`; `Via: 1.1 edgeflow` is appended to requests and responses.
- **Request ids:** `X-Request-Id` is preserved when the client sends one (1-128 visible ASCII characters), otherwise a UUID is generated; it reaches the backend, comes back to the client (also on gateway errors) and appears in the logs.
- **Connection reuse:** keep-alive connections to backends are pooled per backend and reused when safe (the backend allowed keep-alive, the response was complete and nothing was left unread). A pooled connection that died while idle is detected before the request is written and replaced by a new one (never after a request byte was sent).
- **Failures:** a backend's own status (including 4xx/5xx) is forwarded as is. `502` backend unreachable, reset, closed early, invalid response or response over `max_response_bytes`; `504` connect or whole-exchange timeout; `503` nothing routable, registry unavailable or shutting down. The client's connection stays open after a backend failure.
- **Not in this phase:** retries, backoff, circuit breaker and failover are Phase 7 (below); no caching or rate limiting (Phase 8). Requests and responses are buffered, not streamed (bounded by `server.max_request_body_bytes` and `proxy.max_response_bytes`). Details, limitations and the threading model: [architecture.md](architecture.md) section 22.

To try it locally with Docker (two throwaway backends, as in Health Checking):

```bash
docker compose up -d --build
for n in a b; do
  docker run -d --name backend-$n --network edgeflow_default edgeflow:phase9
  curl -X POST http://127.0.0.1:8080/services/shop/instances -H 'Content-Type: application/json' \
    -d "{\"instance_id\":\"$n\",\"host\":\"backend-$n\",\"port\":8080}"
done
sleep 6; curl -i http://127.0.0.1:8080/proxy/shop/health    # answered by a, then b, then a ...
docker rm -f backend-a backend-b; docker compose down -v
```

## Reliability (Phase 7)

With `reliability.enabled: true` (needs `proxy.enabled`; `docker compose` enables it) every `/proxy/{service}/...` request runs under a policy that keeps a failing backend from becoming a failing gateway.

- **Attempts and classification:** an attempt fails when the backend answers with a `retryable_statuses` code, times out, resets or closes without a complete answer, or cannot be reached. Any other backend status (including `404`, `500`) is the application's answer and is forwarded unchanged.
- **Retry rules:** at most `max_attempts` attempts in total. A request that provably never reached a backend (connection refused, name not resolved, connect timeout) is retried whatever its method; after the backend may have processed it (timeout, reset, retryable status) only `retryable_methods` are retried, so a `POST` is not repeated by default.
- **Backoff:** the pause before retry *n* is `base_delay_ms * 2^(n-1)`, capped at `max_delay_ms`, shortened by up to `jitter_percent`. A pause that does not fit in the remaining budget is skipped and the last answer is returned.
- **Total budget:** `reliability.timeout.total_timeout_ms` bounds the whole request; each attempt gets at most the smaller of `proxy.upstream_timeout_ms` and what is left.
- **Circuit breaker (per backend instance):** `CLOSED` -> after `failure_threshold` consecutive failed requests `OPEN` (no traffic) -> after `recovery_timeout_ms` `HALF-OPEN` (up to `half_open_max_requests` probe requests) -> enough successes `CLOSED`; a failed probe re-opens it. A request records at most one failure per backend, so one request cannot open a circuit alone. The breaker is the fast, passive view of real traffic; the Phase 4 health checker is the slow, active view that decides the routable set. They share no state: a backend is used only if it is routable AND its circuit admits a request.
- **Failover:** a retry prefers a backend the request has not tried yet; backends whose circuit refuses are skipped by the router. With a single instance, retries go back to that instance until its circuit opens.
- **Graceful degradation:** when no instance is eligible the client gets `503` at once (about 1 ms in the Compose run) instead of waiting; the last backend answer is returned when retries are exhausted; `/health`, the registry API and other services are unaffected.
- **Not included:** circuit state is in memory and per EdgeFlow process (not shared, lost on restart); retried requests are buffered; no metrics (Phase 9); no cache or rate limiting (Phase 8). Design details and limitations: [architecture.md](architecture.md) section 23.

```bash
docker compose up -d --build        # the Compose config enables reliability with short delays
# with two backends a and b registered for service "shop": make a answer 503 (keep its /health at 200)
for i in $(seq 8); do curl -s -o /dev/null -w '%{http_code} ' http://127.0.0.1:8080/proxy/shop/x; done   # all 200, served by b
docker logs edgeflow | grep -E 'retrying|circuit breaker'
```

## Redis, Response Cache and Rate Limiting (Phase 8)

With `redis.enabled: true` and `cache.enabled` and/or `rate_limit.enabled` (they need `proxy.enabled`; `docker compose` enables all of it and starts `redis:7`) every `/proxy/{service}/...` request first passes the rate limiter, then the cache, and only then reaches the proxy with its retries, circuit breakers and failover. `/health`, `/services/...` and the local endpoints are never limited or cached.

```bash
docker compose up -d --build
curl -si http://127.0.0.1:8080/proxy/shop/item | grep -iE '^HTTP|x-cache'     # X-Cache: MISS, then HIT
for i in $(seq 40); do curl -s -o /dev/null -w '%{http_code} ' -H 'X-API-Key: demo' http://127.0.0.1:8080/proxy/shop/x; done   # 200 x10 then 429
```

- **Token Bucket.** A bucket holds up to `capacity` tokens and gains `refill_per_second` tokens each second (continuously); every request takes one token from each bucket that applies to it: its **client address** (`rate_limit.ip`), its **API key** (header `rate_limit.api_key.header`; the key is stored in Redis only as a SHA-256 hash) and the **service** it addresses (`rate_limit.service`, with `overrides` for named services). If any bucket is empty the request is answered **`429 Too Many Requests`** with `Retry-After` (whole seconds) and `X-RateLimit-Scope: ip|api_key|service`, **nothing is taken from the other buckets**, and the request never reaches the cache, the proxy or a backend. The buckets live in Redis and are updated by one Lua script that Redis runs atomically with its own clock, so any number of EdgeFlow instances (and threads) share the same limits and cannot both take the last token. Idle buckets expire. The client is identified by its TCP peer address; `X-Forwarded-For` is not trusted.
- **Response cache.** Used for `GET` requests without a body, `Authorization`, `Cookie`, `Range`, `If-*` headers or `Cache-Control: no-store|no-cache` (`Pragma: no-cache`); everything else bypasses it. Stored: answers from a backend with a status in `cache.cacheable_statuses` (200) and without `Set-Cookie`, `Vary`, `Cache-Control: no-store|no-cache|private` or `max-age=0`, with a body up to `cache.max_entry_bytes`. Lifetime: `s-maxage`, else `max-age`, else `cache.default_ttl_seconds`, never more than `cache.max_ttl_seconds`; Redis removes the entry when it expires. Key: SHA-256 of `GET`, the service and the path + query exactly as sent. A hit is answered from Redis (the backend is not contacted) with `X-Cache: HIT`, `Age` and the `X-Request-Id` of the request being answered; a miss is forwarded and answered with `X-Cache: MISS`. A successful `POST`/`PUT`/`PATCH`/`DELETE` removes the stored entry of the same URL. Not implemented: revalidation (`ETag`/`304`), `Expires`, `Vary`, request coalescing (concurrent misses of one URL each reach a backend), caching of `HEAD`.
- **When Redis is unavailable** the gateway keeps serving. Starting without Redis only logs a warning. The cache is bypassed (every request goes to a backend). Rate limiting follows `rate_limit.on_redis_failure`: **`allow`** (default, fail open: requests are not limited) or **`reject`** (fail closed: `503 rate limiting is unavailable`). After a failure, Redis commands fail at once for `redis.retry_interval_ms`, then one command probes; an unreachable (not refusing) Redis costs one request per interval up to `redis.connect_timeout_ms`. Recovery is automatic and logged (`redis ... is available again`).
- **Not in this phase:** metrics (Phase 9), a routing cache, Redis Cluster, per-client identification behind another proxy. Details, key formats and limitations: [architecture.md](architecture.md) section 24.

## Metrics (Phase 9)

With `metrics.enabled: true` (`docker compose` enables it) `GET /metrics` returns the Prometheus text format (0.0.4); a Prometheus server scrapes it as it is:

```yaml
scrape_configs:
  - job_name: edgeflow
    static_configs: [{targets: ["edgeflow:8080"]}]
```

| you want | metric |
|---|---|
| requests/sec | `edgeflow_http_requests_per_second` (window) or `rate(edgeflow_http_requests_total[1m])` |
| active connections | `edgeflow_http_active_connections` (and `edgeflow_http_in_flight_requests`) |
| request count | `edgeflow_http_requests_total{kind="proxy\|local"}`, `edgeflow_http_responses_total{class="2xx\|3xx\|4xx\|5xx"}` |
| error rate | `edgeflow_http_error_ratio` (window: 5xx / requests), `edgeflow_http_errors_total` |
| p50 / p95 / p99 | `edgeflow_http_request_latency_seconds{quantile}` (window) or `histogram_quantile(0.99, rate(edgeflow_http_request_duration_seconds_bucket[5m]))` |
| backend latency | `edgeflow_backend_latency_seconds` (histogram), `edgeflow_backend_latency_window_seconds{quantile}` |
| cache hit ratio | `edgeflow_cache_hit_ratio`, `edgeflow_cache_lookups_total{result}` |
| retry count | `edgeflow_retries_total` (and `edgeflow_failovers_total`) |
| circuit-breaker trips | `edgeflow_circuit_breaker_trips_total`, `edgeflow_circuit_breakers{state}` |
| rejected requests | `edgeflow_rejected_requests_total{reason="rate_limited\|rate_limit_unavailable\|connection_limit"}` |

An *error* is a 5xx the client received; a `429` is a *rejection*. Quantiles come from fixed latency buckets (four per octave from 100 us), so they are within about 19 % of the true value, and are `NaN` when the window has no requests. The endpoint is answered above the rate limiter and the cache and is never counted as traffic. Components that are not enabled export nothing. All families and their definitions: [architecture.md](architecture.md) section 25.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The suite covers configuration, logging, the shutdown coordinator, application lifecycle (including real SIGINT/SIGTERM), the networking engine and the reverse proxy. Network tests are real integration tests: they start the server on an OS-assigned loopback port and talk to it over TCP, covering endpoints, parsing errors, limits, keep-alive and connection reuse, pipelining, idle and request timeouts, timer cancellation, client disconnects and resets, write failure, connection limits, graceful and forced shutdown, and 1/10/50/100 concurrent clients. These are correctness tests, not benchmarks.

The registry tests run against a **real PostgreSQL**. Point them at one with `EDGEFLOW_TEST_DB_HOST` (plus optional `EDGEFLOW_TEST_DB_PORT`, `EDGEFLOW_TEST_DB_NAME`, `EDGEFLOW_TEST_DB_USER`, `EDGEFLOW_TEST_DB_PASSWORD`); CI does this with a PostgreSQL service container. Without it, the 58 tests that need a database (and the 53 Phase 8 tests that need Redis, set with `EDGEFLOW_TEST_REDIS_HOST` and optionally `EDGEFLOW_TEST_REDIS_PORT`, some of them together with the database: 111 in all) are reported by CTest as **skipped** (with the reason) and everything else still runs, including the database-unavailable tests. They cover registration, deregistration, lookup, every metadata field, duplicates, validation, SQL metacharacters stored as data, persistence after dropping all in-memory state, concurrent registration, lookup and deregistration, recovery after the server kills a connection, and the HTTP API including a server restart.

At the end of Phase 9 the suite has 786 tests: 786 pass with PostgreSQL and Redis (GCC Debug/Release and Clang Debug, all `-Werror`), and 667 pass with 119 skipped without them (the Docker image build). The 499 Phase 8/9, reliability, proxy, pool, async, server, application and router tests passed 10 repeated runs; AddressSanitizer+UBSan and ThreadSanitizer reported nothing on them. The suites cover unit, integration, concurrency, routing, database, Redis, failure and end-to-end behaviour (the inventory is in [architecture.md](architecture.md) section 25); the Phase 6-9 runtime behaviour, including a real Prometheus scraping the gateway, was observed on real containers with Docker Compose (see `summary.md`). These are correctness results; the performance measurements are under Benchmarking.

## Docker

```bash
docker build -t edgeflow:phase9 .            # builds, runs the tests, produces the runtime image
docker run --rm -p 8080:8080 edgeflow:phase9 # HTTP server only (registry off); stop with Ctrl+C or `docker stop`
docker compose up --build                    # PostgreSQL + EdgeFlow with the registry on; publishes ${EDGEFLOW_HTTP_PORT:-8080}
docker compose down
```

Compose starts `postgres:16` with a health check and a named volume (`docker compose down` keeps your registrations, `down -v` deletes them) and starts EdgeFlow only once the database is healthy, using `config/config.compose.yaml`, which also enables health checking (HTTP `GET /health`, 2 s interval). The database password defaults to a development-only value; override it with `EDGEFLOW_DB_PASSWORD=... docker compose up` or an untracked `.env` file. The image build is bounded to 2 parallel compile jobs (`--build-arg BUILD_JOBS=N` to change it) because Beast is memory-hungry. The image runs as a non-root user and includes a `HEALTHCHECK` that runs `edgeflow --healthcheck` (so no curl is required). The container port must equal `server.port` in the configuration (8080 by default).

## CI

`.github/workflows/ci.yml` starts a PostgreSQL service container, installs dependencies, then configures, builds (warnings as errors), and runs CTest (unit and integration tests) on Ubuntu for GCC Debug, GCC Release, and Clang Debug on every push and pull request to `main`. The workflow has not yet been confirmed by a remote GitHub Actions run; the same GCC Debug/Release and Clang Debug configurations were run locally in Linux containers.

## Benchmarking

Reproducible with `bench/load/run.sh` (wrk against a Docker Compose stack) and `edgeflow_micro_bench` (Google Benchmark); how, and how to read the results: [bench/README.md](bench/README.md). The committed run ([`bench/results/phase9-codespace-4cpu`](bench/results/phase9-codespace-4cpu/summary.md): GitHub Codespace, 4 vCPU AMD EPYC 7763, load generator, gateway, three backends, PostgreSQL and Redis on the same machine; wrk 2 threads, 64 connections, 3 runs of 20 s each):

| scenario | median requests/s | wrk p50 | wrk p99 |
|---|---|---|---|
| backend direct (tiny `GET /`) | 36253 | 1.53 ms | 4.88 ms |
| gateway's own `/health` | 36450 | 1.52 ms | 4.93 ms |
| proxied, cache bypassed (rate limiter, PostgreSQL routing, proxy, backend) | 1784 | 34.6 ms | 54.0 ms |
| proxied, answered from the Redis cache | 11243 | 5.41 ms | 10.9 ms |

The ~10K requests/sec target of the roadmap is met only for cached responses on this machine; the uncached proxied path measured about 1.8K requests/sec, and is the first thing Phase 10 will profile. Not measured yet: availability under failure injection (the ~99.5 % target), a 10-instance deployment with dynamic changes, a 100K+ request distribution run per strategy, and p95 before and after optimisation. Figures from another machine or configuration will differ; the other roadmap targets (p95 ~20 ms, ~99.5 % availability, ~10 instances) remain **targets**.

## Roadmap

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
| 10 | Optimization, Production Hardening & Release | Not started |

Full scope and exit conditions: [Phases.md](Phases.md). Progress log: [summary.md](summary.md).

## Repository

<https://github.com/iamankoo/EdgeFlow>
