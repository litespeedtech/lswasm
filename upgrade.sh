#!/usr/bin/env bash
# upgrade.sh — Upgrade an existing lswasm installation in place.
#
# This script automates the upgrade process described in the README:
#   1. Pull the latest source code.
#   2. Update git submodules.
#   3. Clean rebuild (or incremental if --no-clean is given).
#   4. Stop the running systemd user service.
#   5. Copy the new binary to the installed location.
#   6. Restart the service.
#
# Usage:
#   ./upgrade.sh [options]
#
# Options:
#   --build-dir <path>    Build directory (default: build)
#   --cmake-args <args>   Additional CMake configure arguments (quoted string)
#   --no-clean            Incremental build instead of clean rebuild
#   --no-pull             Skip git pull (use local source as-is)
#   --service-name <name> systemd unit name (default: from install state)
#   --help                Show this help message
#
# The script reads the install state from ~/.local/state/lswasm/install-state.env
# (written by install.sh) to determine the installed binary path and service name.
#
# Example:
#   ./upgrade.sh
#   ./upgrade.sh --cmake-args "-DWASM_RUNTIME=wamr -DCMAKE_BUILD_TYPE=Release"
#   ./upgrade.sh --no-pull --no-clean

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────
BUILD_DIR="build"
CMAKE_ARGS=""
CLEAN=true
PULL=true
SERVICE_NAME_OVERRIDE=""

# ── Parse arguments ─────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"; shift 2 ;;
    --cmake-args)
      CMAKE_ARGS="$2"; shift 2 ;;
    --no-clean)
      CLEAN=false; shift ;;
    --no-pull)
      PULL=false; shift ;;
    --service-name)
      SERVICE_NAME_OVERRIDE="$2"; shift 2 ;;
    --help)
      sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
      exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--build-dir <path>] [--cmake-args <args>] [--no-clean] [--no-pull] [--service-name <name>]" >&2
      exit 1 ;;
  esac
done

# ── Load install state ──────────────────────────────────────────────────
STATE_FILE="${HOME}/.local/state/lswasm/install-state.env"
if [[ ! -f "$STATE_FILE" ]]; then
  echo "Error: install state not found at $STATE_FILE" >&2
  echo "Has lswasm been installed with install.sh?" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$STATE_FILE"

# Allow override of service name.
if [[ -n "$SERVICE_NAME_OVERRIDE" ]]; then
  SERVICE_NAME="$SERVICE_NAME_OVERRIDE"
fi

echo "=== lswasm upgrade ==="
echo "Installed binary: $INSTALLED_BIN"
echo "Service name:     $SERVICE_NAME"
echo "Build directory:  $BUILD_DIR"
echo ""

# ── Step 1: Pull latest source ──────────────────────────────────────────
if $PULL; then
  echo "→ Pulling latest source..."
  git pull
  echo ""
fi

# ── Step 2: Update submodules ────────────────────────────────────────────
echo "→ Updating submodules..."
git submodule sync --recursive
git submodule update --init --recursive
echo ""

# ── Step 3: Build ────────────────────────────────────────────────────────
if $CLEAN; then
  echo "→ Clean rebuild (removing $BUILD_DIR)..."
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "→ Configuring..."
eval "declare -a CMAKE_ARGS_ARRAY=($CMAKE_ARGS)"
cmake -B "$BUILD_DIR" "${CMAKE_ARGS_ARRAY[@]}" .

echo "→ Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo ""

NEW_BIN="${BUILD_DIR}/lswasm"
if [[ ! -f "$NEW_BIN" ]]; then
  echo "Error: build did not produce $NEW_BIN" >&2
  exit 1
fi

# ── Step 4: Stop the service ────────────────────────────────────────────
echo "→ Stopping service $SERVICE_NAME..."
if systemctl --user is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
  systemctl --user stop "$SERVICE_NAME"
  echo "  Service stopped."
else
  echo "  Service was not running."
fi
echo ""

# ── Step 5: Copy the new binary ─────────────────────────────────────────
echo "→ Installing new binary to $INSTALLED_BIN..."
cp "$NEW_BIN" "$INSTALLED_BIN"
chmod 755 "$INSTALLED_BIN"
echo "  Binary updated."
echo ""

# ── Step 6: Restart the service ─────────────────────────────────────────
echo "→ Reloading systemd and restarting $SERVICE_NAME..."
systemctl --user daemon-reload
systemctl --user start "$SERVICE_NAME"
echo "  Service restarted."
echo ""

# ── Done ─────────────────────────────────────────────────────────────────
echo "=== Upgrade complete ==="
echo ""
echo "Check status with:"
echo "  systemctl --user status $SERVICE_NAME"
echo "  journalctl --user -u $SERVICE_NAME -f"
