#!/usr/bin/env bash
# upgrade.sh — Upgrade an existing lswasm installation in place.
#
# This script automates the upgrade process described in the README:
#   1. Pull the latest source code.
#   2. Update git submodules.
#   3. Clean rebuild (or incremental if --no-clean is given).
#   4. Copy the new binary to the installed location.
#
# Usage:
#   ./upgrade.sh [options]
#
# Options:
#   --build-dir <path>    Build directory (default: build)
#   --cmake-args <args>   Additional CMake configure arguments (quoted string)
#   --no-clean            Incremental build instead of clean rebuild
#   --no-pull             Skip git pull (use local source as-is)
#   --help                Show this help message
#
# The script reads the install state from ~/.local/state/lswasm/install-state.env
# (written by install.sh) to determine the installed binary path.
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
    --help)
      sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
      exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--build-dir <path>] [--cmake-args <args>] [--no-clean] [--no-pull]" >&2
      exit 1 ;;
  esac
done

# ── Load install state ──────────────────────────────────────────────────
STATE_DIR="${HOME}/.local/state/lswasm"
STATE_FILE="${STATE_DIR}/install-state.env"
if [[ ! -f "$STATE_FILE" ]]; then
  echo "Error: install state not found at $STATE_FILE" >&2
  echo "Has lswasm been installed with install.sh?" >&2
  exit 1
fi

# Refuse to source the state file unless it is a plain regular file owned by
# the current user.  install.sh writes it under a 0700 directory; anything
# else is a tampering signal.
if [[ -L "$STATE_FILE" ]]; then
  echo "Error: $STATE_FILE is a symlink; refusing to source." >&2
  exit 1
fi
state_owner="$(stat -c '%u' "$STATE_FILE" 2>/dev/null || stat -f '%u' "$STATE_FILE" 2>/dev/null || echo "")"
if [[ -z "$state_owner" || "$state_owner" != "$(id -u)" ]]; then
  echo "Error: $STATE_FILE is not owned by the current user." >&2
  exit 1
fi
state_dir_mode="$(stat -c '%a' "$STATE_DIR" 2>/dev/null || stat -f '%Lp' "$STATE_DIR" 2>/dev/null || echo "")"
if [[ -n "$state_dir_mode" && "$state_dir_mode" != "700" ]]; then
  echo "Warning: $STATE_DIR is mode $state_dir_mode (expected 700)." >&2
fi

# shellcheck source=/dev/null
source "$STATE_FILE"

if [[ -z "${INSTALLED_BIN:-}" ]]; then
  echo "Error: INSTALLED_BIN is missing from $STATE_FILE" >&2
  exit 1
fi

echo "=== lswasm upgrade ==="
echo "Installed binary: $INSTALLED_BIN"
echo "Build directory:  $BUILD_DIR"
echo ""

# ── Step 1: Pull latest source ──────────────────────────────────────────
# SECURITY NOTE: `git pull` builds and installs whatever code is at the
# remote HEAD with the privileges of the current user.  For production
# upgrades, verify the new HEAD before rebuilding — pin to a signed tag
# (`git fetch && git verify-tag <tag> && git checkout <tag>`) or review the
# commit range manually (`git log HEAD..@{u} --stat`).  Pass --no-pull and
# run those steps by hand if you want to gate the upgrade on review.
if $PULL; then
  echo "→ Pulling latest source..."
  git pull
  echo "  New HEAD: $(git rev-parse HEAD)"
  echo ""
fi

# ── Step 2: Update submodules ───────────────────────────────────────────
echo "→ Updating submodules..."
git submodule sync --recursive
git submodule update --init --recursive
echo ""

# ── Step 3: Build ───────────────────────────────────────────────────────
if $CLEAN; then
  echo "→ Clean rebuild (removing $BUILD_DIR)..."
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "→ Configuring..."
# Split CMAKE_ARGS into words via `read`.  Older revisions used `eval` on
# this string, which executed arbitrary shell when the caller supplied
# command substitutions or `; ...` injections in --cmake-args.
# read -a handles standard whitespace-separated arguments; quoting inside
# CMAKE_ARGS is no longer interpreted as shell syntax.
read -r -a CMAKE_ARGS_ARRAY <<< "$CMAKE_ARGS"
cmake -B "$BUILD_DIR" "${CMAKE_ARGS_ARRAY[@]}" .

echo "→ Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo ""

NEW_BIN="${BUILD_DIR}/lswasm"
if [[ ! -f "$NEW_BIN" ]]; then
  echo "Error: build did not produce $NEW_BIN" >&2
  exit 1
fi

# ── Step 4: Copy the new binary ─────────────────────────────────────────
echo "→ Installing new binary to $INSTALLED_BIN..."
cp "$NEW_BIN" "$INSTALLED_BIN"
chmod 755 "$INSTALLED_BIN"
echo "  Binary updated."
echo ""

# ── Done ────────────────────────────────────────────────────────────────
echo "=== Upgrade complete ==="
echo ""
echo "LiteSpeed/OpenLiteSpeed typically launches the updated binary on demand in LSAPI mode."
echo "If you run lswasm manually in --lsproxy mode, restart that process to pick up the new binary."
