#!/usr/bin/env bash
# CodeTLDR SessionStart hook - auto-starts daemon.
# NEVER blocks session startup. Exits 0 on any failure.

# Find binary using shared discovery script
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/../scripts/find-binary.sh"

# Not found — exit silently, never block session startup
[ -z "$CODETLDR" ] && exit 0

# Start daemon idempotently (CLI handles "already running" case)
"$CODETLDR" start >/dev/null 2>&1 || true

exit 0
