#!/usr/bin/env bash
# Shared helpers of bench/profile/*.sh. Source this file; do not run it.
#
# The services run as Docker containers and are reached by their bridge IP addresses, NOT through
# published ports: a published port goes through the userland `docker-proxy`, which used 72 % of a CPU
# in the first profile and would distort every measurement. The gateway is a native process so that
# perf, strace and heaptrack can attach to it.

PROFILE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$PROFILE_DIR/../.." && pwd)"
PREFIX="${PROFILE_PREFIX:-ef-prof}"
GATEWAY_PORT="${GATEWAY_PORT:-8080}"
IMAGE="${EDGEFLOW_IMAGE:-edgeflow:latest}"
BACKENDS="${BACKENDS:-3}"
DB_PASSWORD="bench_password"

container_ip() { docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$1"; }

services_up() {
  services_down
  docker run -d --name "$PREFIX-pg" -e POSTGRES_USER=edgeflow -e POSTGRES_DB=edgeflow -e POSTGRES_PASSWORD="$DB_PASSWORD" \
    postgres:16 postgres -c fsync=off -c synchronous_commit=off -c max_connections=200 \
    -c shared_preload_libraries=pg_stat_statements -c pg_stat_statements.track=all >/dev/null
  docker run -d --name "$PREFIX-redis" redis:7 redis-server --save "" --appendonly no --maxmemory 256mb \
    --maxmemory-policy volatile-ttl >/dev/null
  # BACKEND_CONFIG: an optional configuration file for the backends (the image's default otherwise).
  local mount=()
  [ -n "${BACKEND_CONFIG:-}" ] && mount=(-v "$BACKEND_CONFIG:/etc/edgeflow/config.yaml:ro")
  for n in $(seq "$BACKENDS"); do docker run -d --name "$PREFIX-b$n" "${mount[@]}" "$IMAGE" >/dev/null; done
  PG_IP="$(container_ip "$PREFIX-pg")"; REDIS_IP="$(container_ip "$PREFIX-redis")"
  for i in $(seq 60); do
    docker exec "$PREFIX-pg" pg_isready -U edgeflow -d edgeflow >/dev/null 2>&1 && break; sleep 1
  done
  docker exec "$PREFIX-pg" psql -U edgeflow -d edgeflow -qc "CREATE EXTENSION IF NOT EXISTS pg_stat_statements;" >/dev/null
  for i in $(seq 30); do [ "$(docker exec "$PREFIX-redis" redis-cli ping 2>/dev/null)" = PONG ] && break; sleep 1; done
  BACKEND_IPS=()
  for n in $(seq "$BACKENDS"); do BACKEND_IPS+=("$(container_ip "$PREFIX-b$n")"); done
  for ip in "${BACKEND_IPS[@]}"; do
    for i in $(seq 30); do curl -sf "http://$ip:8080/health" >/dev/null 2>&1 && break; sleep 1; done
  done
}

services_down() {
  docker ps -aq --filter "name=^${PREFIX}-" | xargs -r docker rm -f >/dev/null 2>&1 || true
}

# gateway_config <output file>: the template with this run's addresses. Optional environment
# variables add lines to a section (use \n to separate lines, two spaces of indentation are added):
#   SERVER_OPTIONS  PROXY_OPTIONS  ROUTING_OPTIONS  DB_OPTIONS
gateway_config() {
  local out="$1"
  local opt
  sed -e "s/@PG_HOST@/$PG_IP/" -e "s/@REDIS_HOST@/$REDIS_IP/" -e "s/^  strategy: round_robin/  strategy: ${STRATEGY:-round_robin}/" \
    -e "s/^  upstream_timeout_ms: 10000/  upstream_timeout_ms: ${UPSTREAM_TIMEOUT_MS:-10000}/" \
    "$PROFILE_DIR/gateway.native.yaml" > "$out"
  for section in SERVER PROXY ROUTING DB; do
    local var="${section}_OPTIONS"; opt="${!var:-}"
    if [ -n "$opt" ]; then
      local text; text="$(printf '%b' "$opt" | sed 's/^/  /')"
      python3 - "$out" "@${section}_OPTIONS@" "$text" <<'PY'
import sys
path, marker, text = sys.argv[1:4]
s = open(path).read()
open(path, "w").write(s.replace("  " + marker, text) if "  " + marker in s else s.replace(marker, text))
PY
    else
      sed -i "/@${section}_OPTIONS@/d" "$out"
    fi
  done
}

# gateway_start <binary> <config>: sets GATEWAY_PID.
gateway_start() {
  local bin="$1" config="$2"
  EDGEFLOW_DB_PASSWORD="$DB_PASSWORD" "$bin" --config "$config" > "${GATEWAY_LOG:-/dev/null}" 2>&1 &
  GATEWAY_PID=$!
  for i in $(seq 40); do curl -sf "http://127.0.0.1:$GATEWAY_PORT/health" >/dev/null 2>&1 && return 0; sleep 0.5; done
  echo "the gateway did not start" >&2; return 1
}

gateway_register() {
  local n=0
  for ip in "${BACKEND_IPS[@]}"; do
    n=$((n + 1))
    # 201 the first time, 409 when an earlier gateway process already registered it (the registry is in PostgreSQL).
    # WEIGHTS: optional space-separated weights, one per backend (weighted routing).
    local weight=1; [ -n "${WEIGHTS:-}" ] && weight="$(echo "$WEIGHTS" | cut -d' ' -f"$n")"
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$GATEWAY_PORT/services/bench/instances" \
      -H 'Content-Type: application/json' -d "{\"instance_id\":\"b$n\",\"host\":\"$ip\",\"port\":8080,\"weight\":$weight}")"
    # An instance of an earlier gateway process is still in the registry: bring its weight up to date.
    [ "$code" = 409 ] && curl -s -o /dev/null -X PATCH "http://127.0.0.1:$GATEWAY_PORT/services/bench/instances/b$n" \
      -H 'Content-Type: application/json' -d "{\"weight\":$weight}"
    case "$code" in 201|409) ;; *) echo "registering b$n failed: HTTP $code" >&2; return 1 ;; esac
  done
  for i in $(seq 60); do
    [ "$(curl -s "http://127.0.0.1:$GATEWAY_PORT/services/bench/routable" | python3 -c 'import sys,json;print(json.load(sys.stdin)["count"])')" = "${#BACKEND_IPS[@]}" ] && break
    sleep 1
  done
  curl -sf "http://127.0.0.1:$GATEWAY_PORT/proxy/bench/" >/dev/null
}

gateway_stop() {
  if [ -n "${GATEWAY_PID:-}" ]; then
    kill -TERM "$GATEWAY_PID" 2>/dev/null || true
    for i in $(seq 20); do kill -0 "$GATEWAY_PID" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$GATEWAY_PID" 2>/dev/null || true
    wait "$GATEWAY_PID" 2>/dev/null || true
    GATEWAY_PID=""
  fi
}

# Voluntary + involuntary context switches of every thread of a process (thread hand-offs show up here).
ctxsw_total() { awk '/ctxt_switches/ {s += $2} END {print s + 0}' /proc/"$1"/task/*/status 2>/dev/null; }

# The URL and extra header of a scenario (same definitions as bench/load/run.sh).
scenario_url() {
  case "$1" in
    S1) echo "http://127.0.0.1:$GATEWAY_PORT/health" ;;
    S2) echo "http://127.0.0.1:$GATEWAY_PORT/proxy/bench/" ;;
    S3) echo "http://127.0.0.1:$GATEWAY_PORT/proxy/bench/health" ;;
    *) echo "unknown scenario $1" >&2; return 1 ;;
  esac
}
scenario_header() { [ "$1" = S2 ] && echo "Cache-Control: no-cache" || true; }
