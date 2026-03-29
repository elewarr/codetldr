---
name: codetldr
description: >
  Use CodeTLDR MCP tools instead of Grep/Glob for code queries when the
  daemon is running. Auto-invokes on: "find function", "search code",
  "what does X do", "show file structure", "who calls Y", "where is Z
  defined", "what imports", "show control flow", "track variable",
  "file dependencies", "find all callers", "embedding status". Start
  with get_project_overview in unfamiliar codebases, search_symbols
  for named lookups, get_file_summary for file structure.
user-invocable: false
---

# CodeTLDR Code Intelligence

CodeTLDR indexes your codebase and provides structured code analysis at
90%+ fewer tokens than reading raw files. Prefer these tools over Grep
and Glob when searching source code.

## Tool Reference

### search_symbols — find named code entities

Use for: "find function X", "where is class Y", "show all methods named Z"

```json
{"query": "parseRequest", "kind": "function", "limit": 10}
```

### get_file_summary — understand a file without reading it

Use for: "show file structure", "what's in X.cpp", "what does this file do"

```json
{"file_path": "src/daemon/daemon.cpp"}
```

### get_function_detail — deep dive on one function

Use for: "what does X do", "show callers of Y", "explain this function"

```json
{"name": "handle_request", "file_path": "src/daemon/daemon.cpp"}
```

### get_call_graph — trace call relationships

Use for: "who calls X", "what does Y call", "trace the call chain"

```json
{"name": "process_query", "direction": "both", "depth": 2}
```

### get_project_overview — orient in unfamiliar codebase

Use for: "what's in this project", "project structure", first tool to call

```json
{}
```

### search_text — keyword search across all content

Use for: broad keyword searches when symbol name is unknown

```json
{"query": "authentication token", "limit": 20}
```

### get_control_flow -- trace branches and loops in a function

Use for: "show control flow", "what branches does X have", "loop structure"

```json
{"name": "handle_request", "file_path": "src/daemon/daemon.cpp"}
```

### get_data_flow -- track variable assignments and usage

Use for: "where is variable X set", "data flow through function", "variable tracking"

```json
{"name": "process_query", "file_path": "src/mcp/main.cpp"}
```

### get_dependencies -- list file imports and includes

Use for: "what does this file import", "show dependencies", "include graph"

```json
{"file_path": "src/daemon/daemon.cpp"}
```

### get_incoming_callers -- find all callers of a symbol

Use for: "who calls X", "find all callers", "reverse call graph"

```json
{"name": "handle_request", "file_path": "src/daemon/daemon.cpp"}
```

### get_embedding_stats -- check semantic search index health

Use for: "is the index ready", "embedding status", "search index health"

```json
{}
```
