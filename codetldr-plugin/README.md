# CodeTLDR Claude Code Plugin

Token-efficient code intelligence for LLM agents. Provides structured symbol search, call graphs, and file summaries at 90%+ fewer tokens than reading raw files.

## What This Plugin Provides

- **Hooks** — Automatically starts the CodeTLDR daemon at session start, nudges agents away from broad Grep/Glob searches toward targeted symbol lookup, and shuts down the daemon cleanly on stop
- **Skill** — Teaches Claude Code how and when to use CodeTLDR's analysis tools (`search_symbols`, `get_file_summary`, `get_function_detail`, `get_call_graph`, `get_project_overview`, `search_text`)
- **MCP server registration** — Registers the `codetldr-mcp` stdio server so Claude Code can call CodeTLDR tools directly

## Prerequisites

Both binaries must be on your PATH before installing the plugin:

```bash
# Verify codetldr is available
codetldr --version

# Verify codetldr-mcp is available
codetldr-mcp --version
```

If either binary is missing, build them first:

```bash
cmake -B build -G Ninja && cmake --build build
# Then add build/ to your PATH, or install to /usr/local/bin
```

## Install

Install the plugin into Claude Code (copies to plugin cache):

```bash
claude plugin install ./codetldr-plugin
```

After installation, the plugin is active for all Claude Code sessions. No further configuration needed.

## Local Development / Testing

Test hooks and skill loading without a full install:

```bash
claude --plugin-dir ./codetldr-plugin
```

Test the full plugin including MCP server registration:

```bash
claude --plugin-dir ./codetldr-plugin --mcp-config ./codetldr-plugin/.mcp.json
```

**Note:** `--plugin-dir` does not automatically load `.mcp.json`. The `--mcp-config` flag is required separately when using `--plugin-dir` for development testing. After `claude plugin install`, MCP registration is automatic.

## How MCP Server Discovery Works

The plugin `.mcp.json` uses empty args (`"args": []`), while `codetldr init` generates a project-specific `.mcp.json` with explicit `--project-root /path/to/project`. This is intentional:

- **Plugin (portable):** Cannot know the project root at install time. `codetldr-mcp` falls back to detecting the Git root from the current working directory. This works reliably because Claude Code sets cwd to the project root when spawning MCP servers.
- **Init (project-specific):** Pins the absolute project root path. More reliable if you work with multiple projects or launch Claude Code from a parent directory.

**Recommendation:** For best reliability, run `codetldr init` in each project. The init-generated `.mcp.json` takes precedence over the plugin's when both are present.

## Validate Plugin Structure

```bash
claude plugin validate ./codetldr-plugin
```

## Run Artifact Tests

```bash
bash tests/test_plugin_artifacts.sh
```
