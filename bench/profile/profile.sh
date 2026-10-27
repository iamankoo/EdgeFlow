#!/usr/bin/env bash
# CPU, thread, lock, network, PostgreSQL and Redis profile of a gateway under load (Phase 10).
#
#   bench/profile/profile.sh <label> [binary]
#
# The binary should be a Release build with symbols and frame pointers:
#   cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDGEFLOW_BUILD_TESTS=OFF \
#         -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" && cmake --build build-prof --target edgeflow
# Environment: SCENARIO (S1|S2|S3, default S2), PROXY_OPTIONS / ROUTING_OPTIONS / SERVER_OPTIONS / DB_OPTIONS (extra
# configuration lines, see lib.sh), EDGEFLOW_IMAGE (backends), PERF (the perf binary; sampled with cpu-clock because
# virtual machines have no hardware counters), FLAMEGRAPH_DIR (a clone of github.com/brendangregg/FlameGraph),
# DURATION (seconds of load, default 60). Needs docker, wrk, perf, strace, sysstat, python3, root (sudo) for perf/strace.
#
# Result: bench/results/<label>/ with cpu-top.txt, cpu.svg (flamegraph), threads.txt, locks.txt, network.txt,
# postgres.txt, redis.txt, wrk.txt, metrics.txt, rss.txt.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$here/lib.sh"

label="${1:?usage: profile.sh <label> [binary]}"
bin="${2:-$ROOT_DIR/build-prof/src/edgeflow}"
SCENARIO="${SCENARIO:-S2}"; DURATION="${DURATION:-60}"
PERF="${PERF:-perf}"; FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/opt/FlameGraph}"
out="$ROOT_DIR/bench/results/$label"; mkdir -p "$out"
trap 'gateway_stop; services_down' EXIT

services_up
gateway_config "$out/gateway.yaml"
GATEWAY_LOG="$out/gateway.log" gateway_start "$bin" "$out/gateway.yaml"
gateway_register
url="$(scenario_url "$SCENARIO")"; header="$(scenario_header "$SCENARIO")"
args=(); [ -n "$header" ] && args=(-H "$header")
wrk -t2 -c64 -d5s "${args[@]}" "$url" >/dev/null           # warm-up

docker exec "$PREFIX-pg" psql -U edgeflow -d edgeflow -qAt -c 'select pg_stat_statements_reset();' >/dev/null
docker exec "$PREFIX-redis" redis-cli config resetstat >/dev/null
grep -E 'VmRSS|VmHWM|Threads' "/proc/$GATEWAY_PID/status" | tr '\n' ' ' > "$out/rss.txt"; echo "fds=$(ls "/proc/$GATEWAY_PID/fd" | wc -l) (before)" >> "$out/rss.txt"

wrk -t2 -c64 -d"${DURATION}s" --latency "${args[@]}" "$url" > "$out/wrk.txt" 2>&1 &
wrk_pid=$!
sleep 6

echo "== CPU: perf record (cpu-clock, 499 Hz, call graphs) for 15 s"
sudo "$PERF" record -e cpu-clock -F 499 -g -p "$GATEWAY_PID" -o "$out/perf.data" -- sleep 15 >/dev/null 2>&1
sudo "$PERF" report -i "$out/perf.data" --no-children --stdio --sort symbol -g none --percent-limit 0.8 2>/dev/null \
  | grep -v '^#' | grep -v '^$' | sed 's/  */ /g' | cut -c1-120 > "$out/cpu-top.txt"
if [ -d "$FLAMEGRAPH_DIR" ]; then
  sudo "$PERF" script -i "$out/perf.data" 2>/dev/null | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" > "$out/folded.txt"
  "$FLAMEGRAPH_DIR/flamegraph.pl" --title "EdgeFlow $SCENARIO $label (cpu-clock)" "$out/folded.txt" > "$out/cpu.svg"
fi
sudo rm -f "$out/perf.data"

