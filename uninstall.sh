#!/usr/bin/env bash
# uninstall.sh — Remove the installed lswasm binary and install metadata.
#
# Usage:
#   ./uninstall.sh
#
# The script reads install metadata from
#   ~/.local/state/lswasm/install-state.env
# which is written by install.sh.
#
# Actions performed:
#   1. Delete the installed lswasm binary.
#   2. Remove the install directory if it is empty.
#   3. Remove the state file.

set -euo pipefail

INSTALLED_BIN=""
INSTALL_DIR=""

# ── Load saved state (if any) ───────────────────────────────────────────
STATE_FILE="${HOME}/.local/state/lswasm/install-state.env"
if [[ -f "$STATE_FILE" ]]; then
  # shellcheck source=/dev/null
  source "$STATE_FILE"
fi

if [[ -z "$INSTALLED_BIN" ]]; then
  echo "Error: install state not found at $STATE_FILE" >&2
  echo "Has lswasm been installed with install.sh?" >&2
  exit 1
fi

# ── Remove installed binary ─────────────────────────────────────────────
if [[ -f "$INSTALLED_BIN" ]]; then
  rm -f "$INSTALLED_BIN"
  echo "Removed binary: $INSTALLED_BIN"
else
  echo "Binary already absent: $INSTALLED_BIN"
fi

# ── Remove install directory if empty ───────────────────────────────────
if [[ -n "$INSTALL_DIR" && -d "$INSTALL_DIR" ]]; then
  if rmdir "$INSTALL_DIR" 2>/dev/null; then
    echo "Removed empty install directory: $INSTALL_DIR"
  else
    echo "Install directory not empty, kept: $INSTALL_DIR"
  fi
fi

# ── Remove state file ───────────────────────────────────────────────────
if [[ -f "$STATE_FILE" ]]; then
  rm -f "$STATE_FILE"
  rmdir "$(dirname "$STATE_FILE")" 2>/dev/null || true
  echo "Removed state file: $STATE_FILE"
fi

echo ""
echo "lswasm has been uninstalled."
