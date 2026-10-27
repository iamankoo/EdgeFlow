# EdgeFlow performance: what was measured, what was changed, what it bought

Phase 10 profiled the Phase 9 gateway, changed only what the profiles pointed at, and measured every change against
the previous one in the same harness. Everything here is a measurement on **one machine** (a GitHub Codespace, 4 vCPU
AMD EPYC 7763, 15 GiB, Ubuntu 24.04) where the load generator, the gateway, the backends, PostgreSQL and Redis **share
the CPUs**. The numbers compare steps with each other; they are not the capacity of a dedicated server. The raw files
of every run are in `bench/results/phase10-*`.

## How it was measured

| tool | used for | where |
|---|---|---|
| `perf record -e cpu-clock -g` + FlameGraph | CPU profile (a virtual machine has no hardware counters, so a timer is sampled) | `bench/profile/profile.sh`, `phase10-profile-*/cpu.svg`, `cpu-top.txt` |
| `pidstat -t`, `mpstat` | CPU per thread, per program, whole machine | `threads.txt` |
| `strace -c -f` | system calls, `futex` = mutex / condition-variable waits (lock contention) | `locks.txt` |
| `pg_stat_statements`, `pg_stat_activity` | which SQL statements cost PostgreSQL time and what they wait on | `postgres.txt`, `postgres-waits.txt` |
| `redis-cli info commandstats` | cost of each Redis command | `redis.txt` |
| `nstat`, `ss -s`, `/proc/<pid>/status` | retransmits, listen-queue drops, sockets, descriptors, RSS | `network.txt`, `rss.txt` |
| `heaptrack` | allocations and heap growth | (summary below) |
| `wrk` (2 threads, 64 connections) | throughput and latency; `bench/final/percentiles.lua` adds p95 | `bench/profile/ab.sh`, `bench/final/run.sh` |

The harness reaches PostgreSQL, Redis and the backends by their **container IP addresses**. The first profile used
published ports and showed `docker-proxy` using 72 % of a CPU: a measurement artifact, removed. The gateway under test
is a native process so that `perf` and `strace` can attach.

## Baseline (the Phase 9 gateway, corrected harness)

3 runs of 20 s each, median of the three:

| scenario | req/s | p50 | p99 |
|---|---|---|---|
| S1 gateway `GET /health` | 62,665 | 1.03 ms | 3.16 ms |
| **S2 proxied, cache bypassed** (rate limiter, PostgreSQL routing, connection counting, proxy, backend) | **1,935** | 32.35 ms | 45.98 ms |
| S3 proxied, answered from the Redis cache | 13,627 | 4.53 ms | 8.13 ms |

S2 is the path that does the gateway's real work and the one that missed the roadmap's ~10K req/s target.

## What the profiles showed

| question | finding (measured) |
|---|---|
| Who uses the CPU? | The gateway was **not** the bottleneck: while the machine was ~98 % busy it used about 0.7 core. **PostgreSQL used 180 % of a core**, the three backends and the gateway 87 % together, Redis 11 %, wrk 6 %. |
| What does PostgreSQL do? | `pg_stat_statements`: the `UPDATE service_instances SET connection_count ...` that the proxy ran **twice per request** (+1 before, -1 after) was **86.7 % of PostgreSQL's execution time** (0.305 ms per call, 43,168 calls) against 13.3 % for the routing `SELECT` (0.094 ms, 21,584 calls). `pg_stat_activity` showed `Lock:transactionid` and `LWLock:XidGen` waits: three hot rows updated by every request. |
| Lock contention? | `strace -c`: 68,448 `futex` calls in 8 s (44 % of system-call time), 17 K `poll` (libpq), 30 K `epoll_wait`. After the first two optimizations `futex` was 77 % of the system-call time: the cost had moved to thread hand-offs (below). |
| Network? | Upstream pool: 223,571 connection reuses against 188 new connections (99.92 % reuse). No listen-queue drops or overflows; 547 TCP retransmitted segments in the run. |
| Memory? | `heaptrack`: peak heap **1.28 MB**, 6.3 M allocations in 38 s (about 92 per request), no growth (117.8 KB live at exit). RSS 16.8 MB, 17 threads, 49 descriptors under load. Not a bottleneck, so **not changed**. |
| Redis? | `EVALSHA` (the rate-limit script) about 31-41 µs per call, `HSET`/`HMGET`/`PEXPIRE` 1.4-2.3 µs. Cheap, **not changed**. |
| Micro-benchmark | Round-robin and consistent-hash selection sort the instances on every call: 10-62 µs at 100 instances. With the 3-10 instances measured it is far below the other costs, so **not changed**. |

