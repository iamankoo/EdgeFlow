# EdgeFlow — Current Project Summary

**This file is the resume point for the project.** Tomorrow's session: read this file first, then [Phases.md](Phases.md) (authoritative roadmap), then inspect `git log` and confirm the state below before doing anything. After every completed phase, update this file.


**Checkpoint (UPDATED 2026-10-06): Phases 1-6 are COMPLETE. Phase 6 (reverse proxy) is committed as ONE commit, `feat: implement EdgeFlow phase 6 reverse proxy` (its hash is in `git log`; this file is part of that commit and cannot contain its own hash). PHASE 7 HAS NOT STARTED.** Section 9A describes Phase 6; sections 10-12 are updated to the end of Phase 6.

---

## 0. RESUME POINT — Phase 6 complete, Phase 7 not started

Phase 7 (Reliability Engineering: retries, backoff, circuit breaker, failover, graceful degradation) must only start after explicit authorization from the owner. Before anything else: read this file, then `Phases.md`, run `git log --oneline -8` / `git status`, and confirm `HEAD == origin/main` with a clean tree.

### Final Phase 6 validation (all measured; remote CI NOT confirmed)

- **Docker Release image build** (`docker build -t edgeflow:phase6 .`, Release `-Werror`, `BUILD_JOBS=2`): succeeded; the in-image CTest run (no database) reported 100% passed, 0 failed out of 507 (database tests skipped, 55).
- **507 CTest tests** (379 before Phase 6): GCC Debug, GCC Release and Clang 18 Debug all 507/507 with PostgreSQL, all `-Werror`; without a database 452 pass, 55 skipped, 0 failed. Proxy/pool/async/HTTP-server/Postgres-proxy suites passed 12 consecutive runs. ThreadSanitizer: 0 warnings on 124 proxy/pool/async/HTTP-server/Application tests. ASan+UBSan: 124 proxy/pool/async/HTTP-server/Application tests, 0 reports.
- **Docker Compose runtime demonstration** (real PostgreSQL, real health checker, Python backend containers): results in section 9A.
- Two bugs found by validation and fixed before the commit: a test-helper lifetime bug (TSan) and the connection-count release ordering (count is released before the response is handed back).

### Git identity (resolved 2026-10-06)

The owner's Phase 6 instruction fixes author AND committer to **`Aniket Raj <aniketraj00384@gmail.com>`** (GitHub account `iamankoo`). Phases 1-5 and the summary commit were authored as `iamankoo <aniketraj00384@gmail.com>`; they are not rewritten. Use `Aniket Raj` for all future commits (repository-local git config), still with no Claude/Anthropic/OpenAI/ChatGPT/AI attribution and no `Co-authored-by` or any other trailer.

### Environment notes (carried forward)

- Docker Desktop must be started (`C:\Program Files\Docker\Docker\Docker Desktop.exe`). Build image: `edgeflow:dev3` (compilers, libpq-dev); `edgeflow:valtools` has curl and python3 but not libpq-dev. Mount the repo read-only at `/src`, a named volume at the build dir (`ef_build`, `ef_build_rel`, `ef_build_clang`, `ef_build_asan`), network `ef-net`, `-j2` (the Docker VM has about 3.5 GB).
- Throwaway PostgreSQL: `docker run -d --name ef-pg --network ef-net -e POSTGRES_USER=edgeflow -e POSTGRES_DB=edgeflow -e POSTGRES_PASSWORD=efpw postgres:16`; tests need `EDGEFLOW_TEST_DB_HOST=ef-pg EDGEFLOW_TEST_DB_PASSWORD=efpw`. Remove it afterwards.
- GCC TSan needs `-Wno-tsan` and ASLR disabled (`setarch x86_64 -R`, `--security-opt seccomp=unconfined`).
- On this machine plain `docker stop <container started with docker run>` killed processes after about 1.6 s (a container that ignores SIGTERM is also killed at about 1.6 s), so use `docker stop -t N` when testing a graceful shutdown by hand; Compose sets `stop_grace_period: 10s`.
- Other containers on the machine (supabase, backend-*, etc.) belong to the owner: never touch them.
- Tooling quirks: large bash heredocs may fail (write files with the editor tool); Git Bash needs `MSYS_NO_PATHCONV=1` for `docker exec`/mount paths; the host curl is a Windows binary (use `cygpath -m` for file arguments).

---

(Previous checkpoint, kept for history: end of 2026-10-03, Phases 1-5 complete.)

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
| Logging | spdlog v1.14.1 |
| Configuration | YAML via yaml-cpp 0.8.0 (strict) |
| JSON | nlohmann/json v3.11.3 |
| Testing | GoogleTest v1.15.2 + CTest |
| Containers / CI | Docker (multi-stage, non-root), Docker Compose, GitHub Actions (with a PostgreSQL service container) |

