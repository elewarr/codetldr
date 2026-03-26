#!/usr/bin/env bash
# test_e2e_cli.sh — End-to-end CLI workflow test
# Tests: init, start, search, status, stop in isolated temp directory
# Usage: bash tests/test_e2e_cli.sh
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

# codetldr-daemon must be on PATH for 'codetldr start' to use execvp lookup
BUILD_DIR="${BUILD_DIR:-$(dirname "$CODETLDR")}"
export PATH="$BUILD_DIR:$PATH"

echo "Using codetldr: $CODETLDR"
echo ""

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

# ---- Test: codetldr init ----

echo "--- Testing: codetldr init ---"

INIT_OUT=$("$CODETLDR" --project-root "$TEST_DIR" init 2>&1)
INIT_EXIT=$?

check "init exits 0" "$INIT_EXIT"

test -d "$TEST_DIR/.codetldr"
check "init creates .codetldr directory" $?

echo "$INIT_OUT" | grep -q "Created .codetldr"
check "init output contains 'Created .codetldr'" $?

echo "$INIT_OUT" | grep -q "Detected languages"
check "init output contains language detection" $?

echo "$INIT_OUT" | grep -q "Daemon started"
check "init starts daemon" $?

echo ""
echo "--- Waiting for daemon to reach idle state ---"
wait_for_idle "$TEST_DIR"
check "daemon reaches idle state (within 15s)" $?

# ---- Test: codetldr start (idempotent) ----

echo ""
echo "--- Testing: codetldr start (idempotent) ---"

START_OUT=$("$CODETLDR" --project-root "$TEST_DIR" start 2>&1)
START_EXIT=$?

check "start exits 0" "$START_EXIT"

echo "$START_OUT" | grep -qi "already running"
check "start reports daemon already running" $?

# ---- Test: codetldr status --json ----

echo ""
echo "--- Testing: codetldr status --json ---"

STATUS_OUT=$("$CODETLDR" --project-root "$TEST_DIR" status --json 2>&1)
STATUS_EXIT=$?

check "status --json exits 0" "$STATUS_EXIT"

STATUS_STATE=$(echo "$STATUS_OUT" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('state',''))" 2>/dev/null)
[ "$STATUS_STATE" = "idle" ]
check "status shows idle state" $?

STATUS_FILES=$(echo "$STATUS_OUT" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('files_indexed',0))" 2>/dev/null)
[ "${STATUS_FILES:-0}" -gt 0 ] 2>/dev/null
check "status shows files indexed > 0" $?

# ---- Test: codetldr search ----

echo ""
echo "--- Testing: codetldr search ---"

SEARCH_OUT=$("$CODETLDR" --project-root "$TEST_DIR" search "hello_world" 2>&1)
SEARCH_EXIT=$?

check "search exits 0" "$SEARCH_EXIT"

echo "$SEARCH_OUT" | grep -q "hello_world"
check "search finds hello_world" $?

SEARCH_JSON=$("$CODETLDR" --project-root "$TEST_DIR" search "hello_world" --json 2>&1)
SEARCH_JSON_LEN=$(echo "$SEARCH_JSON" | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d))" 2>/dev/null)
[ "${SEARCH_JSON_LEN:-0}" -gt 0 ] 2>/dev/null
check "search --json returns results (array length > 0)" $?

# ---- Test: codetldr stop ----

echo ""
echo "--- Testing: codetldr stop ---"

STOP_OUT=$("$CODETLDR" --project-root "$TEST_DIR" stop 2>&1)
STOP_EXIT=$?

check "stop exits 0" "$STOP_EXIT"

echo "$STOP_OUT" | grep -qi "stopped"
check "stop confirms daemon stopped" $?

STATUS_AFTER=$("$CODETLDR" --project-root "$TEST_DIR" status --json 2>&1)
STATUS_AFTER_STATE=$(echo "$STATUS_AFTER" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('state',''))" 2>/dev/null)
[ "$STATUS_AFTER_STATE" = "stopped" ] || [ "$STATUS_AFTER_STATE" = "not_running" ]
check "status after stop shows stopped/not_running state" $?

# ---- Summary ----

TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo ""
echo "${PASS_COUNT}/${TOTAL} checks passed"

if [ "$FAIL_COUNT" -gt 0 ]; then
  exit 1
fi
exit 0
