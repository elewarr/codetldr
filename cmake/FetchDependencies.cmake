include(FetchContent)

# SQLiteCpp 3.3.3
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITE_ENABLE_FTS5 ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    SQLiteCpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG        3.3.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SQLiteCpp)
# SQLiteCpp bundles its own sqlite3 source but does not propagate SQLITE_ENABLE_FTS5.
# Add it explicitly to the sqlite3 compile target so FTS5 virtual tables work.
if (TARGET sqlite3)
    target_compile_definitions(sqlite3 PUBLIC SQLITE_ENABLE_FTS5)
endif()

# nlohmann/json 3.12.0
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    URL                    https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# spdlog 1.17.0 -- use bundled fmt to avoid ODR violations
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# CLI11 2.4.2
set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(CLI11)

# ============================================================
# Tree-sitter runtime v0.26.7
# The tree-sitter repo does not provide a CMake build -- we
# create the target manually from lib/src/lib.c.
# SOURCE_SUBDIR "__no_cmake__" prevents add_subdirectory from
# processing the repo's own CMakeLists.txt.
# ============================================================
FetchContent_Declare(tree_sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG        v0.26.7
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(tree_sitter)

add_library(tree_sitter_lib STATIC
    ${tree_sitter_SOURCE_DIR}/lib/src/lib.c)
target_include_directories(tree_sitter_lib PUBLIC
    ${tree_sitter_SOURCE_DIR}/lib/include
    ${tree_sitter_SOURCE_DIR}/lib/src)
target_compile_options(tree_sitter_lib PRIVATE
    -Wno-unused-variable -Wno-unused-function)

# ============================================================
# Helper macro for grammar targets
# Usage: add_tree_sitter_grammar(NAME SOURCE_DIR)
#   Creates target ts_grammar_<NAME> with parser.c and optional
#   scanner.c / scanner.cc, linked against tree_sitter_lib.
# ============================================================
macro(add_tree_sitter_grammar NAME GRAMMAR_DIR)
    set(_sources "${GRAMMAR_DIR}/src/parser.c")

    if(EXISTS "${GRAMMAR_DIR}/src/scanner.cc")
        list(APPEND _sources "${GRAMMAR_DIR}/src/scanner.cc")
        set(_has_cxx_scanner TRUE)
    elseif(EXISTS "${GRAMMAR_DIR}/src/scanner.c")
        list(APPEND _sources "${GRAMMAR_DIR}/src/scanner.c")
        set(_has_cxx_scanner FALSE)
    else()
        set(_has_cxx_scanner FALSE)
    endif()

    add_library(ts_grammar_${NAME} STATIC ${_sources})
    target_include_directories(ts_grammar_${NAME} PUBLIC "${GRAMMAR_DIR}/src")
    target_link_libraries(ts_grammar_${NAME} PUBLIC tree_sitter_lib)
    target_compile_options(ts_grammar_${NAME} PRIVATE
        -Wno-unused-variable -Wno-unused-function)

    if(_has_cxx_scanner)
        target_compile_features(ts_grammar_${NAME} PRIVATE cxx_std_14)
    endif()
endmacro()

# ============================================================
# Grammar: Python
# SOURCE_SUBDIR "__no_cmake__" prevents the grammar's own
# CMakeLists.txt from being processed (avoids conflicting targets).
# ============================================================
FetchContent_Declare(ts_python
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_TAG        v0.25.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_python)
add_tree_sitter_grammar(python ${ts_python_SOURCE_DIR})

# ============================================================
# Grammar: JavaScript
# ============================================================
FetchContent_Declare(ts_javascript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    GIT_TAG        v0.25.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_javascript)
add_tree_sitter_grammar(javascript ${ts_javascript_SOURCE_DIR})

# ============================================================
# Grammar: TypeScript -- two targets from one repo (typescript + tsx)
# ============================================================
FetchContent_Declare(ts_typescript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    GIT_TAG        v0.23.2
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_typescript)
add_tree_sitter_grammar(typescript ${ts_typescript_SOURCE_DIR}/typescript)
add_tree_sitter_grammar(tsx        ${ts_typescript_SOURCE_DIR}/tsx)

# ============================================================
# Grammar: Rust
# ============================================================
FetchContent_Declare(ts_rust
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
    GIT_TAG        v0.24.2
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_rust)
add_tree_sitter_grammar(rust ${ts_rust_SOURCE_DIR})

# ============================================================
# Grammar: Go
# ============================================================
FetchContent_Declare(ts_go
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
    GIT_TAG        v0.23.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_go)
add_tree_sitter_grammar(go ${ts_go_SOURCE_DIR})

# ============================================================
# Grammar: C
# ============================================================
FetchContent_Declare(ts_c
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_TAG        v0.24.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_c)
add_tree_sitter_grammar(c ${ts_c_SOURCE_DIR})

# ============================================================
# Grammar: C++
# ============================================================
FetchContent_Declare(ts_cpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG        v0.23.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_cpp)
add_tree_sitter_grammar(cpp ${ts_cpp_SOURCE_DIR})

# ============================================================
# Grammar: Java
# ============================================================
FetchContent_Declare(ts_java
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-java.git
    GIT_TAG        v0.23.5
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_java)
add_tree_sitter_grammar(java ${ts_java_SOURCE_DIR})

# ============================================================
# Grammar: Kotlin (uses 'main' branch)
# ============================================================
FetchContent_Declare(ts_kotlin
    GIT_REPOSITORY https://github.com/fwcd/tree-sitter-kotlin.git
    GIT_TAG        0.3.8
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_kotlin)
add_tree_sitter_grammar(kotlin ${ts_kotlin_SOURCE_DIR})

# ============================================================
# Grammar: Swift (uses 'with-generated-files' branch -- main branch
# does not pre-generate parser.c, this branch does)
# ============================================================
FetchContent_Declare(ts_swift
    GIT_REPOSITORY https://github.com/alex-pinkus/tree-sitter-swift.git
    GIT_TAG        with-generated-files
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_swift)
add_tree_sitter_grammar(swift ${ts_swift_SOURCE_DIR})

# ============================================================
# Grammar: Objective-C
# ============================================================
FetchContent_Declare(ts_objc
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-objc.git
    GIT_TAG        v3.0.2
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_objc)
add_tree_sitter_grammar(objc ${ts_objc_SOURCE_DIR})

# ============================================================
# Grammar: Ruby
# ============================================================
FetchContent_Declare(ts_ruby
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-ruby.git
    GIT_TAG        v0.23.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_ruby)
add_tree_sitter_grammar(ruby ${ts_ruby_SOURCE_DIR})

# ============================================================
# Grammar: Lua
# tree-sitter-grammars org (NOT tree-sitter/tree-sitter-lua)
# Uses C external scanner (scanner.c) -- detected by macro
# ============================================================
FetchContent_Declare(ts_lua
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-lua.git
    GIT_TAG        v0.5.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(ts_lua)
add_tree_sitter_grammar(lua ${ts_lua_SOURCE_DIR})

# ============================================================
# efsw — cross-platform file system watcher
# macOS FSEvents + Linux inotify in one API
# ============================================================
FetchContent_Declare(efsw
    GIT_REPOSITORY https://github.com/SpartanJ/efsw.git
    GIT_TAG        1.5.1
    GIT_SHALLOW    TRUE
)
set(EFSW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(efsw)
