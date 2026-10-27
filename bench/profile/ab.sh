#!/usr/bin/env bash
# A/B comparison of gateway builds and/or configurations under the same load, on the same machine, in the same
# harness (see bench/profile/README.md). Every variant gets a fresh gateway process; services are shared.
#
#   VARIANTS=$'before|/path/to/edgeflow-old||\nafter|/path/to/edgeflow-new|connection_counting: auto|' \
#     bench/profile/ab.sh <label>
#
# A variant is  name|binary|proxy options|routing options|server options|database options  (options may be empty;
# several lines are separated with \n). Environment: EDGEFLOW_IMAGE (backends), SCENARIOS (default "S1 S2 S3"),
# RUNS (3), DURATION (20), WARMUP (5), THREADS (2), CONNECTIONS (64).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$here/lib.sh"

label="${1:?usage: ab.sh <label>}"
: "${VARIANTS:?set VARIANTS (see the header of this file)}"
SCENARIOS="${SCENARIOS:-S1 S2 S3}"; RUNS="${RUNS:-3}"; DURATION="${DURATION:-20}"; WARMUP="${WARMUP:-5}"
THREADS="${THREADS:-2}"; CONNECTIONS="${CONNECTIONS:-64}"
out="$ROOT_DIR/bench/results/$label"; mkdir -p "$out"
trap 'gateway_stop; services_down' EXIT

services_up
{
  echo "date_utc: $(date -u +%FT%TZ)"; echo "kernel: $(uname -sr)"; echo "cpus: $(nproc)"
  echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"; echo "memory: $(free -h | awk '/^Mem:/{print $2}')"
  echo "wrk: $(wrk --version 2>&1 | head -1)"; echo "backend_image: $IMAGE $(docker image inspect "$IMAGE" --format '{{.Id}}')"
  echo "git_commit: $(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "wrk_threads: $THREADS"; echo "wrk_connections: $CONNECTIONS"; echo "warmup_seconds: $WARMUP"
  echo "run_seconds: $DURATION"; echo "runs: $RUNS"; echo "backends: $BACKENDS (EdgeFlow instances)"
  echo "variants:"; printf '%s\n' "$VARIANTS" | sed 's/^/  /'
} | tee "$out/environment.txt"

while IFS='|' read -r name bin proxy_opts routing_opts server_opts db_opts; do
  [ -z "$name" ] && continue
  export PROXY_OPTIONS="$proxy_opts" ROUTING_OPTIONS="$routing_opts" SERVER_OPTIONS="$server_opts" DB_OPTIONS="$db_opts"
  gateway_config "$out/$name.gateway.yaml"
  for scenario in $SCENARIOS; do
    echo "== $name $scenario"
    GATEWAY_LOG="$out/$name.gateway.log" gateway_start "$bin" "$out/$name.gateway.yaml"
    gateway_register
    url="$(scenario_url "$scenario")"; header="$(scenario_header "$scenario")"
    args=(); [ -n "$header" ] && args=(-H "$header")
    curl -s "$url" "${args[@]}" >/dev/null
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"${WARMUP}s" "${args[@]}" "$url" >/dev/null
    for run in $(seq "$RUNS"); do
      before="$(ctxsw_total "$GATEWAY_PID")"
      wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" --latency "${args[@]}" "$url" \
        | tee "$out/$name-$scenario.run$run.wrk.txt" | grep -E 'Requests/sec|Non-2xx|Socket errors'
      after="$(ctxsw_total "$GATEWAY_PID")"
      requests="$(grep -oE '[0-9]+ requests in' "$out/$name-$scenario.run$run.wrk.txt" | cut -d' ' -f1)"
      # Thread hand-offs show up as context switches of the gateway's threads.
      awk -v n="$name-$scenario run$run" -v d="$((after - before))" -v r="$requests" \
        'BEGIN {printf "%-28s %9d context switches / %8d requests = %.2f per request\n", n, d, r, d / r}' >> "$out/ctxsw.txt"
      curl -s "http://127.0.0.1:$GATEWAY_PORT/metrics" > "$out/$name-$scenario.run$run.metrics.txt"
    done
    gateway_stop
  done
done <<<"$VARIANTS"

python3 "$ROOT_DIR/bench/load/summarize.py" "$out" > "$out/summary.md"
cat "$out/summary.md"