**In the locked stack but NOT yet present in the code:** Redis (Phase 8), Google Benchmark / Prometheus-compatible metrics / wrk / profiling tools (Phases 9-10).

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
| 7 | Reliability Engineering | NOT STARTED | |
| 8 | Redis Cache & Distributed Rate Limiting | NOT STARTED | |
| 9 | Observability, Testing & Performance | NOT STARTED | |
| 10 | Optimization, Production Hardening & Release | NOT STARTED | |

Hashes were checked against `git log` at this checkpoint. History on `main` (oldest first): `5e3be65` docs initialisation (documentation only), then the five phase commits above, then the summary checkpoint commit `docs: update project summary` (changes only that file), then the Phase 6 commit.

Test counts per phase (all with `-Werror`; "skipped" means tests needing a real PostgreSQL when none is configured, reported by CTest as skipped, never silently passed):

| After phase | Total tests | Notes |
|-------------|-------------|-------|
| 1 | 56 | |
| 2 | 127 | |
| 3 | 192 | 30 need PostgreSQL |
| 4 | 272 | 42 need PostgreSQL |
| 5 | 379 | 49 need PostgreSQL: 330 pass + 49 skipped without a database; 379/379 pass with one |
| 6 | **507** | **55 need PostgreSQL**: 452 pass + 55 skipped without a database; 507/507 pass with one |

## 4. Standing Rules

1. **Phases are authoritative** ([Phases.md](Phases.md)). Do not skip phases or implement later-phase features early. Before implementing a phase, cross-check its prompt line by line against `Phases.md` and report any mismatch instead of silently resolving it.
2. **Git identity is locked** (section 2).
3. **Metrics honesty:** performance figures are targets until measured by reproducible tests. No throughput, latency or availability number has been measured so far (see "Final Target" at the end).
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

---

## 10. Current Architecture (end of Phase 6)

```text
Client
   |
   v
HTTP Server  (Boost.Asio/Beast, keep-alive, timeouts, graceful drain)        Phase 2
   |   /proxy/{service}/...
   v
Reverse Proxy  (async boundary, header rules, request ids, timeouts)         Phase 6
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

Module layout (`include/edgeflow/<module>/`, `src/<module>/`, `tests/<module>/`): `config` (strict YAML), `core` (Application, ShutdownCoordinator, SignalHandler, CommandLine), `logging`, `network` (TcpServer, HttpServer, HttpConnection, ConnectionTracker, Http helpers, RequestHandler/LocalRequestHandler, RegistryRequestHandler, HealthProbe), `storage` (Postgres wrapper, Migrator), `discovery` (ServiceInstance, ServiceRegistry, PostgresServiceRegistry, Validation, HealthState, Prober, NameResolver, HealthChecker), `routing` (LoadBalancer, Strategies, Router), `proxy` (ProxyHandler, ProxyHeaders, UpstreamPool, UpstreamClient). SQL lives in `db/migrations/`.

Startup order in `Application::initialize()`: signal handlers -> (if `database.enabled`) pool, migrations, registry, `Router`, registry API wrapped around the local handler -> (if `proxy.enabled`) `ProxyHandler` as the outermost handler -> HTTP server -> (if `health_check.enabled`) health checker. Shutdown runs in reverse: HTTP server (drain, then force-close at the grace period), health checker, reverse proxy (`reverse-proxy` component), signal handlers.

Configuration sections (`config/config.yaml`; every key documented there and in the README): `application`, `server`, `database` (off), `health_check` (off), `routing` (`round_robin`), `proxy` (off), `shutdown`. Unknown keys and out-of-range values are rejected. `config/config.compose.yaml` enables database, HTTP health checks and the proxy for Compose.

HTTP surface: `GET /`, `GET /health`, `POST /echo` (Phase 2); `GET /services`, `POST|GET /services/{svc}/instances`, `GET|PATCH|DELETE /services/{svc}/instances/{id}` (Phase 3); `GET /services/{svc}/routable` (Phase 4); `GET /services/{svc}/route[?key=K]` (Phase 5, decision only); `/proxy/{service}/...` any method (Phase 6).

Threading model: HTTP I/O on `server.worker_threads` Asio threads (the proxy answers asynchronously through `handleAsync`, so a backend never blocks them); the proxy has its own I/O threads (`proxy.io_threads`) and a small pool for blocking registry calls; health checker = one probe/timer thread + one database thread; NameResolver lookups on detached per-host threads (max 16).

## 11. What Is NOT Implemented Yet

```text
Phase 7 (NEXT, NOT STARTED):
- retry
- exponential backoff
- circuit breaker
- failover
- graceful degradation

Phase 8:
- Redis
- response caching
- rate limiting (Token Bucket, per IP / API key / route)

Phase 9:
- complete observability (Prometheus-compatible metrics, tracing)
- reproducible performance benchmarks (wrk / custom load tests)

