#!/usr/bin/env bash
# CodeTLDR PreToolUse hook - redirect Grep/Glob to MCP tools.
# Pass through on ANY uncertainty. Only deny when index is confirmed healthy.

# 1. Find binary using shared discovery script
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/../scripts/find-binary.sh"

# Not found — pass through
[ -z "$CODETLDR" ] && exit 0

# 2. jq required for JSON parsing — pass through if missing
command -v jq >/dev/null 2>&1 || exit 0

# 3. Read hook input from stdin
INPUT=$(cat)

# 4. Parse tool name
TOOL_NAME=$(echo "$INPUT" | jq -r '.tool_name // ""' 2>/dev/null) || exit 0

# 5. Extract the search pattern (Grep uses 'pattern', Glob uses 'pattern')
PATTERN=$(echo "$INPUT" | jq -r '.tool_input.pattern // .tool_input.glob // ""' 2>/dev/null) || exit 0

# 6. Pass through: empty pattern
[ -z "$PATTERN" ] && exit 0

# 7. Pass through: query too short (< 3 chars)
[ "${#PATTERN}" -lt 3 ] && exit 0

# 8. Pass through: contains regex metacharacters
# Heuristic: \d \w \s \W \D \S or brackets/quantifiers indicate regex
if echo "$PATTERN" | grep -qE '\\[dwsWDS]|\[|\(|\{|\^|\$|\+|\?'; then
  exit 0
fi

# 9. Pass through: path targets non-source extensions
# Extract include/path argument for Grep, or glob pattern for Glob
PATH_ARG=$(echo "$INPUT" | jq -r '.tool_input.include // .tool_input.path // ""' 2>/dev/null)
if echo "$PATH_ARG" | grep -qE '\.(md|json|yaml|yml|txt|toml|ini|lock|sum|mod)$'; then
  exit 0
fi

# 10. Check daemon health with 2s timeout — well within 5s hook budget
STATUS=$(timeout 2 "$CODETLDR" status --json 2>/dev/null) || exit 0
STATE=$(echo "$STATUS" | jq -r '.state // ""' 2>/dev/null) || exit 0

# 11. Pass through: daemon not idle (starting, indexing, stopped, error, etc.)
[ "$STATE" != "idle" ] && exit 0

# 12. All checks passed — suggest equivalent MCP tool
if [ "$TOOL_NAME" = "Glob" ]; then
  SUGGESTION="Use get_file_summary or get_project_overview MCP tool instead of Glob for source file discovery."
else
  SUGGESTION="Use search_symbols (exact name) or search_text (keyword) MCP tool instead of Grep for source code search."
fi

jq -n \
  --arg reason "$SUGGESTION" \
  '{
    hookSpecificOutput: {
      hookEventName: "PreToolUse",
      permissionDecision: "deny",
      permissionDecisionReason: $reason
    }
  }'

exit 0
