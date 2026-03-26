---
name: codetldr
description: >
  Use CodeTLDR MCP tools instead of Grep/Glob for code queries when the
  daemon is running. Auto-invokes on: "find function", "search code",
  "what does X do", "show file structure", "who calls Y", "where is Z
  defined", "what imports". Start with get_project_overview in unfamiliar
  codebases, search_symbols for named lookups, get_file_summary for
  file structure.
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
