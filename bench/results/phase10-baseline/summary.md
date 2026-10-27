# Load test result

```text
date_utc: 2026-10-15T07:30:46Z
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
```

## Throughput and latency seen by wrk (client side)

| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |
|---|---|---|---|---|---|---|---|---|---|
| base-S1 | 3 | 62665, 62462, 62696 | 62665 | 1.03 | 1.14 | 3.16 | 9.88 | 0 | 0 |
| base-S2 | 3 | 1881, 1935, 1935 | 1935 | 32.35 | 37.05 | 45.98 | 64.35 | 0 | 0 |
| base-S3 | 3 | 13627, 13289, 13679 | 13627 | 4.53 | 5.40 | 8.13 | 15.62 | 0 | 0 |

wrk reports the latency a client sees (connection included); the gateway's own figures below start when the request reaches its handler and end when the response is ready.

## The gateway's own metrics right after each run (`/metrics`, 10 s window)

| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |
|---|---|---|---|---|---|---|---|---|---|---|---|
| base-S1 | 1 | 61937 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| base-S1 | 2 | 62279 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| base-S1 | 3 | 63111 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| base-S2 | 1 | 1897 | 33.09 | 42.45 | 49.50 | 14.23 | 21.39 | 0.0000 | 1.0000 | 0 | 0 |
| base-S2 | 2 | 1919 | 32.96 | 41.80 | 49.04 | 14.16 | 21.23 | 0.0000 | 1.0000 | 0 | 0 |
| base-S2 | 3 | 1936 | 32.91 | 40.38 | 46.08 | 14.13 | 20.68 | 0.0000 | 1.0000 | 0 | 0 |
| base-S3 | 1 | 13635 | 4.24 | 5.44 | 7.23 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| base-S3 | 2 | 13541 | 4.27 | 5.47 | 7.18 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| base-S3 | 3 | 13710 | 4.23 | 5.36 | 7.07 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
