# Load test result

```text
date_utc: 2026-10-15T08:03:21Z
kernel: Linux 6.8.0-1064-azure
cpus: 4
cpu_model: AMD EPYC 7763 64-Core Processor
memory: 15Gi
wrk: wrk debian/4.1.0-4build2 [epoll] Copyright (C) 2012 Will Glozer
backend_image: edgeflow:phase9 sha256:8cd24ffa32f9b0e62b05252602fc87a75ee7f0f5376422dd9ed9068c6ecbed13
git_commit: ea0292a907c7a66fc021569a2e8234bfad9bb6d4
wrk_threads: 2
wrk_connections: 64
warmup_seconds: 5
run_seconds: 20
runs: 3
backends: 3 (EdgeFlow instances)
variants:
  o2-pool-hops|/workspaces/bin/edgeflow-o2|connection_counting: auto|snapshot_ttl_ms: 250|
  o3-inline|/workspaces/bin/edgeflow-o3|connection_counting: auto|snapshot_ttl_ms: 250|
```

## Throughput and latency seen by wrk (client side)

| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |
|---|---|---|---|---|---|---|---|---|---|
| o2-pool-hops-S2 | 3 | 8341, 8128, 8258 | 8258 | 7.57 | 9.08 | 12.45 | 25.62 | 0 | 0 |
| o3-inline-S2 | 3 | 11647, 11405, 11637 | 11637 | 5.36 | 6.83 | 9.44 | 24.01 | 0 | 0 |

wrk reports the latency a client sees (connection included); the gateway's own figures below start when the request reaches its handler and end when the response is ready.

## The gateway's own metrics right after each run (`/metrics`, 10 s window)

| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |
|---|---|---|---|---|---|---|---|---|---|---|---|
| o2-pool-hops-S2 | 1 | 8427 | 7.15 | 9.36 | 11.54 | 0.47 | 2.58 | 0.0000 | 0.0000 | 0 | 0 |
| o2-pool-hops-S2 | 2 | 8017 | 7.29 | 10.82 | 13.61 | 0.49 | 2.97 | 0.0000 | 0.0000 | 0 | 0 |
| o2-pool-hops-S2 | 3 | 8207 | 7.32 | 9.80 | 11.94 | 0.48 | 2.83 | 0.0000 | 0.0000 | 0 | 0 |
| o3-inline-S2 | 1 | 11555 | 4.88 | 7.13 | 8.87 | 0.42 | 2.53 | 0.0000 | 1.0000 | 0 | 0 |
| o3-inline-S2 | 2 | 11582 | 4.90 | 7.03 | 8.59 | 0.41 | 2.52 | 0.0000 | 1.0000 | 0 | 0 |
| o3-inline-S2 | 3 | 11722 | 4.84 | 6.97 | 8.54 | 0.40 | 2.41 | 0.0000 | 1.0000 | 0 | 0 |
