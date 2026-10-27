#!/usr/bin/env bash
# Dependency-failure and shutdown behaviour of a real gateway process (Phase 10). Output: bench/results/<label>/failures.txt
#
#   bench/hardening/failures.sh <label> <gateway binary>
#
# Checks (each prints PASS or FAIL with the observed values):
#   1. Redis stops          -> proxied requests keep succeeding (cache bypassed, limiter fails open), edgeflow_redis_up = 0
#   2. Redis comes back     -> edgeflow_redis_up = 1 and the cache serves again
#   3. PostgreSQL stops     -> the cached answer is still served, uncached requests get 503 (not a hang), /health stays 200
#   4. PostgreSQL comes back-> proxied requests succeed again without restarting the gateway
#   5. SIGTERM with a request in flight (a backend that takes 3 s) -> that request completes with 200, the process exits 0
#      within the grace period, and no new connection is accepted afterwards
#   6. file descriptors after all of the above = before (no leak)
# Needs docker, python3, curl. PostgreSQL, Redis and the backends run as containers (EDGEFLOW_IMAGE for the image of the
# throwaway backend, python:3.12-slim for the slow one).
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKENDS=2
source "$here/../profile/lib.sh"
label="${1:?usage: failures.sh <label> <binary>}"; bin="${2:?binary}"
out="$ROOT_DIR/bench/results/$label"; mkdir -p "$out"; : > "$out/failures.txt"
trap 'gateway_stop; services_down; docker rm -f "$PREFIX-slow" >/dev/null 2>&1' EXIT
fail=0
say() { echo "$*" | tee -a "$out/failures.txt"; }
check() {  # check <name> <condition result 0/1> <detail>
  if [ "$2" = 0 ]; then say "PASS  $1  $3"; else say "FAIL  $1  $3"; fail=1; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' --max-time "${2:-10}" "$1" 2>/dev/null; }
ncode() { curl -s -o /dev/null -w '%{http_code}' --max-time "${2:-10}" -H 'Cache-Control: no-cache' "$1" 2>/dev/null; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN {printf "%.2f", a - b}'; }
now() { date +%s.%N; }
below() { awk -v a="$1" -v b="$2" 'BEGIN {exit !(a < b)}'; }
metric() { curl -s "http://127.0.0.1:$GATEWAY_PORT/metrics" | awk -v m="$1" '$1 == m {print $2}'; }
all200() {  # all200 <url> <n> [header]: how many of n requests answered 200
  local ok=0 i; for i in $(seq "$2"); do [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 ${3:+-H "$3"} "$1")" = 200 ] && ok=$((ok + 1)); done; echo "$ok"
}

services_up
export STRATEGY=round_robin PROXY_OPTIONS="connection_counting: auto" ROUTING_OPTIONS="snapshot_ttl_ms: 250" SERVER_OPTIONS="" DB_OPTIONS=""
gateway_config "$out/gateway.yaml"
sed -i 's/^  log_level: warn/  log_level: info/' "$out/gateway.yaml"
# short Redis timeouts so that an outage is noticed quickly, as in config.compose.yaml
sed -i 's/^  io_threads: 1$/  io_threads: 1\n  connect_timeout_ms: 250\n  command_timeout_ms: 250\n  retry_interval_ms: 1000/' "$out/gateway.yaml"
GATEWAY_LOG="$out/gateway.log" gateway_start "$bin" "$out/gateway.yaml" || exit 1
gateway_register
base="http://127.0.0.1:$GATEWAY_PORT"
fds_before="$(ls /proc/$GATEWAY_PID/fd | wc -l)"
say "gateway pid $GATEWAY_PID, fds before: $fds_before"

NC='Cache-Control: no-cache'
n="$(all200 "$base/proxy/bench/" 20 "$NC")"; check "baseline: 20 uncached requests" "$([ "$n" = 20 ]; echo $?)" "$n/20 answered 200"
curl -s -o /dev/null "$base/proxy/bench/health"; sleep 0.5
hit="$(curl -si "$base/proxy/bench/health" | grep -i '^x-cache' | tr -d '\r')"; check "baseline: cache HIT" "$([ "$hit" = "X-Cache: HIT" ]; echo $?)" "$hit"

say "--- 1. Redis stops"
docker stop "$PREFIX-redis" >/dev/null
sleep 1
start="$(now)"
n="$(all200 "$base/proxy/bench/" 50 "$NC")"; took="$(secs "$(now)" "$start")"
check "Redis down: uncached requests still served" "$([ "$n" = 50 ]; echo $?)" "$n/50 answered 200 in ${took}s"
n="$(all200 "$base/proxy/bench/health" 20)"; check "Redis down: cacheable URL is served by the backend instead" "$([ "$n" = 20 ]; echo $?)" "$n/20 answered 200"
up="$(metric edgeflow_redis_up)"; check "Redis down: edgeflow_redis_up" "$([ "$up" = 0 ]; echo $?)" "edgeflow_redis_up $up"

say "--- 2. Redis comes back"
docker start "$PREFIX-redis" >/dev/null
for i in $(seq 30); do [ "$(metric edgeflow_redis_up)" = 1 ] && break; curl -s -o /dev/null "$base/proxy/bench/health"; sleep 1; done
up="$(metric edgeflow_redis_up)"; check "Redis back: edgeflow_redis_up" "$([ "$up" = 1 ]; echo $?)" "edgeflow_redis_up $up after ${i}s"
curl -s -o /dev/null "$base/proxy/bench/health"; sleep 0.5
hit="$(curl -si "$base/proxy/bench/health" | grep -i '^x-cache' | tr -d '\r')"; check "Redis back: cache serves again" "$([ "$hit" = "X-Cache: HIT" ]; echo $?)" "$hit"

say "--- 3. PostgreSQL stops"
curl -s -o /dev/null "$base/proxy/bench/health?pgtest=1"; sleep 0.5      # now in the Redis cache
docker stop "$PREFIX-pg" >/dev/null
sleep 1
c="$(code "$base/proxy/bench/health?pgtest=1")"; hit="$(curl -si "$base/proxy/bench/health?pgtest=1" | grep -i '^x-cache' | tr -d '\r')"
check "PostgreSQL down: a cached answer is still served" "$([ "$c" = 200 ] && [ "$hit" = "X-Cache: HIT" ]; echo $?)" "HTTP $c, $hit"
# Burst 1: 50 concurrent uncached requests while the database host does not answer. Nothing is known yet, so they
# wait together for ONE connect timeout (5 s) and are all answered 503; they do not queue one after another.
# Burst 2, straight after: the pool now knows the host is unreachable and refuses at once.
burst() {  # burst <file>
  seq 50 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code} %{time_total}
' --max-time 30 -H "$NC" "$base/proxy/bench/never-cached" > "$1"
}
start="$(now)"; burst "$out/pgdown1.times"; took1="$(secs "$(now)" "$start")"
start="$(now)"; burst "$out/pgdown2.times"; took2="$(secs "$(now)" "$start")"
n503() { awk '$1 == 503 {n++} END {print n + 0}' "$1"; }
quick() { awk '$2 < 1.0 {n++} END {print n + 0}' "$1"; }
check "PostgreSQL down: burst 1, 50 concurrent uncached requests, all answered 503 within one connect timeout" "$([ "$(n503 "$out/pgdown1.times")" = 50 ] && below "$took1" 8; echo $?)" "$(n503 "$out/pgdown1.times")/50 were 503, all done in ${took1}s"
check "PostgreSQL down: burst 2 fails at once instead of waiting again" "$([ "$(n503 "$out/pgdown2.times")" = 50 ] && [ "$(quick "$out/pgdown2.times")" -ge 45 ]; echo $?)" "$(n503 "$out/pgdown2.times")/50 were 503, $(quick "$out/pgdown2.times")/50 in under 1 s, all done in ${took2}s"
c="$(code "$base/health")"; check "PostgreSQL down: /health still answers" "$([ "$c" = 200 ]; echo $?)" "HTTP $c"
c="$(code "$base/services")"; check "PostgreSQL down: the registry API answers 503, not a hang" "$([ "$c" = 503 ]; echo $?)" "HTTP $c"

say "--- 4. PostgreSQL comes back"
docker start "$PREFIX-pg" >/dev/null
for i in $(seq 40); do [ "$(ncode "$base/proxy/bench/" 5)" = 200 ] && break; sleep 1; done
n="$(all200 "$base/proxy/bench/" 30 "$NC")"
check "PostgreSQL back: proxying works again with no gateway restart" "$([ "$n" = 30 ]; echo $?)" "$n/30 answered 200, first success after ${i}s"
c="$(code "$base/services")"; check "PostgreSQL back: the registry API answers 200" "$([ "$c" = 200 ]; echo $?)" "HTTP $c"

say "--- 5. SIGTERM with a request in flight"
docker run -d --name "$PREFIX-slow" -e NAME=slow -v "$ROOT_DIR/demo/backend.py:/backend.py:ro" python:3.12-slim python /backend.py >/dev/null
slow_ip="$(container_ip "$PREFIX-slow")"
for i in $(seq 30); do curl -sf "http://$slow_ip:8080/health" >/dev/null 2>&1 && break; sleep 0.5; done
curl -s -X POST -d slow "http://$slow_ip:8080/__mode" >/dev/null
curl -s -o /dev/null -X POST "$base/services/slowsvc/instances" -H 'Content-Type: application/json' -d "{\"instance_id\":\"s\",\"host\":\"$slow_ip\",\"port\":8080}"
for i in $(seq 40); do [ "$(curl -s "$base/services/slowsvc/routable" | python3 -c 'import sys,json;print(json.load(sys.stdin)["count"])' 2>/dev/null)" = 1 ] && break; sleep 0.5; done
( curl -s -o "$out/inflight.body" -w '%{http_code} %{time_total}\n' --max-time 20 -H "$NC" "$base/proxy/slowsvc/x" > "$out/inflight.result" ) &
curl_pid=$!
sleep 1
t0="$(now)"; kill -TERM "$GATEWAY_PID"
sleep 0.3
refused="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$base/health" 2>/dev/null)"
wait "$curl_pid"
for i in $(seq 100); do kill -0 "$GATEWAY_PID" 2>/dev/null || break; sleep 0.1; done
exit_code=0; wait "$GATEWAY_PID" 2>/dev/null; exit_code=$?
took="$(secs "$(now)" "$t0")"; GATEWAY_PID=""
read -r st tt < "$out/inflight.result"
check "SIGTERM: the in-flight request (3 s backend) completed" "$([ "$st" = 200 ] && below 2.5 "$tt"; echo $?)" "HTTP $st after ${tt}s"
check "SIGTERM: no new connection is served while draining" "$([ "$refused" != 200 ]; echo $?)" "GET /health during shutdown -> '${refused:-connection refused}'"
check "SIGTERM: the process exited with code 0 within the grace period" "$([ "$exit_code" = 0 ] && below "$took" 6; echo $?)" "exit code $exit_code, ${took}s after SIGTERM"
say "--- shutdown log"; grep -iE "shutdown|stopp|drain|signal" "$out/gateway.log" | tail -12 | sed -E 's/^\[[^]]*\] //' | tee -a "$out/failures.txt"

# the descriptor check needs a live process: start another gateway, warm every pool up with one outage and recovery,
# then count descriptors before and after five more outages (a leak would grow with the number of outages)
say "--- 6. descriptors"
GATEWAY_LOG="$out/gateway2.log" gateway_start "$bin" "$out/gateway.yaml" || exit 1
outage() {
  docker stop "$PREFIX-redis" >/dev/null; all200 "$base/proxy/bench/" 20 "$NC" >/dev/null; docker start "$PREFIX-redis" >/dev/null; sleep 2
  all200 "$base/proxy/bench/" 20 "$NC" >/dev/null; curl -s -o /dev/null "$base/proxy/bench/health"
}
outage; sleep 3
f0="$(ls /proc/$GATEWAY_PID/fd | wc -l)"
for round in 1 2 3 4 5; do outage; done
sleep 12
f1="$(ls /proc/$GATEWAY_PID/fd | wc -l)"
check "no descriptor leak across five more Redis outages (after one warm-up outage)" "$([ "$f1" -le "$((f0 + 2))" ]; echo $?)" "fds $f0 -> $f1"

say ""
[ "$fail" = 0 ] && say "ALL CHECKS PASSED" || say "SOME CHECKS FAILED"
exit "$fail"
