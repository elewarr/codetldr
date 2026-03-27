# cmake/SemanticSearchDeps.cmake
# Included from CMakeLists.txt inside if(CODETLDR_ENABLE_SEMANTIC_SEARCH)
# Phase 14: build infrastructure only — no runtime code yet.
include(FetchContent)

# ============================================================
# ONNX Runtime 1.24.4 — prebuilt tgz, manual IMPORTED target
# ORT issue #26186: bundled cmake files have wrong paths on Linux.
# We skip them with SOURCE_SUBDIR and create the target manually.
# ============================================================
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
  set(_ORT_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-osx-arm64-1.24.4.tgz")
  set(_ORT_SONAME "libonnxruntime.1.24.4.dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  set(_ORT_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-linux-x64-1.24.4.tgz")
  set(_ORT_SONAME "libonnxruntime.so.1.24.4")
else()
  message(FATAL_ERROR
    "CODETLDR_ENABLE_SEMANTIC_SEARCH: unsupported platform. Need macOS arm64 or Linux x64.")
endif()

FetchContent_Declare(onnxruntime
  URL            "${_ORT_URL}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR  "__no_cmake__"
)
FetchContent_MakeAvailable(onnxruntime)

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
  IMPORTED_LOCATION             "${onnxruntime_SOURCE_DIR}/lib/${_ORT_SONAME}"
  INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include"
)

# ============================================================
# FAISS 1.14.1 — FetchContent source build
# GPU, Python, extras, MKL disabled. BLAS via Accelerate (macOS)
# or OpenBLAS (Linux, installed in CI).
# ============================================================
set(FAISS_ENABLE_GPU    OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_EXTRAS OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_MKL    OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
if(APPLE)
  set(BLA_VENDOR Apple CACHE STRING "" FORCE)
endif()

FetchContent_Declare(faiss
  GIT_REPOSITORY https://github.com/facebookresearch/faiss.git
  GIT_TAG        v1.14.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(faiss)

# ============================================================
# tokenizers-cpp 0.1.1 — git submodule (not FetchContent)
# STATE.md mandate: FetchContent is flaky in CI (mlc-llm #899).
# ============================================================
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tokenizers-cpp/CMakeLists.txt")
  message(FATAL_ERROR
    "third_party/tokenizers-cpp is not initialized.\n"
    "Run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/tokenizers-cpp)
