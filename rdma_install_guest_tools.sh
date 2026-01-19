#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
KERNEL_DIR="${KERNEL_DIR:-}"
ROOTFS="${ROOTFS:-}"
MNT="${MNT:-/mnt}"

if [[ $EUID -ne 0 ]]; then
  echo "Run this script as root (or via sudo)." >&2
  exit 1
fi
if [[ -z "$KERNEL_DIR" || -z "$ROOTFS" ]]; then
  echo "KERNEL_DIR and ROOTFS are required." >&2
  exit 1
fi
if [[ ! -f "$KERNEL_DIR/Makefile" ]]; then
  echo "KERNEL_DIR does not look like a kernel tree: $KERNEL_DIR" >&2
  exit 1
fi
if [[ ! -f "$ROOTFS" ]]; then
  echo "ROOTFS not found: $ROOTFS" >&2
  exit 1
fi

cleanup() {
  if mountpoint -q "$MNT"; then
    umount "$MNT"
  fi
}
trap cleanup EXIT

mkdir -p "$MNT"
mount -o loop "$ROOTFS" "$MNT"

KREL=$(make -s -C "$KERNEL_DIR" kernelrelease)

install -D -m 0644 \
  "$ROOT_DIR/tools/virtio-rdma-guest/kernel-module/virtio_rdma.ko" \
  "$MNT/lib/modules/${KREL}/extra/virtio_rdma.ko"
install -D -m 0755 \
  "$ROOT_DIR/tools/virtio-rdma-guest/user/rdma_ctl" \
  "$MNT/root/rdma_ctl"

echo "Installed virtio_rdma.ko and rdma_ctl into $ROOTFS."
