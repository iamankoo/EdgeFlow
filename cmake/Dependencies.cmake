# Dependencies are fetched from pinned release tags so that every build, on any
# machine, uses the same versions. Nothing relies on globally installed copies.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
# Older dependency CMake files declare a minimum below what CMake 4 accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# --- spdlog (logging) -------------------------------------------------------
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
  GIT_SHALLOW    TRUE
  SYSTEM)

# --- yaml-cpp (configuration) -----------------------------------------------
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        0.8.0
  GIT_SHALLOW    TRUE
  SYSTEM)

FetchContent_MakeAvailable(spdlog yaml-cpp)

# --- GoogleTest (testing) ---------------------------------------------------
if(EDGEFLOW_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(googletest)
endif()

# --- Boost (dependency only) ------------------------------------------------
# Boost.Asio and Boost.Beast are first used by the networking engine (Phase 2).
# Phase 1 only provides the switch; it is OFF by default and nothing links it.
if(EDGEFLOW_ENABLE_BOOST)
  find_package(Boost 1.83 REQUIRED)
  message(STATUS "Boost ${Boost_VERSION} found (not yet used by any target)")
endif()
