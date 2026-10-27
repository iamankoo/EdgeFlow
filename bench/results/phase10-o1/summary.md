# Load test result

```text
date_utc: 2026-10-15T07:37:24Z
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
  base|/workspaces/bin/edgeflow-base|||
  o1-auto|/workspaces/bin/edgeflow-o1|connection_counting: auto||
```

## Throughput and latency seen by wrk (client side)

| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |
|---|---|---|---|---|---|---|---|---|---|
| base-S2 | 3 | 1929, 1943, 1897 | 1929 | 32.41 | 36.76 | 49.50 | 81.32 | 0 | 0 |
| o1-auto-S2 | 3 | 4798, 4732, 4592 | 4732 | 13.19 | 15.86 | 22.92 | 44.76 | 0 | 0 |

wrk reports the latency a client sees (connection included); the gateway's own figures below start when the request reaches its handler and end when the response is ready.

## The gateway's own metrics right after each run (`/metrics`, 10 s window)

| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |
|---|---|---|---|---|---|---|---|---|---|---|---|
| base-S2 | 1 | 1949 | 32.68 | 39.47 | 45.91 | 14.09 | 20.52 | 0.0000 | 0.0000 | 0 | 0 |
| base-S2 | 2 | 1959 | 32.56 | 39.57 | 49.37 | 14.01 | 21.52 | 0.0000 | 0.0000 | 0 | 0 |
| base-S2 | 3 | 1877 | 33.09 | 45.30 | 51.14 | 14.22 | 23.05 | 0.0000 | 0.0000 | 0 | 0 |
| o1-auto-S2 | 1 | 4824 | 12.45 | 16.97 | 20.86 | 5.01 | 8.25 | 0.0000 | 1.0000 | 0 | 0 |
| o1-auto-S2 | 2 | 4694 | 12.79 | 17.47 | 22.63 | 5.12 | 8.75 | 0.0000 | 1.0000 | 0 | 0 |
| o1-auto-S2 | 3 | 4519 | 13.10 | 19.54 | 25.31 | 5.15 | 9.67 | 0.0000 | 1.0000 | 0 | 0 |
