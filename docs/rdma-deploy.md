## Prerequisites

- Firecracker built in this repo.
- A Linux kernel tree for the guest (used to build `vmlinux` and the module).
- A guest rootfs ext4 image.

Required kernel options (built-in):
- `CONFIG_VIRTIO=y`
- `CONFIG_VIRTIO_MMIO=y` (for mmio) or `CONFIG_VIRTIO_PCI=y` (for PCI)
- `CONFIG_EXT4_FS=y`

If use `--enable-pci`, also enable:
- `CONFIG_PCI=y`
- `CONFIG_PCI_MSI=y`
- `CONFIG_VIRTIO_PCI=y`

## Build Firecracker

```bash
cd /home/liquid/dev-private/firecracker
cargo build
```

## Build guest tools (module + rdma_ctl)

```bash
KERNEL_DIR=/home/liquid/dev-private/source/linux-6.1.155
./rdma_build_guest_tools.sh KERNEL_DIR="$KERNEL_DIR"
```

## Install tools into rootfs

```bash
ROOTFS=/home/liquid/dev-private/source/ubuntu-24.04.ext4
KERNEL_DIR=/home/liquid/dev-private/source/linux-6.1.155
sudo ROOTFS="$ROOTFS" KERNEL_DIR="$KERNEL_DIR" ./rdma_install_guest_tools.sh
```

## Start Firecracker

Run this in a dedicated terminal (it blocks):

```bash
API_SOCKET=/tmp/firecracker.socket
LOG_PATH=/tmp/firecracker.log
ENABLE_PCI=1
sudo API_SOCKET="$API_SOCKET" LOG_PATH="$LOG_PATH" ENABLE_PCI="$ENABLE_PCI" \
  ./rdma_run_firecracker.sh
```

If your guest kernel lacks PCI MSI support, set `ENABLE_PCI=0` and use mmio.

## Configure and start the microVM

```bash
API_SOCKET=/tmp/firecracker.socket
VMLINUX=/home/liquid/dev-private/source/linux-6.1.155/vmlinux
ROOTFS=/home/liquid/dev-private/source/ubuntu-24.04.ext4
KERNEL_ARGS="console=ttyS0 reboot=k panic=1 root=/dev/vda rw rootfstype=ext4"

API_SOCKET="$API_SOCKET" VMLINUX="$VMLINUX" ROOTFS="$ROOTFS" \
  KERNEL_ARGS="$KERNEL_ARGS" ./rdma_config_vm.sh
```

## Guest verification

Inside the guest:

```bash
insmod /lib/modules/$(uname -r)/extra/virtio_rdma.ko
dmesg | grep -i virtio-rdma
ls -l /dev/virtio-rdma0

/root/rdma_ctl create-qp 1
/root/rdma_ctl loop-create-qp 1 100
/root/rdma_ctl send-raw --opcode 0xdeadbeef --qp 1
```

## M3 minimal loop (QUERY_CAPS/REGISTER_MR/POST_SEND/POST_RECV/POLL_CQ)

```bash
/root/rdma_ctl query-caps
/root/rdma_ctl alloc-mr 4096 --pattern=inc
/root/rdma_ctl alloc-mr 4096 --pattern=0xaa
/root/rdma_ctl post-recv 1 2 256 100
/root/rdma_ctl post-send 1 1 256 101
/root/rdma_ctl poll-cq
/root/rdma_ctl poll-cq
/root/rdma_ctl dump-mr 2 0 64
/root/rdma_ctl check-mr 2 0 256 --expect=inc
```

Expected:
- `query-caps` prints version and limits.
- `poll-cq` returns completions with `wr_id`, `bytes`, and `type`.

## M4.2 stress (wrap-around and outstanding WRs)

```bash
/root/rdma_ctl stress --iters 100000 --outstanding 64
```

Expected:
- No errors, no timeouts, no kernel warnings.

## M4.3 lifecycle (destroy/deregister/reset)

```bash
/root/rdma_ctl deregister-mr 1
/root/rdma_ctl deregister-mr 2
/root/rdma_ctl destroy-qp 1
```

Expected:
- Host log shows `DEREGISTER_MR` and `DESTROY_QP`.
- Reloading the module works and creates a clean state.
