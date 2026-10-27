#!/usr/bin/env bash
# A guided, reproducible terminal demonstration of EdgeFlow for an interviewer.
#
#   demo/demo.sh                 run every step; every command is printed before it runs
#   DEMO_PAUSE=1 demo/demo.sh    wait for Enter between steps (live presentation)
#   demo/demo.sh clean           remove everything the demo started
#
# Needs: Docker with Compose, curl, bash and python3 (only to pretty-print a little JSON). The images (EdgeFlow, PostgreSQL, Redis, python:3.12-slim) are pulled/built
# on first use (`docker compose build` takes a few minutes the first time: it compiles EdgeFlow and runs its tests).
# Everything printed below is produced by the real system; nothing is simulated.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

PORT="${EDGEFLOW_HTTP_PORT:-8080}"
GW="http://127.0.0.1:$PORT"
NET="edgeflow_default"
PAUSE="${DEMO_PAUSE:-0}"

backends() { echo demo-a demo-b demo-c; }

clean() {
  docker rm -f $(backends) >/dev/null 2>&1
  docker compose down -v >/dev/null 2>&1
}
if [ "${1:-}" = clean ]; then clean; echo "demo removed"; exit 0; fi

say()  { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
note() { printf '\033[2m   %s\033[0m\n' "$*"; }
run()  { printf '\033[1;33m$ %s\033[0m\n' "$*"; bash -c "$*"; }
next() { if [ "$PAUSE" = 1 ]; then read -r -p "   [Enter to continue] " _; fi; }
wait_for() {  # wait_for <description> <seconds> <command...>
  local what="$1" limit="$2"; shift 2
  for _ in $(seq "$limit"); do "$@" >/dev/null 2>&1 && return 0; sleep 1; done
  echo "timed out waiting for: $what"; return 1
}
has_routable() { [ "$(routable)" = "$1" ]; }
routable() { curl -s "$GW/services/demo/routable" | sed -E 's/.*"count":([0-9]+).*/\1/'; }
register() { curl -s -o /dev/null -w "%{http_code}" -X POST "$GW/services/demo/instances" -H 'Content-Type: application/json' \
  -d "{\"instance_id\":\"$1\",\"host\":\"demo-$1\",\"port\":8080}"; }

say "1. Start PostgreSQL, Redis and EdgeFlow (docker compose), then three demo backends"
note "PostgreSQL = the service registry (source of truth). Redis = response cache + shared rate-limit state."
clean
run "docker compose up -d --build 2>&1 | tail -5"
wait_for "the gateway /health" 90 curl -sf "$GW/health" || exit 1
for n in a b c; do
  docker run -d --name "demo-$n" --network "$NET" -e NAME="$n" -v "$PWD/demo/backend.py:/backend.py:ro" python:3.12-slim python /backend.py >/dev/null
done
note "three backends (demo-a, demo-b, demo-c) are running as containers on the same network"
run "docker compose ps --format 'table {{.Name}}\t{{.Status}}'"
next

say "2. EdgeFlow's startup log: what it connected to and what is switched on"
run "docker logs edgeflow 2>&1 | grep -E 'registry ready|health checking|reverse proxy|reliability|redis ready|cache|rate limit|metrics ready|listening|routing' | cut -c1-200"
next

say "3. Register the backends in the service registry (PostgreSQL) and see them become routable"
for n in a b c; do run "curl -s -o /dev/null -w 'register demo-$n -> HTTP %{http_code}\n' -X POST $GW/services/demo/instances -H 'Content-Type: application/json' -d '{\"instance_id\":\"$n\",\"host\":\"demo-$n\",\"port\":8080}'"; done
note "a new instance is probed by the health checker; two good probes (about 4 s) make it routable"
wait_for "3 routable instances" 40 has_routable 3
run "curl -s $GW/services/demo/routable | python3 -m json.tool 2>/dev/null | grep -E 'count|instance_id|health_status' || curl -s $GW/services/demo/routable"
next

say "4. A normal request through the gateway: /proxy/<service>/<path> is forwarded to a healthy backend"
run "curl -si $GW/proxy/demo/hello | grep -iE '^HTTP|^X-Request-Id|^X-Cache|^\{'"
next

say "5. Load balancing (round robin): six requests, three backends"
note "Cache-Control: no-cache makes every request reach a backend (the cache is shown in step 8)"
run "for i in 1 2 3 4 5 6; do curl -s -H 'Cache-Control: no-cache' $GW/proxy/demo/hello/\$i; echo; done"
next

say "6. Health checking: stop backend demo-b; EdgeFlow notices and stops routing to it"
run "docker stop demo-b >/dev/null && echo 'demo-b stopped'"
note "failure_threshold is 2 and the probe interval 2 s: a few seconds to be marked unhealthy"
for i in 1 2 3 4 5 6 7 8 9 10; do printf '   t=%2ss routable instances: %s\n' "$((i * 2))" "$(routable)"; [ "$(routable)" = 2 ] && break; sleep 2; done
run "curl -s $GW/services/demo/instances | python3 -c \"import sys,json;[print(i['instance_id'], i['health_status']) for i in json.load(sys.stdin)['instances']]\""
run "for i in 1 2 3 4 5 6; do curl -s -H 'Cache-Control: no-cache' $GW/proxy/demo/hello/\$i; echo; done"
note "no request went to demo-b, and no client saw an error"
next

say "7. Retry + failover + circuit breaker: kill a backend and send requests IMMEDIATELY (before health checking notices)"
run "docker start demo-b >/dev/null && echo 'demo-b started'"
wait_for "3 routable instances" 40 has_routable 3
run "docker stop demo-c >/dev/null && echo 'demo-c stopped (the gateway has not noticed yet)'"
run "for i in 1 2 3 4 5 6 7 8; do curl -s -o /dev/null -w '%{http_code} ' -H 'Cache-Control: no-cache' $GW/proxy/demo/x\$i; done; echo"
note "all 200: a request that hit the dead backend was retried on another one (failover)"
run "curl -s $GW/metrics | grep -E '^edgeflow_(retries_total|failovers_total|circuit_breaker_trips_total|circuit_breaker_rejections_total|backend_attempts_total\{outcome=\"not_sent\"\})'"
note "after 3 failures the dead backend's circuit opened: later requests skip it without even trying"
next

say "8. An erroring backend: demo-a starts answering 503; the circuit breaker isolates it"
run "docker start demo-c >/dev/null; curl -s -X POST -d fail http://\$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' demo-a):8080/__mode; echo"
run "for i in \$(seq 12); do curl -s -o /dev/null -w '%{http_code} ' -H 'Cache-Control: no-cache' $GW/proxy/demo/y\$i; done; echo"
run "curl -s $GW/metrics | grep -E '^edgeflow_(retries_total|failovers_total|circuit_breaker_trips_total|circuit_breaker_rejections_total|backend_attempts_total\{outcome=\"retryable_status\"\})'"
note "clients saw 200 (the 503s were retried on healthy backends); the trip counter shows the circuit of demo-a opened"
run "curl -s -X POST -d ok http://\$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' demo-a):8080/__mode; echo"
next

say "9. Redis response cache: the first request is a MISS (reaches a backend), the second is a HIT (does not)"
sleep 3   # let the per-client rate-limit bucket refill after the earlier steps
run "for i in 1 2 3; do curl -si $GW/proxy/demo/cacheable | grep -iE '^X-Cache|served_at'; echo; done"
note "served_at is the backend's clock: it does not change on a HIT, because the backend was not called"
run "curl -s $GW/metrics | grep -E '^edgeflow_cache_(lookups_total|hit_ratio)|^edgeflow_policy_served_from_cache_total'"
next

say "10. Redis rate limiting: a burst of 60 requests from one client (bucket: 30 tokens, refilling 10 per second)"
run "for i in \$(seq 60); do curl -s -o /dev/null -w '%{http_code}\n' $GW/proxy/demo/cacheable; done | sort | uniq -c"
run "curl -si $GW/proxy/demo/cacheable | grep -iE '^HTTP|^Retry-After|^X-RateLimit'"
note "429 = refused by the gateway: no backend was contacted and no retry budget was used"
sleep 4
next

say "11. Backend recovery: demo-c is back; EdgeFlow's health checker routes to it again"
wait_for "3 routable instances" 40 has_routable 3
run "curl -s $GW/services/demo/routable | grep -o '\"count\":[0-9]*'"
run "for i in 1 2 3 4 5 6; do curl -s -H 'Cache-Control: no-cache' $GW/proxy/demo/z\$i; echo; done"
next

say "12. Observability: GET /metrics (Prometheus text format), the gateway's own measurements"
run "curl -s $GW/metrics | grep -E '^edgeflow_(http_requests_total|http_responses_total|http_request_latency_seconds|http_requests_per_second|http_error_ratio|upstream_connections_(created|reused)_total|routing_snapshots_total|rate_limit_decisions_total|redis_up)'"
next

say "13. Graceful shutdown: SIGTERM while requests are in flight; in-flight requests finish, the exit code is 0"
run "(for i in \$(seq 20); do curl -s -o /dev/null -w '%{http_code} ' -H 'Cache-Control: no-cache' $GW/proxy/demo/s\$i; done; echo) & sleep 0.3; docker stop -t 10 edgeflow >/dev/null; wait; echo 'docker stop returned'"
run "docker inspect edgeflow --format 'exit code: {{.State.ExitCode}}  (0 = clean shutdown)'"
run "docker logs edgeflow 2>&1 | tail -12 | cut -c1-200"
next

say "Cleanup: docker compose down -v and remove the demo backends (demo/demo.sh clean)"
echo "done. (the stack is left stopped; run 'demo/demo.sh clean' to remove it)"
