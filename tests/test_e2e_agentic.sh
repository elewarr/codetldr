#!/usr/bin/env bash
# test_e2e_agentic.sh — End-to-end agentic plugin test
# Verifies: claude CLI with plugin uses CodeTLDR MCP tools for code queries
# Usage: bash tests/test_e2e_agentic.sh
# Exit: 0 if all checks pass (or skipped), 1 if any fail.

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

# ---- Graceful skip if claude CLI unavailable ----

if ! command -v claude >/dev/null 2>&1; then
  echo "SKIP: claude CLI not found — agentic test requires claude CLI on PATH"
  echo "Install: https://claude.ai/download"
  exit 0
fi

# ---- Binary discovery ----

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

CODETLDR="${CODETLDR_BIN:-$(command -v codetldr 2>/dev/null)}"
if [ -z "$CODETLDR" ]; then
  for CANDIDATE in \
    "$REPO_ROOT/cmake-build-debug/codetldr" \
    "$REPO_ROOT/build/codetldr"; do
    if [ -x "$CANDIDATE" ]; then
      CODETLDR="$CANDIDATE"
      BUILD_DIR="$(dirname "$CANDIDATE")"
      break
    fi
  done
fi

if [ -z "$CODETLDR" ]; then
  echo "ERROR: codetldr binary not found"
  echo "  Set CODETLDR_BIN env var, put codetldr on PATH, or build to cmake-build-debug/ or build/"
  exit 1
fi

# codetldr-daemon must be on PATH for execvp lookup
BUILD_DIR="${BUILD_DIR:-$(dirname "$CODETLDR")}"
export PATH="$BUILD_DIR:$PATH"

# Verify codetldr-mcp is available
if [ ! -x "$BUILD_DIR/codetldr-mcp" ]; then
  echo "ERROR: codetldr-mcp not found in $BUILD_DIR"
  echo "  Build the project first: cmake --build cmake-build-debug --target codetldr-mcp"
  exit 1
fi

CODETLDR_MCP="$BUILD_DIR/codetldr-mcp"

echo "Using codetldr: $CODETLDR"
echo "Using codetldr-mcp: $CODETLDR_MCP"
echo ""

# ---- Plugin directory check ----

PLUGIN_DIR="$REPO_ROOT/codetldr-plugin"

if [ ! -d "$PLUGIN_DIR/.claude-plugin" ]; then
  echo "ERROR: Plugin directory not found at $PLUGIN_DIR/.claude-plugin"
  echo "  Run the plugin packaging phase first."
  exit 1
fi

# ---- Temp directory setup ----

TEST_DIR=$(mktemp -d)
trap '"$CODETLDR" --project-root "$TEST_DIR" stop 2>/dev/null; rm -rf "$TEST_DIR"' EXIT

mkdir -p "$TEST_DIR/src"
cat > "$TEST_DIR/src/hello.cpp" << 'CPPEOF'
#include <iostream>
void hello_world() { std::cout << "Hello\n"; }
int compute_sum(int a, int b) { return a + b; }
int main() { hello_world(); return 0; }
CPPEOF

# Create .mcp.json in test directory registering codetldr-mcp
cat > "$TEST_DIR/.mcp.json" << MCPEOF
{
  "mcpServers": {
    "codetldr": {
      "command": "$CODETLDR_MCP",
      "args": []
    }
  }
}
MCPEOF

# ---- wait_for_idle helper ----

wait_for_idle() {
  local project="$1"
  local timeout=15
  local elapsed=0
  while [ $elapsed -lt $timeout ]; do
    STATE=$("$CODETLDR" --project-root "$project" status --json 2>/dev/null | \
      python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('state',''))" 2>/dev/null)
    [ "$STATE" = "idle" ] && return 0
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

# ---- Initialize and wait for daemon ----

echo "--- Initializing test project ---"
"$CODETLDR" --project-root "$TEST_DIR" init >/dev/null 2>&1
wait_for_idle "$TEST_DIR"
echo "Daemon ready in $TEST_DIR"
echo ""

# ---- Run claude --print with stream-json output ----

echo "--- Running claude --print (this may take up to 30s) ---"

OUTPUT=$(cd "$TEST_DIR" && claude --print \
  --output-format stream-json \
  --dangerously-skip-permissions \
  --no-session-persistence \
  --max-budget-usd 0.50 \
  "Using the search_symbols tool, find the function named 'hello_world'" 2>/dev/null)
CLAUDE_EXIT=$?

check "claude --print exits successfully" "$CLAUDE_EXIT"
echo ""

# ---- Check MCP server connected ----

echo "--- Checking MCP server connection ---"

echo "$OUTPUT" | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        obj = json.loads(line.strip())
        if obj.get('type') == 'system' and obj.get('subtype') == 'init':
            for s in obj.get('mcp_servers', []):
                if 'codetldr' in s.get('name', '') and s.get('status') == 'connected':
                    print('connected')
                    sys.exit(0)
    except: pass
" 2>/dev/null | grep -q "connected"
check "CodeTLDR MCP server connected" $?

# ---- Check MCP tool was used ----

echo "--- Checking MCP tool usage ---"

echo "$OUTPUT" | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        obj = json.loads(line.strip())
        if obj.get('type') == 'assistant':
            for c in obj.get('message', {}).get('content', []):
                if c.get('type') == 'tool_use' and 'codetldr' in c.get('name', ''):
                    print('used')
                    sys.exit(0)
    except: pass
" 2>/dev/null | grep -q "used"
check "Agent used at least one CodeTLDR MCP tool" $?

# ---- Summary ----

TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo ""
echo "${PASS_COUNT}/${TOTAL} checks passed"

if [ "$FAIL_COUNT" -gt 0 ]; then
  exit 1
fi
exit 0
