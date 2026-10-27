# Load test result

```text
date_utc: 2026-10-15T07:51:35Z
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
  o1-no-snapshot|/workspaces/bin/edgeflow-o2|connection_counting: auto||
  o2-snapshot-250ms|/workspaces/bin/edgeflow-o2|connection_counting: auto|snapshot_ttl_ms: 250|
```

## Throughput and latency seen by wrk (client side)

| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |
|---|---|---|---|---|---|---|---|---|---|
| o1-no-snapshot-S2 | 3 | 4755, 4731, 4809 | 4755 | 13.07 | 15.85 | 20.80 | 46.56 | 0 | 0 |
| o2-snapshot-250ms-S2 | 3 | 8326, 8146, 8269 | 8269 | 7.55 | 9.06 | 12.41 | 26.35 | 0 | 0 |

wrk reports the latency a client sees (connection included); the gateway's own figures below start when the request reaches its handler and end when the response is ready.

## The gateway's own metrics right after each run (`/metrics`, 10 s window)

| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |
|---|---|---|---|---|---|---|---|---|---|---|---|
| o1-no-snapshot-S2 | 1 | 4778 | 12.58 | 17.02 | 20.18 | 5.06 | 8.35 | 0.0000 | 0.0000 | 0 | 0 |
| o1-no-snapshot-S2 | 2 | 4606 | 12.61 | 19.10 | 25.60 | 4.98 | 10.10 | 0.0000 | 0.0000 | 0 | 0 |
| o1-no-snapshot-S2 | 3 | 4771 | 12.60 | 16.98 | 20.42 | 5.07 | 8.24 | 0.0000 | 0.0000 | 0 | 0 |
| o2-snapshot-250ms-S2 | 1 | 8375 | 7.17 | 9.59 | 11.78 | 0.48 | 2.56 | 0.0000 | 1.0000 | 0 | 0 |
| o2-snapshot-250ms-S2 | 2 | 8320 | 7.20 | 9.49 | 11.54 | 0.47 | 2.62 | 0.0000 | 1.0000 | 0 | 0 |
| o2-snapshot-250ms-S2 | 3 | 8258 | 7.24 | 9.79 | 12.07 | 0.47 | 2.59 | 0.0000 | 1.0000 | 0 | 0 |
