#!/usr/bin/env bash
# CodeTLDR SessionStart hook - auto-starts daemon.
# NEVER blocks session startup. Exits 0 on any failure.

# Find binary: check PATH first, then common install locations
CODETLDR=$(command -v codetldr 2>/dev/null)
if [ -z "$CODETLDR" ]; then
  for FALLBACK in "${HOME}/.local/bin/codetldr" "/usr/local/bin/codetldr"; do
    if [ -x "$FALLBACK" ]; then
      CODETLDR="$FALLBACK"
      break
    fi
  done
fi

# Not found — exit silently, never block session startup
[ -z "$CODETLDR" ] && exit 0

# Start daemon idempotently (CLI handles "already running" case)
"$CODETLDR" start >/dev/null 2>&1 || true

exit 0
