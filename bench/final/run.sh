#!/usr/bin/env bash
# The final Phase 10 benchmark of a gateway process, with TEN backend instances (see bench/README.md).
#
#   bench/final/run.sh <label> <gateway binary>
#
# Part 1  every routing strategy under load (>= 100,000 requests each) with the number of requests each backend served
# Part 2  dynamic membership and failure injection under load: a backend is killed and restarted, one is paused (hangs),
#         one is added, one is removed, an "erroring" one (health check passes, every request answers 503) is added and
#         removed; availability and latency are recorded, plus a per-second timeline.
#
# Environment: EDGEFLOW_IMAGE (backends), BACKENDS (10), THREADS (2), CONNECTIONS (64), DUR_FAST (20), DUR_LC (75),
# DUR_DYNAMIC (66). The gateway is a native process (so its CPU and memory can be sampled); PostgreSQL, Redis and the
# backends are containers addressed by their bridge IPs. Writes bench/results/<label>/; bench/final/report.py turns
# the raw files into report.md.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKENDS="${BACKENDS:-10}"   # before lib.sh, which defaults to 3
# shellcheck source=../profile/lib.sh
source "$here/../profile/lib.sh"

label="${1:?usage: run.sh <label> <binary>}"; bin="${2:?gateway binary}"
THREADS="${THREADS:-2}"; CONNECTIONS="${CONNECTIONS:-64}"
DUR_FAST="${DUR_FAST:-20}"; DUR_LC="${DUR_LC:-75}"; DUR_DYNAMIC="${DUR_DYNAMIC:-66}"
export BACKENDS BACKEND_CONFIG="$here/backend.yaml"
out="$ROOT_DIR/bench/results/$label"; mkdir -p "$out"
trap 'gateway_stop; services_down' EXIT

services_up
{
  echo "date_utc: $(date -u +%FT%TZ)"; echo "kernel: $(uname -sr)"; echo "cpus: $(nproc)"
  echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"; echo "memory: $(free -h | awk '/^Mem:/{print $2}')"
  echo "wrk: $(wrk --version 2>&1 | head -1)"; echo "backend_image: $IMAGE $(docker image inspect "$IMAGE" --format '{{.Id}}')"
  echo "git_commit: $(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "gateway_binary: $bin"; echo "backends: $BACKENDS"; echo "wrk_threads: $THREADS"; echo "wrk_connections: $CONNECTIONS"
} | tee "$out/environment.txt"

backend_requests() { curl -s "http://$1:8080/metrics" | awk '/^edgeflow_http_requests_total\{kind="local"\}/ {print $2}'; }
metric() { awk -v m="$1" '$1 == m {print $2}' "$2"; }

run_wrk() {  # run_wrk <seconds> <url> <output file> [extra wrk arguments]
  local seconds="$1" url="$2" file="$3"; shift 3
  wrk -t"$THREADS" -c"$CONNECTIONS" -d"${seconds}s" --latency --timeout 15s -s "$here/percentiles.lua" "$@" "$url" > "$file" 2>&1
}

# --------------------------------------------------------------------------------------------- part 1: strategies
WEIGHTS_LIST="1 1 1 1 1 2 2 2 3 3"
for strategy in round_robin weighted least_connections consistent_hashing; do
  echo "== strategy $strategy"
  export STRATEGY="$strategy" ROUTING_OPTIONS="" PROXY_OPTIONS="" SERVER_OPTIONS="" DB_OPTIONS=""
  duration="$DUR_FAST"
  if [ "$strategy" = least_connections ]; then
    duration="$DUR_LC"                    # it needs live counts: one registry write per request, no kept snapshot
  else
    export PROXY_OPTIONS="connection_counting: auto" ROUTING_OPTIONS="snapshot_ttl_ms: 250"
  fi
  [ "$strategy" = weighted ] && export WEIGHTS="$WEIGHTS_LIST" || unset WEIGHTS
  gateway_config "$out/strat-$strategy.gateway.yaml"
  GATEWAY_LOG="$out/strat-$strategy.gateway.log" gateway_start "$bin" "$out/strat-$strategy.gateway.yaml"
  gateway_register
  url="http://127.0.0.1:$GATEWAY_PORT/proxy/bench/"
  wrk -t"$THREADS" -c"$CONNECTIONS" -d5s -H "Cache-Control: no-cache" "$url" >/dev/null     # warm-up
  : > "$out/strat-$strategy.before.txt"; : > "$out/strat-$strategy.after.txt"
  n=0; for ip in "${BACKEND_IPS[@]}"; do n=$((n + 1)); echo "b$n $(backend_requests "$ip")" >> "$out/strat-$strategy.before.txt"; done
  pidstat -u -r -p "$GATEWAY_PID" 1 "$duration" > "$out/strat-$strategy.pidstat.txt" 2>&1 &
  sampler=$!
  ( sleep $((duration / 2)); docker stats --no-stream --format '{{.Name}} cpu={{.CPUPerc}} mem={{.MemUsage}}' > "$out/strat-$strategy.docker-stats-mid.txt" 2>&1 ) &
  run_wrk "$duration" "$url" "$out/strat-$strategy.wrk.txt" -H "Cache-Control: no-cache"
  wait "$sampler" || true
  n=0; for ip in "${BACKEND_IPS[@]}"; do n=$((n + 1)); echo "b$n $(backend_requests "$ip")" >> "$out/strat-$strategy.after.txt"; done
  curl -s "http://127.0.0.1:$GATEWAY_PORT/metrics" > "$out/strat-$strategy.metrics.txt"
  grep -E 'Requests/sec|^requests|^errors' "$out/strat-$strategy.wrk.txt"
  if [ "$strategy" = consistent_hashing ]; then
    # Requests from ONE client address hash to ONE backend (sticky by design). The spread over many keys is the
    # ring's: 100,000 routing decisions for distinct keys through the registry API.
    python3 - "$GATEWAY_PORT" "$out/keys-consistent_hashing.txt" <<'PY'
