# EdgeFlow — Development Summary Log

This file is the resume point for the project. **After every completed phase, append a summary entry here.** To resume work after a break, read this file together with [Phases.md](Phases.md) to recover context and continue from where development stopped.

## Standing Rules

1. **Phases are authoritative** ([Phases.md](Phases.md)). Do not skip phases or implement later-phase features early.
2. **Git identity (locked):** every commit and push uses the owner's identity, `iamankoo`. The owner is the only author, committer, contributor and repository owner. No other identity, no co-author trailers, and no tool/AI attribution anywhere in commits, files or metadata.
3. **Verify before each commit:** `git config user.name`, `git config user.email`, `git log -1 --format='%an <%ae>'`.
4. **Metrics honesty:** performance figures are targets until measured by reproducible tests.
5. **Stack is locked** ([techstack.md](techstack.md)); no substitutions without owner approval.

## Final Target (end-of-project goal)

By the end of Phase 10 the project should support resume bullets of this shape. **Every number below is a target, not an achieved result.** Each must be replaced with the actual measured value, and a claim must be dropped or reworded if the measurements do not support it.

> **EdgeFlow — API Gateway, Service Discovery & Load Balancer** (C++, TCP/HTTP, PostgreSQL, Redis, Docker)
> - Built a high-performance C++ API gateway supporting service discovery, health-aware routing, request forwarding, connection management, rate limiting, caching, and failover across ~10 service instances.
> - Implemented Round Robin, Least Connections, Weighted Routing, and consistent hashing, efficiently distributing ~100K+ requests across dynamically changing backend instances.
> - Engineered concurrent connection handling with timeouts, retries, circuit breakers, and graceful shutdown, maintaining ~99.5% availability during simulated service failures.
> - Load-tested the gateway at ~10K requests/sec and used profiling to identify bottlenecks, reducing p95 latency from ~45 ms to ~20 ms.

What this requires the project to actually produce:

- A 10-instance backend deployment with instances added/removed/failed dynamically during tests.
- A 100K+ request run across all four routing strategies, with the distribution recorded.
- A failure-injection test (killed/slow/erroring backends) with measured availability.
- A ~10K RPS wrk/custom load test.
- A recorded **baseline** p95 *before* optimization and a re-measured p95 *after* Phase 10 profiling-driven fixes, so any "reduced from X to Y" claim is backed by real before/after numbers.

## Current State

- **Current phase:** Phase 2 — TCP/HTTP Networking Engine — **COMPLETED** (Phase 1: `8b5c6f0`; Phase 2: commit `feat: implement EdgeFlow phase 2 networking engine`, hash in git history)
- **Next:** Phase 3 — Service Discovery & Registry — **NOT STARTED**; begins only when the owner provides its prompt, cross-checked against `Phases.md`.
- **Last updated:** 2026-10-03

## Entry Template

```markdown
### Phase N — <Title> (completed YYYY-MM-DD)
- **Commit(s):** <hash(es)>
- **What was built:**
- **Key files / modules:**
- **Design decisions:**
- **Tests run and results:**
- **Measured results (if any):**
- **Known issues / deviations from Phases.md:**
- **Exit condition met:** yes/no — evidence
- **Next step:**
```

## Phase Summaries

### Initialization / Pre-Phase 1 (completed 2026-10-02)
- **Commit(s):** `5e3be65` — `docs: initialize EdgeFlow architecture and development roadmap` (pushed to `main`)
- **What was built:** Documentation only: `README.md`, `Phases.md`, `architecture.md`, `techstack.md`, and this `summary.md`.
- **Key decisions:** Locked 10-phase roadmap, locked tech stack, locked module architecture, four load-balancing algorithms (Round Robin, Least Connections, Weighted Routing, Consistent Hashing), and the resume-metrics-honesty rule.
- **Tests run and results:** None (no code yet). Docs checked for consistency.
- **Known issues:** None. No source code, build files or CI exist yet.
- **Exit condition met:** yes — docs created, identity verified, pushed.
- **Next step:** Phase 1, once the owner provides its detailed implementation prompt; cross-check it against `Phases.md` first and report any mismatch.

### Phase 1 — Foundation & Core Infrastructure (completed 2026-10-02)
- **Commit(s):** `feat: implement EdgeFlow phase 1 foundation` (single commit on `main`; hash recorded in git history)
- **What was built:** CMake 3.25+/C++20 project; pinned `FetchContent` dependencies (spdlog v1.14.1, yaml-cpp 0.8.0, GoogleTest v1.15.2); strict YAML `ConfigManager`; spdlog `Logger` wrapper; `Application` lifecycle; `ShutdownCoordinator`; `SignalHandler` (SIGINT/SIGTERM); `CommandLine`; multi-stage Dockerfile (non-root); `docker-compose.yml`; GitHub Actions CI (GCC Debug/Release, Clang Debug); `CMakePresets.json`.
- **Key files / modules:** `CMakeLists.txt`, `cmake/`, `include/edgeflow/{config,core,logging}`, `src/`, `tests/`, `config/config.yaml`, `Dockerfile`, `docker-compose.yml`, `.github/workflows/ci.yml`.
- **Design decisions:**
  - Config is strict: unknown keys are errors, all errors are reported together, a failed load keeps the previous config.
  - Config loads before logging exists, so config failures go to stderr (exit 2); init failures exit 1.
  - Signal handlers only set a lock-free atomic; `run()` polls it every 50 ms.
  - Shutdown is idempotent and thread-safe; components register stop callbacks that run in reverse order. Phase 2 networking should register with `Application::shutdownCoordinator()`.
  - Boost is not fetched: `-DEDGEFLOW_ENABLE_BOOST=ON` only locates an installed Boost; no target links it yet.
  - Compose publishes no ports and the image has no HEALTHCHECK, because there is no endpoint yet.
