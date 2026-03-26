#!/usr/bin/env bash
# test_plugin_artifacts.sh — Validate codetldr plugin artifacts
# Checks SKILL.md, hooks.json, and hook scripts for correctness.
# Usage: bash tests/test_plugin_artifacts.sh
# Exit: 0 if all checks pass, 1 if any fail.
#
# Known limitation: --plugin-dir runtime testing
# These tests validate plugin artifacts (file existence, JSON structure, content
# patterns) but do NOT test hook dispatch through `claude --plugin-dir`. That
# pathway depends on Claude Code's internal hook runner and cannot be mocked.
# To manually test full plugin loading:
#   claude --plugin-dir ./codetldr-plugin --mcp-config ./codetldr-plugin/.mcp.json
# The agentic E2E test (test_e2e_agentic.sh) covers MCP tool usage separately.

# Change to project root (relative to this script's location)
cd "$(dirname "$0")/.." || exit 1

PASS_COUNT=0
FAIL_COUNT=0

check() {
  local description="$1"
  local result="$2"
  if [ "$result" = "0" ]; then
    echo "[PASS] $description"
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    echo "[FAIL] $description"
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
}

# ---- SKILL.md checks ----

test -f codetldr-plugin/skills/codetldr/SKILL.md
check "SKILL.md exists at codetldr-plugin/skills/codetldr/SKILL.md" $?

grep -q "name: codetldr" codetldr-plugin/skills/codetldr/SKILL.md 2>/dev/null
check "SKILL.md contains 'name: codetldr'" $?

grep -q "user-invocable: false" codetldr-plugin/skills/codetldr/SKILL.md 2>/dev/null
check "SKILL.md contains 'user-invocable: false'" $?

# Check all 6 tool names
(grep -q "search_symbols" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "get_file_summary" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "get_function_detail" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "get_call_graph" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "get_project_overview" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "search_text" codetldr-plugin/skills/codetldr/SKILL.md) 2>/dev/null
check "SKILL.md contains all 6 tool names" $?

# Check trigger phrases in description
(grep -q "find function" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "search code" codetldr-plugin/skills/codetldr/SKILL.md && \
 grep -q "what does X do" codetldr-plugin/skills/codetldr/SKILL.md) 2>/dev/null
check "SKILL.md description contains trigger phrases (find function, search code, what does X do)" $?

# ---- hooks.json checks ----

python3 -c "import json; json.load(open('codetldr-plugin/hooks/hooks.json'))" 2>/dev/null
check "hooks.json exists and is valid JSON" $?

(grep -q "SessionStart" codetldr-plugin/hooks/hooks.json && \
 grep -q "PreToolUse" codetldr-plugin/hooks/hooks.json && \
 grep -q "Stop" codetldr-plugin/hooks/hooks.json) 2>/dev/null
check "hooks.json contains SessionStart, PreToolUse, Stop" $?

grep -q "Grep|Glob" codetldr-plugin/hooks/hooks.json 2>/dev/null
check "hooks.json PreToolUse has matcher 'Grep|Glob'" $?

# ---- session-start.sh checks ----

test -x codetldr-plugin/hooks/session-start.sh
check "session-start.sh exists and is executable" $?

grep -qE '"?\$CODETLDR"?\s+start|codetldr start' codetldr-plugin/hooks/session-start.sh 2>/dev/null
check "session-start.sh contains 'codetldr start'" $?

# ---- pre-tool-use.sh checks ----

test -x codetldr-plugin/hooks/pre-tool-use.sh
check "pre-tool-use.sh exists and is executable" $?

grep -q "permissionDecision" codetldr-plugin/hooks/pre-tool-use.sh 2>/dev/null
check "pre-tool-use.sh contains 'permissionDecision'" $?

grep -q "timeout 2" codetldr-plugin/hooks/pre-tool-use.sh 2>/dev/null
check "pre-tool-use.sh contains 'timeout 2'" $?

# ---- stop.sh checks ----

test -x codetldr-plugin/hooks/stop.sh
check "stop.sh exists and is executable" $?

grep -qE '"?\$CODETLDR"?\s+stop|codetldr stop' codetldr-plugin/hooks/stop.sh 2>/dev/null
check "stop.sh contains 'codetldr stop'" $?

# ---- Synthetic passthrough tests ----

