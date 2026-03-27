cmake_minimum_required(VERSION 3.20)
project(codetldr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include(cmake/FetchDependencies.cmake)

# Semantic search: ONNX Runtime + FAISS + tokenizers-cpp (Phase 14+)
option(CODETLDR_ENABLE_SEMANTIC_SEARCH
  "Enable semantic search (requires ONNX Runtime, FAISS, Rust toolchain)"
  OFF)

if(CODETLDR_ENABLE_SEMANTIC_SEARCH)
  find_program(RUSTC rustc REQUIRED
    DOC "Rust compiler (rustc) is required to build tokenizers-cpp. Install via: curl https://sh.rustup.rs -sSf | sh")
  include(cmake/SemanticSearchDeps.cmake)

  add_library(codetldr_embedding STATIC
    src/embedding/placeholder.cpp
    src/embedding/vector_store.cpp
    src/embedding/model_manager.cpp
  )
  target_link_libraries(codetldr_embedding
    PUBLIC onnxruntime::onnxruntime faiss tokenizers_cpp
           spdlog::spdlog nlohmann_json::nlohmann_json)
  target_include_directories(codetldr_embedding PUBLIC src)
  target_compile_definitions(codetldr_embedding
    PUBLIC CODETLDR_ENABLE_SEMANTIC_SEARCH)
endif()

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
if(CMAKE_DL_LIBS)
  target_link_libraries(codetldr_foundation PRIVATE ${CMAKE_DL_LIBS})
endif()

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

add_library(codetldr_daemon
  src/daemon/status.cpp
  src/daemon/ipc_server.cpp
  src/daemon/request_router.cpp
  src/daemon/daemon_client.cpp
  src/daemon/daemonize.cpp
  src/daemon/coordinator.cpp
  src/watcher/debouncer.cpp
  src/watcher/file_watcher.cpp
  src/watcher/ignore_filter.cpp
)
target_include_directories(codetldr_daemon PUBLIC src)
target_link_libraries(codetldr_daemon
  PUBLIC codetldr_foundation codetldr_analysis codetldr_query
         nlohmann_json::nlohmann_json spdlog::spdlog CLI11::CLI11 tomlplusplus::tomlplusplus
         efsw-static Threads::Threads)

add_library(codetldr_query
  src/query/search_engine.cpp
  src/query/context_builder.cpp
  src/query/hybrid_search_engine.cpp
)
target_include_directories(codetldr_query PUBLIC src)
target_link_libraries(codetldr_query
  PUBLIC codetldr_foundation nlohmann_json::nlohmann_json)

if(CODETLDR_ENABLE_SEMANTIC_SEARCH)
  target_link_libraries(codetldr_query PUBLIC codetldr_embedding)
  target_compile_definitions(codetldr_query PUBLIC CODETLDR_ENABLE_SEMANTIC_SEARCH)
endif()

add_executable(codetldr-daemon src/daemon/main.cpp)
target_link_libraries(codetldr-daemon PRIVATE codetldr_daemon)

add_executable(codetldr src/cli/main.cpp src/cli/search_cmd.cpp src/cli/init_cmd.cpp)
target_link_libraries(codetldr PRIVATE codetldr_daemon)

add_executable(codetldr-mcp src/mcp/main.cpp)
target_link_libraries(codetldr-mcp PRIVATE codetldr_daemon)

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

add_executable(test_cfg_schema_upgrade tests/test_cfg_schema_upgrade.cpp)
target_link_libraries(test_cfg_schema_upgrade PRIVATE codetldr_foundation)
add_test(NAME cfg_schema_upgrade COMMAND test_cfg_schema_upgrade)

add_executable(test_tree_sitter_parser tests/test_tree_sitter_parser.cpp)
target_link_libraries(test_tree_sitter_parser PRIVATE codetldr_analysis)
add_test(NAME tree_sitter_parser COMMAND test_tree_sitter_parser)

add_executable(test_symbol_extractor tests/test_symbol_extractor.cpp)
target_link_libraries(test_symbol_extractor PRIVATE codetldr_analysis)
add_test(NAME symbol_extractor COMMAND test_symbol_extractor)

add_executable(test_call_extractor tests/test_call_extractor.cpp)
target_link_libraries(test_call_extractor PRIVATE codetldr_analysis)
add_test(NAME call_extractor COMMAND test_call_extractor)

add_executable(test_cfg_extractor tests/test_cfg_extractor.cpp)
target_link_libraries(test_cfg_extractor PRIVATE codetldr_analysis)
add_test(NAME cfg_extractor COMMAND test_cfg_extractor)

add_executable(test_dfg_extractor tests/test_dfg_extractor.cpp)
target_link_libraries(test_dfg_extractor PRIVATE codetldr_analysis)
add_test(NAME dfg_extractor COMMAND test_dfg_extractor)

add_executable(test_daemon_ipc tests/test_daemon_ipc.cpp)
target_link_libraries(test_daemon_ipc PRIVATE codetldr_daemon)
add_test(NAME daemon_ipc COMMAND test_daemon_ipc)

add_executable(test_debouncer tests/test_debouncer.cpp)
target_link_libraries(test_debouncer PRIVATE codetldr_daemon)
add_test(NAME debouncer COMMAND test_debouncer)

add_executable(test_file_watcher tests/test_file_watcher.cpp)
target_link_libraries(test_file_watcher PRIVATE codetldr_daemon)
add_test(NAME file_watcher COMMAND test_file_watcher)

add_executable(test_daemon_lifecycle tests/test_daemon_lifecycle.cpp)
target_link_libraries(test_daemon_lifecycle PRIVATE codetldr_daemon)
add_test(NAME daemon_lifecycle COMMAND test_daemon_lifecycle)

add_executable(test_fts5_search tests/test_fts5_search.cpp)
target_link_libraries(test_fts5_search PRIVATE codetldr_query codetldr_foundation)
add_test(NAME fts5_search COMMAND test_fts5_search)

add_executable(test_hybrid_search tests/test_hybrid_search.cpp)
target_link_libraries(test_hybrid_search PRIVATE codetldr_query codetldr_foundation)
add_test(NAME hybrid_search COMMAND test_hybrid_search)

add_executable(test_context_builder tests/test_context_builder.cpp)
target_link_libraries(test_context_builder PRIVATE codetldr_query codetldr_foundation)
add_test(NAME context_builder COMMAND test_context_builder)

add_executable(test_query_rpc tests/test_query_rpc.cpp)
target_link_libraries(test_query_rpc PRIVATE codetldr_query codetldr_foundation)
add_test(NAME query_rpc COMMAND test_query_rpc)

add_executable(test_ignore_filter tests/test_ignore_filter.cpp)
target_link_libraries(test_ignore_filter PRIVATE codetldr_daemon)
add_test(NAME ignore_filter COMMAND test_ignore_filter)

add_executable(test_new_rpc_methods tests/test_new_rpc_methods.cpp)
target_link_libraries(test_new_rpc_methods PRIVATE codetldr_daemon codetldr_query codetldr_foundation)
add_test(NAME new_rpc_methods COMMAND test_new_rpc_methods)

add_executable(test_cli_init tests/test_cli_init.cpp)
target_link_libraries(test_cli_init PRIVATE codetldr_daemon codetldr_analysis codetldr_foundation)
add_test(NAME cli_init COMMAND test_cli_init)

add_executable(test_mcp_provider tests/test_mcp_provider.cpp)
target_link_libraries(test_mcp_provider PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME mcp_provider COMMAND test_mcp_provider)
add_dependencies(test_mcp_provider codetldr-mcp)
set_tests_properties(mcp_provider PROPERTIES ENVIRONMENT "MCP_BIN=$<TARGET_FILE:codetldr-mcp>")

# Integration tests (bash scripts)
add_test(NAME e2e_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e2e_cli.sh
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(e2e_cli PROPERTIES
  LABELS "integration"
  TIMEOUT 60
)

add_test(NAME e2e_agentic
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e2e_agentic.sh
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(e2e_agentic PROPERTIES
  LABELS "integration;agentic"
  TIMEOUT 120
)

add_test(NAME plugin_artifacts
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_plugin_artifacts.sh
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(plugin_artifacts PROPERTIES
  LABELS "integration"
  TIMEOUT 30
)