## The optimizations (each measured against the step before it)

Each step: the bottleneck the profile named, the change, the rebuilt and re-tested code, and a same-run A/B of three
runs of 20 s on scenario S2. `bench/profile/ab.sh` produces these tables.

| # | bottleneck | change | S2 before -> after (median req/s) | p50 | p99 |
|---|---|---|---|---|---|
| 1 | two PostgreSQL row updates per request, serialised on three hot rows | `proxy.connection_counting: auto`: write `connection_count` only when the strategy that reads it (`least_connections`) is configured | 1,929 -> **4,732** (2.45x) | 32.41 -> 13.19 ms | 49.50 -> 22.92 ms |
| 2 | one PostgreSQL `SELECT` per request to read the routable set (and the thread hand-off waiting for it) | `routing.snapshot_ttl_ms: 250`: keep the routable list of a service for up to 250 ms; no copy of the list when every instance is eligible | 4,755 -> **8,269** (1.74x) | 13.07 -> 7.55 ms | 20.80 -> 12.41 ms |
| 3 | every request hopped to the registry call pool and back although nothing in it could block any more (`futex` 77 % of system-call time) | when counting is off and a fresh snapshot exists, `attempt()` and its end run on the calling I/O thread; an expired snapshot falls back to the pool | 8,258 -> **11,637** (1.41x) | 7.57 -> 5.36 ms | 12.45 -> 9.44 ms |
| - | `TCP_NODELAY` on accepted sockets (a hypothesis) | tried, A/B on S1/S2/S3 | S1 62,208 vs 61,789; S2 11,604 vs 11,654; S3 13,544 vs 13,582: **no difference** | | **reverted** |

Together: S2 **1,935 -> 11,637 req/s (6.0x)**, p50 32.35 -> 5.36 ms, p99 45.98 -> 9.44 ms (the steps were measured
pairwise in separate runs; the end-to-end figure compares the baseline run with the last A/B run). S1 (62,665 ->
62,208) and S3 (13,627 -> 13,544) did not change, as expected: they do not touch the registry. After the third step
there are 1.84 context switches per proxied request.

### What each default means (the trade-offs)

* **The defaults in code are unchanged** (`connection_counting: always`, `snapshot_ttl_ms: 0`): a gateway upgraded
  from Phase 9 behaves exactly as before. `config/config.compose.yaml` and `bench/load/gateway.yaml` switch both on.
* **PostgreSQL stays the source of truth.** `snapshot_ttl_ms` is *bounded staleness*, stated and opt-in: a health or
  registry change reaches routing at most that long late (for 250 ms: a request may still be sent to an instance
  that just turned unhealthy; the retry policy and the circuit breaker cover that window). A snapshot is kept only
  when it is non-empty, an expired snapshot is **never** served while the database is down (the request gets 503),
  and an empty answer is never kept, so a recovering service is routable at once.
* `least_connections` needs live counts: the configuration refuses `snapshot_ttl_ms > 0` with it, and
  `connection_counting: auto` keeps counting for it. It is the slowest strategy for that reason (1,808 req/s below).
* Tests for each change: `ConnectionCountingTest`, `RouterSnapshotTest`, `InlineAttemptTest` (staleness bound,
  recovery, outage after expiry, retries and failover on the inline path, shutdown with requests in flight,
  concurrency; the concurrent ones were also run under ThreadSanitizer).

## Final benchmark: ten backends, every strategy, failure injection

`bench/final/run.sh phase10-final-benchmark <binary>` (report: [`bench/results/phase10-final-benchmark/report.md`](../bench/results/phase10-final-benchmark/report.md)).
Ten EdgeFlow instances act as backends (their `/metrics` give the per-backend counts); the gateway has everything on:
PostgreSQL registry, health checks, reliability, Redis cache and rate limiter, metrics. Every request reaches a backend
(`Cache-Control: no-cache`). wrk: 2 threads, 64 connections.

### Every strategy, 100,000+ requests each

| strategy | requests | req/s | p50 | p90 | p95 | p99 | non-2xx / socket errors | gateway CPU | gateway RSS |
|---|---|---|---|---|---|---|---|---|---|
| round_robin | 203,484 | 10,167 | 6.12 ms | 7.91 ms | 8.87 ms | 11.71 ms | 0 / 0 | 1.58 cores | 16.2 MB |
| weighted | 208,486 | 10,417 | 5.97 ms | 7.86 ms | 8.71 ms | 10.83 ms | 0 / 0 | 1.60 cores | 16.1 MB |
| least_connections | 135,722 | 1,808 | 33.96 ms | 41.47 ms | 45.80 ms | 55.79 ms | 0 / 0 | 0.90 cores | 15.8 MB |
| consistent_hashing | 214,899 | 10,722 | 5.05 ms | 12.27 ms | 20.56 ms | 38.47 ms | 0 / 0 | 1.78 cores | 16.2 MB |

