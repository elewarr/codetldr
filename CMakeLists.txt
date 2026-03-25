cmake_minimum_required(VERSION 3.20)
project(codetldr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/FetchDependencies.cmake)

add_library(codetldr_foundation
  src/common/logging.cpp
  src/config/paths.cpp
  src/config/project_dir.cpp
  src/storage/database.cpp
  src/storage/migrations.cpp
)
target_include_directories(codetldr_foundation PUBLIC src)
target_link_libraries(codetldr_foundation
  PUBLIC SQLiteCpp spdlog::spdlog nlohmann_json::nlohmann_json)

add_library(codetldr_analysis
  src/analysis/tree_sitter/parser.cpp
  src/analysis/tree_sitter/language_registry.cpp
  src/analysis/tree_sitter/extractor.cpp
  src/analysis/pipeline.cpp
)
target_include_directories(codetldr_analysis PUBLIC src)
target_link_libraries(codetldr_analysis
  PUBLIC codetldr_foundation tree_sitter_lib
  PRIVATE ts_grammar_python ts_grammar_javascript ts_grammar_typescript
          ts_grammar_tsx ts_grammar_rust ts_grammar_c ts_grammar_cpp
          ts_grammar_java ts_grammar_kotlin ts_grammar_swift ts_grammar_objc)

enable_testing()

add_executable(test_xdg_paths tests/test_xdg_paths.cpp)
target_link_libraries(test_xdg_paths PRIVATE codetldr_foundation)
add_test(NAME xdg_paths COMMAND test_xdg_paths)

add_executable(test_sqlite_storage tests/test_sqlite_storage.cpp)
target_link_libraries(test_sqlite_storage PRIVATE codetldr_foundation)
add_test(NAME sqlite_storage COMMAND test_sqlite_storage)

add_executable(test_project_init tests/test_project_init.cpp)
target_link_libraries(test_project_init PRIVATE codetldr_foundation)
add_test(NAME project_init COMMAND test_project_init)

add_executable(test_schema_v2v3 tests/test_schema_v2v3.cpp)
target_link_libraries(test_schema_v2v3 PRIVATE codetldr_foundation)
add_test(NAME schema_v2v3 COMMAND test_schema_v2v3)

add_executable(test_tree_sitter_parser tests/test_tree_sitter_parser.cpp)
target_link_libraries(test_tree_sitter_parser PRIVATE codetldr_analysis)
add_test(NAME tree_sitter_parser COMMAND test_tree_sitter_parser)

add_executable(test_symbol_extractor tests/test_symbol_extractor.cpp)
target_link_libraries(test_symbol_extractor PRIVATE codetldr_analysis)
add_test(NAME symbol_extractor COMMAND test_symbol_extractor)

add_executable(test_call_extractor tests/test_call_extractor.cpp)
target_link_libraries(test_call_extractor PRIVATE codetldr_analysis)
add_test(NAME call_extractor COMMAND test_call_extractor)
