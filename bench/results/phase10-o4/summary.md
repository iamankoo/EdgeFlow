# Load test result

```text
date_utc: 2026-10-15T08:07:24Z
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
  o3-nagle-on|/workspaces/bin/edgeflow-o3|connection_counting: auto|snapshot_ttl_ms: 250|
  o4-nodelay|/workspaces/bin/edgeflow-o4|connection_counting: auto|snapshot_ttl_ms: 250|
```

## Throughput and latency seen by wrk (client side)

| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |
|---|---|---|---|---|---|---|---|---|---|
| o3-nagle-on-S1 | 3 | 62985, 62208, 61941 | 62208 | 1.02 | 1.15 | 3.18 | 8.77 | 0 | 0 |
| o3-nagle-on-S2 | 3 | 11604, 11850, 11413 | 11604 | 5.36 | 6.89 | 9.40 | 35.34 | 0 | 0 |
| o3-nagle-on-S3 | 3 | 13544, 13721, 13351 | 13544 | 4.54 | 5.50 | 8.09 | 19.59 | 0 | 0 |
| o4-nodelay-S1 | 3 | 62076, 61789, 61704 | 61789 | 1.04 | 1.14 | 3.11 | 13.66 | 0 | 0 |
| o4-nodelay-S2 | 3 | 11654, 11709, 11358 | 11654 | 5.34 | 6.74 | 9.35 | 26.79 | 0 | 0 |
| o4-nodelay-S3 | 3 | 13582, 13694, 13288 | 13582 | 4.54 | 5.46 | 8.11 | 17.10 | 0 | 0 |

wrk reports the latency a client sees (connection included); the gateway's own figures below start when the request reaches its handler and end when the response is ready.

## The gateway's own metrics right after each run (`/metrics`, 10 s window)

| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |
|---|---|---|---|---|---|---|---|---|---|---|---|
| o3-nagle-on-S1 | 1 | 63216 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| o3-nagle-on-S1 | 2 | 62500 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| o3-nagle-on-S1 | 3 | 63325 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| o3-nagle-on-S2 | 1 | 11505 | 4.87 | 7.13 | 8.79 | 0.42 | 2.32 | 0.0000 | 1.0000 | 0 | 0 |
| o3-nagle-on-S2 | 2 | 11811 | 4.79 | 6.81 | 8.56 | 0.40 | 2.35 | 0.0000 | 1.0000 | 0 | 0 |
| o3-nagle-on-S2 | 3 | 11406 | 4.86 | 7.37 | 9.94 | 0.42 | 2.58 | 0.0000 | 1.0000 | 0 | 0 |
| o3-nagle-on-S3 | 1 | 13497 | 4.26 | 6.04 | 7.93 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o3-nagle-on-S3 | 2 | 13790 | 4.22 | 5.38 | 7.21 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o3-nagle-on-S3 | 3 | 13135 | 4.27 | 6.37 | 9.52 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S1 | 1 | 62107 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S1 | 2 | 61983 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S1 | 3 | 61198 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S2 | 1 | 11632 | 4.87 | 6.88 | 8.56 | 0.40 | 2.31 | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S2 | 2 | 11792 | 4.83 | 6.74 | 8.49 | 0.38 | 2.30 | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S2 | 3 | 11002 | 5.04 | 7.67 | 10.08 | 0.40 | 2.58 | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S3 | 1 | 13617 | 4.23 | 5.65 | 7.49 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S3 | 2 | 13630 | 4.26 | 5.38 | 7.10 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| o4-nodelay-S3 | 3 | 13048 | 4.29 | 6.36 | 8.86 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
