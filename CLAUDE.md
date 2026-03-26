<!-- GSD:project-start source:PROJECT.md -->
## Project

**CodeTLDR**

A C++ code analysis tool that gives LLM agents token-efficient, structured understanding of codebases. Like llm-tldr but built as a native C++ daemon with uniform multi-language support via Tree-sitter and LSP. Runs as a background daemon launched by Claude Code hooks, exposes analysis through MCP and CLI, and provides semantic code search via self-contained embedding inference.

**Core Value:** Give LLM agents exactly the code context they need — structure, call graphs, control flow, data flow, and dependencies — at 90%+ fewer tokens than reading raw files.

### Constraints

- **Language**: C++ — core implementation language for performance and self-contained deployment
- **Build system**: CMake — first-class support for all dependencies (Tree-sitter, ONNX Runtime, SQLite, FAISS)
- **Platforms**: macOS first (development platform), Linux second
- **Minimal dependencies**: Tree-sitter, ONNX Runtime, tokenizers-cpp, SQLite, FAISS — no heavy compiler infrastructure
- **Self-contained inference**: No external model serving, no Python runtime, no Node.js runtime
- **XDG compliance**: Follow freedesktop.org Base Directory Specification for all paths
<!-- GSD:project-end -->

<!-- GSD:stack-start source:research/STACK.md -->
## Technology Stack

