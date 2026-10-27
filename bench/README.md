# EdgeFlow benchmarks

Two kinds of measurement, both reproducible with the commands below. Everything here reports what
was measured on one machine; none of it is a promise about another.

| | what it measures | tool | where |
|---|---|---|---|
| micro-benchmarks | the CPU cost of one function on the request path | Google Benchmark | `bench/micro` |
| load tests | requests/sec and latency of a running gateway under concurrent HTTP load | wrk | `bench/load` |
| profiling and A/B (Phase 10) | where a request spends its time (CPU, locks, PostgreSQL, Redis, network, memory) and whether a change helped | perf, FlameGraph, strace, pidstat, `pg_stat_statements`, wrk | `bench/profile` |
| final benchmark (Phase 10) | ten backends, every routing strategy at 100,000+ requests with the per-backend distribution, membership changes and failure injection | wrk (Lua report), `docker` | `bench/final` |
| hardening tests (Phase 10) | malformed input, resource exhaustion, dependency outages, shutdown under load against a running gateway | python3, curl, docker | `bench/hardening` |

## Micro-benchmarks

```bash
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDGEFLOW_BUILD_BENCHMARKS=ON -DEDGEFLOW_BUILD_TESTS=OFF
cmake --build build-bench --target edgeflow_micro_bench
./build-bench/bench/edgeflow_micro_bench --benchmark_min_time=0.5s
```

They cover the four routing strategies at 3, 10 and 100 instances, proxy header translation,
the circuit breaker's closed path, the retry decision, SHA-256, RESP encoding and parsing, the
cache entry format and key, the rate limiter's bucket plan, and the metrics (histogram, windowed
statistics, `/metrics` rendering). Results of the run documented in `summary.md`:
[`results/phase9-codespace-4cpu/micro-benchmarks.txt`](results/phase9-codespace-4cpu/micro-benchmarks.txt).

## Load test

Needs Docker with Compose, `wrk`, `curl` and `python3`, and the EdgeFlow image
(`docker build -t edgeflow:phase10 .`).

```bash
bench/load/run.sh my-label                  # about 6 minutes: 4 scenarios x (5 s warm-up + 3 x 20 s)
RUNS=5 DURATION=30 CONNECTIONS=128 bench/load/run.sh longer   # other parameters
KEEP=1 bench/load/run.sh keep               # leave the stack running afterwards
```

`run.sh` starts the stack of `bench/load/docker-compose.yml` (PostgreSQL, Redis, three EdgeFlow
instances used as fast C++ backends, and the gateway under test configured by
`bench/load/gateway.yaml`), registers the backends, records the environment, runs the scenarios
with `wrk --latency`, scrapes the gateway's `/metrics` after every run, and writes
`bench/results/<label>/` with every raw output and a `summary.md`. The stack is removed afterwards.

| scenario | request | what it exercises |
|---|---|---|
| S0 backend direct | `GET /` on a backend | the ceiling of the C++ HTTP server for a tiny answer |
| S1 gateway local | `GET /health` on the gateway | the gateway's own HTTP server and the metrics recording |
| S2 proxy, cache bypassed | `GET /proxy/bench/` with `Cache-Control: no-cache` | rate limiter (Redis script), routing (PostgreSQL), connection counting, proxy, reliability, backend |
| S3 proxy, cache hit | `GET /proxy/bench/health` | rate limiter (Redis script) and a cache lookup (Redis `GET`); no backend, no PostgreSQL |

The gateway runs with everything on: registry, health checks, routing, proxy, reliability, Redis
cache, Redis rate limiting (a bucket so large it never refuses, but the script still runs per request)
and metrics; only logging is quieted to `warn`.

### How to read a result

- **wrk's numbers are the client's view** (connection time included); **the gateway's own figures**
  (`/metrics`) start when the request reaches its handler. They agree on throughput; the gateway's
  latency is lower by the time spent in the kernel and the load generator.
- The load generator, the gateway, three backends, PostgreSQL and Redis **share the machine**, so
  every scenario is limited by the whole box, not by the gateway alone. Compare scenarios with each
  other and runs with runs on the same machine; do not read a number as the capacity of a dedicated
  gateway.
- wrk keeps a fixed number of connections busy (closed-loop): it does not model clients that
  arrive independently, and it under-reports the latency of a saturated server (coordinated
  omission). The latency figures are the latency at that fixed concurrency.
- PostgreSQL runs with `fsync=off` and Redis without persistence so the benchmark does not measure
  the disk. The backends are EdgeFlow instances answering a tiny banner, so the backend is cheap.
