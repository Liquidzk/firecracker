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
