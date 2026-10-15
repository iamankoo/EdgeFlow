# Micro-benchmarks (Google Benchmark) of the hot paths. Not built by default:
#   cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDGEFLOW_BUILD_BENCHMARKS=ON
#   cmake --build build-bench --target edgeflow_micro_bench
#   ./build-bench/bench/edgeflow_micro_bench --benchmark_min_time=0.5s
# The load tests (wrk against a running stack) live in bench/load, see bench/README.md.

include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.5
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(benchmark)

add_executable(edgeflow_micro_bench micro/MicroBenchmarks.cpp)
target_link_libraries(edgeflow_micro_bench PRIVATE edgeflow::core benchmark::benchmark)
edgeflow_enable_warnings(edgeflow_micro_bench)
