# EdgeFlow security and production-hardening review (Phase 10)

This is the record of a review of the code, the configuration and the container, what it found, what was changed, how
it was tested, and what is **deliberately not done**. It is written for an operator deciding how to deploy EdgeFlow.
Nothing here is a claim of "secure"; each row says what was checked and what the evidence is.

## Threat model in one paragraph

EdgeFlow terminates plain HTTP/1.1 from untrusted clients and forwards to backends chosen from a registry. The
attacker we design against is an anonymous network client (malformed or abusive requests, slow or numerous
connections, request floods, secrets in URLs) and, secondarily, a caller who can reach the registry API. We do **not**
defend against a hostile registrant with the token, a compromised PostgreSQL/Redis, or an attacker on the network path
(there is no TLS: terminate it in front, see "Not done").

## Findings and what was done

| # | area | finding | action | evidence |
|---|---|---|---|---|
| 1 | **registry API is unauthenticated** | Anyone who can reach the port can register, change or remove instances; because the proxy connects to the registered `host:port`, that is also a way to make the gateway connect to any address it can reach (SSRF-like). | **Fixed (opt-in):** `admin.token_env` names an environment variable with a bearer token; `/services` and below then require `Authorization: Bearer <token>` (constant-time comparison, `401` + `WWW-Authenticate` otherwise, registry untouched). A named-but-empty variable stops the gateway from starting (fail closed). Without it a **warning is logged at every start**. | `AdminAuthTest` (verbs x paths, wrong/short/long/case-variant tokens, other paths stay open, config validation) |
| 2 | secrets committed | none found | scan of the whole tree: no private keys, tokens (`ghp_`, `github_pat`, `AKIA`, `xox*`, `sk-`), `.env` files or PEM/key files tracked; the only password literals are deliberate test fixtures and the documented development defaults below | `grep` scan in the audit; `.gitignore` already excludes `.env*`, `*.pem`, `*.key` |
| 3 | unsafe defaults | the shipped development database password `edgeflow_dev_password` (Compose default) and the benchmark harness password `bench_password` | kept (they protect a throwaway local container), now **documented as development-only** in `docker-compose.yml` and the README; production must set `EDGEFLOW_DB_PASSWORD`. The configuration can never contain the password: only the name of the environment variable (`password_env`); a literal `password:` key is rejected at load | `ConfigManagerTest` (`password:` rejected); config review |
| 4 | unnecessary exposed ports | Compose published `8080` on all interfaces; the registry API was therefore open to the LAN | **Fixed:** `127.0.0.1:${EDGEFLOW_HTTP_PORT:-8080}:8080` by default (`EDGEFLOW_BIND=0.0.0.0` to expose deliberately). PostgreSQL and Redis publish **no** port; the image `EXPOSE`s only 8080 | `docker-compose.yml`; `docker compose ps` in the demo |
| 5 | insecure logging | request targets (path **and query**) were written to the log at `debug`, `info`, `warn` and `error`; a URL such as `?token=...` would leak | **Fixed:** every logged target goes through `network::loggableTarget()`: query replaced by `?<query omitted>`, path cut at 200 characters. Credential headers (`Authorization`, `Cookie`, `X-API-Key`) were never logged; API keys are stored in Redis only as SHA-256; the database/Redis passwords are never logged (only the variable *name*) | `LoggableTargetTest`, `LogRedactionTest` (secrets in query, `Authorization`, `Cookie`, `X-API-Key` across success, backend failure, unknown service, rejected target and local 404: none appears in the debug-level log) |
| 6 | malformed input | request smuggling, bad framing, oversize fields, odd paths | Beast's parser and the size limits answer them; **verified against a running gateway** with raw sockets | `bench/hardening/abuse.sh`: garbage request line 400, HTTP/9.9 400, unknown method 405, 20 KB target 431, header block over `max_header_bytes` 431, 5000 headers 431, `Content-Length` over the limit 413 (without sending the body), conflicting `Content-Length` 400, `Content-Length` + `Transfer-Encoding` 400, negative length 400, NUL in target 400, space in header name 400, bad chunk size 400, `/proxy/../../etc/passwd` 400, `%00`, CRLF-injection attempt in a path 400, giant `X-Forwarded-For` 431 |
| 7 | resource exhaustion | slowloris, idle sockets, connection floods, pipelining floods, abandoned connections | existing limits hold: slow headers get `408` after `request_timeout_ms`; idle keep-alive connections are closed at `keep_alive_timeout_ms`; connections over `max_connections` are closed at once; 3000 pipelined requests in one write are all answered; 500 abandoned connections (a third reset) leave nothing behind | `abuse.sh`: slowloris closed after 2.0 s with `408` (limit 2 s), idle connection closed after 3.0 s (limit 3 s), body that never completes `408` after 2.0 s, 300 idle connections against a limit of 200: 96 of 100 surplus ones closed by the server and `/health` still answered, 3000/3000 pipelined answered in 0.15 s |
| 8 | connection / descriptor leaks | none found | file descriptors before and after all of the above (and after repeated Redis outages in `failures.sh`) are equal; resident memory grew by 0.9 MB over the whole abuse run | `abuse.sh`: `fds 20 -> 20`, rss 15,044 -> 15,900 kB |
| 9 | timeout behaviour | a hung backend ties a client connection for the whole upstream timeout | measured and documented, not a bug: with `proxy.upstream_timeout_ms: 10000` a hung backend stalled the 64 closed-loop connections for ~10 s; with `1000` it cost p99 423 ms. The right value is the slowest legitimate backend response | `docs/PERFORMANCE.md` (failure injection) |
| 10 | interactions of retry, circuit breaker, cache and rate limit | a retry storm must not spend rate-limit tokens or defeat the limiter; cached answers must survive an open circuit; a limited request must not count against a backend | **Verified:** a request takes one token however many attempts it needs; a refused (`429`) request makes no attempt and does not touch retry or circuit counters; a stored answer is served while its backend is dead and its circuit open | `PolicyTest.RetriesInsideOneRequestSpendNoRateLimitTokens`, `ARefusedRequestNeverTouchesTheCircuitBreakerOrTheRetryBudget`, `CachedAnswersKeepBeingServedWhileTheBackendIsDownAndItsCircuitIsOpen` |
| 11 | container | (see below) | non-root user, read-only root filesystem, all capabilities dropped, `no-new-privileges`, **new:** `pids_limit: 256`, `mem_limit: 512m`, small `tmpfs` for `/tmp`, stripped binary, smaller build context | `docker-compose.yml`, `Dockerfile` |
| 12 | shutdown | in-flight requests must finish and the exit status must be clean | verified on a running gateway, see "Dependency failures and shutdown" | `failures.sh` |
| 13 | database outage amplification | an unreachable PostgreSQL host made every request wait out the connect timeout and pin a thread | **Fixed:** fail-fast window after a slow connect failure (above) | `PgPoolUnreachableTest`, `failures.sh` (burst 2: 50/50 `503` in 0.15 s) |

