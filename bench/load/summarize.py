#!/usr/bin/env python3
"""Summarise a bench/load/run.sh result directory as Markdown (no third-party packages)."""
import glob
import os
import re
import statistics
import sys

UNIT = {"us": 1e-3, "ms": 1.0, "s": 1000.0}


def ms(text):
    m = re.fullmatch(r"([\d.]+)(us|ms|s)", text)
    return float(m.group(1)) * UNIT[m.group(2)] if m else float("nan")


def parse_wrk(path):
    text = open(path, encoding="utf-8").read()
    out = {}
    m = re.search(r"Requests/sec:\s+([\d.]+)", text)
    out["rps"] = float(m.group(1)) if m else float("nan")
    for pct in ("50", "75", "90", "99"):
        m = re.search(rf"^\s+{pct}%\s+(\S+)", text, re.M)
        out["p" + pct] = ms(m.group(1)) if m else float("nan")
    m = re.search(r"Latency\s+(\S+)\s+(\S+)\s+(\S+)", text)
    out["max"] = ms(m.group(3)) if m else float("nan")
    m = re.search(r"(\d+) requests in", text)
    out["requests"] = int(m.group(1)) if m else 0
    m = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", text)
    out["non2xx"] = int(m.group(1)) if m else 0
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
    out["socket_errors"] = sum(int(g) for g in m.groups()) if m else 0
    return out


def gateway_gauges(path):
    """The gateway's own view right after a run (window = the last seconds of the run)."""
    values = {}
    if not os.path.exists(path):
        return values
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        name, _, value = line.rpartition(" ")
        values[name] = value
    return values


def num(values, key):
    try:
        return float(values[key])
    except (KeyError, ValueError):
        return float("nan")


def fmt(value, digits=2):
    return "n/a" if value != value else f"{value:.{digits}f}"


def main(directory):
    env = open(os.path.join(directory, "environment.txt"), encoding="utf-8").read()
    names = sorted({re.sub(r"\.run\d+\.wrk\.txt$", "", os.path.basename(p))
                    for p in glob.glob(os.path.join(directory, "*.wrk.txt"))})
    print("# Load test result\n")
    print("```text\n" + env.strip() + "\n```\n")
    print("## Throughput and latency seen by wrk (client side)\n")
    print("| scenario | runs | req/s per run | median req/s | p50 ms | p90 ms | p99 ms | max ms | non-2xx | socket errors |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    gateway_rows = []
    for name in names:
        runs = [parse_wrk(p) for p in sorted(glob.glob(os.path.join(directory, name + ".run*.wrk.txt")))]
        rps = [r["rps"] for r in runs]
        med = lambda key: statistics.median(r[key] for r in runs)
        print(f"| {name} | {len(runs)} | {', '.join(f'{x:.0f}' for x in rps)} | {statistics.median(rps):.0f} | "
              f"{fmt(med('p50'))} | {fmt(med('p90'))} | {fmt(med('p99'))} | {fmt(max(r['max'] for r in runs))} | "
              f"{sum(r['non2xx'] for r in runs)} | {sum(r['socket_errors'] for r in runs)} |")
        for i, path in enumerate(sorted(glob.glob(os.path.join(directory, name + ".run*.metrics.txt"))), 1):
            g = gateway_gauges(path)
            gateway_rows.append((name, i, g))
    print("\nwrk reports the latency a client sees (connection included); the gateway's own figures below start when "
          "the request reaches its handler and end when the response is ready.\n")
    print("## The gateway's own metrics right after each run (`/metrics`, 10 s window)\n")
    print("| scenario | run | req/s (gauge) | p50 ms | p95 ms | p99 ms | backend p50 ms | backend p99 ms | 5xx ratio | cache hit ratio | circuit trips | retries |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name, i, g in gateway_rows:
        q = lambda base, quantile: num(g, f'{base}{{quantile="{quantile}"}}') * 1000.0
        print(f"| {name} | {i} | {fmt(num(g, 'edgeflow_http_requests_per_second'), 0)} | "
              f"{fmt(q('edgeflow_http_request_latency_seconds', '0.5'))} | {fmt(q('edgeflow_http_request_latency_seconds', '0.95'))} | "
              f"{fmt(q('edgeflow_http_request_latency_seconds', '0.99'))} | "
              f"{fmt(q('edgeflow_backend_latency_window_seconds', '0.5'))} | {fmt(q('edgeflow_backend_latency_window_seconds', '0.99'))} | "
              f"{fmt(num(g, 'edgeflow_http_error_ratio'), 4)} | {fmt(num(g, 'edgeflow_cache_hit_ratio'), 4)} | "
              f"{fmt(num(g, 'edgeflow_circuit_breaker_trips_total'), 0)} | {fmt(num(g, 'edgeflow_retries_total'), 0)} |")


if __name__ == "__main__":
    main(sys.argv[1])
