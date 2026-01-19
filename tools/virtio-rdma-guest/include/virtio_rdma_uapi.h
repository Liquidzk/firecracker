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

#endif