import collections, json, sys, http.client
port, path = int(sys.argv[1]), sys.argv[2]
c = http.client.HTTPConnection("127.0.0.1", port)
counts = collections.Counter()
for i in range(100000):
    c.request("GET", f"/services/bench/route?key=client-{i}")
    r = c.getresponse()
    counts[json.loads(r.read())["selected"]["instance_id"]] += 1
with open(path, "w") as f:
    for k in sorted(counts, key=lambda s: int(s[1:])):
        f.write(f"{k} {counts[k]}\n")
print("100000 keys over", len(counts), "backends")
PY
  fi
  gateway_stop
  unset WEIGHTS
done

# --------------------------------------------------------------------------- part 2: membership and failures
docker run -d --name "$PREFIX-b11" -v "$here/backend.yaml:/etc/edgeflow/config.yaml:ro" "$IMAGE" >/dev/null
docker run -d --name "$PREFIX-bad" -v "$here/bad_backend.py:/bad.py:ro" python:3.12-slim python /bad.py >/dev/null
NEW_IP="$(container_ip "$PREFIX-b11")"; BAD_IP="$(container_ip "$PREFIX-bad")"
for ip in "$NEW_IP" "$BAD_IP"; do for i in $(seq 40); do curl -sf "http://$ip:8080/health" >/dev/null 2>&1 && break; sleep 0.5; done; done

