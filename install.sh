#!/usr/bin/env bash
# install.sh — Install the lswasm binary and record install metadata.
#
# Usage:
#   ./install.sh --bin <path> --install-dir <path>
#
# Required flags:
#   --bin <path>          Path to the compiled lswasm binary.
#   --install-dir <path>  Directory where the binary will be copied.
#
# Example:
#   ./install.sh --bin ./build/lswasm --install-dir /opt/lswasm

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────
BIN=""
INSTALL_DIR=""

# ── Parse arguments ─────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin)
      BIN="$2"; shift 2 ;;
    --install-dir)
      INSTALL_DIR="$2"; shift 2 ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 --bin <path> --install-dir <path>" >&2
      exit 1 ;;
  esac
done

# ── Validate required flags ─────────────────────────────────────────────
if [[ -z "$BIN" ]]; then
  echo "Error: --bin is required." >&2
  exit 1
fi
if [[ -z "$INSTALL_DIR" ]]; then
  echo "Error: --install-dir is required." >&2
  exit 1
fi

# ── Validate source binary ──────────────────────────────────────────────
if [[ ! -f "$BIN" ]]; then
  echo "Error: binary not found: $BIN" >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "Error: binary is not executable: $BIN" >&2
  exit 1
fi

# ── Resolve all paths to absolute ───────────────────────────────────────
# Expand leading tilde (not expanded by the shell when values are quoted).
BIN="${BIN/#\~/$HOME}"
INSTALL_DIR="${INSTALL_DIR/#\~/$HOME}"

BIN="$(realpath "$BIN")"
INSTALL_DIR="$(realpath -m "$INSTALL_DIR")"

# ── Install binary ──────────────────────────────────────────────────────
INSTALLED_BIN="${INSTALL_DIR}/lswasm"
IS_UPGRADE=false

if [[ -f "$INSTALLED_BIN" ]]; then
  IS_UPGRADE=true
  echo "Existing installation detected at $INSTALLED_BIN"
fi

mkdir -p "$INSTALL_DIR"
cp "$BIN" "$INSTALLED_BIN"
chmod 755 "$INSTALLED_BIN"
if $IS_UPGRADE; then
  echo "Updated binary at $INSTALLED_BIN"
else
  echo "Installed binary to $INSTALLED_BIN"
fi

# ── Persist install metadata for upgrade/uninstall ──────────────────────
# The state file is sourced by upgrade.sh as shell, so it must be writable
# only by the current user.  We:
#   1. Lock the state directory to 0700 (owner-only) so a malicious neighbor
#      cannot plant a symlink at the state-file path.
#   2. Write to a freshly-created temporary file under a tight umask and
#      atomically rename it over the final target, so a symlink swap mid-write
#      cannot redirect the contents to an unrelated file.
STATE_DIR="${HOME}/.local/state/lswasm"
STATE_FILE="${STATE_DIR}/install-state.env"
mkdir -p "$STATE_DIR"
chmod 0700 "$STATE_DIR"

# Refuse to operate if the existing state path is a symlink — that would
# indicate either a tampering attempt or an unusual operator configuration
# that this script is not designed for.
if [[ -L "$STATE_FILE" ]]; then
  echo "Error: $STATE_FILE is a symlink; refusing to overwrite." >&2
  exit 1
fi

TMP_STATE_FILE="$(mktemp "${STATE_DIR}/install-state.env.XXXXXX")"
trap 'rm -f "$TMP_STATE_FILE"' EXIT
chmod 0600 "$TMP_STATE_FILE"
{
  printf 'INSTALLED_BIN=%q\n' "$INSTALLED_BIN"
  printf 'INSTALL_DIR=%q\n' "$INSTALL_DIR"
} > "$TMP_STATE_FILE"
mv -f "$TMP_STATE_FILE" "$STATE_FILE"
trap - EXIT
echo "State saved to $STATE_FILE"

echo ""
echo "Done."
echo "LiteSpeed/OpenLiteSpeed typically launches lswasm in LSAPI mode on demand."
echo "Point your external app configuration at: $INSTALLED_BIN"
