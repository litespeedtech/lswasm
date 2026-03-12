#!/usr/bin/env bash
# install.sh — Install lswasm as a systemd user service.
#
# Usage:
#   ./install.sh --bin <path> --install-dir <path> [--service-name <name>] \
#                [-- <lswasm args ...>]
#
# Required flags:
#   --bin <path>          Path to the compiled lswasm binary.
#   --install-dir <path>  Directory where the binary will be copied.
#
# Optional flags:
#   --service-name <name> systemd unit name (default: lswasm.service).
#
# Everything after "--" is forwarded verbatim as lswasm arguments in the
# systemd ExecStart line.  If --module or --uds are not present among the
# forwarded arguments the user is prompted interactively.
#
# Example:
#   ./install.sh --bin ./build/lswasm --install-dir /opt/lswasm \
#                -- --module /etc/lswasm/filter.wasm --uds /run/lswasm.sock

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────
SERVICE_NAME="lswasm.service"
BIN=""
INSTALL_DIR=""
LSWASM_ARGS=()

# ── Parse arguments ─────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin)
      BIN="$2"; shift 2 ;;
    --install-dir)
      INSTALL_DIR="$2"; shift 2 ;;
    --service-name)
      SERVICE_NAME="$2"; shift 2 ;;
    --)
      shift; LSWASM_ARGS=("$@"); break ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 --bin <path> --install-dir <path> [--service-name <name>] [-- <lswasm args>]" >&2
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

# ── Helper: check if a flag is present in the forwarded args ─────────────
has_arg() {
  local needle="$1"; shift
  for a in "$@"; do
    if [[ "$a" == "$needle" ]]; then return 0; fi
  done
  return 1
}

# ── Helper: get the value immediately following a flag ───────────────────
get_arg_value() {
  local needle="$1"; shift
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == "$needle" && $# -ge 2 ]]; then
      echo "$2"
      return 0
    fi
    shift
  done
  return 1
}

# ── Prompt for --module / --uds if missing from forwarded args ──────────

if ! has_arg "--module" "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}"; then
  read -rp "Path to WASM filter module (--module): " MODULE_PATH
  MODULE_PATH="${MODULE_PATH/#\~/$HOME}"
  if [[ -n "$MODULE_PATH" ]]; then
    LSWASM_ARGS+=("--module" "$MODULE_PATH")
  fi
fi

if ! has_arg "--uds" "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}"; then
  read -rp "Unix domain socket path (--uds) [/tmp/lswasm.sock]: " UDS_PATH
  UDS_PATH="${UDS_PATH:-/tmp/lswasm.sock}"
  UDS_PATH="${UDS_PATH/#\~/$HOME}"
  LSWASM_ARGS+=("--uds" "$UDS_PATH")
fi

# ── Validate paths in forwarded args ────────────────────────────────────
# Validate --module path exists and is a file.
MODULE_VAL="$(get_arg_value "--module" "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}" || true)"
if [[ -n "$MODULE_VAL" && ! -f "$MODULE_VAL" ]]; then
  echo "Error: WASM module file not found: $MODULE_VAL" >&2
  exit 1
fi

# Validate --uds parent directory exists (or can be created).
UDS_VAL="$(get_arg_value "--uds" "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}" || true)"
if [[ -n "$UDS_VAL" ]]; then
  UDS_PARENT="$(dirname "$UDS_VAL")"
  if [[ ! -d "$UDS_PARENT" ]]; then
    echo "Error: parent directory for UDS path does not exist: $UDS_PARENT" >&2
    exit 1
  fi
fi

# Validate --port is a number in valid range (if present).
PORT_VAL="$(get_arg_value "--port" "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}" || true)"
if [[ -n "$PORT_VAL" ]]; then
  if ! [[ "$PORT_VAL" =~ ^[0-9]+$ ]] || (( PORT_VAL < 1 || PORT_VAL > 65535 )); then
    echo "Error: --port must be a number between 1 and 65535: $PORT_VAL" >&2
    exit 1
  fi
fi

# Validate --sock-perm is a valid octal mode (if present).
PERM_VAL="$(get_arg_value "--sock-perm" "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}" || true)"
if [[ -n "$PERM_VAL" ]]; then
  if ! [[ "$PERM_VAL" =~ ^[0-7]{1,4}$ ]]; then
    echo "Error: --sock-perm must be an octal mode (e.g. 0666): $PERM_VAL" >&2
    exit 1
  fi
fi

