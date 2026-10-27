# EdgeFlow: a ten-minute terminal demonstration

This is the script an interviewer can run, with its **real output**: `demo/demo.sh` was run end to end on the project's
Docker Compose stack (GitHub Codespace, `edgeflow:phase10`, `postgres:16`, `redis:7`, three Python demo backends) and the
output below is that run, unedited except that terminal colour codes were removed. Nothing is simulated. Timestamps
and request ids differ on every run; the behaviour does not.

## Run it yourself

```bash
git clone https://github.com/iamankoo/EdgeFlow && cd EdgeFlow
demo/demo.sh                  # whole session, about 3 minutes once the image is built (the first build takes a few minutes)
DEMO_PAUSE=1 demo/demo.sh     # pause for Enter between steps (for presenting live)
demo/demo.sh clean            # remove the containers and the volume
```

Needs Docker with Compose, `curl`, `bash`, and `python3` (to pretty-print a little JSON). The gateway is published on
`127.0.0.1:8080` only (Phase 10 hardening); every command the script runs is printed before it runs.

## The ninety-second pitch

*"EdgeFlow is an API gateway in C++. Services register in a PostgreSQL registry; EdgeFlow health-checks them and sends
client requests to healthy ones with a choice of four load-balancing strategies. When a backend fails it retries
on another one, and a circuit breaker per backend stops sending it traffic until it recovers. Redis gives it a response
cache and a token-bucket rate limiter shared by all gateway instances. It exposes Prometheus metrics, shuts down
gracefully, and I measured it: about 11.6K requests a second on the path that reaches a backend, 0 failed requests
while backends were killed, hung and added under load, and I found and fixed the bottlenecks with a profiler."*

## What to expect to be asked

* **Why PostgreSQL for the registry and not Redis or etcd?** Durable and transactional, the source of truth; registrations and
  health state survive restarts. The cost (a read per routing decision) was measured and addressed with a bounded 250 ms
  snapshot, explicitly opt-in ([PERFORMANCE.md](PERFORMANCE.md)).
* **What happens if Redis or PostgreSQL goes down?** Redis: the cache is bypassed and the rate limiter fails open (or closed,
  by configuration). PostgreSQL: cached answers keep being served, uncached requests get a fast `503`, no restart needed
  ([SECURITY.md](SECURITY.md), measured).
* **Why is `least_connections` slower?** It needs a live per-backend counter and that lives in PostgreSQL (two writes per request);
  the profile showed those updates were 87 % of PostgreSQL's time. The other strategies skip them (`connection_counting: auto`).
* **What is the weakest part?** No TLS (terminate it in front), `/metrics` is unauthenticated, the registry API is open unless
  `admin.token_env` is set, and the rate limiter trusts the TCP peer address only. All listed in SECURITY.md.

## The session


### Step 1: Start PostgreSQL, Redis and EdgeFlow (docker compose), then three demo backends

**What to say.** Three pieces of infrastructure and one gateway: PostgreSQL is the registry (the source of truth for which instances exist and how healthy they are), Redis holds the cache and the rate-limit buckets, EdgeFlow is the C++ process. The three demo backends are tiny Python servers (`demo/backend.py`) so that we can break them on purpose.

```text
   PostgreSQL = the service registry (source of truth). Redis = response cache + shared rate-limit state.
$ docker compose up -d --build 2>&1 | tail -5
 Container edgeflow-redis Waiting 
 Container edgeflow-redis Healthy 
 Container edgeflow-postgres Healthy 
 Container edgeflow Starting 
 Container edgeflow Started 
   three backends (demo-a, demo-b, demo-c) are running as containers on the same network
$ docker compose ps --format 'table {{.Name}}\t{{.Status}}'
NAME                STATUS
edgeflow            Up 2 seconds (health: starting)
edgeflow-postgres   Up 7 seconds (healthy)
edgeflow-redis      Up 7 seconds (healthy)
```


### Step 2: EdgeFlow's startup log: what it connected to and what is switched on

**What to say.** This is what EdgeFlow reports at startup, and it is worth reading aloud: it applied the schema to PostgreSQL, connected to Redis, and enabled the reverse proxy, the reliability layer (3 attempts, circuit breaker after 3 consecutive failures), the cache, the rate limiter and metrics. It listens on one port.

