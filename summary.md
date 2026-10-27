# EdgeFlow — Current Project Summary

**This file is the resume point for the project.** Tomorrow's session: read this file first, then [Phases.md](Phases.md) (authoritative roadmap), then inspect `git log` and confirm the state below before doing anything. After every completed phase, update this file.


**Checkpoint (UPDATED 2026-10-27): Phases 1-10 are COMPLETE. Phase 10 (optimization, production hardening and release) is committed as ONE commit, `feat: complete EdgeFlow phase 10 production hardening and release` (its hash is in `git log`; this file is part of that commit and cannot contain its own hash). THE ROADMAP IS FINISHED; nothing after Phase 10 is planned or started.** Section 9E describes Phase 10; sections 10-15 are updated to the end of Phase 10.

---

## 0. RESUME POINT — Phase 10 complete, roadmap finished

All ten phases are done. Any further work is new scope and needs the owner's instructions. Before anything else: read this file, then `Phases.md`, run `git log --oneline -8` / `git status`, and confirm `HEAD == origin/main` with a clean tree. The documents to read for the finished project are `README.md`, `docs/FLOWCHART.md`, `docs/PERFORMANCE.md`, `docs/SECURITY.md` and `docs/DEMO.md`.

### Final Phase 10 validation (all measured; remote CI NOT confirmed)

- **828 CTest tests** (786 before Phase 10, +42): GCC Debug, GCC Release and Clang 18 Debug all 828/828 with real PostgreSQL and real Redis, all `-Werror`. **The database tests must run serially** (`ctest -j1`): one of them terminates PostgreSQL backends and a few routing tests are timing-sensitive; under `-j3` two of them failed, serially none does. Docker Release image build without a database or Redis: **828 run, 703 pass, 125 skipped, 0 failed** (`edgeflow:phase10`, 127 MB).
- **Repeat runs on the final tree:** the Reliability, HealthChecker and Prober suites 100 consecutive iterations with 0 failures, the previously flaky `ReliabilityTest.APostThatNeverReachedABackendIsRetriedWithItsBody` 500 iterations with 0 failures, `InlineAttemptTest` 100 iterations with 0 failures, and a final serial Debug run 828/828. **ASan + UBSan** (leak detection on) and **ThreadSanitizer**: the 541 tests of the concurrency-relevant suites, 541/541, 0 reports (run before the last two test-only helper fixes described below).
- **Runtime checks on a real gateway process:** `bench/hardening/abuse.sh` 29/29, `bench/hardening/failures.sh` 18/18.
- **Benchmarks, flame graphs, the terminal demo:** section 9E and `docs/`.

### Git identity (resolved 2026-10-06)

The owner's Phase 6 instruction fixes author AND committer to **`Aniket Raj <aniketraj00384@gmail.com>`** (GitHub account `iamankoo`). Phases 1-5 and the summary commit were authored as `iamankoo <aniketraj00384@gmail.com>`; they are not rewritten. Use `Aniket Raj` for all future commits (repository-local git config; a fresh Codespace needs it set: its default email has a different case), still with no Claude/Anthropic/OpenAI/ChatGPT/AI attribution and no `Co-authored-by` or any other trailer.

### Environment notes (carried forward)

- **Heavy validation runs in a GitHub Codespace** (the owner's Windows machine has too little free RAM for Docker sanitizer builds; Docker Desktop died twice under memory pressure, and the local machine's low memory can stop long-running local waiting loops). A 4-core / 16 GB codespace was enough for `-j3` builds, `-j2` sanitizer builds, the image build, a five-container Compose stack and the load test. Install `ninja-build libboost-dev libpq-dev postgresql-client curl python3 wrk`. Never use the local Docker for the image build, the sanitizers or the benchmarks. An idle codespace shuts down after its idle timeout and takes its running jobs and containers with it (files under `/workspaces` survive). Poll a long job gently (a `gh codespace ssh` every 90-120 s, not every few seconds): the GitHub API quota is shared.
- In the Codespace, containers on a user-defined Docker bridge could not reach each other over TCP until `sudo sysctl -w net.bridge.bridge-nf-call-iptables=0 net.bridge.bridge-nf-call-ip6tables=0` was set (a VM setting, not a repository change); published ports work without it.
- Real services for the tests: `docker run -d --name ef-pg -p 5432:5432 -e POSTGRES_USER=edgeflow -e POSTGRES_DB=edgeflow -e POSTGRES_PASSWORD=efpw postgres:16` and `docker run -d --name ef-redis -p 6379:6379 redis:7`; tests need `EDGEFLOW_TEST_DB_HOST=127.0.0.1 EDGEFLOW_TEST_DB_PASSWORD=efpw EDGEFLOW_TEST_REDIS_HOST=127.0.0.1` (the Redis tests are skipped without the last one). Use a unique key prefix per test (the helpers do); never `FLUSHALL` a shared Redis. Stop these containers before a benchmark: the load test must have the machine to itself.
- TSan: GCC needs `-Wno-tsan` and `-Wno-error=array-bounds`; ASLR must be disabled (`setarch x86_64 -R`) for the test executable ALSO DURING THE BUILD, because `gtest_discover_tests` runs it after linking. Test helpers must be cooperative: no thread may close a socket (or an acceptor) another thread is blocked on; closing an acceptor does not wake a blocking `accept()` (connect to it instead), and a blocking `read` is woken by an OS-level `shutdown` or a receive timeout (`FakeRedisServer`, `RedisRelay` follow that).
- Benchmarks: `cmake -DEDGEFLOW_BUILD_BENCHMARKS=ON -DEDGEFLOW_BUILD_TESTS=OFF` (Release) then `edgeflow_micro_bench`; `bench/load/run.sh <label>` needs Docker, `wrk`, `curl`, `python3` and the image. The Dockerfile copies `bench/` because a test validates `bench/load/gateway.yaml`.
- Tooling quirks: large bash heredocs may fail (write files with the editor tool); a Python script that writes shell scripts on Windows produces CRLF (strip `\r` before running them in the Codespace); never `pkill -f <script>` over SSH (it matches the SSH shell itself) and never wait on a marker file that a previous run may have left behind (use a fresh name).
- On the local Windows machine plain `docker stop` killed containers after about 1.6 s, so use `docker stop -t N` when testing graceful shutdown; Compose sets `stop_grace_period: 10s`. Other containers on the owner's machine (supabase, backend-*, etc.) belong to the owner: never touch them.

---

(Previous checkpoint, kept for history: end of 2026-10-15, Phases 1-8 complete.)

---

## 1. Project Identity

```text
Project:    EdgeFlow
Repository: iamankoo/EdgeFlow  (https://github.com/iamankoo/EdgeFlow)
Purpose:    API Gateway, Service Discovery & Load Balancer
Language:   C++20
```

Technology actually present in the repository today (the stack is locked, see [techstack.md](techstack.md); no substitutions without owner approval):

