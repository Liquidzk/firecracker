# virtio-rdma guest module (minimal)

This is a minimal out-of-tree Linux kernel module for the Firecracker
virtio-rdma device. It only probes, creates one control virtqueue,
and logs basic information.

Build
1) Ensure you have a Linux kernel build tree available.
2) Run:

```
make KERNEL_DIR=/path/to/guest/kernel/build
```

Build userspace tool
```
cc -O2 -Wall -I./include -o user/rdma_ctl user/rdma_ctl.c
```

Clean
```
make clean KERNEL_DIR=/path/to/guest/kernel/build
```

Load in guest
1) Copy `kernel-module/virtio_rdma.ko` and `user/rdma_ctl` into the guest.
2) Load/unload and run:

```
insmod virtio_rdma.ko
./rdma_ctl create-qp 1
./rdma_ctl loop-create-qp 1 100
./rdma_ctl send-raw --opcode 0xdeadbeef --qp 1
rmmod virtio_rdma
```

Timeout configuration
- The module parameter `ctrl_timeout_ms` controls the ioctl timeout (default 1000ms):

```
insmod virtio_rdma.ko ctrl_timeout_ms=10
```

Expected dmesg logs
- `virtio-rdma: probed`
- `virtio-rdma: features=... vq_size=...`
- `virtio-rdma: CREATE_QP qp_id=1 -> status=0`
- `virtio-rdma: RAW opcode=0xdeadbeef qp_id=1 -> status=...`
- `virtio-rdma: removed`