```text
$ docker logs edgeflow 2>&1 | grep -E 'registry ready|health checking|reverse proxy|reliability|redis ready|cache|rate limit|metrics ready|listening|routing' | cut -c1-200
2026-10-26 21:21:36.566 [info] [edgeflow] service registry ready (PostgreSQL postgres:5432/edgeflow, 1 migration(s) applied, pool size 4), routing strategy round_robin
2026-10-26 21:21:36.567 [info] [edgeflow] redis ready (redis:6379, 4 connection(s), command timeout 250ms)
2026-10-26 21:21:36.567 [info] [edgeflow] reverse proxy ready: /proxy/{service}/..., connect timeout 2000ms, upstream timeout 5000ms, up to 32 idle connection(s) per backend, 2 I/O thread(s), connecti
2026-10-26 21:21:36.567 [info] [edgeflow] reliability ready: total timeout 12000ms, retries on (up to 3 attempt(s), backoff 100-2000ms), circuit breaker on (opens after 3 consecutive failure(s), recov
2026-10-26 21:21:36.567 [info] [edgeflow] gateway policy ready: rate limiting on, response cache on
2026-10-26 21:21:36.567 [info] [edgeflow] metrics ready: GET /metrics (Prometheus text format), 10 s window
2026-10-26 21:21:36.567 [info] [edgeflow] HTTP server listening on 0.0.0.0:8080 (2 worker thread(s))
```


### Step 3: Register the backends in the service registry (PostgreSQL) and see them become routable

**What to say.** Registration is a plain HTTP call. A new instance is not trusted: the health checker probes it (`GET /health`, every 2 s) and only after two good probes does it become *routable*. The registry knows two separate facts, *registered* (`status`) and *healthy* (`health_status`).

```text
$ curl -s -o /dev/null -w 'register demo-a -> HTTP %{http_code}\n' -X POST http://127.0.0.1:8080/services/demo/instances -H 'Content-Type: application/json' -d '{"instance_id":"a","host":"demo-a","port":8080}'
register demo-a -> HTTP 201
$ curl -s -o /dev/null -w 'register demo-b -> HTTP %{http_code}\n' -X POST http://127.0.0.1:8080/services/demo/instances -H 'Content-Type: application/json' -d '{"instance_id":"b","host":"demo-b","port":8080}'
register demo-b -> HTTP 201
$ curl -s -o /dev/null -w 'register demo-c -> HTTP %{http_code}\n' -X POST http://127.0.0.1:8080/services/demo/instances -H 'Content-Type: application/json' -d '{"instance_id":"c","host":"demo-c","port":8080}'
register demo-c -> HTTP 201
   a new instance is probed by the health checker; two good probes (about 4 s) make it routable
$ curl -s http://127.0.0.1:8080/services/demo/routable | python3 -m json.tool 2>/dev/null | grep -E 'count|instance_id|health_status' || curl -s http://127.0.0.1:8080/services/demo/routable
    "count": 3,
            "connection_count": 0,
            "health_status": "healthy",
            "instance_id": "a",
            "connection_count": 0,
            "health_status": "healthy",
            "instance_id": "b",
            "connection_count": 0,
            "health_status": "healthy",
            "instance_id": "c",
```


### Step 4: A normal request through the gateway: /proxy/<service>/<path> is forwarded to a healthy backend

**What to say.** `/proxy/<service>/<path>` is the data path. The prefix is stripped, a backend is chosen, and the answer comes back with an `X-Request-Id` (generated here, propagated to the backend) and `X-Cache: MISS` because nothing was cached yet.

```text
$ curl -si http://127.0.0.1:8080/proxy/demo/hello | grep -iE '^HTTP|^X-Request-Id|^X-Cache|^\{'
HTTP/1.1 200 OK
X-Request-Id: b5a9fda9-8ed3-4fd1-a9da-efdf0535c3c7
X-Cache: MISS
{"backend": "a", "path": "/hello"}
```


### Step 5: Load balancing (round robin): six requests, three backends

**What to say.** Round robin over the three routable instances. `Cache-Control: no-cache` is only there so that every request reaches a backend. The other strategies (least connections, weighted, consistent hashing) are one config line (`routing.strategy`).

```text
   Cache-Control: no-cache makes every request reach a backend (the cache is shown in step 8)
$ for i in 1 2 3 4 5 6; do curl -s -H 'Cache-Control: no-cache' http://127.0.0.1:8080/proxy/demo/hello/$i; echo; done
{"backend": "b", "path": "/hello/1"}
{"backend": "c", "path": "/hello/2"}
{"backend": "a", "path": "/hello/3"}
{"backend": "b", "path": "/hello/4"}
{"backend": "c", "path": "/hello/5"}
{"backend": "a", "path": "/hello/6"}
```


