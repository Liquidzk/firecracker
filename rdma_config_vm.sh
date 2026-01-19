#!/usr/bin/env bash
set -euo pipefail

API_SOCKET="${API_SOCKET:-/tmp/firecracker.socket}"
VMLINUX="${VMLINUX:-}"
ROOTFS="${ROOTFS:-}"
KERNEL_ARGS="${KERNEL_ARGS:-console=ttyS0 reboot=k panic=1 root=/dev/vda rw rootfstype=ext4}"
VCPU_COUNT="${VCPU_COUNT:-1}"
MEM_SIZE_MIB="${MEM_SIZE_MIB:-512}"
RDMA_ID="${RDMA_ID:-rdma0}"

if [[ -z "$VMLINUX" || -z "$ROOTFS" ]]; then
  echo "VMLINUX and ROOTFS are required." >&2
  exit 1
fi

curl_cmd=(sudo curl --unix-socket "$API_SOCKET" -i)

"${curl_cmd[@]}" -X PUT http://localhost/machine-config \
  -H "Content-Type: application/json" \
  -d "{\"vcpu_count\":${VCPU_COUNT},\"mem_size_mib\":${MEM_SIZE_MIB},\"smt\":false}"

"${curl_cmd[@]}" -X PUT http://localhost/boot-source \
  -H "Content-Type: application/json" \
  -d "{\"kernel_image_path\":\"${VMLINUX}\",\"boot_args\":\"${KERNEL_ARGS}\"}"

"${curl_cmd[@]}" -X PUT http://localhost/drives/rootfs \
  -H "Content-Type: application/json" \
  -d "{\"drive_id\":\"rootfs\",\"path_on_host\":\"${ROOTFS}\",\"is_root_device\":true,\"is_read_only\":false}"

"${curl_cmd[@]}" -X PUT "http://localhost/rdma-devices/${RDMA_ID}" \
  -H "Content-Type: application/json" \
  -d "{\"id\":\"${RDMA_ID}\"}"

"${curl_cmd[@]}" -X PUT http://localhost/actions \
  -H "Content-Type: application/json" \
  -d '{"action_type":"InstanceStart"}'
