# CodeTLDR

A C++ code analysis tool that gives LLM agents token-efficient, structured understanding of codebases. Runs as a background daemon, exposes analysis through MCP (Model Context Protocol) and CLI, and provides semantic code search via self-contained embedding inference.

**Core value:** Give LLM agents exactly the code context they need — structure, call graphs, control flow, data flow, and dependencies — at 90%+ fewer tokens than reading raw files.

## Installation

### Homebrew (recommended)

```console
$ brew tap codetldr/tap
$ brew install codetldr
```

This installs three binaries: `codetldr` (CLI), `codetldr-daemon` (background daemon), and `codetldr-mcp` (MCP stdio server).

Verify the installation:

```console
$ codetldr doctor
[PASS] Binary found: codetldr (v0.5.0)
[PASS] Daemon reachable (PID 12483)
[PASS] MCP server binary found: /opt/homebrew/bin/codetldr-mcp
[SKIP] Hook scripts (plugin root not found -- set CLAUDE_PLUGIN_ROOT or run from project dir)

All checks passed.
```

### Build from source

Requirements:
- CMake 3.20+
- Ninja (recommended)
- C++17 compiler (Clang or GCC)
- Rust toolchain (`rustc` and `cargo` on PATH) — required at build time for tokenizers-cpp

```console
$ git clone https://github.com/codetldr/codetldr.git
$ cd codetldr
$ cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
$ cmake --build build
$ sudo cmake --install build
```

All other dependencies (Tree-sitter, SQLite, spdlog, nlohmann/json, CLI11, toml++, SQLiteCpp, efsw) are fetched automatically by CMake at configure time via FetchContent.

To enable semantic search (optional, requires ONNX Runtime and FAISS):

```console
$ cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODETLDR_ENABLE_SEMANTIC_SEARCH=ON
$ cmake --build build
```

## Quick Start

### Initialize a project

```console
$ cd your-project
$ codetldr init
[codetldr] Initializing project at /path/to/your-project
[codetldr] Starting daemon...
[codetldr] Indexing 142 files across 3 languages...
[codetldr] Done. Daemon running on .codetldr/daemon.sock
```

This creates a `.codetldr/` directory in your project root with the index database and daemon socket.

### Your first MCP tool call

With the daemon running, you can query through the MCP interface. Here is an example `search_symbols` request and response:

Request:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "search_symbols",
    "arguments": {
      "query": "parse",
      "limit": 3
    }
  }
}
```

Response:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"results\":[{\"file_path\":\"src/parser.py\",\"line\":42,\"name\":\"parse_expression\",\"kind\":\"function\",\"signature\":\"def parse_expression(self, tokens: list) -> AST\"}],\"search_source\":\"fts5\",\"query\":\"parse\"}"
      }
    ]
  }
}
```

For a quick project overview, use `get_project_overview` (no parameters required) — it returns language breakdown, file counts, and top-level symbols.

## Language Support

### Tree-sitter Grammars (14)

All 14 grammars are compiled into the binary. No runtime download required.