- **Tests run and results:** 56/56 CTest tests pass (GCC 13 Release `-Werror` during `docker build`; Clang Debug `-Werror` in a container). No compiler warnings.
- **Runtime validation (Docker, Ubuntu 24.04):** container starts, logs the four startup events, stays alive, handles `docker stop` (SIGTERM) and SIGINT, logs the three shutdown events, exits 0. Compose `up`/`stop`/`down` verified the same way. Bad config exits 2.
- **Measured results:** None (performance work belongs to Phases 9-10).
- **Known issues / deviations from Phases.md:** None. Environment limitation: the Windows dev machine has no C++20 compiler or CMake (MinGW GCC 6.3), so builds and tests were run in Linux containers. The GitHub Actions workflow was syntax-checked locally; its remote run was not verified at the time of the commit.
- **Exit condition met:** yes — demonstrated by running the container (start, config load, init, lifecycle logs, clean shutdown).
- **Next step:** Phase 2 once the owner provides its prompt; cross-check it against `Phases.md` first.

### Phase 2 — TCP/HTTP Networking Engine (completed 2026-10-03)
- **Commit(s):** `feat: implement EdgeFlow phase 2 networking engine` (single commit on `main`; hash recorded in git history)
- **What was built:** An asynchronous HTTP/1.1 server on Boost.Asio + Boost.Beast: TCP listener (configurable host/port), per-connection state machine (Idle/Reading/Handling/Writing/Closing), request parsing and response generation, keep-alive, idle and request timeouts (408), request body/header limits (413/431), connection limit, a local request handler (`GET /`, `GET /health`, `POST /echo`, 404/405), concurrent clients on a small `io_context` worker pool, graceful drain integrated with the Phase 1 `ShutdownCoordinator`, `edgeflow --healthcheck`, Docker `HEALTHCHECK`, Compose port publishing, CI changes.
- **Key files / modules:** `include/edgeflow/network/*`, `src/network/*`, `tests/network/*`, `tests/support/NetTestSupport.hpp`; config keys under `server.*` (`request_timeout_ms`, `keep_alive_timeout_ms`, `max_request_body_bytes`, `max_header_bytes`, `max_connections`, `worker_threads`).
- **Design decisions:**
  - Per-connection `steady_timer` with a generation counter instead of Beast `tcp_stream` timeouts (those close the socket before a 408 can be sent).
  - Idle wait uses `socket.async_wait(wait_read)`, not `http::async_read_some`: Beast's `async_read_some` reads until the whole header is complete, which left stalled partial headers under the idle limit and never produced a 408. The request timeout now starts when the first byte arrives.
  - `draining` is an atomic set by `beginDrain()` from the stopping thread, because a synchronous handler blocks the connection's strand; a draining connection answers with `Connection: close` and never returns to Idle.
  - Final responses use a lingering close (shutdown send, drain) so the client can read the response.
  - Boost comes from the system (`libboost-dev` >= 1.83, header-only); nlohmann/json v3.11.3 via FetchContent.
  - Beast `string_view` values are converted to `std::string_view` before being passed to the logger (fmt has no formatter for `boost::core::string_view`).
  - The Dockerfile build is bounded with `ARG BUILD_JOBS=2`.
- **Tests run and results:** 127/127 CTest tests pass with GCC 13.3 Debug `-Werror`, Clang 18.1.3 Debug `-Werror`, and inside the Release `-Werror` Docker build; 5 consecutive full GCC runs passed. Real-socket tests cover endpoints, parsing errors, limits, keep-alive and reuse, pipelining, timeouts, disconnects and resets, write failure, connection limit, graceful and forced shutdown, and 1/10/50/100 concurrent clients.
- **Runtime validation (real binary, Linux container):** `/` 200, `/health` 200, unknown path 404, wrong method 405 with `Allow`, `Connection: close` honoured, malformed requests 400 + close, oversized header 431, oversized body 413, curl and raw-socket keep-alive over one connection, partial header and stalled body 408 at 5 s, idle close at 10 s, 10/50/100 concurrent clients (50/250/500 requests) all correct, SIGTERM drains and exits 0, stalled connection force-closed at the grace period.
- **Docker:** `docker build -t edgeflow:phase2 .` succeeds (Release, `-Werror`, tests run in the build, `-j2`); the container becomes healthy, serves `/` and `/health`, and `docker stop` exits 0 gracefully. `docker compose up --build` / `down` verified twice (healthy, HTTP 200, clean teardown).
- **Measured results:** None. Correctness only; no throughput or latency numbers exist.
- **Known issues / deviations from Phases.md:** None. The GitHub Actions workflow (now installing `libboost-dev`) has not been confirmed by a remote run. `Expect: 100-continue` is not implemented. The request handler is synchronous.
- **Defects found and fixed during validation:** fmt could not format Beast `string_view` (HttpConnection log calls); partial-header timeout (above); in-flight request kept the connection alive during drain (above); a test discarded a `[[nodiscard]]` result under Clang; two test-only compile errors (missing include, `constexpr` struct with `shared_ptr`).
- **Exit condition met:** yes — the real server was shown handling concurrent keep-alive clients over HTTP/1.1, locally and in Docker/Compose.
- **Next step:** Phase 3 once the owner provides its prompt; cross-check it against `Phases.md` first.

## Environment Notes

- The Windows host has no C++20 compiler or CMake; all builds run in Linux containers (Docker Desktop must be running).
- The Docker VM has about 3.7 GB RAM. Building Beast code with default ninja parallelism exhausted memory and hung the engine, so always build with `-j2` (the Dockerfile defaults to `BUILD_JOBS=2`).
- Large bash heredocs in this environment can fail; write big files with an editor tool.
