# EdgeFlow benchmarks

Two kinds of measurement, both reproducible with the commands below. Everything here reports what
was measured on one machine; none of it is a promise about another.

| | what it measures | tool | where |
|---|---|---|---|
| micro-benchmarks | the CPU cost of one function on the request path | Google Benchmark | `bench/micro` |
| load tests | requests/sec and latency of a running gateway under concurrent HTTP load | wrk | `bench/load` |

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
(`docker build -t edgeflow:phase9 .`).

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