### Step 6: Health checking: stop backend demo-b; EdgeFlow notices and stops routing to it

**What to say.** Health checking is *active*: nobody told EdgeFlow that demo-b died. After `failure_threshold` (2) missed probes (about 8 s here, because of the probe interval and the registry refresh) it is marked unhealthy and leaves the routable set, and the next six requests go only to a and c. The window before that is what step 7 is about.

```text
$ docker stop demo-b >/dev/null && echo 'demo-b stopped'
demo-b stopped
   failure_threshold is 2 and the probe interval 2 s: a few seconds to be marked unhealthy
   t= 2s routable instances: 3
   t= 4s routable instances: 3
   t= 6s routable instances: 3
   t= 8s routable instances: 2
$ curl -s http://127.0.0.1:8080/services/demo/instances | python3 -c "import sys,json;[print(i['instance_id'], i['health_status']) for i in json.load(sys.stdin)['instances']]"
a healthy
b unhealthy
c healthy
$ for i in 1 2 3 4 5 6; do curl -s -H 'Cache-Control: no-cache' http://127.0.0.1:8080/proxy/demo/hello/$i; echo; done
{"backend": "c", "path": "/hello/1"}
{"backend": "a", "path": "/hello/2"}
{"backend": "c", "path": "/hello/3"}
{"backend": "a", "path": "/hello/4"}
{"backend": "c", "path": "/hello/5"}
{"backend": "a", "path": "/hello/6"}
   no request went to demo-b, and no client saw an error
```


### Step 7: Retry + failover + circuit breaker: kill a backend and send requests IMMEDIATELY (before health checking notices)

**What to say.** Health checking is slow on purpose (it has to avoid flapping). Retries and the circuit breaker are the *fast* path: we stopped demo-c and immediately sent eight requests. All eight returned 200: the ones routed to the dead backend failed to connect (`not_sent`: the request provably never reached anything) and were retried on another backend (`failovers_total`). After 3 failures the circuit for that backend opened (`circuit_breaker_trips_total 1`) so the following requests did not even try it.

```text
$ docker start demo-b >/dev/null && echo 'demo-b started'
demo-b started
$ docker stop demo-c >/dev/null && echo 'demo-c stopped (the gateway has not noticed yet)'
demo-c stopped (the gateway has not noticed yet)
$ for i in 1 2 3 4 5 6 7 8; do curl -s -o /dev/null -w '%{http_code} ' -H 'Cache-Control: no-cache' http://127.0.0.1:8080/proxy/demo/x$i; done; echo
200 200 200 200 200 200 200 200 
   all 200: a request that hit the dead backend was retried on another one (failover)
$ curl -s http://127.0.0.1:8080/metrics | grep -E '^edgeflow_(retries_total|failovers_total|circuit_breaker_trips_total|circuit_breaker_rejections_total|backend_attempts_total\{outcome="not_sent"\})'
edgeflow_backend_attempts_total{outcome="not_sent"} 3
edgeflow_retries_total 3
edgeflow_failovers_total 3
edgeflow_circuit_breaker_trips_total 1
edgeflow_circuit_breaker_rejections_total 0
   after 3 failures the dead backend's circuit opened: later requests skip it without even trying
```


### Step 8: An erroring backend: demo-a starts answering 503; the circuit breaker isolates it

**What to say.** A harder failure: a backend that is *up* (its `/health` answers 200) but answers every real request with 503. The health checker cannot see this; real traffic can. The 503s are retried on a healthy backend (clients still got 200s), and three failures open that backend's circuit (trips: 2). By design, after `recovery_timeout_ms` (6 s) the circuit lets one probe request through and closes again if it succeeds (this is covered by the tests; the demo does not wait for it).

