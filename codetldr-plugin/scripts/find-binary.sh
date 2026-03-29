#!/usr/bin/env bash
# Shared binary discovery for CodeTLDR hooks.
# Sourced by hook scripts — sets CODETLDR to the binary path or empty string.
# Fallback order per D-02: /opt/homebrew/bin, /usr/local/bin, PATH, ~/.local/bin

CODETLDR=$(command -v codetldr 2>/dev/null)
if [ -z "$CODETLDR" ]; then
  for FALLBACK in "/opt/homebrew/bin/codetldr" "/usr/local/bin/codetldr" "${HOME}/.local/bin/codetldr"; do
    if [ -x "$FALLBACK" ]; then
      CODETLDR="$FALLBACK"
      break
    fi
  done
fi
