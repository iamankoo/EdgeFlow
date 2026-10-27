#!/usr/bin/env bash
# Malformed input + resource exhaustion against a real gateway (Phase 10). Output: bench/results/<label>/abuse.txt
#
#   bench/hardening/abuse.sh <label> <gateway binary>
# Needs docker (PostgreSQL, Redis and the backends run as containers: EDGEFLOW_IMAGE) and python3.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$here/../profile/lib.sh"
label="${1:?usage: abuse.sh <label> <binary>}"; bin="${2:?binary}"
out="$ROOT_DIR/bench/results/$label"; mkdir -p "$out"
trap 'gateway_stop; services_down' EXIT

services_up
export SERVER_OPTIONS="request_timeout_ms: 2000\nkeep_alive_timeout_ms: 3000\nmax_connections: 200\nmax_header_bytes: 8192"
export PROXY_OPTIONS="" ROUTING_OPTIONS="" DB_OPTIONS=""
gateway_config "$out/gateway.yaml"
GATEWAY_LOG="$out/gateway.log" gateway_start "$bin" "$out/gateway.yaml"
gateway_register
set +e
python3 "$here/abuse.py" --port "$GATEWAY_PORT" --max-connections 200 --request-timeout 2 --keep-alive-timeout 3 \
  --pid "$GATEWAY_PID" | tee "$out/abuse.txt"
status=${PIPESTATUS[0]}
echo "--- gateway log lines at warn/error level during the run" | tee -a "$out/abuse.txt"
grep -E '\[(warn|error|critical)\]' "$out/gateway.log" | sed -E 's/^\[[^]]*\] //' | sort | uniq -c | sort -rn | head -15 | tee -a "$out/abuse.txt"
exit "$status"