echo "== locks: strace -c -f of the gateway for 8 s (futex = mutex / condition-variable waits)"
sudo timeout 8 strace -f -c -p "$GATEWAY_PID" -o "$out/locks.txt" >/dev/null 2>&1 || true

echo "== threads and the machine"
{
  echo "-- per-thread CPU of the gateway (pidstat -t, 5 s; wait = runnable but not running)"
  pidstat -t -p "$GATEWAY_PID" 1 5 2>/dev/null | grep Average | awk '{printf "tid=%-8s usr=%-6s sys=%-6s wait=%-6s cpu=%-6s %s\n", $4, $5, $6, $8, $9, $NF}'
  echo; echo "-- the whole machine (mpstat, 5 s): %usr %nice %sys %iowait %irq %soft %steal %guest %gnice %idle"
  mpstat 1 5 | tail -1
  echo; echo "-- CPU by program (pidstat -u, 5 s, % of one CPU, summed per command)"
  pidstat -u 1 5 2>/dev/null | grep Average | awk 'NR>1{a[$NF]+=$8} END{for(k in a) printf "%-16s %6.1f\n", k, a[k]}' | sort -k2 -nr | head -8
} > "$out/threads.txt"

echo "== PostgreSQL waits while loaded"
{
  for i in 1 2 3 4 5 6 7 8; do
    docker exec "$PREFIX-pg" psql -U edgeflow -d edgeflow -qAt \
      -c "select coalesce(wait_event_type,'none')||':'||coalesce(wait_event,'-')||' ('||state||')' from pg_stat_activity where datname='edgeflow' and pid<>pg_backend_pid();"
    sleep 0.5
  done | sort | uniq -c | sort -rn | head -8
} > "$out/postgres-waits.txt"

echo "== network"
{
  echo "-- ss -s"; ss -s | sed -n 2,3p
  echo "-- TCP counters since the namespace started (nstat)"
  nstat -az 2>/dev/null | grep -E 'TcpRetransSegs|TcpExtTCPTimeouts|TcpExtListenDrops|TcpExtListenOverflows|TcpOutRsts|TcpExtDelayedACKs' | awk '{printf "%-28s %s\n", $1, $2}'
} > "$out/network.txt"

wait "$wrk_pid" || true
grep -E 'Requests/sec|requests in|50%|75%|90%|99%|Non-2xx|Socket errors' "$out/wrk.txt" > "$out/wrk-summary.txt"
curl -s "http://127.0.0.1:$GATEWAY_PORT/metrics" > "$out/metrics.txt"
{ grep -E 'VmRSS|VmHWM|Threads' "/proc/$GATEWAY_PID/status" | tr '\n' ' '; echo "fds=$(ls "/proc/$GATEWAY_PID/fd" | wc -l) (after)"; } >> "$out/rss.txt"

docker exec -i "$PREFIX-pg" psql -U edgeflow -d edgeflow -P pager=off > "$out/postgres.txt" <<'SQL'
select calls, round(total_exec_time::numeric,0) as total_ms, round(mean_exec_time::numeric,3) as mean_ms,
       round((100*total_exec_time/sum(total_exec_time) over ())::numeric,1) as pct,
       left(replace(query, chr(10), ' '), 110) as query
from pg_stat_statements where dbid=(select oid from pg_database where datname='edgeflow')
order by total_exec_time desc limit 8;
SQL
{ docker exec "$PREFIX-redis" redis-cli info commandstats | grep -E 'cmdstat_(evalsha|eval|get|set|hget|hmget|hset|pexpire|script)' ;
  docker exec "$PREFIX-redis" redis-cli info stats | grep -E 'total_commands_processed|instantaneous_ops_per_sec|total_connections_received|rejected_connections' ;
  docker exec "$PREFIX-redis" redis-cli slowlog len | sed 's/^/slowlog_entries: /'; } > "$out/redis.txt"

echo "== done: $out"
cat "$out/wrk-summary.txt"
