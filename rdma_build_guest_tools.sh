#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
KERNEL_DIR="${KERNEL_DIR:-}"

for arg in "$@"; do
  case "$arg" in
    KERNEL_DIR=*)
      KERNEL_DIR="${arg#KERNEL_DIR=}"
      ;;
    *)
      ;;
  esac
done

if [[ -z "$KERNEL_DIR" ]]; then
  echo "KERNEL_DIR is required (path to guest kernel source/build tree)." >&2
  exit 1
fi
if [[ ! -f "$KERNEL_DIR/Makefile" ]]; then
  echo "KERNEL_DIR does not look like a kernel tree: $KERNEL_DIR" >&2
  exit 1
fi

make -C "$KERNEL_DIR" prepare modules_prepare
make -C "$KERNEL_DIR" \
  M="$ROOT_DIR/tools/virtio-rdma-guest/kernel-module" \
  modules

cc -O2 -Wall \
  -I"$ROOT_DIR/tools/virtio-rdma-guest/include" \
  -o "$ROOT_DIR/tools/virtio-rdma-guest/user/rdma_ctl" \
  "$ROOT_DIR/tools/virtio-rdma-guest/user/rdma_ctl.c"

echo "Built: virtio_rdma.ko and rdma_ctl."