## Recommended Stack
### Core Technologies
| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| **C++17** | — | Implementation language | Required by ONNX Runtime, tokenizers-cpp, toml++, and all major deps. C++20 is viable but adds no necessary features here — stay on 17 for compiler compat. |
| **CMake** | 3.20+ | Build system | First-class support across every dep in this stack. FetchContent available for header-only deps. `find_package` works for ORT and FAISS system installs. All CI systems ship CMake. |
| **Tree-sitter** | 0.26.7 | L1 AST parsing, approximate CFG/DFG | Incremental, error-tolerant parser. 170+ community grammars. C library (~2 MB), trivially embedded. The C API is stable; C++ wrappers are thin sugar on top. Latest release: March 14, 2025. |
| **ONNX Runtime** | 1.24.4 | Embedding model inference | Self-contained C++ inference — no Python, no model server. CoreML EP for Apple Neural Engine acceleration on macOS. Supports INT8 quantized models. Sub-20ms per embedding on Apple Silicon. Latest release: March 17, 2025. |
| **FAISS** | 1.14.1 | Vector similarity search | The standard C++ library for dense vector search at scale. Native CPU BLAS for performance. IndexFlatL2 for small corpora, IVFFlat for larger. No separate process, no network. Latest release: March 6, 2025. |
| **SQLite** | 3.49.x (bundled) | Structured index storage | Zero-config, single-file database. The right choice when the database is per-project and the daemon is the only writer. Use WAL mode for concurrent reads during indexing. |
| **tokenizers-cpp** (mlc-ai) | 0.1.1 | HuggingFace tokenizer inference in C++ | Cross-platform C++ binding to HF tokenizers (Rust-backed). Supports `tokenizer.json` directly — covers BPE, WordPiece, Unigram (all models in target set). Used in production by MLC LLM. Released May 2025. Requires Rust toolchain at build time. |
### Supporting Libraries
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| **nlohmann/json** | 3.12.0 | JSON serialization (LSP JSON-RPC, config, MCP protocol) | Always. JSON for LSP messages, MCP tool responses, status payloads. Header-only, no compile-time cost. Apr 2025. |
| **spdlog** | 1.17.0 | Structured logging | Always. Async logging for daemon, file/console sinks, log rotation. Bundles fmt 12.1.0. Header-only or compiled. Jan 2025. |
| **CLI11** | 2.4.2 | Command-line argument parsing | For all CLI entry points (codetldr init, status, search, etc.). Header-only, zero deps. |
| **toml++** | 3.4.0 | TOML config file parsing | config.toml and models.toml. Header-only C++17. No deps. Oct 2024. |
| **SQLiteCpp** | 3.3.3 | C++ RAII wrapper around SQLite3 | Cleaner API than raw sqlite3.h. RAII for statements and transactions. Actively maintained. May 2025. |
| **efsw** | latest (master) | Cross-platform file system watching | macOS FSEvents + Linux inotify in one API. Callbacks on file change/create/delete. Use for debounced incremental reindexing. CMake supported via community cmake file. |
### Development Tools
| Tool | Purpose | Notes |
|------|---------|-------|
| **CMake** | Build system | Minimum 3.20 for FetchContent improvements. Use `cmake --preset` for reproducible builds. |
| **Ninja** | Fast build backend | Always use with CMake: `-G Ninja`. Significantly faster than Make on incremental builds. |
| **clang-format** | Code style enforcement | Match LspCpp's standard: clang-format 18. Automate in CI. |
| **AddressSanitizer** | Memory safety in debug | Essential for a daemon. Enable in dev builds: `-fsanitize=address`. |
| **Rust toolchain** | Build dep for tokenizers-cpp | tokenizers-cpp wraps HF tokenizers (Rust). Rust must be on PATH during CMake configure. Only a build-time dep — not present in the final binary. |
## Dependency Management: CMake + FetchContent / CPM
# Recommended pattern: prefer system install, fall back to fetch
## Installation
# System dependencies (macOS)
# Rust (for tokenizers-cpp build dep)
# All other deps are fetched by CMake at configure time via FetchContent:
# - tree-sitter (C source, compiled in-tree)
# - nlohmann/json (header-only)
# - spdlog (header-only)
# - CLI11 (header-only)
# - toml++ (header-only)
# - SQLiteCpp (source, compiled in-tree)
# - efsw (source, compiled in-tree)
# - tokenizers-cpp (source + Rust, compiled in-tree)
# - FAISS (source or prebuilt, compiled in-tree)
# Build
## Alternatives Considered
| Recommended | Alternative | When to Use Alternative |
|-------------|-------------|-------------------------|
| tokenizers-cpp (mlc-ai) | ONNX Runtime Extensions tokenizer | If you want a pure-ONNX pipeline with no Rust dep. More fragile — tokenizer export fidelity varies by model. Use only if Rust is a hard constraint. |
| tokenizers-cpp (mlc-ai) | sentencepiece C++ | Only for SPM models (LLaMA-family). Not appropriate for BGE/Jina/CodeRankEmbed which use BPE with `tokenizer.json`. |
| SQLiteCpp | Raw sqlite3.h C API | If you need serialized threading mode (SQLiteCpp doesn't support it). For this daemon (single writer), SQLiteCpp is fine. |
| efsw | libuv `uv_fs_event_t` | If you're already pulling libuv for async I/O. Avoid adding libuv just for FS watching — efsw is simpler and purpose-built. |
| efsw | fswatch (CLI tool) | fswatch is a standalone CLI, not a library. Not embeddable. |
| nlohmann/json | RapidJSON | RapidJSON is faster for high-throughput parsing, but DOM API is worse DX. LSP JSON-RPC volumes don't justify the complexity trade-off. |
| spdlog | fmt + custom logging | spdlog bundles fmt — no reason to maintain your own async logging on top of fmt. |
| FetchContent / CPM | vcpkg | vcpkg is better when you need 100+ deps or share packages across many projects. Overkill here. |
| FetchContent / CPM | Conan | Same — Conan excels at large enterprise dep graphs. Adds toolchain complexity without benefit for this project's dep count. |
| FAISS | hnswlib | hnswlib is faster for ANN at large scale. FAISS is better maintained, has a native C++ API without Python bindings overhead, and handles exact search at the code chunk scale (< 100K vectors typical). Re-evaluate if corpus grows to 1M+ chunks. |
| LspCpp (client mode) | Custom JSON-RPC stdio wrapper | LspCpp has heavy deps (boost, rapidjson). See "What NOT to Use" below. |
## What NOT to Use
| Avoid | Why | Use Instead |
|-------|-----|-------------|
| **LspCpp** | Depends on Boost and RapidJSON — 2 heavy transitive deps for something you can implement in ~500 lines with nlohmann/json and `popen`/`posix_spawn`. The library is primarily server-focused; client usage is secondary. | Roll a minimal LSP stdio client: spawn process, read/write JSON-RPC headers + body, use nlohmann/json for message parsing. ~500 lines. No external dep. |
| **libjson-rpc-cpp** | Unmaintained (last commit 2020). Has TCP/HTTP transport complexity you don't need — LSP uses stdio. | Inline JSON-RPC 2.0 over stdio: trivial header + content parsing. Use nlohmann/json for message bodies. |
| **LLVM/libclang** | 500+ MB download, complex build, only benefits C/C++/ObjC. PROJECT.md explicitly defers this as future optional enhancer. | Tree-sitter for L1/L3/L4 across all languages. |
| **Boost** | No feature in this stack requires Boost that isn't covered by stdlib + a smaller dep (asio → skip entirely, use Unix sockets directly; filesystem → std::filesystem in C++17). | std::filesystem (C++17), POSIX sockets directly. |
| **cpp-mcp (hkr04)** | 24 commits, limited adopters, MCP spec conformance only to 2024-11-05. MCP protocol is stdio JSON-RPC — implement directly. | Implement MCP provider as a stdio JSON-RPC server using nlohmann/json. The protocol is ~50 lines of message dispatch. |
| **Conan / vcpkg** | Adds toolchain wrapper complexity for a project with a small, well-defined dep set. FetchContent handles everything needed. | CMake FetchContent + `find_package` with fallback. |
| **Python/Node.js runtime** | PROJECT.md explicitly requires self-contained C++ binary. No interpreter deps. | All inference, tokenization, and analysis in C++. |
## Stack Patterns by Variant
- Enable CoreML EP in ONNX Runtime session options: `OrtSessionOptionsAppendExecutionProvider_CoreML(session_options, 0)`
- Use `kFSEventStreamCreateFlagFileEvents` (macOS FSEvents) via efsw — already default on macOS
- Use `std::filesystem` for XDG path construction
- Apple Neural Engine acceleration automatically selected when CoreML EP is active
- ONNX Runtime CPU EP only (no CoreML)
- efsw falls back to inotify automatically — no code change needed
- Build FAISS with `-DFAISS_OPT_LEVEL=avx2` for modern server CPUs
- CodeRankEmbed (137M, INT8 ~70MB) — best for code; recommended default
- bge-large-en-v1.5 (335M, INT8 ~170MB) — better for mixed code+prose
- INT8 ONNX export required; use `optimum-cli export onnx --quantize` once at model prep time
- Use `posix_spawn` (not `fork`/`exec`) for spawning LSP child processes — safer in daemon context
- Communicate over stdin/stdout with framed JSON-RPC (Content-Length header)
- Per-language: one LSP process instance, reconnect on crash
## Version Compatibility
| Package | Compatible With | Notes |
|---------|-----------------|-------|
| ONNX Runtime 1.24.x | tokenizers-cpp 0.1.1 | No direct linkage — ORT handles inference, tokenizers-cpp handles tokenization. No ABI conflict. |
| SQLiteCpp 3.3.3 | SQLite 3.49.2 | SQLiteCpp bundles SQLite source; do not link against system libsqlite3 separately. |
| spdlog 1.17.0 | fmt 12.1.0 | spdlog bundles fmt — do not link a separate fmt target or you'll get ODR violations. Set `SPDLOG_FMT_EXTERNAL=OFF`. |
| nlohmann/json 3.12.0 | C++17 | Requires C++17 minimum. Compatible with all compilers that support tree-sitter. |
| FAISS 1.14.1 | BLAS (OpenBLAS or Accelerate) | On macOS use `-DFAISS_ENABLE_GPU=OFF -DBLA_VENDOR=Apple` to use Accelerate framework. On Linux use OpenBLAS. |
| tokenizers-cpp 0.1.1 | Rust 1.70+ | Must have `rustc` and `cargo` on PATH during CMake configure. The Rust code compiles to a static lib linked into the final binary — no runtime Rust dep. |
## Sources
- [tree-sitter releases](https://github.com/tree-sitter/tree-sitter/releases) — v0.26.7 confirmed Mar 2025
- [ONNX Runtime releases](https://github.com/microsoft/onnxruntime/releases) — v1.24.4 confirmed Mar 2025
- [ONNX Runtime CoreML EP docs](https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html) — CoreML C++ API verified
- [FAISS releases](https://github.com/facebookresearch/faiss/releases) — v1.14.1 confirmed Mar 2025
- [FAISS INSTALL.md](https://github.com/facebookresearch/faiss/blob/main/INSTALL.md) — CMake flags verified
- [tokenizers-cpp (mlc-ai)](https://github.com/mlc-ai/tokenizers-cpp) — v0.1.1 confirmed May 2025
- [SQLiteCpp releases](https://github.com/SRombauts/SQLiteCpp/releases) — v3.3.3 confirmed May 2025
- [nlohmann/json releases](https://github.com/nlohmann/json/releases) — v3.12.0 confirmed Apr 2025
- [spdlog releases](https://github.com/gabime/spdlog/releases) — v1.17.0 confirmed Jan 2025
- [CLI11 releases](https://github.com/CLIUtils/CLI11/releases) — v2.4.2 confirmed May 2024
- [toml++ releases](https://github.com/marzer/tomlplusplus/releases) — v3.4.0 confirmed Oct 2024
- [efsw GitHub](https://github.com/SpartanJ/efsw) — macOS FSEvents + Linux inotify confirmed
- [cpp-tree-sitter](https://github.com/nsumner/cpp-tree-sitter) — CPM-based CMake integration pattern
- [LspCpp](https://github.com/kuafuwang/LspCpp) — evaluated and rejected (Boost dep, server-primary)
- [CMake FetchContent vs vcpkg/conan discussion](https://discourse.cmake.org/t/fetchcontent-vs-vcpkg-conan/6578) — dependency management rationale (MEDIUM confidence — community source)
<!-- GSD:stack-end -->

<!-- GSD:conventions-start source:CONVENTIONS.md -->
## Conventions

Conventions not yet established. Will populate as patterns emerge during development.
<!-- GSD:conventions-end -->

<!-- GSD:architecture-start source:ARCHITECTURE.md -->
## Architecture

Architecture not yet mapped. Follow existing patterns found in the codebase.
<!-- GSD:architecture-end -->

<!-- GSD:workflow-start source:GSD defaults -->
## GSD Workflow Enforcement

Before using Edit, Write, or other file-changing tools, start work through a GSD command so planning artifacts and execution context stay in sync.

Use these entry points:
- `/gsd:quick` for small fixes, doc updates, and ad-hoc tasks
- `/gsd:debug` for investigation and bug fixing
- `/gsd:execute-phase` for planned phase work

Do not make direct repo edits outside a GSD workflow unless the user explicitly asks to bypass it.
<!-- GSD:workflow-end -->



<!-- GSD:profile-start -->
## Developer Profile

> Profile not yet configured. Run `/gsd:profile-user` to generate your developer profile.
> This section is managed by `generate-claude-profile` -- do not edit manually.
<!-- GSD:profile-end -->

**Always use question tool for questions. Always try to provide a solution or recommendation. Much appreciated, arigato.**