Distribution of the requests over the ten backends (the backends' own counters):

* **round_robin**: every backend 10.0 % (20,366-20,367 of 203,663).
* **weighted** (weights 1,1,1,1,1,2,2,2,3,3): 5.9 % / 11.8 % / 17.6 % per backend, exactly the weights' shares
  (largest deviation 0.00 percentage points).
* **least_connections**: 8.1 % to 11.6 % per backend, no backend starved or overloaded.
* **consistent_hashing** is keyed by the client address, so one load generator is *sticky* to one backend by design
  (99.9 % to b9; that is also why its p95/p99 are higher: one backend serves everything). The ring itself was measured
  with 100,000 routing decisions for distinct keys: 8.9 % to 11.5 % per backend.

### Dynamic membership and failure injection (10 backends, 66 s of load)

Timeline, identical in both runs: kill b1 (t=5 s), start it again (16 s), pause b2 so that it accepts connections but
never answers (22-34 s), add b11 and remove b3 (40 s), add a backend whose health check passes but every request
answers 503 (46 s), remove it (56 s).

| `proxy.upstream_timeout_ms` | requests | req/s | p50 | p95 | p99 | non-2xx | no answer | availability |
|---|---|---|---|---|---|---|---|---|
| 10000 | 546,773 | 8,274 | 6.54 ms | 6,283 ms | 9,335 ms | 0 | 0 | 100.000 % |
| 1000 | 634,001 | 9,596 | 6.35 ms | 10.24 ms | 422.89 ms | 0 | 0 | 100.000 % |

* **No client ever received an error** (0 non-2xx answers, 0 connection errors or timeouts) in either run, through a
  killed backend, a hung one, an erroring one and membership changes: the killed backend's refused connections were
  retried on other backends (81 retries in the first run and 86 in the second, every one a failover), the circuit breaker of each failing backend opened after
  3 failures (3 trips), and the health checker removed the dead and the hung backend from the routable set (10 -> 9
  routable instances about 8-9 s after each failure) and restored them after recovery.
* **A hung backend is the expensive failure.** With the benchmark's 10 s upstream timeout, the requests sent to the
  paused backend sat for 10 s each; with 64 closed-loop connections, all of them ended up waiting and throughput fell
  to about 1 request per second for ~10 s (p95 6.3 s). With `upstream_timeout_ms: 1000` the same failure cost a p99
  of 423 ms and p95 stayed at 10.2 ms. The timeout, not the code, decides this; choose it from what the slowest
  legitimate backend response is.
* Availability here means 2xx answers over answered requests (and no unanswered ones). It is measured with one
  gateway process; it is not an uptime figure for a deployment.

### The roadmap targets, stated honestly

| target (Phases.md) | measured | verdict |
|---|---|---|
| ~10 service instances | 10 backends, plus dynamic add/remove/fail | **measured** |
| ~100K+ requests | 136K-215K per strategy (and 547K-634K in the failure runs) | **measured** |
| ~99.5 % availability under failure | 100.000 % (0 failed of 546,773 and of 634,001) in one gateway process | **measured** for these failures; a hung backend costs latency, see above |
| ~10K requests/sec | 10.2K-10.7K for round_robin, weighted and consistent_hashing on this shared 4-vCPU machine; **1.8K for least_connections** (every request writes two PostgreSQL rows) | **met except least_connections** |
| p95 ~ 20 ms | 8.7-8.9 ms (round_robin, weighted), 20.6 ms (consistent_hashing, one backend serving everything), 45.8 ms (least_connections) | **met for two strategies, at the limit for one, missed for one** |

`least_connections` remains the known slow path: it needs a live count per backend, kept in PostgreSQL. Moving that
counter into the gateway process (or Redis) would remove the cost but changes where a shared counter lives; that is a
design decision for a later release and was not done speculatively.

## Reproduce

```bash
docker build -t edgeflow:phase10 .                       # image for the backends (and the gateway when run as a container)
bench/profile/ab.sh <label>                              # A/B of variants (see the header of the file)
bench/profile/profile.sh <label> <binary>                # CPU / locks / PostgreSQL / Redis / network profile
bench/final/run.sh <label> <binary> && python3 bench/final/report.py <label>   # the final benchmark (about 12 minutes)
```

They need Docker, `wrk`, `perf`, `strace`, `sysstat`, `python3` and a Release binary built with
`-g -fno-omit-frame-pointer` for the profile (see the headers). Figures from another machine will differ.