```text
$ docker start demo-c >/dev/null; curl -s -X POST -d fail http://$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' demo-a):8080/__mode; echo
{"backend": "a", "mode": "fail"}
$ for i in $(seq 12); do curl -s -o /dev/null -w '%{http_code} ' -H 'Cache-Control: no-cache' http://127.0.0.1:8080/proxy/demo/y$i; done; echo
200 200 200 200 200 200 200 200 200 200 200 200 
$ curl -s http://127.0.0.1:8080/metrics | grep -E '^edgeflow_(retries_total|failovers_total|circuit_breaker_trips_total|circuit_breaker_rejections_total|backend_attempts_total\{outcome="retryable_status"\})'
edgeflow_backend_attempts_total{outcome="retryable_status"} 3
edgeflow_retries_total 6
edgeflow_failovers_total 6
edgeflow_circuit_breaker_trips_total 2
edgeflow_circuit_breaker_rejections_total 0
   clients saw 200 (the 503s were retried on healthy backends); the trip counter shows the circuit of demo-a opened
$ curl -s -X POST -d ok http://$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' demo-a):8080/__mode; echo
{"backend": "a", "mode": "ok"}
```


### Step 9: Redis response cache: the first request is a MISS (reaches a backend), the second is a HIT (does not)

**What to say.** The cache: the first request is a MISS and reaches a backend; the next two are HITs served from Redis. `served_at` is the backend's own clock and does not change, which proves the backend was not called. The metrics count 2 hits and 2 misses (the other miss is the request of step 4), hit ratio 0.5.

```text
$ for i in 1 2 3; do curl -si http://127.0.0.1:8080/proxy/demo/cacheable | grep -iE '^X-Cache|served_at'; echo; done
X-Cache: MISS
{"backend": "b", "path": "/cacheable", "served_at": "21:22:27"}

X-Cache: HIT
{"backend": "b", "path": "/cacheable", "served_at": "21:22:27"}

X-Cache: HIT
{"backend": "b", "path": "/cacheable", "served_at": "21:22:27"}

   served_at is the backend's clock: it does not change on a HIT, because the backend was not called
$ curl -s http://127.0.0.1:8080/metrics | grep -E '^edgeflow_cache_(lookups_total|hit_ratio)|^edgeflow_policy_served_from_cache_total'
edgeflow_policy_served_from_cache_total 2
edgeflow_cache_lookups_total{result="hit"} 2
edgeflow_cache_lookups_total{result="miss"} 2
edgeflow_cache_lookups_total{result="error"} 0
edgeflow_cache_hit_ratio 0.5
```


### Step 10: Redis rate limiting: a burst of 60 requests from one client (bucket: 30 tokens, refilling 10 per second)

**What to say.** Rate limiting is a token bucket per client address (30 tokens, refilled at 10 a second in this demo configuration). 60 requests in a burst: 31 got through (30 tokens plus one refilled while the loop ran) and 29 were refused with `429` and `Retry-After`. The state lives in Redis, so two EdgeFlow instances share one limit. A 429 never reaches a backend and uses no retry budget.

```text
$ for i in $(seq 60); do curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/proxy/demo/cacheable; done | sort | uniq -c
     31 200
     29 429
$ curl -si http://127.0.0.1:8080/proxy/demo/cacheable | grep -iE '^HTTP|^Retry-After|^X-RateLimit'
HTTP/1.1 429 Too Many Requests
Retry-After: 1
X-RateLimit-Scope: ip
   429 = refused by the gateway: no backend was contacted and no retry budget was used
```


### Step 11: Backend recovery: demo-c is back; EdgeFlow's health checker routes to it again

**What to say.** Recovery needs no operator: demo-c came back, the health checker saw two good probes, and it is in the rotation again.

```text
$ curl -s http://127.0.0.1:8080/services/demo/routable | grep -o '"count":[0-9]*'
"count":3
$ for i in 1 2 3 4 5 6; do curl -s -H 'Cache-Control: no-cache' http://127.0.0.1:8080/proxy/demo/z$i; echo; done
{"backend": "c", "path": "/z1"}
{"backend": "a", "path": "/z2"}
{"backend": "b", "path": "/z3"}
{"backend": "c", "path": "/z4"}
{"backend": "a", "path": "/z5"}
{"backend": "b", "path": "/z6"}
```


### Step 12: Observability: GET /metrics (Prometheus text format), the gateway's own measurements

**What to say.** Everything observable is on `/metrics` in Prometheus format. Note the routing counters: 35 routing decisions came from the 250 ms snapshot and 11 from PostgreSQL (the Phase 10 optimization), 38 upstream connections were reused against 8 created, and the rate limiter allowed 73 and limited 30. The p99 of 1.99 s is the window's slowest requests; the connect timeout is 2 s, which is consistent with requests that were first sent to the stopped backend in step 7 and then retried elsewhere (every client still got an answer).