## Dependency failures and shutdown (measured on a running gateway)

`bench/hardening/failures.sh` starts a real gateway process (everything on) with PostgreSQL, Redis and backends as
containers, breaks one thing at a time, and checks what clients see. Result of the committed run
([`bench/results/phase10-failures/failures.txt`](../bench/results/phase10-failures/failures.txt)), **18 of 18 checks passed**:

| situation | observed |
|---|---|
| **Redis stopped** | 50 of 50 uncached requests answered 200 (in 0.63 s in total); the cacheable URL was served by the backend instead of the cache (20/20 200); `edgeflow_redis_up` 0 |
| **Redis back** | `edgeflow_redis_up` 1 after 3 s without restarting the gateway; `X-Cache: HIT` again |
| **PostgreSQL stopped** | a cached answer is still served (`200`, `X-Cache: HIT`); `/health` answers `200`; the registry API answers `503` (not a hang). 50 concurrent uncached requests: all `503`, together in 5.04 s (one connect timeout, they do not queue behind each other); a second burst straight after: 50/50 `503` in 0.15 s |
| **PostgreSQL back** | proxying works again with no restart (30/30 `200`, first success after 2 s), the registry API answers `200` |
| **SIGTERM with a request in flight** | a request to a backend that takes 3 s completed with `200` after 3.00 s; `GET /health` during the drain was refused; the process exited **0**, 2.01 s after SIGTERM (grace period 5 s); the log shows the order `http-server` -> `health-checker` -> `reverse-proxy` -> `redis` -> `signal-handlers` -> `shutdown completed` |
| **descriptors** | 21 before and 21 after five further Redis outages (after one warm-up outage) |

A defect found by this test and fixed in Phase 10: when the database *host* does not answer (packets dropped, as when
a container is stopped), the PostgreSQL pool made **every** request that needed a connection wait out the whole
connect timeout (5 s per request, and a worker thread pinned meanwhile), so an outage multiplied latency by the
queue length. The pool now remembers a *slow* connect failure and refuses further attempts at once for one second,
letting a single thread probe at the end of it (`PgPool`, `PgPoolUnreachableTest`: 20 attempts inside the window cost
nothing, only one of eight concurrent threads waits, a quick refusal never starts a back-off). Redis already had the
same protection (`redis.retry_interval_ms`).


## Container and dependency review

* **Image:** `ubuntu:24.04` runtime with only `libpq5`; the binary is stripped; user `edgeflow` (uid 10001, no shell,
  no home); `HEALTHCHECK` runs `edgeflow --healthcheck` (no curl in the image); one `EXPOSE`.
* **Compose:** `read_only: true`, `cap_drop: [ALL]`, `security_opt: no-new-privileges`, `pids_limit: 256`,
  `mem_limit: 512m` (measured use is ~17 MB and 17 threads), `tmpfs /tmp`, `stop_grace_period` above the shutdown grace
  period, the gateway published on loopback only, PostgreSQL and Redis not published, Redis memory-bounded with
  `volatile-ttl` and no persistence.
* **Dependencies:** C++ libraries are pinned to exact tags by `FetchContent` (spdlog 1.14.1, yaml-cpp 0.8.0,
  nlohmann/json 3.11.3, GoogleTest 1.15.2, Google Benchmark 1.8.5); Boost and libpq come from the distribution.
  Base images are pinned by tag (`ubuntu:24.04`, `postgres:16`, `redis:7`), **not by digest**, and no vulnerability
  scanner was run: do both in a real deployment pipeline.

## Not done, and why

| item | why / what to do instead |
|---|---|
| TLS | out of scope by the locked stack (HTTP/1.1 over plain TCP). Terminate TLS in front (a load balancer, a sidecar, or a reverse proxy) and keep EdgeFlow on a private network. |
| Authentication on `/metrics` | it exposes internals (counts, latencies, Redis state). Bind or firewall it; do not publish it with the proxy port on an untrusted network. |
| Per-client identity behind another proxy | the rate limiter keys on the TCP peer address and **does not trust `X-Forwarded-For`** (spoofable); behind a proxy all clients share one bucket until a trusted-proxy setting exists. |
| Validation of what a registrant may point the proxy at | the registry stores any `host:port`; protect the registry API (`admin.token_env`) and the network. |
| Secrets management | secrets are read from environment variables by name; use your platform's secret store to set them. |
| HTTP request smuggling beyond the parser | relies on Boost.Beast's parser (conflicting framing is rejected, see the checks above); no fuzzing campaign was run. |
