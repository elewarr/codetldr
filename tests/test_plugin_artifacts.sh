#!/usr/bin/env bash
# test_plugin_artifacts.sh — Validate codetldr plugin artifacts
# Checks SKILL.md, hooks.json, and hook scripts for correctness.
# Usage: bash tests/test_plugin_artifacts.sh
# Exit: 0 if all checks pass, 1 if any fail.

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

# ---- Summary ----

TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo ""
echo "${PASS_COUNT}/${TOTAL} checks passed"

if [ "$FAIL_COUNT" -gt 0 ]; then
  exit 1
fi
exit 0
