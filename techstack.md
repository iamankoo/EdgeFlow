# EdgeFlow — Technology Stack

The stack below is **locked**. No technology may be substituted or added without explicit approval from the project owner. In particular, Kubernetes, Kafka, RabbitMQ and similar infrastructure are out of scope.

| Area | Technology | Why |
|------|-----------|-----|
| Language | C++20 | Predictable latency, control over memory and threading, coroutine and concepts support for a systems-level gateway. |
| Build | CMake | De facto standard for C++; portable across GCC/Clang, integrates with GoogleTest, Benchmark and CI. |
| Compilers | GCC / Clang | Both supported to catch portability issues and to use sanitizers and profiling tools. |
| Networking | Boost.Asio | Mature asynchronous I/O and executor model for TCP; the basis of the concurrency design. |
| HTTP | Boost.Beast (HTTP/1.1 initially) | HTTP parsing and serialization built on Asio, avoiding a hand-rolled parser. |
| Persistence | PostgreSQL | Durable, transactional source of truth for service metadata. |
| Shared state / cache | Redis | Low-latency shared store for response caching and distributed rate-limit state. |
| Containers | Docker, Docker Compose | Reproducible local deployment of the gateway, backends, PostgreSQL and Redis. |
| Unit testing | GoogleTest | Standard C++ test framework with CMake/CTest integration. |
| Benchmarking | Google Benchmark | Micro-benchmarks for hot paths such as routing and rate limiting. |
| Logging | spdlog | Fast, structured-friendly logging with minimal overhead. |
| JSON | nlohmann/json | Ergonomic JSON handling for APIs and metadata. |
| Configuration | YAML | Human-readable, hierarchical configuration. |
| Metrics | Prometheus-compatible | Standard exposition format that works with common tooling. |
| Profiling | perf, Valgrind, flamegraphs | CPU, memory and contention analysis to drive evidence-based optimization. |
| Load testing | wrk, custom C++ load generators | wrk for raw HTTP throughput; custom tools where wrk cannot model the scenario. |
| CI | GitHub Actions | Automated build and test on every push. |

## Load-balancing algorithms (locked)

1. Round Robin
2. Least Connections
3. Weighted Routing
4. Consistent Hashing

All four are implemented behind one strategy abstraction so they can be swapped without changing the gateway.

> Specific YAML and dependency-management libraries are selected at implementation time (Phase 1) within this locked stack.
