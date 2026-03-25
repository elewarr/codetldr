cmake_minimum_required(VERSION 3.20)
project(codetldr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/FetchDependencies.cmake)

add_library(codetldr_foundation
  src/common/logging.cpp
  src/config/paths.cpp
  src/storage/database.cpp
  src/storage/migrations.cpp
)
target_include_directories(codetldr_foundation PUBLIC src)
target_link_libraries(codetldr_foundation
  PUBLIC SQLiteCpp spdlog::spdlog nlohmann_json::nlohmann_json)

enable_testing()

add_executable(test_xdg_paths tests/test_xdg_paths.cpp)
target_link_libraries(test_xdg_paths PRIVATE codetldr_foundation)
add_test(NAME xdg_paths COMMAND test_xdg_paths)

add_executable(test_sqlite_storage tests/test_sqlite_storage.cpp)
target_link_libraries(test_sqlite_storage PRIVATE codetldr_foundation)
add_test(NAME sqlite_storage COMMAND test_sqlite_storage)
