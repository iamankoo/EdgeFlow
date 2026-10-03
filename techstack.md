# EdgeFlow — Technology Stack

The stack below is **locked**. No technology may be substituted or added without explicit approval from the project owner. In particular, Kubernetes, Kafka, RabbitMQ and similar infrastructure are out of scope.

| Area | Technology | Status | Why |
|------|-----------|--------|-----|
| Language | C++20 | Phase 1 active | Predictable latency, control over memory and threading, coroutine and concepts support for a systems-level gateway. |
| Build | CMake | Phase 1 active | De facto standard for C++; portable across GCC/Clang, integrates with GoogleTest, Benchmark and CI. |
| Compilers | GCC / Clang | Phase 1 active (CI configured for both) | Both supported to catch portability issues and to use sanitizers and profiling tools. |
| Networking | Boost.Asio | Phase 2 implemented | Mature asynchronous I/O and executor model for TCP; the basis of the concurrency design. |
| HTTP | Boost.Beast (HTTP/1.1 initially) | Phase 2 implemented | HTTP parsing and serialization built on Asio, avoiding a hand-rolled parser. |
| Persistence | PostgreSQL | Phase 3 implemented | Durable, transactional source of truth for service metadata. |
| Shared state / cache | Redis | Planned (Phase 8) | Low-latency shared store for response caching and distributed rate-limit state. |
| Containers | Docker, Docker Compose | Phase 1 active | Reproducible local deployment of the gateway, backends, PostgreSQL and Redis. |
| Unit testing | GoogleTest | Phase 1 active (Phase 2 adds real-socket integration tests) | Standard C++ test framework with CMake/CTest integration. |
| Benchmarking | Google Benchmark | Planned (Phase 9) | Micro-benchmarks for hot paths such as routing and rate limiting. |
| Logging | spdlog | Phase 1 active | Fast, structured-friendly logging with minimal overhead. |
| JSON | nlohmann/json | Phase 2 active (response bodies) | Ergonomic JSON handling for APIs and metadata. |
| Configuration | YAML | Phase 1 active (yaml-cpp) | Human-readable, hierarchical configuration. |
| Metrics | Prometheus-compatible | Planned (Phase 9) | Standard exposition format that works with common tooling. |
| Profiling | perf, Valgrind, flamegraphs | Planned (Phase 10) | CPU, memory and contention analysis to drive evidence-based optimization. |
| Load testing | wrk, custom C++ load generators | Planned (Phase 9) | wrk for raw HTTP throughput; custom tools where wrk cannot model the scenario. |
| CI | GitHub Actions | Phase 1 active | Automated build and test on every push. |

## Load-balancing algorithms (locked)

1. Round Robin
2. Least Connections
3. Weighted Routing
4. Consistent Hashing

All four will be implemented (Phase 5) behind one strategy abstraction so they can be swapped without changing the gateway.

## Implementation choices (within the locked stack)

| Concern | Choice |
|---------|--------|
| YAML library | yaml-cpp 0.8.0 |
| Dependency management | CMake `FetchContent` with pinned tags: spdlog v1.14.1, yaml-cpp 0.8.0, nlohmann/json v3.11.3, GoogleTest v1.15.2 |
| Boost | Taken from the system (`libboost-dev`, minimum 1.83) and required at configure time. Asio and Beast are header-only, so only headers are used and the runtime image needs no Boost packages. Fetching Boost was rejected as slow |
| HTTP | Beast parser/serializer over plain TCP (`tcp::socket`); no TLS and no HTTP/2 (HTTP/1.1 only, per the roadmap) |
| Timeouts | Per-connection Asio `steady_timer` (not Beast's `tcp_stream` timeouts, because those close the socket on expiry and a 408 must be written first) |
| PostgreSQL client | libpq (the PostgreSQL C client library; `libpq-dev` to build, `libpq5` at runtime) used directly through a thin RAII wrapper with parameterised statements; no ORM or additional database framework. Server: `postgres:16` in Docker Compose |
| Schema management | Plain SQL files in `db/migrations/`, embedded into the binary at build time and applied at startup by a small migrator (`schema_migrations` table, advisory lock) |
| Minimum CMake | 3.25 |
