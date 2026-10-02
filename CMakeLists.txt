cmake_minimum_required(VERSION 3.25)

project(EdgeFlow
  VERSION 0.1.0
  DESCRIPTION "API gateway, service discovery and load balancer"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(EDGEFLOW_BUILD_TESTS "Build the unit tests" ON)
option(EDGEFLOW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(EDGEFLOW_ENABLE_BOOST "Locate Boost (dependency only; used from Phase 2)" OFF)

include(CompilerWarnings)
include(Dependencies)

add_subdirectory(src)

if(EDGEFLOW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS edgeflow RUNTIME DESTINATION bin)
install(FILES config/config.yaml DESTINATION etc/edgeflow)
