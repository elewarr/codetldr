#!/usr/bin/env bash
# CodeTLDR Stop hook - shuts down daemon on session end.
# Exits 0 on any failure.

# Find binary using shared discovery script
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/../scripts/find-binary.sh"

# Not found — exit silently
[ -z "$CODETLDR" ] && exit 0

# Stop daemon cleanly
"$CODETLDR" stop >/dev/null 2>&1 || true

exit 0
