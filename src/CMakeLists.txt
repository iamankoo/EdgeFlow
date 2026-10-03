# Embed db/migrations/*.sql into the binary so the runtime image needs no SQL files.
file(GLOB EDGEFLOW_MIGRATION_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/db/migrations/*.sql")
list(SORT EDGEFLOW_MIGRATION_FILES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EDGEFLOW_MIGRATION_FILES})
set(EDGEFLOW_MIGRATIONS_BODY "")
foreach(migration_file IN LISTS EDGEFLOW_MIGRATION_FILES)
  get_filename_component(migration_name "${migration_file}" NAME_WE)
  file(READ "${migration_file}" migration_sql)
  string(APPEND EDGEFLOW_MIGRATIONS_BODY
    "    {\"${migration_name}\", R\"edgeflow_sql(${migration_sql})edgeflow_sql\"},\n")
endforeach()
set(EDGEFLOW_MIGRATIONS_TMP "${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinMigrations.cpp.in")
set(EDGEFLOW_MIGRATIONS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinMigrations.cpp")
file(WRITE "${EDGEFLOW_MIGRATIONS_TMP}"
  "// Generated from db/migrations/*.sql by src/CMakeLists.txt. Do not edit.\n"
  "#include \"edgeflow/storage/Migrator.hpp\"\n\n"
  "namespace edgeflow::storage {\n\n"
  "const std::vector<Migration>& builtinMigrations() {\n"
  "  static const std::vector<Migration> migrations = {\n"
  "${EDGEFLOW_MIGRATIONS_BODY}"
  "  };\n"
  "  return migrations;\n"
  "}\n\n"
  "}  // namespace edgeflow::storage\n")
# configure_file only rewrites the output when the content changed, avoiding needless rebuilds.
configure_file("${EDGEFLOW_MIGRATIONS_TMP}" "${EDGEFLOW_MIGRATIONS_SOURCE}" COPYONLY)

add_library(edgeflow_core STATIC
  config/ConfigManager.cpp
  core/Application.cpp
  core/CommandLine.cpp
  core/ShutdownCoordinator.cpp
  core/SignalHandler.cpp
  discovery/PostgresServiceRegistry.cpp
  discovery/ServiceInstance.cpp
  discovery/Validation.cpp
  logging/Logger.cpp
  network/ConnectionTracker.cpp
  network/HealthProbe.cpp
  network/Http.cpp
  network/HttpConnection.cpp
  network/HttpServer.cpp
  network/LocalRequestHandler.cpp
  network/RegistryRequestHandler.cpp
  network/TcpServer.cpp
  storage/Migrator.cpp
  storage/Postgres.cpp
  "${EDGEFLOW_MIGRATIONS_SOURCE}")
add_library(edgeflow::core ALIAS edgeflow_core)

target_include_directories(edgeflow_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(edgeflow_core PUBLIC EDGEFLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(edgeflow_core
  PUBLIC spdlog::spdlog Boost::headers Threads::Threads
  PRIVATE yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json PostgreSQL::PostgreSQL)
target_compile_features(edgeflow_core PUBLIC cxx_std_20)
edgeflow_enable_warnings(edgeflow_core)

add_executable(edgeflow main.cpp)
target_link_libraries(edgeflow PRIVATE edgeflow::core)
edgeflow_enable_warnings(edgeflow)