Phase 10:
- profiling (perf, Valgrind, flamegraphs)
- optimization
- production hardening
- final release
```

Nothing above exists even partially in the repository. Phase 6 deliberately has no retry beyond the single stale-pooled-connection safeguard, no circuit breaker, no failover, no cache, no routing cache, no metrics and no TLS.

## 12. Next Step — Phase 7 (NOT STARTED; needs explicit owner authorization)

Phase 7 is **Reliability Engineering** (see `Phases.md` for the exact list). Before starting: read this file and `Phases.md`, check `git log`/`git status`, confirm Phase 6 is committed and `HEAD == origin/main`, cross-check the Phase 7 prompt against `Phases.md`, and wait for authorization.

Facts worth knowing when designing Phase 7 (observations, not decisions): the proxy has one attempt per request except the stale-pooled-connection safeguard (which must not be doubled up by a retry layer: a request whose first byte was sent must only be retried if it is safe to do so); `Router::route` returns one instance (failover needs "next candidate" selection); gateway errors are mapped in `ProxyHandler` (`statusFor`), a natural place for breaker outcomes; `connection_count` is released before the response is handed back and must stay so; the routable set is still a PostgreSQL read per decision (no routing cache); counts are not reconciled after a hard kill.

---

## 13. Environment and How To Resume Work (local development facts)

- The Windows host has **no C++20 compiler or CMake**; all builds and tests run in Linux containers (Docker Desktop must be running; it may need starting: `C:\Program Files\Docker\Docker\Docker Desktop.exe`).
- The Docker VM has **about 3.7 GB RAM**. Always build with `-j2` (the Dockerfile defaults to `BUILD_JOBS=2`); unbounded parallelism exhausted memory and hung the engine once.
- Local helper images/volumes created during development (not in the repo, recreate if missing): `edgeflow:dev` -> `edgeflow:dev3` (Ubuntu 24.04, GCC 13, clang, cmake, ninja, git, libboost-dev, libpq-dev, postgresql-client); `edgeflow:valtools` (adds curl, netcat, python3); build volumes `ef_build` (GCC Debug), `ef_build_clang`, `ef_build_rel` (GCC Release); network `ef-net`.
- Typical full validation: start a throwaway PostgreSQL (`postgres:16`, user/db `edgeflow`) on a Docker network, run the build container on that network with `EDGEFLOW_TEST_DB_HOST=<container>` plus optional `EDGEFLOW_TEST_DB_PORT/NAME/USER/PASSWORD`, then `cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DEDGEFLOW_WARNINGS_AS_ERRORS=ON && cmake --build /build -j2 && ctest --test-dir /build --output-on-failure`. Without `EDGEFLOW_TEST_DB_HOST` the database tests are skipped with a reason.
- `docker build -t edgeflow:phase6 .` builds Release with `-Werror` and runs the tests (database tests skipped); `docker compose up -d --build` starts PostgreSQL + EdgeFlow with the registry, health checking, routing and the reverse proxy on (`EDGEFLOW_DB_PASSWORD` overrides the development-only default).
- Tooling quirks: large bash heredocs can fail in this environment (write big files with an editor tool or a script file); Git Bash rewrites `/tmp/...` arguments passed to `docker exec` (set `MSYS_NO_PATHCONV=1`); poll result files that a background job writes only after the job truncated them (use a unique file per run), otherwise a stale file can be mistaken for a fresh result.
- Do not leave test infrastructure running: remove throwaway containers, networks and volumes after a validation run.

## 14. Final Target (end-of-project goal; every number is a TARGET, not an achieved result)

By the end of Phase 10 the project should support resume bullets of this shape. **Each number must be replaced with the actual measured value, and a claim dropped or reworded if the measurements do not support it.**

> **EdgeFlow — API Gateway, Service Discovery & Load Balancer** (C++, TCP/HTTP, PostgreSQL, Redis, Docker)
> - Built a high-performance C++ API gateway supporting service discovery, health-aware routing, request forwarding, connection management, rate limiting, caching, and failover across ~10 service instances.
> - Implemented Round Robin, Least Connections, Weighted Routing, and consistent hashing, efficiently distributing ~100K+ requests across dynamically changing backend instances.
> - Engineered concurrent connection handling with timeouts, retries, circuit breakers, and graceful shutdown, maintaining ~99.5% availability during simulated service failures.
> - Load-tested the gateway at ~10K requests/sec and used profiling to identify bottlenecks, reducing p95 latency from ~45 ms to ~20 ms.

What this requires the project to actually produce: a 10-instance backend deployment with instances added/removed/failed dynamically during tests; a 100K+ request run across all four routing strategies with the distribution recorded; a failure-injection test (killed/slow/erroring backends) with measured availability; a ~10K RPS wrk/custom load test; and a recorded **baseline** p95 before optimization plus a re-measured p95 after Phase 10 profiling-driven fixes. **None of these measurements exists yet.** Phase 5's distribution tests are correctness checks, not benchmarks.

## 15. Open Items

- Remote GitHub Actions run: never confirmed for any phase (workflow unchanged in Phase 6; same configurations validated locally in containers).
- No performance measurements exist.
- Known design limitations carried forward: PostgreSQL read per routing decision (no routing cache); one probe type and one routing strategy for all instances; `Expect: 100-continue` unsupported; buffered (not streamed) proxying; `connection_count` not reconciled after a hard kill.