```text
$ curl -s http://127.0.0.1:8080/metrics | grep -E '^edgeflow_(http_requests_total|http_responses_total|http_request_latency_seconds|http_requests_per_second|http_error_ratio|upstream_connections_(created|reused)_total|routing_snapshots_total|rate_limit_decisions_total|redis_up)'
edgeflow_http_requests_total{kind="proxy"} 103
edgeflow_http_requests_total{kind="local"} 36
edgeflow_http_responses_total{class="2xx"} 109
edgeflow_http_responses_total{class="3xx"} 0
edgeflow_http_responses_total{class="4xx"} 30
edgeflow_http_responses_total{class="5xx"} 0
edgeflow_http_responses_total{class="other"} 0
edgeflow_http_requests_per_second 8.1
edgeflow_http_error_ratio 0
edgeflow_http_request_latency_seconds{quantile="0.5"} 0.000236658879
edgeflow_http_request_latency_seconds{quantile="0.95"} 0.0475342534
edgeflow_http_request_latency_seconds{quantile="0.99"} 1.9889485
edgeflow_upstream_connections_created_total 8
edgeflow_upstream_connections_reused_total 38
edgeflow_routing_snapshots_total{source="snapshot"} 35
edgeflow_routing_snapshots_total{source="registry"} 11
edgeflow_rate_limit_decisions_total{result="allowed"} 73
edgeflow_rate_limit_decisions_total{result="limited"} 30
edgeflow_rate_limit_decisions_total{result="degraded_allowed"} 0
edgeflow_rate_limit_decisions_total{result="degraded_rejected"} 0
edgeflow_redis_up 1
```


### Step 13: Graceful shutdown: SIGTERM while requests are in flight; in-flight requests finish, the exit code is 0

**What to say.** `docker stop` sends SIGTERM. EdgeFlow stops accepting, lets the in-flight request finish (`waiting up to 5000ms for 1 connection(s)`), stops its components in reverse dependency order (HTTP server, health checker, proxy, Redis) and exits with code 0. The trailing `000`s are the curl calls that started after the listener closed (connection refused), which is the correct behaviour while draining.

```text
$ (for i in $(seq 20); do curl -s -o /dev/null -w '%{http_code} ' -H 'Cache-Control: no-cache' http://127.0.0.1:8080/proxy/demo/s$i; done; echo) & sleep 0.3; docker stop -t 10 edgeflow >/dev/null; wait; echo 'docker stop returned'
200 200 200 200 200 200 200 200 200 200 200 200 200 200 200 200 200 000 000 000 
docker stop returned
$ docker inspect edgeflow --format 'exit code: {{.State.ExitCode}}  (0 = clean shutdown)'
exit code: 0  (0 = clean shutdown)
$ docker logs edgeflow 2>&1 | tail -12 | cut -c1-200
2026-10-26 21:22:33.184 [info] [edgeflow] shutdown sequence started (grace period 5s)
2026-10-26 21:22:33.184 [info] [edgeflow] stopping component 'http-server'
2026-10-26 21:22:33.184 [info] [edgeflow] HTTP server stopping: no longer accepting connections
2026-10-26 21:22:33.184 [info] [edgeflow] waiting up to 5000ms for 1 connection(s) to finish
2026-10-26 21:22:33.220 [info] [edgeflow] HTTP server stopped (160 connection(s) served, 0 rejected)
2026-10-26 21:22:33.220 [info] [edgeflow] stopping component 'health-checker'
2026-10-26 21:22:33.220 [info] [edgeflow] health checker stopped (77 probe(s) run, 7 state change(s))
2026-10-26 21:22:33.220 [info] [edgeflow] stopping component 'reverse-proxy'
2026-10-26 21:22:33.221 [info] [edgeflow] reverse proxy stopped
2026-10-26 21:22:33.221 [info] [edgeflow] stopping component 'redis'
2026-10-26 21:22:33.221 [info] [edgeflow] stopping component 'signal-handlers'
2026-10-26 21:22:33.221 [info] [edgeflow] shutdown completed
```


## Reproducibility notes

* The demo configuration is `config/config.compose.yaml` (short probe intervals, a 30-token IP bucket, a 6 s circuit
  recovery) so that every effect is visible within seconds; the production-style defaults are in `config/config.yaml`.
* The output above shows the *behaviour*; counts such as "3 retries" depend on which requests meet the dead
  backend and can differ by one or two between runs.
* The run is a single pass; it was not retried or edited. `demo/demo.sh` starts by removing anything a previous run left, so
  it can be repeated.
