# virtio-rdma guest module (minimal)

This is a minimal out-of-tree Linux kernel module for the Firecracker
virtio-rdma device. It probes, creates one control virtqueue, and supports
simple control-path ioctls for a minimal RDMA-like workflow.

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
./rdma_ctl query-caps
./rdma_ctl create-qp 1
./rdma_ctl alloc-mr 4096 --pattern=inc
./rdma_ctl alloc-mr 4096 --pattern=0xaa
./rdma_ctl post-recv 1 2 256 100
./rdma_ctl post-send 1 1 256 101
./rdma_ctl poll-cq
./rdma_ctl poll-cq
./rdma_ctl dump-mr 2 0 64
./rdma_ctl check-mr 2 0 256 --expect=inc
./rdma_ctl stress --iters 100000 --outstanding 64
./rdma_ctl loop-create-qp 1 100
./rdma_ctl send-raw --opcode 0xdeadbeef --qp 1
./rdma_ctl deregister-mr 1
./rdma_ctl deregister-mr 2
./rdma_ctl destroy-qp 1
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
- `virtio-rdma: ALLOC_MR id=1 len=4096 pattern=1`
- `virtio-rdma: POST_RECV qp_id=1 mr_id=1 wr_id=100 len=256`
- `virtio-rdma: POST_SEND qp_id=1 mr_id=1 wr_id=101 len=256`
- `virtio-rdma: RAW opcode=0xdeadbeef qp_id=1 -> status=...`
- `virtio-rdma: removed`
