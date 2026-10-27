include(GoogleTest)

add_executable(edgeflow_tests
  cache/ResponseCacheTest.cpp
  config/ConfigManagerRedisTest.cpp
  config/ConfigManagerTest.cpp
  core/ApplicationTest.cpp
  core/CommandLineTest.cpp
  core/ShutdownCoordinatorTest.cpp
  discovery/HealthCheckerPostgresTest.cpp
  discovery/HealthCheckerTest.cpp
  discovery/HealthStateTest.cpp
  discovery/NameResolverTest.cpp
  discovery/PostgresServiceRegistryTest.cpp
  discovery/ProberTest.cpp
  discovery/ValidationTest.cpp
  gateway/PolicyHandlerTest.cpp
  gateway/PolicyPostgresTest.cpp
  logging/LoggerTest.cpp
  network/AdminAuthTest.cpp
  network/AsyncHandlerTest.cpp
  network/HealthProbeTest.cpp
  network/HttpServerTest.cpp
  network/LogRedactionTest.cpp
  network/RegistryApiTest.cpp
  network/RequestHandlerTest.cpp
  network/TcpServerTest.cpp
  observability/MetricsApplicationTest.cpp
  observability/MetricsFullStackTest.cpp
  observability/MetricsHandlerTest.cpp
  observability/MetricsTest.cpp
  proxy/ConnectionCountingTest.cpp
  proxy/InlineAttemptTest.cpp
  proxy/ProxyHandlerTest.cpp
  proxy/ProxyHeadersTest.cpp
  proxy/ProxyPostgresTest.cpp
  proxy/UpstreamPoolTest.cpp
  ratelimit/RateLimiterTest.cpp
  redis/RedisClientTest.cpp
  redis/RespTest.cpp
  reliability/CircuitBreakerTest.cpp
  reliability/ReliabilityPostgresTest.cpp
  reliability/ReliabilityProxyTest.cpp
  reliability/RetryPolicyTest.cpp
  routing/RouterSnapshotTest.cpp
  routing/RoutingPostgresTest.cpp
  routing/RouterTest.cpp
  routing/StrategyTest.cpp
  storage/PostgresTest.cpp)

target_include_directories(edgeflow_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(edgeflow_tests PRIVATE
  EDGEFLOW_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  EDGEFLOW_DEFAULT_CONFIG="${PROJECT_SOURCE_DIR}/config/config.yaml"
  EDGEFLOW_COMPOSE_CONFIG="${PROJECT_SOURCE_DIR}/config/config.compose.yaml"
  EDGEFLOW_BENCH_CONFIG="${PROJECT_SOURCE_DIR}/bench/load/gateway.yaml")
target_link_libraries(edgeflow_tests PRIVATE edgeflow::core nlohmann_json::nlohmann_json GTest::gtest_main)
edgeflow_enable_warnings(edgeflow_tests)

# A hung network test must fail the run rather than stall it.
gtest_discover_tests(edgeflow_tests PROPERTIES TIMEOUT 120)

# Executable-level checks: process exit codes for the command line surface.
add_test(NAME cli_version COMMAND edgeflow --version)
add_test(NAME cli_help COMMAND edgeflow --help)
add_test(NAME cli_missing_config_fails
  COMMAND edgeflow --config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/does_not_exist.yaml")
set_tests_properties(cli_missing_config_fails PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_invalid_config_fails
  COMMAND edgeflow --config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/invalid_port.yaml")
set_tests_properties(cli_invalid_config_fails PROPERTIES WILL_FAIL TRUE)
