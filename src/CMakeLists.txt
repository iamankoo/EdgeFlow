add_library(edgeflow_core STATIC
  config/ConfigManager.cpp
  core/Application.cpp
  core/CommandLine.cpp
  core/ShutdownCoordinator.cpp
  core/SignalHandler.cpp
  logging/Logger.cpp)
add_library(edgeflow::core ALIAS edgeflow_core)

target_include_directories(edgeflow_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(edgeflow_core PUBLIC EDGEFLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(edgeflow_core PUBLIC spdlog::spdlog PRIVATE yaml-cpp::yaml-cpp)
target_compile_features(edgeflow_core PUBLIC cxx_std_20)
edgeflow_enable_warnings(edgeflow_core)

add_executable(edgeflow main.cpp)
target_link_libraries(edgeflow PRIVATE edgeflow::core)
edgeflow_enable_warnings(edgeflow)