# Validate --env entries are KEY=VALUE (if present).
for i in "${!LSWASM_ARGS[@]}"; do
  if [[ "${LSWASM_ARGS[$i]}" == "--env" ]]; then
    next_i=$((i + 1))
    if [[ $next_i -lt ${#LSWASM_ARGS[@]} ]]; then
      env_val="${LSWASM_ARGS[$next_i]}"
      if [[ "$env_val" != *=* ]]; then
        echo "Error: --env value must be KEY=VALUE: $env_val" >&2
        exit 1
      fi
    fi
  fi
done

# ── Resolve all paths to absolute ────────────────────────────────────────
# Expand leading tilde (not expanded by the shell when values are quoted).
BIN="${BIN/#\~/$HOME}"
INSTALL_DIR="${INSTALL_DIR/#\~/$HOME}"

BIN="$(realpath "$BIN")"
INSTALL_DIR="$(realpath -m "$INSTALL_DIR")"

for i in "${!LSWASM_ARGS[@]}"; do
  case "${LSWASM_ARGS[$i]}" in
    --module)
      next_i=$((i + 1))
      if [[ $next_i -lt ${#LSWASM_ARGS[@]} ]]; then
        LSWASM_ARGS[$next_i]="${LSWASM_ARGS[$next_i]/#\~/$HOME}"
        LSWASM_ARGS[$next_i]="$(realpath "${LSWASM_ARGS[$next_i]}")"
      fi
      ;;
    --uds)
      next_i=$((i + 1))
      if [[ $next_i -lt ${#LSWASM_ARGS[@]} ]]; then
        LSWASM_ARGS[$next_i]="${LSWASM_ARGS[$next_i]/#\~/$HOME}"
        LSWASM_ARGS[$next_i]="$(realpath -m "${LSWASM_ARGS[$next_i]}")"
      fi
      ;;
  esac
done

# ── Install binary ──────────────────────────────────────────────────────
INSTALLED_BIN="${INSTALL_DIR}/lswasm"
IS_UPGRADE=false

# Detect existing installation and stop the service before overwriting.
if [[ -f "$INSTALLED_BIN" ]]; then
  IS_UPGRADE=true
  echo "Existing installation detected at $INSTALLED_BIN"
  if systemctl --user is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
    echo "Stopping service $SERVICE_NAME..."
    systemctl --user stop "$SERVICE_NAME"
  fi
fi

mkdir -p "$INSTALL_DIR"
cp "$BIN" "$INSTALLED_BIN"
chmod 755 "$INSTALLED_BIN"
if $IS_UPGRADE; then
  echo "Updated binary at $INSTALLED_BIN"
else
  echo "Installed binary to $INSTALLED_BIN"
fi

# ── Build ExecStart with safe quoting ────────────────────────────────────
# printf %q produces bash-style quoting; systemd honours backslash-escaped
# tokens in ExecStart lines.
EXEC_START="$INSTALLED_BIN"
for arg in "${LSWASM_ARGS[@]+"${LSWASM_ARGS[@]}"}"; do
  EXEC_START+=" $(printf '%q' "$arg")"
done

# ── Generate systemd user unit ───────────────────────────────────────────
UNIT_DIR="${HOME}/.config/systemd/user"
UNIT_PATH="${UNIT_DIR}/${SERVICE_NAME}"
mkdir -p "$UNIT_DIR"

cat > "$UNIT_PATH" <<EOF
[Unit]
Description=lswasm — WASM HTTP Proxy Server
After=network.target

[Service]
Type=simple
ExecStart=${EXEC_START}
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=default.target
EOF

echo "Created systemd user unit: $UNIT_PATH"

# ── Persist install metadata for uninstall ───────────────────────────────
STATE_DIR="${HOME}/.local/state/lswasm"
STATE_FILE="${STATE_DIR}/install-state.env"
mkdir -p "$STATE_DIR"
cat > "$STATE_FILE" <<EOF
INSTALLED_BIN=${INSTALLED_BIN}
INSTALL_DIR=${INSTALL_DIR}
SERVICE_NAME=${SERVICE_NAME}
UNIT_PATH=${UNIT_PATH}
EOF
echo "State saved to $STATE_FILE"

# ── Enable and start the service ─────────────────────────────────────────
systemctl --user daemon-reload
systemctl --user enable --now "$SERVICE_NAME"
echo "Service $SERVICE_NAME enabled and started."

echo ""
echo "Done.  Check status with:"
echo "  systemctl --user status $SERVICE_NAME"
echo "  journalctl --user -u $SERVICE_NAME -f"
