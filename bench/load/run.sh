#!/usr/bin/env bash
# Reproducible load test of EdgeFlow with wrk. See bench/README.md.
#
#   bench/load/run.sh [label]
#
# Environment (all optional):
#   EDGEFLOW_IMAGE   image to run            (default edgeflow:phase10)
#   RUNS             measured runs/scenario  (default 3)
#   DURATION         seconds per run         (default 20)
#   WARMUP           seconds of warm-up      (default 5)
#   THREADS          wrk threads             (default 2)
#   CONNECTIONS      wrk connections         (default 64)
#   KEEP=1           leave the stack running afterwards
#
# Result: bench/results/<label>/ with the environment, every raw wrk output, the gateway's
# /metrics after each run and summary.md.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
label="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
out="$root/bench/results/$label"
RUNS="${RUNS:-3}"; DURATION="${DURATION:-20}"; WARMUP="${WARMUP:-5}"
THREADS="${THREADS:-2}"; CONNECTIONS="${CONNECTIONS:-64}"
export EDGEFLOW_IMAGE="${EDGEFLOW_IMAGE:-edgeflow:phase10}"
compose=(docker compose -f "$here/docker-compose.yml")
gateway=http://127.0.0.1:8080
backend=http://127.0.0.1:9101

for tool in docker wrk curl python3; do
  command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done
mkdir -p "$out"

cleanup() { [ "${KEEP:-0}" = 1 ] || "${compose[@]}" down -v >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "== starting the stack ($EDGEFLOW_IMAGE)"
"${compose[@]}" up -d >/dev/null
for i in $(seq 60); do curl -sf "$gateway/health" >/dev/null 2>&1 && curl -sf "$backend/health" >/dev/null 2>&1 && break; sleep 1; done
curl -sf "$gateway/health" >/dev/null || { echo "the gateway did not come up" >&2; "${compose[@]}" logs gateway | tail -20 >&2; exit 1; }

echo "== registering the backends"
for n in 1 2 3; do
  curl -sf -X POST "$gateway/services/bench/instances" -H 'Content-Type: application/json' \
    -d "{\"instance_id\":\"b$n\",\"host\":\"backend$n\",\"port\":8080}" >/dev/null
done
for i in $(seq 60); do
  [ "$(curl -s "$gateway/services/bench/routable" | python3 -c 'import sys,json;print(json.load(sys.stdin)["count"])')" = 3 ] && break; sleep 1
done
curl -sf "$gateway/proxy/bench/" >/dev/null || { echo "the proxied request failed" >&2; exit 1; }

echo "== environment"
{
  echo "date_utc: $(date -u +%FT%TZ)"
  echo "kernel: $(uname -sr)"
  echo "cpus: $(nproc)"
  echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "memory: $(free -h | awk '/^Mem:/{print $2}')"
  echo "docker: $(docker --version)"
  echo "wrk: $(wrk --version 2>&1 | head -1)"
  echo "image: $EDGEFLOW_IMAGE $(docker image inspect "$EDGEFLOW_IMAGE" --format '{{.Id}}')"
  echo "git_commit: $(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "wrk_threads: $THREADS"; echo "wrk_connections: $CONNECTIONS"
  echo "warmup_seconds: $WARMUP"; echo "run_seconds: $DURATION"; echo "runs: $RUNS"
  echo "gateway_config_sha256: $(sha256sum "$here/gateway.yaml" | cut -d' ' -f1)"
} | tee "$out/environment.txt"

# name | url | extra request header (empty: none)
#   S0 the backend itself (the ceiling of the C++ server for a tiny answer)
#   S1 the gateway's own /health (HTTP server + metrics only)
#   S2 a proxied request that bypasses the cache: rate limiter (Redis), routing (PostgreSQL), proxy, backend
#   S3 a proxied request answered from the Redis cache: rate limiter + cache lookup, no backend
scenarios=(
  "S0-backend-direct|$backend/|"
  "S1-gateway-local|$gateway/health|"
  "S2-proxy-no-cache|$gateway/proxy/bench/|Cache-Control: no-cache"
  "S3-proxy-cache-hit|$gateway/proxy/bench/health|"
)

curl -s "$gateway/proxy/bench/health" >/dev/null   # fill the cache entry of S3
sleep 1

for scenario in "${scenarios[@]}"; do
  IFS='|' read -r name url header <<<"$scenario"
  echo "== $name  ($url)"
  args=()
  [ -n "$header" ] && args=(-H "$header")
  wrk -t"$THREADS" -c"$CONNECTIONS" -d"${WARMUP}s" "${args[@]}" "$url" >/dev/null
  for run in $(seq "$RUNS"); do
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" --latency "${args[@]}" "$url" | tee "$out/$name.run$run.wrk.txt" | grep -E 'Requests/sec|Non-2xx|Socket errors'
    curl -s "$gateway/metrics" > "$out/$name.run$run.metrics.txt"   # the window covers the end of the run
  done
done

python3 "$here/summarize.py" "$out" > "$out/summary.md"
echo "== summary ($out/summary.md)"
cat "$out/summary.md"
