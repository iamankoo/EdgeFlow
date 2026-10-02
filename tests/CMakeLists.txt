include(GoogleTest)

add_executable(edgeflow_tests
  config/ConfigManagerTest.cpp
  core/ApplicationTest.cpp
  core/CommandLineTest.cpp
  core/ShutdownCoordinatorTest.cpp
  logging/LoggerTest.cpp)

target_include_directories(edgeflow_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(edgeflow_tests PRIVATE
  EDGEFLOW_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  EDGEFLOW_DEFAULT_CONFIG="${PROJECT_SOURCE_DIR}/config/config.yaml")
target_link_libraries(edgeflow_tests PRIVATE edgeflow::core GTest::gtest_main)
edgeflow_enable_warnings(edgeflow_tests)

gtest_discover_tests(edgeflow_tests)

# Executable-level checks: process exit codes for the command line surface.
add_test(NAME cli_version COMMAND edgeflow --version)
add_test(NAME cli_help COMMAND edgeflow --help)
add_test(NAME cli_missing_config_fails
  COMMAND edgeflow --config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/does_not_exist.yaml")
set_tests_properties(cli_missing_config_fails PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_invalid_config_fails
  COMMAND edgeflow --config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/invalid_port.yaml")
set_tests_properties(cli_invalid_config_fails PROPERTIES WILL_FAIL TRUE)
