# Load test result

```text
date_utc: 2026-10-15T06:13:28Z
kernel: Linux 6.8.0-1064-azure
cpus: 4
cpu_model: AMD EPYC 7763 64-Core Processor
memory: 15Gi
docker: Docker version 29.8.0-1, build 88096ef00576baf72a9cb45caa45c0544c40e0a7
wrk: wrk debian/4.1.0-4build2 [epoll] Copyright (C) 2012 Will Glozer
image: edgeflow:phase9 sha256:8cd24ffa32f9b0e62b05252602fc87a75ee7f0f5376422dd9ed9068c6ecbed13
git_commit: ab817151e0760113d6cff9bce8be7c441d019a69
wrk_threads: 2
wrk_connections: 64
warmup_seconds: 5
run_seconds: 20
runs: 3
gateway_config_sha256: 52f427ccf4c8c1fc8f69229c3db9b9ed365a9d91f75a00272dcbfd8a739c7151
```

## Throughput and latency seen by wrk (client side)

| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |
|---|---|---|---|---|---|---|---|---|---|
| S0-backend-direct | 3 | 35554, 36253, 36611 | 36253 | 1.53 | 2.83 | 4.88 | 22.05 | 0 | 0 |
| S1-gateway-local | 3 | 35635, 36647, 36450 | 36450 | 1.52 | 2.81 | 4.93 | 23.22 | 0 | 0 |
| S2-proxy-no-cache | 3 | 1727, 1790, 1784 | 1784 | 34.58 | 42.06 | 54.03 | 88.09 | 0 | 0 |
| S3-proxy-cache-hit | 3 | 10911, 11243, 11438 | 11243 | 5.41 | 7.26 | 10.90 | 49.67 | 0 | 0 |

wrk reports the latency a client sees (connection included); the gateway's own figures below start when the request reaches its handler and end when the response is ready.

## The gateway's own metrics right after each run (`/metrics`, 10 s window)

| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |
|---|---|---|---|---|---|---|---|---|---|---|---|
| S0-backend-direct | 1 | 0 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| S0-backend-direct | 2 | 0 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| S0-backend-direct | 3 | 0 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| S1-gateway-local | 1 | 34617 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| S1-gateway-local | 2 | 36786 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| S1-gateway-local | 3 | 36277 | 0.05 | 0.10 | 0.10 | n/a | n/a | 0.0000 | 0.0000 | 0 | 0 |
| S2-proxy-no-cache | 1 | 1709 | 35.12 | 49.67 | 59.34 | 15.04 | 26.90 | 0.0000 | 0.0000 | 0 | 0 |
| S2-proxy-no-cache | 2 | 1790 | 34.30 | 45.15 | 50.97 | 14.77 | 23.61 | 0.0000 | 0.0000 | 0 | 0 |
| S2-proxy-no-cache | 3 | 1792 | 34.20 | 44.22 | 53.93 | 14.65 | 24.17 | 0.0000 | 0.0000 | 0 | 0 |
| S3-proxy-cache-hit | 1 | 10787 | 4.65 | 8.30 | 11.15 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| S3-proxy-cache-hit | 2 | 11332 | 4.59 | 7.11 | 9.97 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
| S3-proxy-cache-hit | 3 | 11410 | 4.56 | 6.80 | 8.82 | n/a | n/a | 0.0000 | 1.0000 | 0 | 0 |