# dynamic_run <name> <upstream timeout ms> <connect timeout ms>: the whole membership/failure scenario against a fresh gateway process.
dynamic_run() {
  local name="$1" connect="$3"; export UPSTREAM_TIMEOUT_MS="$2"
  echo "== dynamic membership and failure injection: $name (proxy.upstream_timeout_ms $2)"
  export STRATEGY=round_robin ROUTING_OPTIONS="snapshot_ttl_ms: 250" SERVER_OPTIONS="" DB_OPTIONS=""
  export PROXY_OPTIONS="connection_counting: auto
connect_timeout_ms: $connect"
  unset WEIGHTS
  local pre="$out/dyn-$name"
  gateway_config "$pre.gateway.yaml"
  GATEWAY_LOG="$pre.gateway.log" gateway_start "$bin" "$pre.gateway.yaml"
  # Start from the same registry every time: the ten backends and nothing else.
  for id in b1 b2 b3 b4 b5 b6 b7 b8 b9 b10 b11 bad; do
    curl -s -o /dev/null -X DELETE "http://127.0.0.1:$GATEWAY_PORT/services/bench/instances/$id"
  done
  gateway_register
  local base="http://127.0.0.1:$GATEWAY_PORT" url="http://127.0.0.1:$GATEWAY_PORT/proxy/bench/"
  wrk -t"$THREADS" -c"$CONNECTIONS" -d5s -H "Cache-Control: no-cache" "$url" >/dev/null
  register() { curl -s -o /dev/null -w '%{http_code}' -X POST "$base/services/bench/instances" -H 'Content-Type: application/json' \
    -d "{\"instance_id\":\"$1\",\"host\":\"$2\",\"port\":8080}"; }
  : > "$pre.events.txt"
  event() { echo "t=$(( $(date +%s) - t0 ))s $*" | tee -a "$pre.events.txt"; }

  sample() {  # one line per second: time, routable instances, cumulative responses by class, retries, failovers, circuit trips
    echo "t,routable,resp_2xx,resp_4xx,resp_5xx,retries,failovers,circuit_trips,circuit_rejections" > "$pre.timeline.csv"
    while true; do
      local routable m; m="$pre.m.tmp"
      routable="$(curl -s "$base/services/bench/routable" | python3 -c 'import sys,json;print(json.load(sys.stdin)["count"])' 2>/dev/null || echo -)"
      curl -s "$base/metrics" > "$m" || true
      echo "$(( $(date +%s) - t0 )),$routable,$(awk '/^edgeflow_http_responses_total\{class="2xx"\}/{print $2}' "$m"),$(awk '/^edgeflow_http_responses_total\{class="4xx"\}/{print $2}' "$m"),$(awk '/^edgeflow_http_responses_total\{class="5xx"\}/{print $2}' "$m"),$(awk '$1=="edgeflow_retries_total"{print $2}' "$m"),$(awk '$1=="edgeflow_failovers_total"{print $2}' "$m"),$(awk '$1=="edgeflow_circuit_breaker_trips_total"{print $2}' "$m"),$(awk '$1=="edgeflow_circuit_breaker_rejections_total"{print $2}' "$m")" >> "$pre.timeline.csv"
      sleep 1
    done
  }
  at() { local target="$1"; shift; while [ $(( $(date +%s) - t0 )) -lt "$target" ]; do sleep 0.2; done; "$@"; }
  kill_b1()   { docker kill "$PREFIX-b1" >/dev/null; event "KILL b1 (docker kill: connection refused from now on)"; }
  start_b1()  {
    docker start "$PREFIX-b1" >/dev/null; sleep 1; local ip; ip="$(container_ip "$PREFIX-b1")"
    event "START b1 again (address ${ip}; registered ${BACKEND_IPS[0]})"
    if [ "$ip" != "${BACKEND_IPS[0]}" ]; then
      curl -s -o /dev/null -X DELETE "$base/services/bench/instances/b1"; register b1 "$ip" >/dev/null
      BACKEND_IPS[0]="$ip"; event "b1 got a new address: deregistered and registered again"
    fi
  }
  pause_b2()  { docker pause "$PREFIX-b2" >/dev/null; event "PAUSE b2 (accepts connections but never answers)"; }
  unpause_b2(){ docker unpause "$PREFIX-b2" >/dev/null; event "UNPAUSE b2"; }
  add_b11()   { event "ADD b11 -> HTTP $(register b11 "$NEW_IP")"; }
  del_b3()    { event "REMOVE b3 -> HTTP $(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$base/services/bench/instances/b3")"; }
  add_bad()   { event "ADD bad (health check passes, every request answers 503) -> HTTP $(register bad "$BAD_IP")"; }
  del_bad()   { event "REMOVE bad -> HTTP $(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$base/services/bench/instances/bad")"; }
  midstats()  { docker stats --no-stream --format '{{.Name}} cpu={{.CPUPerc}} mem={{.MemUsage}}' > "$pre.docker-stats-mid.txt" 2>&1; }

  t0="$(date +%s)"
  sample & local sampler=$!
  pidstat -u -r -p "$GATEWAY_PID" 1 "$DUR_DYNAMIC" > "$pre.pidstat.txt" 2>&1 &
  local pidsampler=$!
  run_wrk "$DUR_DYNAMIC" "$url" "$pre.wrk.txt" -H "Cache-Control: no-cache" &
  local wrkpid=$!
  at 5 kill_b1; at 15 start_b1; at 22 pause_b2; at 28 midstats; at 34 unpause_b2
  at 40 add_b11; del_b3; at 46 add_bad; at 56 del_bad
  wait "$wrkpid" || true
  wait "$pidsampler" || true
  kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true
  curl -s "$base/metrics" > "$pre.metrics.txt"
  rm -f "$pre.m.tmp"
  curl -s "$base/services/bench/instances" | python3 -c '
import sys, json
for i in json.load(sys.stdin)["instances"]:
    print(i["instance_id"], i["host"], i["health_status"], i["status"])' | sort -V > "$pre.final-instances.txt"
  grep -E 'Requests/sec|^requests|^errors' "$pre.wrk.txt"
  gateway_stop
  docker unpause "$PREFIX-b2" >/dev/null 2>&1 || true; docker start "$PREFIX-b1" >/dev/null 2>&1 || true
  sleep 2
}

dynamic_run 10s 10000 2000
dynamic_run 1s 1000 500
echo "== done: $out (python3 bench/final/report.py $label)"
