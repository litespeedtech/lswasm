#!/usr/bin/env bash
# uninstall.sh — Remove the lswasm systemd user service and installed binary.
#
# Usage:
#   ./uninstall.sh [--service-name <name>]
#
# Optional flags:
#   --service-name <name>  systemd unit name (default: read from state file,
#                          or lswasm.service if no state file exists).
#
# The script reads install metadata from
#   ~/.local/state/lswasm/install-state.env
# which is written by install.sh.  Any values provided on the command line
# override those in the state file.
#
# Actions performed:
#   1. Stop and disable the systemd user service (if running).
#   2. Remove the unit file and reload systemd.
#   3. Delete the installed lswasm binary.
#   4. Remove the install directory if it is empty.
#   5. Remove the state file.

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────
SERVICE_NAME=""
INSTALLED_BIN=""
INSTALL_DIR=""
UNIT_PATH=""

# ── Load saved state (if any) ───────────────────────────────────────────
STATE_FILE="${HOME}/.local/state/lswasm/install-state.env"
if [[ -f "$STATE_FILE" ]]; then
  # shellcheck source=/dev/null
  source "$STATE_FILE"
fi

# ── Parse arguments ─────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --service-name)
      SERVICE_NAME="$2"; shift 2 ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--service-name <name>]" >&2
      exit 1 ;;
  esac
done

# ── Apply defaults ──────────────────────────────────────────────────────
SERVICE_NAME="${SERVICE_NAME:-lswasm.service}"
UNIT_PATH="${UNIT_PATH:-${HOME}/.config/systemd/user/${SERVICE_NAME}}"

# ── Stop and disable the service ─────────────────────────────────────────
if systemctl --user is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
  echo "Stopping $SERVICE_NAME ..."
  systemctl --user stop "$SERVICE_NAME"
fi

if systemctl --user is-enabled --quiet "$SERVICE_NAME" 2>/dev/null; then
  echo "Disabling $SERVICE_NAME ..."
  systemctl --user disable "$SERVICE_NAME"
fi

# ── Remove unit file ────────────────────────────────────────────────────
if [[ -f "$UNIT_PATH" ]]; then
  rm -f "$UNIT_PATH"
  echo "Removed unit file: $UNIT_PATH"
fi

systemctl --user daemon-reload
echo "systemd user daemon reloaded."

# ── Remove installed binary ─────────────────────────────────────────────
if [[ -n "$INSTALLED_BIN" && -f "$INSTALLED_BIN" ]]; then
  rm -f "$INSTALLED_BIN"
  echo "Removed binary: $INSTALLED_BIN"
fi

# ── Remove install directory if empty ────────────────────────────────────
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