| Area | Technology |
|------|------------|
| Language / build | C++20, CMake >= 3.25 (Ninja), `CMakePresets.json` |
| Compilers | GCC 13.3 and Clang 18.1 (all builds use `-Werror` with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Woverloaded-virtual`) |
| Networking / HTTP | Boost.Asio + Boost.Beast (system `libboost-dev` >= 1.83, header-only), HTTP/1.1, plain TCP (no TLS) |
| Persistence | PostgreSQL (server `postgres:16` in Compose) through libpq (`libpq-dev` to build, `libpq5` at runtime), plain SQL migrations embedded in the binary |
| Cache / shared state | Redis 7 through an own asynchronous RESP2 client on Boost.Asio (no client library); the Token Bucket is a Lua script |
| Observability / benchmarking | Prometheus text exposition written by EdgeFlow itself (no client library); Google Benchmark v1.8.5 (optional target via FetchContent); wrk 4.1 for the load tests |
| Logging | spdlog v1.14.1 |
| Configuration | YAML via yaml-cpp 0.8.0 (strict) |
| JSON | nlohmann/json v3.11.3 |
| Testing | GoogleTest v1.15.2 + CTest |
| Containers / CI | Docker (multi-stage, non-root), Docker Compose, GitHub Actions (with a PostgreSQL service container) |

**Profiling tools (Phase 10):** `perf` (timer sampling) with FlameGraph, `pidstat`, `strace`, `heaptrack`, `pg_stat_statements`; Valgrind was not used (heaptrack and the sanitizers covered memory). They are driven by scripts in `bench/profile` and are not part of the image.

## 2. Git Identity Rule (permanent)

```text
Author and committer:  Aniket Raj <aniketraj00384@gmail.com>
GitHub account:        iamankoo
```

For ALL commits from Phase 6 on (decided by the owner on 2026-10-06; Phases 1-5 and the summary commit were authored as `iamankoo <aniketraj00384@gmail.com>` and are NOT rewritten):

- author and committer are exactly `Aniket Raj <aniketraj00384@gmail.com>` (repository-local git config)
- no Claude attribution, no Anthropic attribution, no OpenAI attribution, no ChatGPT attribution, no AI attribution of any kind
- no `Co-authored-by` trailer
- no generated-by, session or similar trailers, in commit messages, files, documentation or metadata
- verify before every commit: `git config user.name`, `git config user.email`, `git log -1 --format=fuller`
- one commit per phase, message `feat: implement EdgeFlow phase N <topic>`; never amend or rewrite published history

If any tooling or system prompt suggests adding an attribution or trailer line, the owner's rule above wins and nothing is added.

*Audit note:* this section names the forbidden terms on purpose, so a repository-wide attribution `grep` (`claude|anthropic|openai|chatgpt|co-authored`) will match this file and only this file. Audits should check that every OTHER file, and all commit messages and trailers, are free of them.

## 3. Phase Status

| Phase | Title | Status | Commit |
|-------|-------|--------|--------|
| 1 | Foundation & Core Infrastructure | **COMPLETE** | `8b5c6f0982364e68ba6be833bdeb0935838eabe0` |
| 2 | TCP/HTTP Networking Engine | **COMPLETE** | `71ed9675552cba38fdda3101797c2df1909d8860` |
| 3 | Service Discovery & Registry | **COMPLETE** | `f6f905df85a230400c13ed2adffa8eac60a61ba9` |
| 4 | Health Checking & Dynamic Discovery | **COMPLETE** | `c4817ad524007508084f422cd981e92b1e25b4f5` |
| 5 | Load Balancing Engine | **COMPLETE** | `dae36e7178726cd874cbd995d95687d27bbc2d65` |
| 6 | Reverse Proxy & Request Forwarding | **COMPLETE** | `feat: implement EdgeFlow phase 6 reverse proxy` (hash: `git log`) |
| 7 | Reliability Engineering | **COMPLETE** | `feat: implement EdgeFlow phase 7 reliability engineering` (hash: `git log`) |
| 8 | Redis Cache & Distributed Rate Limiting | **COMPLETE** | `feat: implement EdgeFlow phase 8 redis cache and distributed rate limiting` (hash: `git log`) |
| 9 | Observability, Testing & Performance | **COMPLETE** | `feat: implement EdgeFlow phase 9` (hash: `git log`) |
| 10 | Optimization, Production Hardening & Release | **COMPLETE** | `feat: complete EdgeFlow phase 10 production hardening and release` (hash: `git log`) |

Hashes were checked against `git log` at this checkpoint. History on `main` (oldest first): `5e3be65` docs initialisation (documentation only), then the five phase commits above, then the summary checkpoint commit `docs: update project summary` (changes only that file), then the Phase 6 commit, then the Phase 7 commit, then the Phase 8 commit, then the Phase 9 commit.

Test counts per phase (all with `-Werror`; "skipped" means tests needing a real PostgreSQL when none is configured, reported by CTest as skipped, never silently passed):

| After phase | Total tests | Notes |
|-------------|-------------|-------|
| 1 | 56 | |
| 2 | 127 | |
| 3 | 192 | 30 need PostgreSQL |
| 4 | 272 | 42 need PostgreSQL |
| 5 | 379 | 49 need PostgreSQL: 330 pass + 49 skipped without a database; 379/379 pass with one |
| 6 | 507 | 55 need PostgreSQL: 452 pass + 55 skipped without a database; 507/507 pass with one |
| 7 | 630 | 58 need PostgreSQL: 572 pass + 58 skipped without a database; 630/630 pass with one |
| 8 | 733 | 111 need PostgreSQL or Redis: 622 pass + 111 skipped without them; 733/733 pass with both |
| 9 | 786 | 119 need PostgreSQL or Redis: 667 pass + 119 skipped without them; 786/786 pass with both |
| 10 | **828** | serial `ctest -j1` with both services: 828/828; without them: **828 run, 703 pass, 125 skipped, 0 failed** (`edgeflow:phase10`, 127 MB) |

## 4. Standing Rules

1. **Phases are authoritative** ([Phases.md](Phases.md)). Do not skip phases or implement later-phase features early. Before implementing a phase, cross-check its prompt line by line against `Phases.md` and report any mismatch instead of silently resolving it.
2. **Git identity is locked** (section 2).
3. **Metrics honesty:** performance figures are targets until measured by reproducible tests. Throughput and latency have been measured since Phase 9 (section 9D, `bench/results/`); no availability number has been measured so far (see "Final Target" at the end).
4. **Stack is locked** ([techstack.md](techstack.md)).
5. **PostgreSQL is the persistent source of service metadata.** Nothing may hold an authoritative duplicate of registry state in memory.
6. Validate before committing: GCC Debug, GCC Release and Clang Debug with `-Werror`, the full suite against a real PostgreSQL, repeated runs of the concurrency-sensitive tests, Docker image build, and a real runtime demonstration; then audit (attribution, secrets, identity, `git diff --cached --check`), then ONE commit and push.

---

## 5. Phase 1 — Foundation & Core Infrastructure (commit `8b5c6f0`)

- **Built:** CMake >= 3.25 / C++20 project; dependencies pinned with `FetchContent` (spdlog v1.14.1, yaml-cpp 0.8.0, GoogleTest v1.15.2; nlohmann/json joined in Phase 2); strict YAML `ConfigManager`; spdlog `Logger` wrapper; `Application` lifecycle (construct, `initialize()`, `run()`, `shutdown()`); `ShutdownCoordinator` (components register stop callbacks, run in reverse order, idempotent and thread-safe); `SignalHandler` (SIGINT/SIGTERM set a lock-free atomic that `run()` polls every 50 ms); `CommandLine` (`-c/--config`, `--version`, `-h/--help`; later `--healthcheck`); multi-stage Dockerfile (non-root); `docker-compose.yml`; GitHub Actions CI (GCC Debug, GCC Release, Clang Debug); `CMakePresets.json`.
- **Decisions:** config is strict (unknown keys are errors, all errors reported together, a failed load keeps the previous config); config loads before logging exists so config failures go to stderr (exit 2) while initialisation failures exit 1; signal handlers only set an atomic.
- **Validated:** 56/56 tests (GCC 13 Release `-Werror` during `docker build`, Clang Debug `-Werror` in a container); the container started, logged its lifecycle, stayed up, exited 0 on `docker stop` (SIGTERM) and SIGINT; Compose verified; bad config exits 2.
- **Environment fact:** the Windows development machine has no C++20 compiler or CMake, so every build and test runs in Linux containers.

## 6. Phase 2 — TCP/HTTP Networking Engine (commit `71ed967`, full `71ed9675552cba38fdda3101797c2df1909d8860`)

- **Built:** asynchronous HTTP/1.1 server on Boost.Asio + Beast: `TcpServer` (async accept on its own strand, retry with back-off on accept errors, idempotent `stop()`), `HttpConnection` (Beast `request_parser`, per-connection state machine Idle/Reading/Handling/Writing/Closing, keep-alive loop, lingering close after final responses), `HttpServer` (owns the `io_context`, `server.worker_threads` threads, the `TcpServer` and the `ConnectionTracker`; enforces `max_connections`; graceful `stop(grace)`), `ConnectionTracker` (weak references, used to drain and force-close), `RequestHandler` interface + `LocalRequestHandler` (`GET /`, `GET /health` -> `{"service":"edgeflow","status":"ok"}`, `POST /echo`, 404, 405 with `Allow`), `HealthProbe` (the `edgeflow --healthcheck` client used by the container HEALTHCHECK).
- **Behaviour:** HTTP/1.1 persistent by default, `Connection: close` honoured, HTTP/1.0 answered and closed, pipelined requests served in order, chunked request bodies reassembled; 400 (malformed), 404, 405, 408 (request timeout), 413 (body limit), 431 (header limit), 500 (handler exception, logged).
- **Config keys (`server.*`):** `host` (0.0.0.0), `port` (8080), `request_timeout_ms` (5000), `keep_alive_timeout_ms` (10000), `max_request_body_bytes` (1 MiB), `max_header_bytes` (8192), `max_connections` (1024), `worker_threads` (2); `shutdown.grace_period_seconds` (5).
- **Decisions:** one `io_context` run by a small worker pool, no thread per connection; each connection's socket, timer and handlers share a strand; per-connection `steady_timer` with a generation counter instead of Beast `tcp_stream` timeouts (those close the socket before a 408 can be written); the idle wait uses `socket.async_wait(wait_read)` (consumes nothing) because Beast's `async_read_some` keeps reading until the whole header is complete, which left stalled partial headers under the idle limit and never produced a 408; the request timeout starts when the first byte arrives; `draining` is an atomic set from the stopping thread (a synchronous handler blocks the connection's strand), and a draining connection answers `Connection: close` and never returns to idle.
- **Bugs found and fixed during validation:** fmt could not format Beast `boost::core::string_view` in log calls (converted to `std::string_view`); partial-header timeout (above); in-flight request kept the connection alive during drain (above); a Clang-only `[[nodiscard]]` error in a test; two test compile errors; a test helper that could hang on `accept`.
- **Validated:** 127/127 tests with GCC 13 Debug, Clang 18 Debug and the Release `-Werror` image build; 5 consecutive full runs; real binary exercised with curl and raw sockets (malformed requests 400, oversized header 431, oversized body 413, partial header and stalled body 408 at 5 s, idle close at 10 s, keep-alive over one connection, 10/50/100 concurrent clients all correct); SIGTERM drains and exits 0; a stalled connection is force-closed at the grace period; Docker image (30.5 MB, non-root, working HEALTHCHECK) and Compose verified; the Dockerfile build is bounded with `ARG BUILD_JOBS=2` (unbounded parallelism exhausted the Docker VM's memory).
- **Known limitation still relevant:** `RequestHandler::handle` is **synchronous** (it runs on an I/O worker thread). `Expect: 100-continue` is not implemented.

## 7. Phase 3 — Service Discovery & Registry (commit `f6f905d`)

- **Built:** a PostgreSQL-backed registry of backend service instances. `ServiceRegistry` interface; `PostgresServiceRegistry` (every operation is one parameterised, atomic SQL statement; no in-memory copy); libpq wrapper (`PgConnection`, `PgPool` lazily opened fixed-size pool, `PgResult`; parameters always sent out of band); migrator (embedded SQL files, `schema_migrations` table, advisory lock, each migration in a transaction, idempotent and safe to run concurrently); JSON API under `/services`; `database` config section; Docker Compose with `postgres:16` (health-gated start, named volume `edgeflow-pgdata`); CI PostgreSQL service.
- **Model:** a service is a logical name (lowercase `[a-z0-9._-]`, 1-64); an instance is identified by **`(service, instance_id)`** and has `host`, `port`, **registration status** (`active`, `draining`, `disabled`), **health status** (`unknown`, `healthy`, `unhealthy`), `version`, `weight` (0-1000, default 1), `connection_count`, and DB-assigned `registered_at`/`updated_at`. Registration status and health status are deliberately separate. Identity (`service`, `instance_id`, `host`, `port`) is immutable.
- **Semantics:** registration is NOT an upsert (a duplicate `(service, instance_id)` or `(service, host, port)` is 409); deregistration deletes the row and a repeat is 404; the service stays known with zero instances (empty list, 200) while a never-registered service is 404; `adjustConnectionCount` is an atomic add clamped at zero; reads (lookup, get, list, routable, list instances) are retried once on a lost connection, writes are not (unknown whether committed) except the idempotent `updateHealth`; `listServices` orders with `COLLATE "C"`.
- **Schema (`db/migrations/001_service_registry.sql`):** `services(id, name UNIQUE, created_at)`, `service_instances(... UNIQUE(service_id, instance_id), UNIQUE(service_id, host, port) ...)` with CHECK constraints repeating the validation (name/id format, port range, status and health vocabularies, version length, weight range, non-negative connections).
- **Config (`database.*`):** `enabled` (default **false**, so Phase 2 setups keep working), `host`, `port`, `name`, `user`, `password_env` (the NAME of the environment variable holding the password; the password can never be in the YAML and is never logged), `pool_size` (4), `connect_timeout_seconds` (5). Enabled + unreachable database = startup failure (exit 1). `config/config.yaml` ships disabled; `config/config.compose.yaml` enables it for Compose.
- **Failure behaviour:** a database outage is reported as `DatabaseUnavailable` (HTTP 503), never as success; dead pooled connections are detected (two reads, because libpq only sees the closed socket on the second) and replaced; `/` and `/health` keep working during an outage.
- **Validated:** 192/192 (GCC Debug, Clang Debug, Release image build; 30 skipped without a database); 5 consecutive runs; persistence after dropping all in-memory state; 8-way racing registrations (exactly one winner); concurrent register/lookup/deregister; recovery after the server kills a connection; SQL metacharacters stored as data; real Compose demonstration (register, discover, restart EdgeFlow, full stack down/up with the volume, deregister, PostgreSQL stopped -> 503 and recovery).
- **Known limitation:** registry calls block an I/O worker for the duration of a query (the Phase 2 handler is synchronous; statement timeout 10 s; the pool size bounds concurrency).

## 8. Phase 4 — Health Checking & Dynamic Discovery (commit `c4817ad524007508084f422cd981e92b1e25b4f5`, `feat: implement EdgeFlow phase 4 health checking`)

### Health state machine (`HealthTracker`, pure, no I/O)

- States `UNKNOWN`, `HEALTHY`, `UNHEALTHY`. The **first probe of an unknown instance decides at once** (no prior state to flap from).
- Healthy -> unhealthy after `failure_threshold` **consecutive** failures; unhealthy -> healthy after `success_threshold` consecutive successes. A result in the opposite direction **resets the streak**; a transition starts a fresh streak; the state never returns to `UNKNOWN`.
- **Only transitions are written to PostgreSQL** (steady health causes no database writes); a failed write is retried after the next probe (no retry loop).

### TCP health checks

A successful TCP connect within `timeout_ms` = healthy (proves only that the port accepts connections, not that the application works). Connection refused, host unreachable, unresolvable name, connection reset and timeout are unhealthy; cancellation (instance removed, shutdown) never invokes the callback.

### HTTP health checks

`GET <http_path>` (default `/health`, `Connection: close`); **2xx = healthy**. Redirects (3xx, not followed), 1xx, 4xx, 5xx, a malformed response, a connection closed before or during the response, a reset and a timeout are all **unhealthy**. Only the response head is read. One timeout bounds the whole probe (resolve + connect + request + response head).

### `HealthChecker`

- Timer-driven, **no busy loop**. One private `io_context` thread owns all checker state (no locks); a **separate one-thread Asio pool** runs the blocking PostgreSQL calls, so database latency never delays probing. Probes are capped by `max_concurrent_checks`.
- **Registry refresh** every `refresh_interval_ms` with one `listInstances()` query: new instances are probed immediately (no restart needed); deregistered instances are dropped and their pending probe cancelled; **disabled** instances are not probed (last known health kept); **draining** instances are probed but never routable; a **re-registered** instance (different `registered_at`, host or port) restarts as a new instance and results computed for the old incarnation are discarded; `updateHealth` is additionally pinned to `registered_at` in SQL, so a **stale result can never be written onto a newer instance**. A failed refresh keeps the known instances (logged once when it begins, once when it recovers).
- The checker holds only runtime state (counters, timers), **not a copy of the registry**. When it is on it owns `health_status`: a value written by hand is corrected by the next probe.
- **Shutdown:** the HTTP server stops first, then `stop()` cancels all timers and probes on the checker thread, stops it, waits for an in-flight database call and joins; idempotent, concurrent callers all wait; verified with SIGTERM while checks were active (exit 0, about 0.7 s).

### `NameResolver` (important bug found during Compose validation)

Asio's resolver runs `getaddrinfo()` one lookup at a time on a single background thread. When backend B was stopped, its DNS name started blocking; backend A's lookup queued behind it and timed out, so **a healthy instance was wrongly marked unhealthy** (both failed at the same instant in the log). Fix: per-host lookup isolation (each distinct host on its own detached thread), same-host lookups share one lookup, at most 16 lookups run at once (beyond that a probe fails at once instead of queueing behind slow ones), cancelled or timed-out lookups are never delivered (so a late result cannot reach a torn-down `io_context`). Regression tests added (`ProberIsolationTest`, `NameResolverTest`) and **the original Compose scenario was rerun successfully**: with B down, A stayed routable the whole time and only B changed state.

### Routable view

```text
ACTIVE   + HEALTHY    = routable
DRAINING + HEALTHY    = NOT routable
DISABLED + HEALTHY    = NOT routable
any status + UNHEALTHY or UNKNOWN = NOT routable
```

`ServiceRegistry::lookupRoutable(service)` and `GET /services/{svc}/routable`: neutral filter, no ordering, no strategy. Plain discovery (`lookupService`, `GET /services/{svc}/instances`) still returns everything.

### Config (`health_check.*`, default **disabled**; requires `database.enabled`, validated at load)

`enabled`, `type` (`tcp` default | `http`), `interval_ms` (5000), `timeout_ms` (2000, must not exceed `interval_ms`), `http_path` (`/health`), `failure_threshold` (3), `success_threshold` (2), `refresh_interval_ms` (5000), `max_concurrent_checks` (32). Compose config enables it with HTTP checks every 2 s, thresholds 2/2.

### Phase 4 validation

- **272/272 tests** with GCC 13 Debug, GCC 13 Release and Clang 18 Debug, all `-Werror`, against real PostgreSQL (42 skipped without a database); the Phase 4 tests passed 10 consecutive runs; deterministic threshold tests use a manually driven prober (no network, no sleeps).
- **Real runtime** (Docker Compose, PostgreSQL, two or more backend containers resolved by Docker DNS): registered instance became routable after its first probe; stopping B excluded it (about 2.7-5.9 s) and restarting it reintroduced it (about 5.2-5.5 s); an HTTP 500 with the port open was excluded (about 3.2 s) and recovery reintroduced it (about 2.6-3.6 s); an instance registered while running was picked up in about 0.7-1.9 s; a deregistered instance stopped receiving probes; a disabled healthy instance was neither routable nor probed; EdgeFlow restarted while B was down and the routable view excluded B immediately; with PostgreSQL stopped the registry API returned 503 while `/health` stayed 200, checks continued, and the pending verdict was written once the database returned; `docker stop` about 0.7 s, exit 0, HTTP server stopping before the health checker. **These timings are single-run observations on a development machine with 2 s intervals, NOT benchmarks.**
- Test-infrastructure bugs fixed on the way: a helper that deadlocked on `accept` at teardown; tests that compared the tracked count with one service instead of the whole registry.
- **Limitations:** one probe type for all instances; the routable view is a database query on each call.

---

## 9. Phase 5 — Load Balancing Engine (completed 2026-10-03; commit `dae36e7178726cd874cbd995d95687d27bbc2d65`, `feat: implement EdgeFlow phase 5 load balancing`)

**Phase 5 is complete.** Exit condition ("requests can be distributed among healthy backend instances using all four strategies") was demonstrated in tests and on real containers.

### `LoadBalancer` abstraction (`include/edgeflow/routing/`)

- One interface, `LoadBalancer::select(instances, context)`, implemented by all four strategies (non-virtual `select` + virtual `doSelect`; the empty set is handled once, in the base). It returns a **pointer into the caller's set** (null when empty).
- **Input is the routable set** that discovery already filtered (active AND healthy). The strategies contain **no health logic, no database access, no HTTP/forwarding logic**, and never own or duplicate the registry. An instance that left the set cannot be selected; a recovered one is selected again as soon as discovery lists it.
- **Identity is `(service, instance_id)`**, never vector position, pointer or mutable metadata. Every strategy puts the input into canonical order of that identity (an instance listed twice counts once), so **input order is irrelevant**.
- `RoutingContext{key}` carries the optional request key; only consistent hashing uses it.

### The four strategies

- **Round Robin:** cycles `a, b, c, a, ...` in canonical order. State = the identity chosen last; next = first instance with a greater identity, wrapping to the smallest, so the **cycle survives instances being added or removed** between calls. One mutex; **thread-safe and tested concurrently**.
- **Least Connections:** picks the lowest `connection_count`; **ties go to the smallest id** (deterministic). It only **reads** the count from the snapshot; it does **not** treat a routing decision as a connection. **The Phase 6 proxy will maintain the real count.** Stateless.
- **Weighted Routing:** **smooth weighted round-robin** (the nginx algorithm): deterministic, **exact proportions** over any window of (sum of weights) picks, interleaved not bursty; weights 5/3/2 give **500/300/200 over 1000 selections** and the sequence `abcaabacba`. **Weight 0 = no share** while any weight is positive; **all-zero falls back to equal shares** (stay reachable); sums are **64-bit** (no overflow with 4294967295 weights). State: a running score per instance, pruned to the current set.
- **Consistent Hashing:** **immutable hash ring** with **160 virtual nodes** per instance at `stableHash("service/instance_id#replica")`; **platform-stable hash** (FNV-1a + murmur3 finaliser, identical everywhere, unlike `std::hash`); a key is served by the first point at or after `stableHash(key)`, wrapping. **The ring is rebuilt only when the set of instance identities changes** (cached, shared snapshot, concurrent selections do not block each other). Deterministic key mapping; **minimal remapping**: adding an instance moves keys only to it, removing one moves only its keys. Weights are not used; an empty key is an ordinary key.

### `Router`

Takes **one snapshot** from `ServiceRegistry::lookupRoutable(service)`, passes it to the configured strategy and returns the chosen instance (a copy). Errors: `ServiceNotFound` (never registered), **`NoRoutableInstance`** (new error code, service exists but nothing routable; HTTP **503**), plus the registry's own errors (`InvalidArgument`, `DatabaseUnavailable`). No registry access or lock is held while choosing. It does **not** forward anything.

### Configuration

```text
routing.strategy   default: round_robin
supported:         round_robin | least_connections | weighted | consistent_hashing
```

Unknown values and unknown keys in the section are rejected at startup with a message naming `routing.strategy`. The application builds the `Router` over the registry when `database.enabled`; startup logs the strategy.

### Diagnostic route

`GET /services/{svc}/route[?key=K]` (percent-decoded key) returns `{service, strategy, key, selected:{...instance}}`: 200, 404 unknown service, **503** nothing routable, 405 wrong method (404 when no router exists). **It is diagnostic only: it reports the routing decision, forwards nothing, changes no state (not even `connection_count`), and does NOT implement Phase 6.**

### Fixes made during Phase 5

- **Phase 3 regression fix:** `listServices` ordering depended on the database locale (`en_US` collation ignores punctuation; found when the Clang and Release runs met a table with more varied names while a test asserted byte order). **Fix: `ORDER BY name COLLATE "C"`**, so ordering is deterministic across environments; `ServiceRegistry.hpp` documents byte order.
- A **hand-computed test expectation was wrong** (the weighted sequence): the implementation was right (at round 5 two scores tie and the smaller identity wins); the test was corrected to `abcaabacba`, no behaviour change.
- A `-Werror` dangling-else in a test was fixed with braces.

### Phase 5 testing

- **379/379 tests** pass with **GCC 13 Debug, GCC 13 Release and Clang 18 Debug, all `-Werror`, against real PostgreSQL**. **Without a database: 330 pass, 49 skipped** (the Docker image build runs in that mode, Release `-Werror`, `BUILD_JOBS=2`).
- **Repeated runs:** the 183 health-checking and routing tests passed 10 consecutive runs on GCC Debug (68 on Release).
- **Shared routing contract tests** run against all four strategies: empty and single-instance sets, pointer into the caller's set, input unchanged, only members of the given set chosen, input-order independence, sets changing between calls, **8 concurrent threads**.
- **Per-strategy tests:** Round Robin cycles, order independence, removal/addition, listed-twice, **24 000 concurrent selections over 3 instances give exactly 8 000 / 8 000 / 8 000**; Least Connections minimum, ties, changing counts, huge counts; Weighted exact windows, 100 000-sample proportions, 1000:1 weights, zero and all-zero weights, overflow-sized weights, re-weighting, 10 000 concurrent picks exactly 5000/3000/2000; Consistent Hashing stickiness, spread, add-moves-only-to-new, remove-moves-only-its-keys, restore, virtual-node effect, a ring rebuilt concurrently, stable hash.
- **Router / boundary tests:** with A, B, C healthy and D unhealthy (D given the best weight and connection count) **no strategy ever selects D**; draining and disabled instances are never selected even when healthy; an instance that turns unhealthy stops receiving requests and a recovered one returns; distinct error cases; configuration and factory tests; HTTP route endpoint tests.
- **End-to-end test** with **real PostgreSQL + the real health checker + real TCP backends** (all four strategies, failover and recovery, consistent-hash remapping while the live set changes, concurrent routing while health flaps).

### Phase 5 runtime validation (Docker Compose; PostgreSQL, health checker, four backend containers; **20/20 checks passed**)

Backends a, b, c healthy; d answers HTTP 500 (unhealthy) and has the best-looking numbers (weight 9, 0 connections).

- **Round Robin:** `abcabcabc`.
- **Least Connections** (a=5, b=2, c=7): selected **b**; after b was set to 8 it **switched to a**; when b returned to 2 it **returned to b**.
- **Weighted** (a=5, b=3, c=2): **500 / 300 / 200** over 1000 requests.
- **Consistent Hashing:** 400 keys mapped stably over a, b, c; **adding e moved 94 keys, all to e**; **stopping b remapped only b's 103 keys** and **b was never selected while absent**; **restarting b restored exactly its old keys**.
- **Health integration:** backend **D was excluded and never selected by any strategy**.

### Phase 5 limitations (real, not achievements)

- **Requests are NOT forwarded yet** (the route endpoint only reports a decision).
- **`connection_count` is not maintained by EdgeFlow** until the proxy exists; Least Connections only reads whatever was written through the registry.
- **The routable set is read from PostgreSQL on every decision**; there is **no routing cache**.
- **One strategy applies to all services.**
- **The remote GitHub Actions run has not been confirmed** (the workflow installs libpq, starts a PostgreSQL service and runs the same configurations that were run locally in containers).
- **No performance figures have been measured.**

---

## 9A. Phase 6 — Reverse Proxy & Request Forwarding (completed 2026-10-06; commit `feat: implement EdgeFlow phase 6 reverse proxy`)

**Phase 6 is complete.** Exit condition ("Client -> EdgeFlow -> Backend -> EdgeFlow -> Client works reliably") was demonstrated in tests and on real containers (PostgreSQL + health checker + backend containers). The commit hash is the one in `git log` for that message. Full design: [architecture.md](architecture.md) section 22.

### What was built

- **Service mapping `/proxy/{service}/...`**: prefix stripped, path and query forwarded verbatim (`/proxy/orders/` -> `/`, `/proxy/orders/api/users?id=42` -> `/api/users?id=42`). `/proxy`, `/proxy/`, an invalid service name -> `400`; unknown service -> `404`. `/`, `/health`, `/echo`, `/services/...` unchanged; `GET /services/{svc}/route` is still only a decision.
- **`proxy` module** (`include/edgeflow/proxy/`, `src/proxy/`, `tests/proxy/`): `ProxyHandler` (outermost request handler, owns the operations and threads), `ProxyHeaders` (pure URL/header/request-id rules), `UpstreamPool` (idle keep-alive connections), `UpstreamClient` (one request to one backend).
- **Asynchronous request boundary** (the design question the Phase 5 summary left open): `RequestHandler::handleAsync(request, RequestContext, done) -> CancelFunction`; the default calls the synchronous `handle()` inline, so every Phase 1-5 handler is unchanged. `HttpConnection` in state `Handling` no longer blocks its strand/worker: the answer comes through a callback (any thread, first answer wins) re-posted onto the connection's strand; closing the connection (forced close at the end of the grace period) calls the cancel function. A graceful drain lets the request finish and answers `Connection: close`. The handler learns the client address. HEAD responses keep the backend's `Content-Length`.
- **Threading**: server I/O workers (client sockets) are never blocked by a backend or the database. The proxy has its own `io_context` (`proxy.io_threads`, every exchange serialised on its own strand) and a small thread pool for the **blocking registry calls** (`route`, `+1`, `-1`; sized to `database.pool_size`). No routing cache was introduced (Phase 8 territory); the routable set is still read from PostgreSQL per request.
- **Routing**: `Router::route(service, {client address})` over the Phase 4 routable view; all four strategies work; consistent hashing is keyed by the client address. No health logic, no PostgreSQL selection, no `LoadBalancer` bypass in the proxy.
- **Header propagation**: hop-by-hop headers (`Connection`, `Keep-Alive`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`, `Proxy-*`, anything named by `Connection`/`Trailer`) never cross; `Expect` and `Content-Length` are dropped and recomputed; `Host` rewritten to the instance; `X-Forwarded-For` extends the chain; `X-Forwarded-Proto: http`; `Via: 1.1 edgeflow` on request and response; repeated headers (Set-Cookie) preserved.
- **Request ids**: client `X-Request-Id` (1-128 visible ASCII) preserved, otherwise a random UUID v4; sent upstream, returned to the client (also on gateway errors), present in log lines `proxy [<id>] ...`.
- **Connection reuse and pool**: per-backend idle LIFO pool, cap `proxy.max_idle_connections` (0 = off), TTL `proxy.idle_timeout_ms`, liveness check at check-out (non-blocking peek: EOF, reset or unsolicited bytes -> dropped), eviction on use, `close()` at shutdown. Reusable only after a complete keep-alive response with nothing left over; every error path discards.
- **Stale pooled connection safeguard (approved, strictly limited)**: a pooled connection found dead **before any request byte was written** (at check-out, or a first write that wrote 0 bytes) is discarded and the request goes out once on a fresh connection. Never after the first byte, never on a fresh connection, never on an HTTP status; no counter, backoff, breaker or failover. Verified: stale connection replaced and request sent exactly once; a request already sent to a backend that then hangs up is NOT repeated (502, one request seen by the backend, no new connection).
- **Timeouts**: `proxy.connect_timeout_ms` (resolve + connect) and `proxy.upstream_timeout_ms` (whole exchange, including a body that stalls) both answer `504`, cancel the timers, close the connection (never pooled), release the count; the client's connection stays usable.
- **Failure mapping**: backend status codes (incl. 4xx/5xx) are forwarded unchanged; `502` refused/reset/closed early/invalid HTTP/oversize/unresolvable name; `504` timeouts; `503` nothing routable, registry unavailable, shutting down. Bodies are generic JSON (no internal addresses) with the request id. A backend failure never closes the client connection.
- **Connection counting**: `adjustConnectionCount(+1)` after routing, `-1` on every outcome (success, failure, timeout, malformed/oversize, cancellation, shutdown) **before the response is handed back** (found during validation: releasing it after the response made a sequential client's next request see the previous one still counted, which broke Least Connections; the release is now part of the single completion path). The routing decision itself never touches the count. If the `+1` fails the request proceeds uncounted (logged).
- **Buffering and limits**: string bodies; requests bounded by `server.max_request_body_bytes` (413), responses by `proxy.max_response_bytes` (default 16 MiB, `502` beyond, connection discarded), response headers 64 KiB.
- **Shutdown**: `reverse-proxy` is registered with the shutdown coordinator before the health checker and the HTTP server, so it stops last of the three. The server drains/force-closes (cancelling upstream exchanges); `ProxyHandler::stop()` refuses new proxy requests (503), cancels what is left, waits until every operation has answered and released its count, closes the pool, joins the threads. Idempotent.
- **Configuration `proxy.*`** (strict, default off, requires `database.enabled`): `enabled`, `connect_timeout_ms` (2000), `upstream_timeout_ms` (30000, not below the connect timeout), `io_threads` (2), `max_idle_connections` (32), `idle_timeout_ms` (30000), `max_response_bytes` (16 MiB). `config/config.yaml` ships it off; `config/config.compose.yaml` enables it (5 s upstream timeout). Compose image tag is now `edgeflow:phase6`.

### Phase 6 testing

- **507 CTest tests** (379 before; +128): header/mapping/request-id/pool unit tests, 66 end-to-end `ProxyTest` tests against scripted real TCP backends (forwarding, bodies, headers, request ids, reuse, stale connections, timeouts, malformed/oversize responses, 502/503/504 mapping, connection counts, shutdown), `AsyncHandlerTest` (async boundary, cancellation, close during handling), `ConfigManagerProxyTest`, and 6 `ProxyPostgresTest` on real PostgreSQL + the health checker + `Application`.
- **GCC Debug, GCC Release, Clang 18 Debug: 507/507 pass with PostgreSQL** (all `-Werror`). Without a database: **452 pass, 55 skipped, 0 failed** (55 = 49 earlier + 6 `ProxyPostgresTest`). The Docker Release image build (no database) reported the same: 100% passed, 0 failed of 507.
- **Repeats:** the proxy/pool/async/HTTP-server/Postgres-proxy suites (117 tests) passed 12 consecutive runs.
- **ThreadSanitizer** (GCC, `-Wno-tsan`, `setarch x86_64 -R`): 124 proxy/pool/async/HttpServer/Application tests, 0 warnings after fixing a test-helper destruction-order bug (`UpstreamPoolTest::Loopback`; test code only).
- **Bugs found and fixed during validation:** the connection count was released after the response (Least Connections saw a finished request as active); now released first.
- **AddressSanitizer + UBSan** (GCC, `-fsanitize=address,undefined -O1`, `detect_leaks=1`, real PostgreSQL): the same 124 proxy/pool/async/HttpServer/Application tests passed, with 0 sanitizer reports.

### Phase 6 runtime validation

Docker Compose (`edgeflow:phase6`, `postgres:16`, `config.compose.yaml`: HTTP health checks every 2 s, round robin, 2 s connect and 5 s upstream timeout), real Python HTTP/1.1 backend containers `a`, `b` (healthy), `c` (`/health` answers 500, unhealthy), `s` (solo service); throwaway helper, not in the repository. Everything below is the literal observed behaviour of a single run on a development machine; **no performance claim is made**.

- **Startup log:** `reverse proxy ready: /proxy/{service}/..., connect timeout 2000ms, upstream timeout 5000ms, up to 32 idle connection(s) per backend, 2 I/O thread(s)`. Routable set of `demo` = `a`, `b` only; `c` shows `unhealthy`.
- **Mapping, headers, request id:** `GET /proxy/demo/api/users/7?x=1&y=two%20words&z=` reached backend `a` as target `/api/users/7?x=1&y=two%20words&z=` with `X-Custom`, `Authorization` preserved, `Host: ef-be-a:9000`, `X-Forwarded-Proto: http`, `X-Forwarded-For: 172.31.0.1`, `Via: 1.1 edgeflow` and the client's `X-Request-Id: client-req-123`, which was also returned to the client together with the backend's `X-Backend` and a `Via` header. Without a client id two calls returned two different generated UUIDs.
- **Bodies and methods:** a 2000-byte POST body arrived complete (`body_length` 2000, `Content-Length` recomputed); a JSON POST (17 bytes), PUT and DELETE were forwarded with the right method; HEAD returned the backend's `Content-Length` with no body.
- **Routing:** 10 requests to `demo` went 5 to `a` and 5 to `b`; `c` (unhealthy) never received one.
- **Response headers:** two `Set-Cookie` headers were both delivered; the backend's `Keep-Alive` header was dropped.
- **Backend statuses forwarded unchanged:** 201, 301, 400, 404, 418, 500, 503 (body of the 500 intact).
- **Connection reuse:** before 40 sequential client requests the solo backend had seen 1 connection and 14 requests; afterwards **1 connection and 54 requests**; three further calls reported `connection_id` 1 with `requests_on_this_connection` 55, 56, 57 (health probes are not counted by the helper).
- **Connection counts:** `connection_count` 0 before, **1 during** a 3 s request (that request returned 200 in 3.01 s), 0 after; with 3 concurrent 2.5 s requests it was **3 during** and 0 after.
- **502:** backend closing without answering -> `502 the backend closed the connection without a complete response`; connection reset -> the same; garbage bytes -> `502 the backend sent an invalid response`; instance on a live host with a closed port (forced `healthy`) -> `502 the backend could not be reached` in about 13 ms. The backend stayed usable afterwards (200).
- **504:** a backend sleeping 7 s -> `504 the backend did not respond in time` after **5.02 s** (`upstream_timeout_ms` 5000); counts back to 0. A backend container stopped while still registered (its address no longer answers) also gave 504, about the connect timeout, and its count returned to 0.
- **503 / 404 / 400:** a service whose only instance is unhealthy -> 503 (also `refused` after the health checker corrected it, and `solo` about 7 s after its container was stopped); never-registered service -> 404; `/proxy/` -> 400; invalid service name -> 400. When the stopped backend was started again it was routable and served 200 again.
- **Stale pooled connection:** after a warm request the backend container was restarted (`docker restart`), and the next request returned **200 on a new upstream connection** (`requests_on_this_connection` 1). Whether the safeguard replaced a dead pooled connection or the pool had already dropped it is not distinguishable from the client side; the unit and end-to-end tests cover the exact rule (see above).
- **Graceful shutdown, request in flight:** `docker stop edgeflow` with a 3 s request in flight (grace 5 s): the request returned **200 in 3.01 s**, the log shows `waiting up to 5000ms for 1 connection(s) to finish`, then the server, health checker and `reverse-proxy` stopped in order, exit code 0.
- **Shutdown outliving the grace period** (a second container with `grace_period_seconds: 1`, a 4.5 s request, `docker stop -t 30`): `1 connection(s) still open after the grace period; closing them`, the proxy logged `upstream cancelled ... (503)`, the proxy stopped, exit code 0, and the instance's `connection_count` was released (unchanged by the cancelled request).
- **Finding (not an EdgeFlow defect, documented):** with Docker's default `docker stop` on this machine the process was SIGKILLed after about 1.6 s (exit 137) — the same happens to a container that ignores SIGTERM — so two such runs left `connection_count` stuck at +1 each. Counts are not reconciled after a hard kill or crash.
- **Cleanup:** the demo stack (`docker compose down -v`), backend containers and the throwaway second EdgeFlow were removed.

### Phase 6 limitations (real, not achievements)

- A client that disconnects while its request waits for the backend is noticed only when the response is written; the exchange still ends at the upstream deadline, which releases everything. Half-closed clients are therefore served normally.
- Idle pooled connections of a backend that is never contacted again are evicted the next time the pool is used (no reaper thread) or at shutdown.
- HTTP/1.1 over plain TCP to backends only; no `Upgrade`/WebSocket, `CONNECT`, HTTP/2 or TLS upstream; `Expect: 100-continue` is stripped; chunked trailers dropped; requests/responses are buffered, not streamed.
- `X-Forwarded-For` extends whatever chain the client sent (no trusted-proxy list). Consistent hashing is keyed by the client address, not by a header.
- Every proxied request costs three PostgreSQL round trips (routable read, `+1`, `-1`), the last one before the response; the lookup queue is a shared FIFO. This is a measured-later cost: no routing cache exists (not added on purpose) and no performance figure has been measured.
- `connection_count` is not reconciled after a hard kill (SIGKILL, crash, OOM): in-flight requests of a killed process leave their +1 behind (observed in the runtime demonstration). A graceful stop, including one that outlives the grace period, releases every count.
- One strategy applies to all services; no retry/backoff/circuit breaker/failover (Phase 7); no cache or rate limiting (Phase 8); no metrics (Phase 9).
- The remote GitHub Actions run has still not been confirmed (the workflow is unchanged and runs the same configurations that were run locally in containers).

---

## 9B. Phase 7 — Reliability Engineering (completed 2026-10-07; commit `feat: implement EdgeFlow phase 7 reliability engineering`)

**Phase 7 is complete.** Exit condition ("Backend failures do not unnecessarily cascade into gateway failures") was demonstrated in tests and on real containers (PostgreSQL + health checker + backend containers). Full design: [architecture.md](architecture.md) section 23.

### What was built

- **`reliability` module** (`include/edgeflow/reliability/`, `src/reliability/`, `tests/reliability/`): `ReliabilityManager`, `RetryPolicy` + `BackoffPolicy` (`Retry.hpp`), `CircuitBreaker` + `CircuitBreakerRegistry`, `Failover.hpp` (`AttemptLedger`, `backendKey`). The module performs no I/O; the proxy classifies attempts (`Success`, `RetryableStatus`, `NotSent`, `MaybeProcessed`, `Permanent`, `Cancelled`) and acts on its decisions.
- **Timeouts:** `reliability.timeout.total_timeout_ms` bounds the whole request; each attempt gets `min(proxy.upstream_timeout_ms, what is left)`.
- **Retry policy and limits:** `max_attempts` counts all attempts (1-10); `retryable_statuses` (default 502/503/504); `retryable_methods` (default GET/HEAD/OPTIONS) apply after the backend MAY have processed the request; a request that provably never reached a backend is retried whatever its method. The Phase 6 stale-pooled-connection safeguard is unchanged and not counted as a retry.
- **Exponential backoff with jitter:** `min(max_delay, base * 2^(n-1))` shortened by up to `jitter_percent`; the pause is a cancellable timer, no thread blocks; a pause that does not fit the remaining budget ends the request with the last answer.
- **Circuit breaker per backend instance:** CLOSED -> OPEN (after `failure_threshold` consecutive failed requests, at most one failure per request per backend) -> HALF-OPEN (after `recovery_timeout_ms`, up to `half_open_max_requests` probes) -> CLOSED; a failed probe re-opens. State-epoch tickets ignore stale results. Separate from, and sharing no state with, the Phase 4 health checker.
- **Failover:** `Router::route(service, context, Filter)` applies an `eligible` (circuit admits) and a `preferred` (not yet tried) filter to the single routable snapshot before the strategy chooses. Strategies are untouched.
- **Graceful degradation:** nothing eligible -> immediate `503`; retries exhausted -> the last backend answer; gateway, `/health`, registry API and other services unaffected. `reliability.enabled: false` keeps Phase 6 behaviour (one attempt).
- **Configuration `reliability.*`** (strict, default off, requires `proxy.enabled`; defaults: total 30000 ms, 3 attempts, 100-2000 ms backoff, 20 % jitter, circuit 5 failures / 30000 ms recovery / 1 probe). `config/config.compose.yaml` enables it with short delays (total 12000 ms, 3 attempts, 100-2000 ms, circuit 3 / 6000 ms / 1). Compose image tag is `edgeflow:phase9`.

### Phase 7 testing

- **630 CTest tests** (507 before; +123): `RetryPolicyTest`, `CircuitBreakerTest` (deterministic, injected clock/random), `ReliabilityProxyTest` (end-to-end against scripted real TCP backends), `ReliabilityPostgresTest` (3, real PostgreSQL + health checker + `Application`), configuration tests, `RouterTest` filter tests. Results per configuration: see section 0.
- The sanitizer runs reported nothing, and no production code was changed during the final validation. One validation-environment problem (not a code defect): TSan binaries could not start with ASLR enabled, so the TSan build must run under `setarch x86_64 -R` (section 0).

### Phase 7 runtime validation (Docker Compose, Codespace; `edgeflow:phase9`, `postgres:16`, `config.compose.yaml`)

Python backend containers `a`, `b` (service `shop`) and `s` (service `solo`) controllable at run time (`ok`, 503, 500, connection reset, hang); their `/health` stays 200 so the health checker keeps them routable and only the reliability layer reacts. Everything below is the literal observed behaviour of a single run; **no performance claim is made**.

- **Startup log:** `reliability ready: total timeout 12000ms, retries on (up to 3 attempt(s), backoff 100-2000ms), circuit breaker on (opens after 3 consecutive failure(s), recovery 6000ms, 1 probe(s))`.
- **Healthy:** 6 GET to `shop` -> 200 alternating `a`, `b` (about 4 ms each), no retry.
- **Backend failure, retry, failover:** `a` set to answer 503: 8 of 8 GET to `shop` returned **200 from `b`**; the log shows `attempt 1 of 3 failed (retryable_status); retrying in 84ms` (also 83 and 97 ms); the first requests took 90-105 ms (one failed attempt + pause + retry), later ones 4-45 ms. `a` had received 6 requests in total (3 healthy + 3 failed).
- **Circuit OPEN:** `circuit breaker shop/a@ef-be-a:9000: closed -> open` after the 3rd failed request. 10 further GET: all 200 from `b`; `a` received exactly 1 more request, which is a HALF-OPEN probe once the 6 s recovery elapsed (log: `open -> half-open`, then `half-open -> open` because the probe failed).
- **HALF-OPEN recovery -> CLOSED:** after `a` was set back to ok, the next request logged `open -> half-open` and `half-open -> closed` within 1 ms and `a` served again (6 GET: 200 alternating `a`, `b`). Over the whole run the log shows 6 `closed -> open` transitions and 14 further `open -> half-open` / `half-open -> open` / `half-open -> closed` transitions.
- **Retry limit and backoff:** single-instance service `solo` whose backend always answers 503: request 1 -> **503 after exactly 3 backend hits** in 0.302 s with pauses **99 ms and 191 ms** (the second pause doubles); request 2 the same (83 ms, 190 ms; 0.284 s); request 3: 1 hit, the circuit opened, `no backend left for another attempt`, 503; requests 4 and 5: **503 in about 1.3 ms with 0 backend hits** (open circuit, fast failure). `/health` answered 200 throughout.
- **Retryable methods and statuses** (configured 502/503/504 and GET/HEAD/OPTIONS): POST and DELETE with a 503 -> **1 backend hit each, 503 forwarded (not retried)**; GET with a backend 500 -> **1 hit, 500 forwarded** (500 is not retryable), twice; HEAD with a 503 on a clean circuit -> **3 hits** (retried); GET with a connection reset -> 502 after 3 hits (0.281 s); POST with a connection reset -> **502 after 1 hit** (not retried). A first HEAD run showed 1 hit because the POST and DELETE just before had already put 2 failures on the same backend, so HEAD's own first failure opened the circuit: the circuit, not the method rule, stopped that retry.
- **Total budget:** backend hanging, `upstream_timeout_ms` 5000, total 12000: the GET returned **504 after 12.00 s** with 3 backend hits; attempt 1 timed out at 5001 ms, pause 83 ms, attempt 2 at 10088 ms, pause 187 ms, attempt 3 was limited to the **1721 ms** that were left (`no complete response within 1721ms`).
- **Backend container stopped (not yet noticed by the health checker):** 6 GET + 3 POST to `shop` while `b` was stopped: **9 of 9 returned 200 from `a`** (POST included: the failure was `not_sent`, retried whatever the method); 3 requests took about 2.1 s (the 2 s connect timeout on the unreachable backend) before failing over; the log shows `attempt 1 of 3 failed (not_sent); retrying in 92-96 ms`, `instance shop/b ... is now unhealthy`, `circuit breaker shop/b ...: closed -> open`; afterwards the routable set was `['a']`. After the container was started again, `b` was routable and served 200 when checked 12 s later (not measured more finely).
- **Graceful degradation:** both `shop` backends answering 503: 3 requests answered **503 after about 0.28 s** (3 attempts), then 5 requests **503 in about 1 ms** (all circuits open); gateway `/health` 200 and the unrelated service `solo` 200 during this. After both backends were set back to ok, 4 of 4 requests were 200 (alternating `a`, `b`) about 8 s later.
- **No cascading:** in every scenario with at least one healthy backend of the service, **no client request received a gateway error**; with all backends of one service failing, only that service answered 503 and the gateway and other services stayed available.
- **Shutdown with a request in flight:** `docker stop -t 15` while a request to a hanging backend was in flight (the hang outlasted the 5 s grace period): the log shows `upstream cancelled ... (503)` and `shutdown exceeded the configured grace period of 5s`; the client's connection was closed (curl status 000 after 6.07 s); all components stopped in order (HTTP server, health checker, reverse proxy), exit code 0. The release of `connection_count` for that cancelled request was NOT checked in this run (the database volume was removed first); it is covered by the Phase 6/7 tests.
- **Cleanup:** the demo stack (`docker compose down -v`) and backend containers were removed.

### Phase 7 limitations (real, not achievements)

- Circuit state is in memory and per process: not shared, lost on restart.
- Retried requests are buffered; no hedging; classification only by status code, method and failure kind (no idempotency keys, no `Retry-After`).
- The routable set is still read from PostgreSQL per attempt (no routing cache); a retry costs another routing round trip.
- `connection_count` is not reconciled after a hard kill (unchanged). A request cancelled by shutdown after the grace period gets a closed connection.
- The counters in `ReliabilityManager::Stats` are not exported (metrics are Phase 9).
- One strategy applies to all services; no cache or rate limiting (Phase 8).
- The remote GitHub Actions run has still not been confirmed. **No performance figures were measured** (the demo timings above are single-run observations with 2 s health intervals, not benchmarks).

---

## 9C. Phase 8 — Redis Cache & Distributed Rate Limiting (completed 2026-10-15; commit `feat: implement EdgeFlow phase 8 redis cache and distributed rate limiting`)

**Phase 8 is complete.** Exit condition ("Caching and rate limiting operate correctly under concurrent traffic") was demonstrated in tests (concurrent threads, two limiter instances, two gateway instances) and on real containers (PostgreSQL + Redis + health checker + backends + several EdgeFlow instances). Full design: [architecture.md](architecture.md) section 24.

### What was built

- **`redis` module** (`include/edgeflow/redis/`, `src/redis/`): `RedisClient`, an **own asynchronous RESP2 client on Boost.Asio** (the stack names Redis but no client library; a blocking C client would have held a thread per stalled request). Pool of `redis.pool_size` connections (one command at a time each), bounded queue (1024), `connect_timeout_ms` / `command_timeout_ms` bounds, **fail-fast window** (`retry_interval_ms`) after a failure with one probe command afterwards, stale idle connections replaced before use, AUTH from the environment variable named by `redis.password_env`, `stop()` that fails everything in flight with `Cancelled`. `RespParser` (incremental, bounded, invalid for good after a violation) and `Sha256` (own implementation, checked against published vectors).
- **`ratelimit` module**: `RateLimiter`, a distributed **Token Bucket**. Scopes **ip**, **api_key** (header `rate_limit.api_key.header`, key stored only as SHA-256), **service** (with per-service overrides). One **Lua script executed atomically in Redis**, using **Redis's own clock**, takes a token from every applicable bucket **only if all have one** (a refused request takes nothing from any bucket). `429` with `Retry-After` and `X-RateLimit-Scope`; keys expire when the bucket would be full again (+1 s). `rate_limit.on_redis_failure`: `allow` (default, fail open) or `reject` (503).
- **`cache` module**: `ResponseCache`. Key = SHA-256 of `GET`, service and upstream target. Cacheable requests: GET without body/Authorization/Cookie/Range/If-*/no-cache/no-store. Stored responses: from a backend (carry the proxy's `Via`), status in `cache.cacheable_statuses` (200), no Set-Cookie/Vary/Content-Range/no-store/no-cache/private/max-age=0, body <= `cache.max_entry_bytes`. Lifetime `s-maxage` > `max-age` > `cache.default_ttl_seconds`, capped by `cache.max_ttl_seconds`, enforced by Redis (`SET ... PX`). A successful POST/PUT/PATCH/DELETE removes the entry of the same URL. Hit: `X-Cache: HIT`, `Age`, the request's own `X-Request-Id`; miss: `X-Cache: MISS`. Damaged entries are never served (removed). Not implemented: revalidation/ETag, Expires, Vary, coalescing, HEAD caching.
- **`gateway` module**: `PolicyHandler` wraps the proxy: rate limit -> cache -> proxy (Phase 6/7 retries, circuit breakers and failover untouched). Fully asynchronous (no I/O worker waits for Redis); a refused or cached request never reaches the proxy; the request id is chosen once and handed to the proxy; only `/proxy/...` requests are limited or cached (`/health`, `/services/...` never).
- **Application**: Redis client created when `redis.enabled` and cache or rate limiting is on; **start-up does not fail when Redis is down** (warning, then runs degraded and picks Redis up by itself); shutdown order http-server -> health-checker -> reverse-proxy -> **redis** (stops last).
- **Configuration**: new strict sections `redis.*`, `cache.*`, `rate_limit.*` (all default off; cache and rate limiting require `redis.enabled` and `proxy.enabled`). `config/config.compose.yaml` enables them (small buckets: ip 30 / 10 per s, api_key 10 / 2 per s, service 200 / 100 per s, service `strict` 5 / 1 per s; cache default 10 s; `connect_timeout_ms: 250`). `docker-compose.yml` adds `redis:7` (no persistence, `maxmemory 64mb`, `volatile-ttl`, 128 MB container limit, health-gated start); image tag `edgeflow:phase9`.

### Phase 8 testing

- **733 CTest tests** (630 before; +103): `RespTest` (parser, encoder, SHA-256 vectors), `RedisClientTest` (scripted fake server for failure injection: refused, hung, garbage, closed mid-command, AUTH, stale connection, queue limit, stop; real Redis: set/get/expiry, binary and 1 MiB values, 1600 concurrent INCR, outage/recovery through a TCP relay), `RateLimiterTest` (capacity, continuous refill, retry-after, refill cap, per-IP/API-key/service buckets, overrides, all-or-nothing, key expiry, script-cache loss, **2-3 limiter instances with their own connections sharing one bucket: 600 concurrent requests grant exactly 100**, 20 clients x 3 rounds each granted exactly their own 5, fail-open/fail-closed, outage and recovery), `ResponseCacheTest` (every rule, serialization incl. truncation at every length, Redis storage/expiry/invalidation/corruption, concurrent writers never produce a mixed entry), `PolicyTest` (end-to-end on the real server and proxy with scripted backends and real Redis: miss/hit without backend contact, expiry, bypass rules, invalidation, headers kept, request ids, 400 concurrent hits -> 1 backend request, 16 concurrent cold requests all answered, 429 with Retry-After, **160 concurrent requests -> exactly 40 granted**, **two gateway instances share one bucket and one cache** (30 tokens total -> exactly 28 granted after 2 spent), Redis down/hung/restarted, reject policy, shutdown while Redis hangs), `PolicyPostgresTest` (the whole `Application` over real PostgreSQL + Redis + health checker, start-up with Redis down, shutdown order), `ConfigManagerRedisTest`.
- **Results (all `-Werror`, real PostgreSQL and real Redis, final tree):** GCC Debug **733/733**, GCC Release **733/733**, Clang 18 Debug **733/733**. **Repeat run:** the 446 tests matching `Resp|Sha256|EncodeCommand|RedisClientTest|RateLimiterTest|ResponseCacheTest|PolicyTest|PolicyPostgres|ConfigManagerRedis|Reliab|Circuit|Retry|Backoff|Proxy|Pool|Async|HttpServer|Application|Router` passed **10 consecutive iterations**. **ASan + UBSan** (GCC, `-O1`, `detect_leaks=1`): the same 446, **446/446, 0 reports**. **ThreadSanitizer** (GCC, `-O1 -Wno-tsan`, ASLR off): the same 446, **446/446, 0 warnings**. Without a database and Redis (Docker Release image build): **733 run, 622 pass, 111 skipped, 0 failed** (111 = 58 earlier + 53 new that need PostgreSQL or Redis).
- **What validation found (test code only, no production defect):** a hang when stopping the fake Redis server (closing an acceptor from another thread does not wake a blocking `accept`); the relay's threads depended on another thread closing sockets under them; the first TSan run reported 47 warnings, **every one in the test helpers** (`FakeRedisServer` closing sockets across threads, a test pushing to a vector that async tasks were already reading), **none with a frame in `src/` or `include/`**; the helpers were made cooperative (receive timeouts, stop flags, OS-level `shutdown` only) and the rerun is clean. One test flooded the client with unpaced fire-and-forget stores; it now paces them. The TSan build needs `-Wno-error=array-bounds` for one GCC 13 false positive at `-O1` in test code (a `std::vector<unsigned>` copy); nothing is suppressed at run time.

### Phase 8 runtime validation (Docker Compose, GitHub Codespace; `edgeflow:phase9` 128 MB, `postgres:16`, `redis:7`, `config.compose.yaml`)

Python backend containers `a`, `b` (service `shop`) and `s` (service `strict`) that count requests, plus up to four EdgeFlow containers on the same Redis (`:8080` the Compose service; `:8081` a second instance, same config; `:8083` cache only; `:8084` `on_redis_failure: reject`). Single run on a development codespace; **no performance claim is made**; counts are exact for the capacity (plus whatever refilled in the 0.15-0.5 s each burst took).

- **Start-up log:** `redis ready (redis:6379, 4 connection(s), command timeout 250ms)`, `gateway policy ready: rate limiting on, response cache on`, `EdgeFlow ready`.
- **Cache:** first GET `x-cache=MISS` (backend hit 1), the next two `HIT` with the backend still at **1 hit for 3 client requests**; each reports the id the client sent (`miss-1`, `hit-2`) or its own generated one; another URL is another entry; **the second EdgeFlow instance answered `HIT` from the entry the first stored**; after the 10 s default lifetime (waited 11 s) the next request was a `MISS` (backend asked again, new body) and the one after a `HIT`; `Authorization` and `Cache-Control: no-cache` requests bypassed it (no `X-Cache` header, backend asked); a `POST` to the URL returned 200 and the next GET was a `MISS` with a new body (invalidated); a backend answer with `Set-Cookie` was `MISS` three times in three requests (never stored); Redis held the entry with `PTTL` 9187 ms of 10 s.
- **Rate limiting under concurrent load** (`Cache-Control: no-cache` to keep the cache out of it; every request from one client address):
  - API key `k1` (capacity 10, refill 2/s): **40 requests, 20 threads, 0.33 s -> 10 x 200, 30 x 429** (`X-RateLimit-Scope: api_key`, `Retry-After: 1`, the request id, JSON body); key `k2`: **10 x 200, 30 x 429** (own bucket).
  - Client address (capacity 30, refill 10/s): **100 requests, 30 threads, 0.29 s -> 30 x 200, 70 x 429** (scope `ip`).
  - Service `strict` (own limit capacity 5, refill 1/s): **20 requests -> 5 x 200, 15 x 429** (scope `service`).
  - **Two EdgeFlow instances, one shared bucket:** fresh key `k3` (capacity 10), 40 requests alternating between `:8080` and `:8081`: **10 x 200, 30 x 429** in total (not 20).
  - Redis held `edgeflow:rl:ip:<address>`, `edgeflow:rl:key:<sha256>` (the key is not in the clear), `edgeflow:rl:svc:shop`, `edgeflow:rl:svc:strict`.
- **Cache under concurrency** (rate limiting off on `:8083`): **200 concurrent GETs of one cold URL (50 threads, 0.41 s): 12 MISS and 188 HIT, 12 backend requests** (no coalescing: concurrent misses each reach a backend); the same 200 again: **200 HIT, 0 backend requests, one body**; 400 concurrent requests over 8 URLs: **392 HIT, 8 MISS, 8 backend requests**.
- **Redis stopped (`docker stop`, the address stops answering):** every proxied request still returned 200 and went to a backend (no cache hit); **150 concurrent requests -> 150 x 200, no 429** (the fail-open policy; the bucket is 30); `/health` and the registry API stayed 200; the `reject` instance answered **503 `rate limiting is unavailable`** with its own `/health` 200. Log: `redis redis:6379 is unavailable (timeout: connecting to Redis timed out); commands fail at once for 1000ms and then one is let through to probe it`. **Cost of an unreachable Redis:** with `connect_timeout_ms: 1000` one request per retry interval took **1.00 s** (3 of 3 probe-window requests) and the others 3-44 ms; with `connect_timeout_ms: 250` the probe requests took **0.255 s**; so the Compose config uses 250.
- **Redis started again:** `redis redis:6379 is available again` is logged; MISS then HIT again; **150 concurrent requests -> 31 x 200, 119 x 429**; the `reject` instance served again.
- **Redis restarted (`docker restart`) under load:** 6000 requests, 30 threads, 4.24 s (8 URLs, cache on): **6000 x 200** (5386 HIT, 77 MISS, 537 without cache while Redis was away); log: unavailable (`closed: reading from Redis failed: ...`) then `available again` one second later.
- **Graceful shutdown (`docker stop`):** exit code 0; order `http-server`, `health-checker`, `reverse-proxy`, `redis`, `signal-handlers`.
- **Cleanup:** the stack, the extra EdgeFlow and backend containers and volumes were removed.

### Phase 8 limitations (real, not achievements)

- Redis is a single shared dependency: with it down there is no cache and (with `allow`) no limiting; with `reject` no proxied request is served. An unreachable (not refusing) Redis costs one request per `retry_interval_ms` up to `connect_timeout_ms`. Redis `maxmemory` eviction can drop an entry or a bucket.
- Client identity for limiting is the TCP peer address (`X-Forwarded-For` is not trusted); behind another proxy all clients share one bucket.
- No request coalescing (12 concurrent cold requests -> 12 backend requests in the demonstration), no revalidation, `Vary`, `Expires` or `HEAD` caching; invalidation covers the URL that was written to, not other query strings.
- Single Redis only (no cluster); a script run is one Redis round trip per proxied request, plus one per cacheable GET.
- Counters (`RateLimiter::Stats`, `ResponseCache::Stats`, `PolicyHandler::Stats`, `RedisClient::Stats`) are kept but not exported (metrics are Phase 9).
- PostgreSQL is still read per routing attempt (no routing cache); `connection_count` is still not reconciled after a hard kill; circuit state is per process.
- The remote GitHub Actions run has still not been confirmed. The workflow now also starts a Redis service container (`EDGEFLOW_TEST_REDIS_HOST/PORT`) next to PostgreSQL, so the Redis-gated tests run there instead of being skipped; that change itself has not run on GitHub. **No performance figures were measured.**

---

## 9D. Phase 9 — Observability, Testing & Performance (completed 2026-10-15; commit `feat: implement EdgeFlow phase 9`)

**Phase 9 is complete.** Exit condition ("EdgeFlow has reproducible benchmark and test results") is met: the metrics exist and are verified, the test suite covers every category the phase lists and was run in every configuration below, and the benchmarks are scripted (`bench/`) with a committed result directory. Full design and the metric catalogue: [architecture.md](architecture.md) section 25; how to reproduce the measurements: [bench/README.md](bench/README.md).

### Checklist of the Phase 9 requirements and where each is met

| requirement | implemented as | verified by |
|---|---|---|
| requests/sec | `edgeflow_http_requests_per_second` (window), `edgeflow_http_requests_total{kind}` | `MetricsTest`, `MetricsApplicationTest.ARateOverTheCompletedSecondsAndLatencyQuantilesAppear`, Prometheus run, load test (gauge 1709/1790/1792 vs wrk 1727/1790/1784 in S2) |
| active connections | `edgeflow_http_active_connections`, `..._accepted_total`, `..._connections_rejected_total`, `edgeflow_http_in_flight_requests` | `MetricsApplicationTest.OpenAndRejectedConnectionsAreReported` |
| request count | `edgeflow_http_requests_total`, `edgeflow_http_responses_total{class}`, `edgeflow_proxy_requests_total` | `MetricsFullStackTest.RequestCountsLatencyAndBackendLatencyFollowRealTraffic`, `...ConcurrentTrafficIsCountedExactly` |
| error rate | `edgeflow_http_error_ratio` (window), `edgeflow_http_errors_total` (5xx) | `MetricsFullStackTest.ErrorsShowUpInTheErrorRatio...` (6 of 7 requests), `MetricsTest` |
| p50 / p95 / p99 latency | `edgeflow_http_request_latency_seconds{quantile}` (window) + histogram | `MetricsTest`, `HistogramTest.QuantilesOfAUniformSpread...`, load test |
| backend latency | `edgeflow_backend_latency_seconds` (histogram), `edgeflow_backend_latency_window_seconds{quantile}` | `MetricsFullStackTest` (120 ms backend answers are in the tail), load test |
| cache hit ratio | `edgeflow_cache_hit_ratio`, `edgeflow_cache_lookups_total{result}` | `MetricsFullStackTest.CacheHitRatioFollowsHitsAndMisses` (12 hits / 16), Prometheus run (10 / 20 = 0.5) |
| retry count | `edgeflow_retries_total`, `edgeflow_failovers_total` | `MetricsFullStackTest.RetriesFailoversAndCircuitTripsAreCounted`, Prometheus run |
| circuit-breaker trips | `edgeflow_circuit_breaker_trips_total`, `edgeflow_circuit_breakers{state}` | same, `CircuitBreakerRegistryTest.StateCountsReportEveryStateOnce` |
| rejected requests | `edgeflow_rejected_requests_total{reason}` | `MetricsFullStackTest.RejectedRequestsAreCounted...` (15 of 25 refused), Prometheus run (64 = 49 + 15) |
| test categories (unit, integration, concurrency, routing, database, Redis, failure, end-to-end) | existing suites + 53 new tests; inventory in architecture.md section 25 | all configurations below |
| load test, ~10K req/s target, result from measurements | `bench/load` (wrk) + `bench/micro` (Google Benchmark) + committed `bench/results/phase9-codespace-4cpu/` | the run documented below |
| reproducible benchmark and test results | scripts, pinned image, recorded environment, 3 runs per scenario | `bench/README.md`, `bench/results/` |

### What was built

- **`observability` module**: `Histogram` (78 fixed log-linear buckets, four per octave from 100 us to about 62 s, lock-free), `WindowedStats` (per-second slots, requests/sec, error ratio and quantiles over `metrics.window_seconds`; injectable clock), `MetricsWriter` (Prometheus text format 0.0.4, label escaping, `NaN` for undefined values), `Metrics` (request-path recording + collectors), `MetricsHandler` (the outermost handler: serves `GET <metrics.path>`, times everything else).
- **Hooks**: `ProxyHandler::attachMetrics` (time of every backend exchange that answered; attempts counted by outcome), `CircuitBreakerRegistry::stateCounts()`, one collector per component in `Application::registerMetricsCollectors()` (HTTP server, proxy and upstream pool, reliability, cache, rate limiter, Redis). Components that are off contribute no families.
- **Configuration**: `metrics.enabled` (default **off**), `metrics.path` (`/metrics`; reserved prefixes refused), `metrics.window_seconds` (10, 1-300). The endpoint is unauthenticated: trusted networks only.
- **Benchmarks** (`bench/`): optional Google Benchmark target (`-DEDGEFLOW_BUILD_BENCHMARKS=ON`, v1.8.5 via FetchContent, off by default so the image and the default build are unchanged), and a wrk load test (`bench/load/run.sh`: Compose stack with PostgreSQL, Redis, three EdgeFlow backends and the gateway; four scenarios; raw outputs and `/metrics` snapshots saved; `summarize.py`). The Dockerfile now copies `bench/` so that the image build also checks that `bench/load/gateway.yaml` is a valid configuration.
- **CI**: unchanged beyond Phase 8's Redis service (the new tests run there; not confirmed remotely).

### Phase 9 testing

- **786 CTest tests** (733 before; +53): `HistogramTest`, `LatencyBucketsTest`, `WindowedStatsTest` (fake clock, no sleeps, plus a real-clock concurrent writer/reader test), `MetricsWriterTest` (format, escaping, cumulative buckets validated by a strict exposition parser in `tests/support/MetricsTestSupport.hpp`), `MetricsTest`, `MetricsHandlerTest`, `MetricsApplicationTest` (real server: counts, rate, quantiles, connection limit, custom path, concurrent scraping), `MetricsConfigTest` (incl. the shipped and benchmark configurations), `MetricsFullStackTest` (the whole Application over real PostgreSQL + Redis: each metric against the behaviour that provokes it, Redis outage, 480 concurrent requests counted exactly), `CircuitBreakerRegistryTest.StateCountsReportEveryStateOnce`.
- **Results on the final tree (all `-Werror`, real PostgreSQL and Redis):** GCC Debug **786/786**, GCC Release **786/786**, Clang 18 Debug **786/786**. **Repeat run:** the 499 tests matching `Resp|Sha256|EncodeCommand|RedisClientTest|RateLimiterTest|ResponseCacheTest|PolicyTest|PolicyPostgres|ConfigManagerRedis|Metrics|Histogram|LatencyBuckets|WindowedStats|CircuitBreakerRegistry|Reliab|Circuit|Retry|Backoff|Proxy|Pool|Async|HttpServer|Application|Router` passed **10 consecutive iterations**. **ASan + UBSan** (`detect_leaks=1`): the same 499, **499/499, 0 reports**. **ThreadSanitizer** (ASLR off, `-Wno-tsan -Wno-error=array-bounds`): the same 499, **499/499, 0 warnings**. Docker Release image build (no database, no Redis): **786 run, 667 pass, 119 skipped, 0 failed**, 128 MB.
- **What validation found:** (1) the first image build failed because the new config test read `bench/load/gateway.yaml`, which the Dockerfile did not copy (fixed by copying `bench/`, not by weakening the test); (2) a sequence-point error (`i++` and `i` in one expression) in a micro-benchmark, caught by `-Werror`; (3) two arithmetic mistakes in my own new tests (request kinds counted wrongly, a test depending on the circuit not re-tripping within its recovery interval, now expressed as "tripped at least once and still open or probing"). No production defect was found by the sanitizers.

### Phase 9 runtime validation (Docker Compose + a real Prometheus server, GitHub Codespace)

`edgeflow:phase9` (128 MB) with `config.compose.yaml` (metrics on), `postgres:16`, `redis:7`, Python backends `a`, `b` (service `shop`) and `s` (`strict`), and **`prom/prometheus` scraping `edgeflow:8080/metrics` every 2 s** (`/api/v1/targets`: health `up`, `lastError` empty: the exposition is accepted by a real Prometheus). Single run; counts are exact, nothing here is a performance claim.

- **Traffic and what the metrics said** (162 proxied requests were sent in four bursts, plus 12 local calls for registration): 20 requests over 10 URLs twice (10 misses, 10 hits) -> `edgeflow_cache_lookups_total` hit 10, miss 10, `edgeflow_cache_hit_ratio` 0.5; 12 sequential requests with backend `a` answering 503 (all 12 clients got 200) -> `edgeflow_retries_total` 3, `edgeflow_failovers_total` 3, `edgeflow_circuit_breaker_trips_total` 1, `edgeflow_backend_attempts_total{outcome="retryable_status"}` 3; a burst of 80 concurrent requests -> 31 x 200, 49 x 429; 30 requests while Redis was stopped for 4 s -> 30 x 200, `edgeflow_redis_failures_total` 30, `edgeflow_redis_up` back to 1 after the restart; a burst of 20 against the `strict` service -> 5 x 200, 15 x 429.
- **Totals Prometheus returned** match what the clients saw: `edgeflow_http_requests_total` proxy 162 and local 12; `edgeflow_http_responses_total` 2xx 110, 4xx 64, 5xx 0; `edgeflow_http_errors_total` 0; `edgeflow_rejected_requests_total{reason="rate_limited"}` **64** (= 49 + 15); backend answers 91 (`success` 88 + `retryable_status` 3 = `edgeflow_backend_latency_seconds_count` 91); `histogram_quantile(0.99, ...request_duration_seconds_bucket)` 0.316 s and the backend median 0.033 s (Python backends); `edgeflow_circuit_breakers` closed 3 at the end (the failing backend's circuit had recovered).
- **Windowed gauges** read after a quiet period: `edgeflow_http_requests_per_second` 0.1, p50 0.05 ms (the idle scrape traffic only), as designed (they follow the last 10 s).
- **Shutdown:** `docker stop` exit code 0; order `http-server`, `health-checker`, `reverse-proxy`, `redis`, `signal-handlers`; no container left after cleanup.

### Phase 9 performance measurements (the first ever; `bench/results/phase9-codespace-4cpu/`)

Environment: GitHub Codespace, **4 vCPU AMD EPYC 7763, 15 GiB**, Linux 6.8, Docker 29.8, wrk 4.1.0 (2 threads, 64 connections, 5 s warm-up, 3 runs of 20 s), commit `ab81715` plus the Phase 9 changes, image `edgeflow:phase9`. The load generator, the gateway (2 HTTP worker threads, 2 proxy I/O threads, 8 PostgreSQL connections, 4 Redis connections), three EdgeFlow backends, PostgreSQL (`fsync=off`) and Redis **share those 4 vCPUs**. Every scenario ran with **0 non-2xx responses and 0 socket errors**.

| scenario | requests/s per run | median req/s | wrk p50 | wrk p90 | wrk p99 |
|---|---|---|---|---|---|
| S0 backend direct (`GET /`) | 35554, 36253, 36611 | **36253** | 1.53 ms | 2.83 ms | 4.88 ms |
| S1 gateway local (`GET /health`) | 35635, 36647, 36450 | **36450** | 1.52 ms | 2.81 ms | 4.93 ms |
| S2 proxy, cache bypassed (rate limiter + PostgreSQL routing + proxy + backend) | 1727, 1790, 1784 | **1784** | 34.6 ms | 42.1 ms | 54.0 ms |
| S3 proxy, cache hit (rate limiter + Redis cache lookup) | 10911, 11243, 11438 | **11243** | 5.41 ms | 7.26 ms | 10.9 ms |

- **Against the ~10K requests/s target:** reached only for cached responses (S3, 10.9K-11.4K per run); a proxied request that reaches a backend ran at about **1.8K requests/s** (S2). No claim of 10K for uncached proxying can be made from these measurements.
- The gateway's own `/metrics` figures agree with wrk's (S2 gauge 1709, 1790, 1792 req/s; p50 about 34-35 ms; backend p50 about 15 ms, backend p99 24-27 ms; S3 gauge 10787-11410 req/s, hit ratio 1.0). The three runs of a scenario spread by 3.5 % (S2) and 4.7 % (S3) of the median.
- **Observations for Phase 10 (nothing was optimised in Phase 9):** the 20x gap between S1/S0 and S2 is the cost of the uncached path (three PostgreSQL round trips per request plus a Redis script plus the upstream exchange); the 15 ms "backend latency" of S2, for a backend that answers in about 1.5 ms when called directly (S0), suggests that the time is spent inside the gateway (the exchange waiting for the proxy's two I/O threads or the registry call pool) rather than at the backend; this is a hypothesis, not a measurement, and is for the Phase 10 profiling to confirm.
- **Micro-benchmarks** (Release, same machine, `micro-benchmarks.txt`): round robin 0.2 us (3 instances), 0.57 us (10), **10.6 us (100)**; least connections 5 ns / 46 ns / 0.47 us; weighted 0.33 / 1.4 / **62 us**; consistent hashing 0.25 / 0.72 / 11.9 us (selection time grows with the instance count because each call orders the set); `mapProxyTarget` 51 ns; `makeUpstreamRequest` 554 ns; `makeClientResponse` 0.4 us (256 B) / 5.9 us (16 KiB); circuit breaker closed path 18 ns; retry decision 6 ns; SHA-256 0.4 us (32 B); cache entry serialize / deserialize 0.28 / 0.21 us (1 KiB); rate-limiter bucket plan 0.57 us; **metrics: histogram record 8.4 ns, windowed record 54 ns, `recordRequest` 58 ns (single thread; about 4M/s over 4 threads), `/metrics` render 32 us after 10000 requests**.

### Phase 9 limitations (real, not achievements)

- The ~10K requests/s target is met only on the cache-hit path, on this 4 vCPU shared machine; uncached proxying measured 1.8K requests/s. Results from another machine will differ; the load test is closed-loop and under-reports the latency of a saturated server.
- The windowed gauges are per process; quantiles have the bucket resolution (within about 19 %); no per-service/per-backend labels (cardinality); no tracing beyond request ids in the logs; `/metrics` is unauthenticated.
- The load test compares scenarios on one machine; it is not a capacity plan. No availability figure (the ~99.5 % target) and no 10-instance or 100K-request distribution run were made in Phase 9.
- The remote GitHub Actions run has still not been confirmed.

---

## 9E. Phase 10 — Optimization, Production Hardening & Release (completed 2026-10-27; commit `feat: complete EdgeFlow phase 10 production hardening and release`)

Everything below is measured; the details, raw files and commands are in `docs/PERFORMANCE.md`, `docs/SECURITY.md`, `docs/DEMO.md`, `docs/FLOWCHART.md`, `bench/README.md` and `bench/results/phase10-*`. All runs: a GitHub Codespace, 4 vCPU AMD EPYC 7763, 15 GiB, with the load generator, gateway, backends, PostgreSQL and Redis sharing the CPUs.

### Profiling findings (baseline = the Phase 9 gateway)

- The gateway used ~0.7 core while the machine was ~98 % busy; **PostgreSQL used 180 %**. `pg_stat_statements`: the `UPDATE service_instances SET connection_count` run twice per request was **86.7 %** of PostgreSQL execution time. `strace -c`: heavy `futex` (thread hand-offs). Upstream pool reuse 99.92 %. Heap peak 1.28 MB, no growth (not changed). Redis commands 1.4-41 µs (not changed). A first profile that used published ports showed `docker-proxy` at 72 % of a CPU: a measurement artifact, removed by addressing containers by IP.

### Optimizations (same-run A/B of 3 x 20 s on the uncached proxied path S2)

1. `proxy.connection_counting: always|auto|never` (code default `always`, the shipped configs use `auto`): with `auto`, `connection_count` is written only for `least_connections`. 1,929 -> 4,732 req/s.
2. `routing.snapshot_ttl_ms` (default 0 = off; the shipped configs use 250): a bounded-staleness per-service routable snapshot in `Router`; empty results and errors are never kept; refused together with `least_connections`. 4,755 -> 8,269 req/s.
3. Inline attempts: with no counting and a fresh snapshot the attempt runs on the I/O thread instead of hopping to the registry call pool. 8,258 -> 11,637 req/s.
4. `TCP_NODELAY`: no difference, reverted.

Total S2 **1,935 -> 11,637 req/s (6.0x)**, p50 32.35 -> 5.36 ms, p99 45.98 -> 9.44 ms.

### Final benchmark (10 backends, `bench/final/run.sh`, wrk 64 connections)

round_robin 10,167 req/s (p95 8.87 ms), weighted 10,417 (p95 8.71 ms), consistent_hashing 10,722 (p95 20.56 ms; one client address is sticky to one backend), **least_connections 1,808 req/s, p95 45.8 ms (targets missed; it needs the live counter in PostgreSQL)**. 136K-215K requests per strategy, 0 failed. Failure injection (backends killed, hung, added, removed under load): **100.000 % availability, 0 failed of 546,773 and of 634,001 requests**. A hung backend costs latency, set by `proxy.upstream_timeout_ms`: with 10 s p95 was 6.28 s, with 1 s p95 10.2 ms.

### Hardening

- `admin.token_env` (opt-in): bearer token for `/services` (constant-time comparison, `401` otherwise; a named but empty variable stops the start; an unset one logs a warning at every start). `network::loggableTarget()` removes the query string from every logged target. Compose publishes the gateway on `127.0.0.1` only, with `pids_limit`, `mem_limit` and a tmpfs `/tmp`; the image binary is stripped and the build uses a BuildKit cache mount.
- **Defect found by the failure test and fixed:** with the PostgreSQL host unreachable (packets dropped) every request waited out the 5 s connect timeout while pinning a worker thread. `PgPool` now remembers a slow connect failure and refuses further attempts for one second, one thread probing at the end of it (`PgPoolUnreachableTest`; measured: 50 concurrent requests 503 in 5.04 s, the next burst 50/50 in 0.15 s).
- Verified on a running gateway: SIGTERM with an in-flight request (completed with 200, exit code 0 after 2.01 s, shutdown order http-server -> health-checker -> reverse-proxy -> redis -> signal-handlers), Redis down and up, PostgreSQL down and up without restarts, no descriptor leak.

### Documentation delivered

`docs/FLOWCHART.md` (the architecture flowchart with every component where it really sits), `docs/PERFORMANCE.md`, `docs/SECURITY.md`, `docs/DEMO.md` (a terminal demonstration with real captured output, reproducible with `demo/demo.sh`), `bench/README.md`, a new README, `architecture.md` section 26, `techstack.md`.

### Test-suite findings during Phase 10 (fixed in the tests; no product defect)

- `closedPort()` (test helper) released the port it found, so a rare run gave the same number to the server the test started next (`ReliabilityTest.APostThatNeverReachedABackendIsRetriedWithItsBody`). It now keeps the socket bound (not listening) for the next 64 calls; the first version kept every socket forever and a process that repeated the tests ran out of descriptors, which the 100x repeat run found.
- `InlineAttemptTest.ConcurrentClientsAreAllAnsweredAndNothingIsCounted` asserted at most 3 registry reads for 8 concurrent clients, but clients arriving together on a cold snapshot may each read once (there is no single-flight); the bound is now the client count (8) against 800 requests without the snapshot.

### Phase 10 limitations (real, not achievements)

- One machine type, closed-loop load, everything sharing 4 vCPUs; the numbers compare steps with each other, not a dedicated server's capacity. `least_connections` misses the throughput and p95 targets. The availability figure is for backend failures in one gateway process, not a deployment's uptime.
- No TLS, `/metrics` unauthenticated, the registry API open unless `admin.token_env` is set, the rate limiter trusts the TCP peer address only; no fuzzing campaign; no vulnerability scan of the images (see `docs/SECURITY.md`).
- The Mermaid diagrams in `docs/FLOWCHART.md` were written but not rendered in a browser (the Chrome extension was not connected); the text flowchart is the reference.
- The remote GitHub Actions run has still not been confirmed. The tree was validated in the Codespace only; the Codespace was deleted at the end of Phase 10.

---

## 10. Current Architecture (end of Phase 10)

```text
Client
   |
   v
HTTP Server  (Boost.Asio/Beast, keep-alive, timeouts, graceful drain)        Phase 2
   |
   v
Metrics  (times every request, serves GET /metrics; collectors read all counters)  Phase 9
   |   /proxy/{service}/...
   v
Policy: Token Bucket rate limit -> response cache  (Redis = shared state)    Phase 8
   |   allowed, cache miss / bypass
   v
Reverse Proxy  (async boundary, header rules, request ids, timeouts)         Phase 6
   |  ^  Reliability: total budget, retry + backoff, per-instance circuit breaker,   Phase 7
   |  |               failover filter, graceful degradation
   |        |
   |        +--> UpstreamPool / UpstreamClient (keep-alive connections to backends)
   v
Router   (one snapshot per decision; one strategy)                           Phase 5
   |   Round Robin | Least Connections | Weighted | Consistent Hashing
   v
Routable Instance View   (ACTIVE AND HEALTHY, lookupRoutable)                Phase 4
   |        ^
   |        +-- HealthChecker: TCP/HTTP probes -> state machine -> updateHealth   Phase 4
   v
Service Discovery / Registry  (PostgreSQL = source of truth)                 Phase 3

Selected backend instance <-> EdgeFlow (forward request, return response)    Phase 6
```

Module layout (`include/edgeflow/<module>/`, `src/<module>/`, `tests/<module>/`): `config` (strict YAML), `core` (Application, ShutdownCoordinator, SignalHandler, CommandLine), `logging`, `network` (TcpServer, HttpServer, HttpConnection, ConnectionTracker, Http helpers, RequestHandler/LocalRequestHandler, RegistryRequestHandler, HealthProbe), `storage` (Postgres wrapper, Migrator), `discovery` (ServiceInstance, ServiceRegistry, PostgresServiceRegistry, Validation, HealthState, Prober, NameResolver, HealthChecker), `routing` (LoadBalancer, Strategies, Router), `proxy` (ProxyHandler, ProxyHeaders, UpstreamPool, UpstreamClient), `reliability` (ReliabilityManager, RetryPolicy, BackoffPolicy, CircuitBreaker, CircuitBreakerRegistry, AttemptLedger), `redis` (RedisClient, RespParser, Sha256), `ratelimit` (RateLimiter), `cache` (ResponseCache), `gateway` (PolicyHandler), `observability` (Metrics, Histogram, WindowedStats, MetricsWriter, MetricsHandler). `bench/` holds the benchmarks (not part of the default build). SQL lives in `db/migrations/`.

Startup order in `Application::initialize()`: signal handlers -> (if `database.enabled`) pool, migrations, registry, `Router`, registry API wrapped around the local handler -> (if `proxy.enabled`) `ProxyHandler` as the outermost handler -> HTTP server -> (if `health_check.enabled`) health checker. Shutdown runs in reverse: HTTP server (drain, then force-close at the grace period), health checker, reverse proxy (`reverse-proxy` component), signal handlers.

Configuration sections (`config/config.yaml`; every key documented there and in the README): `application`, `server`, `database` (off), `health_check` (off), `routing` (`round_robin`), `proxy` (off), `reliability` (off), `redis` (off), `cache` (off), `rate_limit` (off), `metrics` (off), `shutdown`. Unknown keys and out-of-range values are rejected. `config/config.compose.yaml` enables database, HTTP health checks and the proxy for Compose.

Phase 8 in `Application::initialize()` (inside the `proxy.enabled` block): when `redis.enabled` and cache or rate limiting is on, the `RedisClient` is created and registered for shutdown FIRST (so it stops LAST), a `PING` logs whether Redis answers (start-up never fails because of Redis), the proxy is built and registered, and `PolicyHandler(proxy, limiter, cache)` becomes the outermost handler. Shutdown order: http-server, health-checker, reverse-proxy, redis, signal-handlers.

Phase 9 in `Application::initialize()`: when `metrics.enabled` (and no custom request handler) `Metrics` is created first and attached to the proxy (`attachMetrics`), the collectors are registered once the handler chain exists (`registerMetricsCollectors`), and `MetricsHandler` wraps the whole chain as the outermost handler.

HTTP surface: `GET /`, `GET /health`, `POST /echo` (Phase 2); `GET /services`, `POST|GET /services/{svc}/instances`, `GET|PATCH|DELETE /services/{svc}/instances/{id}` (Phase 3); `GET /services/{svc}/routable` (Phase 4); `GET /services/{svc}/route[?key=K]` (Phase 5, decision only); `/proxy/{service}/...` any method (Phase 6; Phase 7 adds retries, circuit breaking and failover behind it, no new routes); `GET <metrics.path>` (Phase 9, when enabled).

Threading model: HTTP I/O on `server.worker_threads` Asio threads (the proxy answers asynchronously through `handleAsync`, so a backend never blocks them); the proxy has its own I/O threads (`proxy.io_threads`) and a small pool for blocking registry calls; health checker = one probe/timer thread + one database thread; NameResolver lookups on detached per-host threads (max 16).

## 11. What Is NOT Implemented

```text
Phases 1-10: COMPLETE (sections 5-9E).
```

Deliberately absent (see `docs/SECURITY.md` and the limitations above): TLS, tracing beyond request ids, circuit state shared between gateway processes, a trusted-proxy setting for `X-Forwarded-For`, `Expect: 100-continue`, streamed (rather than buffered) proxying, a `least_connections` counter outside PostgreSQL.

## 12. Next Step

None: the roadmap in `Phases.md` is finished. Candidate follow-ups would be new scope, not decisions: move the `least_connections` counter out of PostgreSQL, add TLS termination, authenticate `/metrics`, a single-flight snapshot refresh, confirm the GitHub Actions run, render and check the Mermaid diagrams.

---

## 13. Environment and How To Resume Work (local development facts)

**Phase 10 note:** all builds, sanitizers, benchmarks and image builds ran in a GitHub Codespace, never in the local Docker. The bullets below that describe local Docker builds are history from earlier phases.

- The Windows host has **no C++20 compiler or CMake**; all builds and tests run in Linux containers (Docker Desktop must be running; it may need starting: `C:\Program Files\Docker\Docker\Docker Desktop.exe`).
- The Docker VM has **about 3.7 GB RAM**. Always build with `-j2` (the Dockerfile defaults to `BUILD_JOBS=2`); unbounded parallelism exhausted memory and hung the engine once.
- Local helper images/volumes created during development (not in the repo, recreate if missing): `edgeflow:dev` -> `edgeflow:dev3` (Ubuntu 24.04, GCC 13, clang, cmake, ninja, git, libboost-dev, libpq-dev, postgresql-client); `edgeflow:valtools` (adds curl, netcat, python3); build volumes `ef_build` (GCC Debug), `ef_build_clang`, `ef_build_rel` (GCC Release); network `ef-net`.
- Typical full validation: start a throwaway PostgreSQL (`postgres:16`, user/db `edgeflow`) on a Docker network, run the build container on that network with `EDGEFLOW_TEST_DB_HOST=<container>` plus optional `EDGEFLOW_TEST_DB_PORT/NAME/USER/PASSWORD`, then `cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DEDGEFLOW_WARNINGS_AS_ERRORS=ON && cmake --build /build -j2 && ctest --test-dir /build --output-on-failure`. Without `EDGEFLOW_TEST_DB_HOST` the database tests are skipped with a reason.
- `docker build -t edgeflow:phase10 .` builds Release with `-Werror` and runs the tests (database tests skipped); `docker compose up -d --build` starts PostgreSQL + EdgeFlow with the registry, health checking, routing, the reverse proxy and reliability on (`EDGEFLOW_DB_PASSWORD` overrides the development-only default).
- Tooling quirks: large bash heredocs can fail in this environment (write big files with an editor tool or a script file); Git Bash rewrites `/tmp/...` arguments passed to `docker exec` (set `MSYS_NO_PATHCONV=1`); poll result files that a background job writes only after the job truncated them (use a unique file per run), otherwise a stale file can be mistaken for a fresh result.
- Do not leave test infrastructure running: remove throwaway containers, networks and volumes after a validation run.

## 14. Final Target — what was achieved (measured; see section 9E and `docs/PERFORMANCE.md`)

The original resume-bullet targets and what the measurements support (one 4 vCPU shared machine; use these numbers, not the targets):

| target | measured |
|---|---|
| ~10 service instances | 10 backends, with instances added, removed, killed and hung under load |
| ~100K+ requests | 136K-215K per strategy; 547K and 634K in the failure runs |
| ~99.5 % availability under failures | 100.000 % (0 failed of 546,773 and of 634,001) in one gateway process |
| ~10K requests/sec | 10.2K-10.7K for round_robin, weighted and consistent_hashing; 1.8K for least_connections |
| p95 ~20 ms | 8.7-8.9 ms (round_robin, weighted), 20.6 ms (consistent_hashing), 45.8 ms (least_connections) |
| profiling-driven improvement | uncached proxied path 1,935 -> 11,637 req/s (6.0x), p50 32.35 -> 5.36 ms |

## 15. Open Items

- Remote GitHub Actions run: never confirmed for any phase.
- `least_connections` is the slow strategy (1.8K req/s, p95 45.8 ms) because its counter lives in PostgreSQL.
- Known design limitations: circuit state per process and in memory; `/metrics` unauthenticated; no TLS; Redis is a single shared dependency and the limiter identifies clients by TCP peer address; routing snapshots can be stale up to `snapshot_ttl_ms`; `Expect: 100-continue` unsupported; buffered (not streamed) proxying; `connection_count` not reconciled after a hard kill; the Mermaid diagrams were not rendered.