| Language | Extensions | Grammar Source |
|----------|-----------|----------------|
| Python | `.py`, `.pyi` | [tree-sitter-python](https://github.com/tree-sitter/tree-sitter-python) |
| JavaScript | `.js`, `.mjs`, `.cjs` | [tree-sitter-javascript](https://github.com/tree-sitter/tree-sitter-javascript) |
| TypeScript | `.ts` | [tree-sitter-typescript](https://github.com/tree-sitter/tree-sitter-typescript) |
| TSX | `.tsx` | [tree-sitter-typescript](https://github.com/tree-sitter/tree-sitter-typescript) |
| Rust | `.rs` | [tree-sitter-rust](https://github.com/tree-sitter/tree-sitter-rust) |
| Go | `.go` | [tree-sitter-go](https://github.com/tree-sitter/tree-sitter-go) |
| C | `.c`, `.h` | [tree-sitter-c](https://github.com/tree-sitter/tree-sitter-c) |
| C++ | `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hxx` | [tree-sitter-cpp](https://github.com/tree-sitter/tree-sitter-cpp) |
| Java | `.java` | [tree-sitter-java](https://github.com/tree-sitter/tree-sitter-java) |
| Kotlin | `.kt`, `.kts` | [tree-sitter-kotlin](https://github.com/fwcd/tree-sitter-kotlin) |
| Swift | `.swift` | [tree-sitter-swift](https://github.com/alex-pinkus/tree-sitter-swift) |
| Objective-C | `.m`, `.mm` | [tree-sitter-objc](https://github.com/tree-sitter-grammars/tree-sitter-objc) |
| Ruby | `.rb`, `.rake`, `.gemspec`, `.ru` | [tree-sitter-ruby](https://github.com/tree-sitter/tree-sitter-ruby) |
| Lua | `.lua` | [tree-sitter-lua](https://github.com/tree-sitter-grammars/tree-sitter-lua) |

### LSP Backends (10)

LSP backends are discovered automatically at daemon startup. When available, they provide enhanced symbol resolution, cross-file references, and call hierarchy.

| Language | Server Binary | Discovery Method | Notes |
|----------|--------------|------------------|-------|
| C/C++ | `clangd` | PATH lookup | Requires `compile_commands.json` for accurate results |
| Python | `pyright-langserver` or `basedpyright-langserver` | PATH lookup | Falls back to basedpyright if pyright not found |
| TypeScript/JavaScript | `typescript-language-server` | PATH lookup | Also handles `.js`/`.jsx` files |
| Rust | `rust-analyzer` | PATH lookup | Version-checked at startup |
| Go | `gopls` | PATH lookup | Version-checked at startup |
| Kotlin | `kotlin-language-server` | PATH lookup | 120s handshake timeout (JVM cold start) |
| Java | `jdtls` | PATH lookup | Requires Java 21+; 180s handshake timeout |
| Ruby | `ruby-lsp` | PATH lookup | Version-checked at startup |
| Lua | `lua-language-server` | PATH lookup | Version-checked at startup |
| Swift | `sourcekit-lsp` | `xcrun --find sourcekit-lsp` | Requires Xcode or Swift toolchain; no call hierarchy support |

When an LSP backend is not available, CodeTLDR falls back to Tree-sitter-only analysis (symbols, calls, CFG, DFG) with no degradation in those capabilities.

## MCP Tools Reference

CodeTLDR exposes 11 tools via the MCP protocol. The MCP server (`codetldr-mcp`) communicates over stdio using newline-delimited JSON-RPC.

### search_symbols

Search for symbols (functions, classes, methods) by name using FTS5 full-text search with BM25 ranking, with workspace/symbol LSP fast path when available.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | yes | Symbol name or partial name to search for |
| `kind` | string | no | Filter by symbol kind: function, class, method, struct, enum |
| `limit` | integer | no | Maximum results (default 20) |

```json
{"method":"tools/call","params":{"name":"search_symbols","arguments":{"query":"parse","kind":"function","limit":5}}}
```

### search_text

Full-text search over symbol names, documentation, and source content. Use for finding code by keyword, not by exact symbol name.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | yes | Search terms |
| `limit` | integer | no | Maximum results (default 20) |

```json
{"method":"tools/call","params":{"name":"search_text","arguments":{"query":"error handling"}}}
```

### get_file_summary

Get a token-efficient structured summary of a source file: all symbols with signatures, line ranges, and documentation. Condensed format uses <10% of raw source tokens.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | yes | Absolute or project-relative path to the source file |
| `format` | string | no | Output format: condensed (default), detailed, diff_aware |

```json
{"method":"tools/call","params":{"name":"get_file_summary","arguments":{"file_path":"src/main.py"}}}
```

### get_function_detail

Get detailed information about a specific function or method: signature, documentation, callers, callees, and line range.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | yes | Exact function or method name |
| `file_path` | string | no | Scope search to this file |

```json
{"method":"tools/call","params":{"name":"get_function_detail","arguments":{"name":"parse_expression"}}}
```

### get_call_graph

Get forward (callees) and backward (callers) call relationships for a function. Returns approximate call graph from AST analysis.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | yes | Function name |
| `direction` | string | no | callers, callees, or both (default: both) |
| `depth` | integer | no | Traversal depth (default 1) |

```json
{"method":"tools/call","params":{"name":"get_call_graph","arguments":{"name":"handle_request","direction":"callers","depth":2}}}
```

### get_project_overview

Get a high-level overview of the project: language breakdown, file count, top-level symbols by language, indexing status. Use as first call to orient in an unfamiliar codebase.

No parameters.

```json
{"method":"tools/call","params":{"name":"get_project_overview","arguments":{}}}
```

### get_control_flow

Get control flow graph (CFG) for a function: branches, loops, returns, and switch cases. Returns nodes with type, condition, line, and nesting depth.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | yes | Exact function or method name |
| `file_path` | string | no | Scope search to this file |

```json
{"method":"tools/call","params":{"name":"get_control_flow","arguments":{"name":"process_data","file_path":"src/pipeline.py"}}}
```

### get_data_flow

Get data flow graph (DFG) for a function: assignments, parameter bindings, and return values. Returns edges with type, lhs variable, rhs snippet, and line.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | yes | Exact function or method name |
| `file_path` | string | no | Scope search to this file |

```json
{"method":"tools/call","params":{"name":"get_data_flow","arguments":{"name":"transform","file_path":"src/utils.rs"}}}
```

### get_embedding_stats

Get embedding pipeline observability metrics: model status, inference latency percentiles (p50/p95/p99), throughput, queue depth, FAISS index size, and health status.

No parameters.

```json
{"method":"tools/call","params":{"name":"get_embedding_stats","arguments":{}}}
```

### get_incoming_callers

Get the list of callers for a symbol using LSP call hierarchy (callHierarchy/incomingCalls). Falls back to lsp_references, then Tree-sitter approximate data.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | yes | Symbol name to find callers of |
| `file` | string | no | Scope to this file path |

```json
{"method":"tools/call","params":{"name":"get_incoming_callers","arguments":{"name":"validate_input"}}}
```

### get_dependencies

Get cross-file import/include/require dependencies for a file. Returns which files this file imports and which files import it, resolved via LSP with Tree-sitter fallback.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | yes | Absolute or project-relative file path |

```json
{"method":"tools/call","params":{"name":"get_dependencies","arguments":{"file":"src/auth/login.ts"}}}
```

## Troubleshooting

### sourcekit-lsp not found (missing Xcode CLT)

**Symptom:** Swift files get Tree-sitter-only analysis; `codetldr doctor` or daemon logs show "sourcekit-lsp not found".

**Fix:** Install Xcode Command Line Tools:

```console
$ xcode-select --install
```

Or install the full Xcode app from the Mac App Store. CodeTLDR discovers sourcekit-lsp via `xcrun --find sourcekit-lsp`, so it must be available through Xcode's toolchain.

### Homebrew PATH issues

**Symptom:** `codetldr: command not found` after `brew install codetldr`, especially on Apple Silicon Macs.

**Fix:** Ensure `/opt/homebrew/bin` is in your PATH. Add to your shell profile:

```console
$ echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
$ eval "$(/opt/homebrew/bin/brew shellenv)"
```

CodeTLDR's hooks include a fallback chain that checks `/opt/homebrew/bin`, `/usr/local/bin`, and `~/.local/bin`, but your shell PATH should include the Homebrew prefix for direct CLI usage.

### Duplicate MCP registration

**Symptom:** MCP tools appear twice in your agent's tool list, typically when both `codetldr init` and the Claude Code plugin are active.

**Fix:** If you installed via the Claude Code plugin, you do not need to run `codetldr init` for MCP registration — the plugin handles it. If you see duplicates:

1. Check `.mcp.json` in your project root — remove the `codetldr` entry if the plugin is installed
2. Or remove the plugin and use `codetldr init` for manual setup

The `codetldr init` command detects plugin presence and skips `.mcp.json` generation to avoid this conflict.

### Empty Swift index (kDegraded warning)

**Symptom:** Daemon logs show "degraded" status for Swift; LSP queries return empty results even though sourcekit-lsp is found.

**Fix:** sourcekit-lsp needs a build index to provide symbol information. Build your Swift project first:

```console
$ swift build        # Swift Package Manager projects
$ xcodebuild build   # Xcode projects
```

After building, restart the daemon (`codetldr stop && codetldr start`) and the index should populate. The "degraded" status indicates sourcekit-lsp is running but has no index data yet.

## License

MIT