# Test 16: empty input exits 0 with no deny output
OUTPUT=$(echo '{}' | bash codetldr-plugin/hooks/pre-tool-use.sh 2>/dev/null)
EXIT_CODE=$?
if [ "$EXIT_CODE" = "0" ] && ! echo "$OUTPUT" | grep -q "deny"; then
  check "Synthetic passthrough: empty input exits 0 with no deny output" 0
else
  check "Synthetic passthrough: empty input exits 0 with no deny output" 1
fi

# Test 17: short pattern (< 3 chars) exits 0
OUTPUT=$(echo '{"tool_name":"Grep","tool_input":{"pattern":"ab"}}' | bash codetldr-plugin/hooks/pre-tool-use.sh 2>/dev/null)
EXIT_CODE=$?
if [ "$EXIT_CODE" = "0" ] && ! echo "$OUTPUT" | grep -q "deny"; then
  check "Synthetic passthrough: short pattern exits 0 without deny" 0
else
  check "Synthetic passthrough: short pattern exits 0 without deny" 1
fi

# Test 18: regex metachar in pattern exits 0
OUTPUT=$(echo '{"tool_name":"Grep","tool_input":{"pattern":"foo\\dbar"}}' | bash codetldr-plugin/hooks/pre-tool-use.sh 2>/dev/null)
EXIT_CODE=$?
if [ "$EXIT_CODE" = "0" ] && ! echo "$OUTPUT" | grep -q "deny"; then
  check "Synthetic passthrough: regex pattern exits 0 without deny" 0
else
  check "Synthetic passthrough: regex pattern exits 0 without deny" 1
fi

# ---- .claude-plugin/plugin.json checks ----

test -f codetldr-plugin/.claude-plugin/plugin.json
check ".claude-plugin/plugin.json exists" $?

python3 -c "import json; json.load(open('codetldr-plugin/.claude-plugin/plugin.json'))" 2>/dev/null
check ".claude-plugin/plugin.json is valid JSON" $?

grep -q '"name"' codetldr-plugin/.claude-plugin/plugin.json 2>/dev/null
check "plugin.json contains name field" $?

grep -q '"codetldr"' codetldr-plugin/.claude-plugin/plugin.json 2>/dev/null
check "plugin.json name is codetldr" $?

grep -q '"version"' codetldr-plugin/.claude-plugin/plugin.json 2>/dev/null
check "plugin.json contains version field" $?

grep -q '"1.1.0"' codetldr-plugin/.claude-plugin/plugin.json 2>/dev/null
check "plugin.json version is 1.1.0" $?

grep -q '"description"' codetldr-plugin/.claude-plugin/plugin.json 2>/dev/null
check "plugin.json contains description field" $?

# ---- .mcp.json checks ----

test -f codetldr-plugin/.mcp.json
check ".mcp.json exists at plugin root" $?

python3 -c "import json; json.load(open('codetldr-plugin/.mcp.json'))" 2>/dev/null
check ".mcp.json is valid JSON" $?

grep -q '"mcpServers"' codetldr-plugin/.mcp.json 2>/dev/null
check ".mcp.json contains mcpServers key" $?

grep -q '"codetldr"' codetldr-plugin/.mcp.json 2>/dev/null
check ".mcp.json registers codetldr server" $?

grep -q '"codetldr-mcp"' codetldr-plugin/.mcp.json 2>/dev/null
check ".mcp.json command is codetldr-mcp" $?

# ---- hooks.json path update checks ----

grep -q 'CLAUDE_PLUGIN_ROOT' codetldr-plugin/hooks/hooks.json 2>/dev/null
check "hooks.json uses \${CLAUDE_PLUGIN_ROOT} paths" $?

# Verify no bare relative paths remain (should NOT match "hooks/session-start.sh" without CLAUDE_PLUGIN_ROOT prefix)
! grep -P '"hooks/[a-z]' codetldr-plugin/hooks/hooks.json 2>/dev/null
check "hooks.json has no bare relative command paths" $?

# ---- README.md checks ----

test -f codetldr-plugin/README.md
check "README.md exists at plugin root" $?

grep -q 'claude plugin install' codetldr-plugin/README.md 2>/dev/null
check "README.md contains install command" $?

grep -q '\-\-plugin-dir' codetldr-plugin/README.md 2>/dev/null
check "README.md contains --plugin-dir test command" $?

# ---- Summary ----

TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo ""
echo "${PASS_COUNT}/${TOTAL} checks passed"

if [ "$FAIL_COUNT" -gt 0 ]; then
  exit 1
fi
exit 0