- Repeat runs of one scenario differ by a few percent (`summary.md` lists every run); a difference
  of that size between two builds is noise.

### Results

The run committed with Phase 9 is in
[`results/phase9-codespace-4cpu/`](results/phase9-codespace-4cpu/summary.md) (GitHub Codespace, 4 vCPU
AMD EPYC 7763, 15 GiB, commit `ab81715` plus the Phase 9 changes; wrk 2 threads, 64 connections, 3 runs of 20 s):

| scenario | median requests/s | wrk p50 | wrk p99 |
|---|---|---|---|
| S0 backend direct | 36253 | 1.53 ms | 4.88 ms |
| S1 gateway local | 36450 | 1.52 ms | 4.93 ms |
| S2 proxy, cache bypassed | 1784 | 34.6 ms | 54.0 ms |
| S3 proxy, cache hit | 11243 | 5.41 ms | 10.9 ms |

The ~10K requests/s target of `Phases.md` was therefore **reached only for cached responses** on this
machine; an uncached proxied request, which costs three PostgreSQL round trips plus a Redis script
and the proxy exchange, ran at about 1.8K requests/s. Understanding and improving that is the work of
Phase 10 (profiling and optimisation); no optimisation was made in Phase 9.

The Phase 9 run above is kept as the **before** of Phase 10. The **after**, and everything measured in Phase 10, is
below and in [`../docs/PERFORMANCE.md`](../docs/PERFORMANCE.md).

## Profiling and A/B comparison (Phase 10)

```bash
# profile a native gateway under load (needs docker, wrk, perf, strace, sysstat, python3, sudo)
EDGEFLOW_IMAGE=edgeflow:phase10 PERF=perf FLAMEGRAPH_DIR=/opt/FlameGraph bench/profile/profile.sh my-profile build-prof/src/edgeflow

# compare two builds / configurations in the same run (variant = name|binary|proxy opts|routing opts|server opts|db opts)
VARIANTS=$'before|/path/old||
after|/path/new|connection_counting: auto|snapshot_ttl_ms: 250' bench/profile/ab.sh my-ab
```

`bench/profile/lib.sh` starts PostgreSQL (with `pg_stat_statements`), Redis and the backends as containers and
reaches them by their bridge IP addresses (a published port goes through `docker-proxy`, which distorted the
first profile); the gateway is a native process. Results of the Phase 10 runs, all committed:

| directory | what |
|---|---|
| `results/phase10-baseline/` | the Phase 9 gateway in this harness (S1/S2/S3), plus its flamegraph |
| `results/phase10-o1/` | `connection_counting: auto` against the baseline (S2: 1,929 -> 4,732 req/s) |
| `results/phase10-o2/` | `routing.snapshot_ttl_ms: 250` (S2: 4,755 -> 8,269) |
| `results/phase10-o3/` | inline attempts (S2: 8,258 -> 11,637) |
| `results/phase10-o4/` | `TCP_NODELAY` on accepted sockets: no effect, reverted; context switches per request |
| `results/phase10-profile-o1/`, `phase10-profile-o2/` | CPU, threads, `futex`, PostgreSQL and Redis profiles after the first two steps (`cpu.svg` is a flamegraph) |
| `results/phase10-hardening-abuse/` | `abuse.sh`: 29 malformed-input / exhaustion checks |

## Final benchmark (Phase 10)

```bash
EDGEFLOW_IMAGE=edgeflow:phase10 bench/final/run.sh my-final build/src/edgeflow     # about 12 minutes
python3 bench/final/report.py my-final                                              # writes results/my-final/report.md
```

Part 1 runs each routing strategy against ten backends for long enough to exceed 100,000 requests and records every
backend's own request count (the backends run `bench/final/backend.yaml`, which turns `/metrics` on). Part 2 runs a
66-second scenario against ten backends twice (`proxy.upstream_timeout_ms` 10000 and 1000): kill and restart a
backend, pause one (it accepts connections and never answers), add one, remove one, add one that passes its health check
and answers every request with 503, and remove it; wrk's percentiles come from `percentiles.lua` (p95 is not in
`--latency`). Committed result: [`results/phase10-final-benchmark/report.md`](results/phase10-final-benchmark/report.md).

## Hardening tests (Phase 10)

```bash
bench/hardening/abuse.sh my-abuse build/src/edgeflow        # 29 checks: malformed requests, slow clients, connection exhaustion, pipelining, fd leak
bench/hardening/failures.sh my-failures build/src/edgeflow  # Redis down/up, PostgreSQL down/up, SIGTERM with a request in flight
```
