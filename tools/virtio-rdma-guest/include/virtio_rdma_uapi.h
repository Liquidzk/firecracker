/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VIRTIO_RDMA_UAPI_H
#define VIRTIO_RDMA_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VIRTIO_RDMA_IOCTL_CREATE_QP _IOW('R', 0x01, __u32)

struct virtio_rdma_raw {
	__u32 opcode;
	__u32 qp_id;
	__u32 flags;
};

#define VIRTIO_RDMA_RAW_F_TRUNCATE 0x1
#define VIRTIO_RDMA_IOCTL_SEND_RAW _IOW('R', 0x02, struct virtio_rdma_raw)

struct virtio_rdma_caps {
	__u32 version_major;
	__u32 version_minor;
	__u32 max_qp;
	__u32 max_mr;
	__u32 max_cq;
	__u32 max_wr;
};

struct virtio_rdma_mr {
	__u32 mr_id;
	__u32 len;
	__u64 addr;
};

struct virtio_rdma_wr {
	__u32 qp_id;
	__u32 mr_id;
	__u32 len;
	__u32 flags;
	__u64 wr_id;
};

struct virtio_rdma_cqe {
	__u64 wr_id;
	__u32 status;
	__u32 bytes;
	__u32 opcode;
	__u32 reserved;
};

#define VIRTIO_RDMA_IOCTL_QUERY_CAPS _IOR('R', 0x03, struct virtio_rdma_caps)
#define VIRTIO_RDMA_IOCTL_REGISTER_MR _IOWR('R', 0x04, struct virtio_rdma_mr)
#define VIRTIO_RDMA_IOCTL_POST_SEND _IOW('R', 0x05, struct virtio_rdma_wr)
#define VIRTIO_RDMA_IOCTL_POST_RECV _IOW('R', 0x06, struct virtio_rdma_wr)
#define VIRTIO_RDMA_IOCTL_POLL_CQ _IOR('R', 0x07, struct virtio_rdma_cqe)

struct virtio_rdma_mr_alloc {
	__u32 len;
	__u32 pattern;
	__u32 pattern_byte;
	__u32 mr_id;
	__u64 addr;
};

struct virtio_rdma_mr_rw {
	__u32 mr_id;
	__u32 offset;
	__u32 len;
	__u32 reserved;
	__u64 user_ptr;
};

#define VIRTIO_RDMA_PATTERN_ZERO 0
#define VIRTIO_RDMA_PATTERN_INC 1
#define VIRTIO_RDMA_PATTERN_RAND 2
#define VIRTIO_RDMA_PATTERN_CONST 3

#define VIRTIO_RDMA_IOCTL_ALLOC_MR _IOWR('R', 0x08, struct virtio_rdma_mr_alloc)
#define VIRTIO_RDMA_IOCTL_READ_MR _IOW('R', 0x09, struct virtio_rdma_mr_rw)
#define VIRTIO_RDMA_IOCTL_DEREGISTER_MR _IOW('R', 0x0a, __u32)
#define VIRTIO_RDMA_IOCTL_DESTROY_QP _IOW('R', 0x0b, __u32)

#endif
