#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
FIRECRACKER_BIN="${FIRECRACKER_BIN:-$ROOT_DIR/build/cargo_target/debug/firecracker}"
API_SOCKET="${API_SOCKET:-/tmp/firecracker.socket}"
LOG_PATH="${LOG_PATH:-/tmp/firecracker.log}"
ENABLE_PCI="${ENABLE_PCI:-1}"

if [[ ! -x "$FIRECRACKER_BIN" ]]; then
  echo "Firecracker binary not found or not executable: $FIRECRACKER_BIN" >&2
  exit 1
fi

sudo rm -f "$API_SOCKET"

args=(--api-sock "$API_SOCKET" --log-path "$LOG_PATH" --level Debug)
if [[ "$ENABLE_PCI" == "1" ]]; then
  args+=(--enable-pci)
fi

exec sudo "$FIRECRACKER_BIN" "${args[@]}"
