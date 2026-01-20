// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal virtio-rdma guest driver for Firecracker.
 *
 * This is intentionally tiny: it probes, creates one control virtqueue, and logs.
 * No RDMA verbs, no uAPI, no netdev integration.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/wait.h>

#include "virtio_rdma_uapi.h"

/* Must match Firecracker's VirtioDeviceType::Rdma value. */
#define VIRTIO_ID_RDMA 42

/* RDMA opcodes (must match host). */
#define RDMA_OPCODE_CREATE_QP 1
#define RDMA_OPCODE_QUERY_CAPS 2
#define RDMA_OPCODE_REGISTER_MR 3
#define RDMA_OPCODE_POST_SEND 4
#define RDMA_OPCODE_POST_RECV 5
#define RDMA_OPCODE_POLL_CQ 6
#define RDMA_OPCODE_DESTROY_QP 7
#define RDMA_OPCODE_DEREGISTER_MR 8

#define RDMA_STATUS_OK 0
#define RDMA_STATUS_ERR 1
#define RDMA_STATUS_EMPTY 2

#define RDMA_MAX_MR 16
#define RDMA_MAX_MR_LEN (1U << 20)

struct virtio_rdma_req {
	__le64 addr;
	__le64 wr_id;
	__le32 opcode;
	__le32 qp_id;
	__le32 mr_id;
	__le32 len;
	__le32 flags;
	__le32 reserved;
};

struct virtio_rdma_resp {
	__le64 wr_id;
	__le32 status;
	__le32 opcode;
	__le32 bytes;
	__le32 value0;
	__le32 value1;
	__le32 value2;
	__le32 value3;
	__le32 value4;
};

struct virtio_rdma_mr_entry {
	u32 id;
	u32 len;
	phys_addr_t addr;
	void *kaddr;
	u32 pattern;
	u32 pattern_byte;
};

struct virtio_rdma_dev {
	struct virtio_device *vdev;
	struct virtqueue *ctrl_vq;
	struct miscdevice miscdev;
	struct mutex ioctl_lock;
	struct completion ctrl_done;
	struct virtio_rdma_req *req;
	struct virtio_rdma_resp *resp;
	struct virtio_rdma_resp last_resp;
	u32 last_status;
	u32 last_qp_id;
	bool inflight;
	bool timed_out;
	struct virtio_rdma_mr_entry mrs[RDMA_MAX_MR];
	unsigned int mr_count;
	bool qp_created;
};

static unsigned int ctrl_timeout_ms = 1000;
module_param(ctrl_timeout_ms, uint, 0644);
MODULE_PARM_DESC(ctrl_timeout_ms, "Control queue timeout in milliseconds");

static void virtio_rdma_free_buffers(struct virtio_rdma_dev *vrdev);

static void virtio_rdma_ctrl_cb(struct virtqueue *vq)
{
	struct virtio_rdma_dev *vrdev = vq->vdev->priv;
	unsigned int len;
	void *buf;

	while ((buf = virtqueue_get_buf(vq, &len)) != NULL) {
		if (!vrdev->inflight || !vrdev->resp)
			continue;
		vrdev->last_resp = *vrdev->resp;
		vrdev->last_status = le32_to_cpu(vrdev->last_resp.status);
		if (vrdev->timed_out) {
			vrdev->inflight = false;
			vrdev->timed_out = false;
			virtio_rdma_free_buffers(vrdev);
		}
		complete(&vrdev->ctrl_done);
	}
}

static void virtio_rdma_free_buffers(struct virtio_rdma_dev *vrdev)
{
	kfree(vrdev->req);
	kfree(vrdev->resp);
	vrdev->req = NULL;
	vrdev->resp = NULL;
}

static u32 virtio_rdma_rand32(u32 *state)
{
	u32 x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void virtio_rdma_fill_pattern(
	void *buf, u32 len, u32 pattern, u32 pattern_byte)
{
	u8 *p = buf;
	u32 i;

	switch (pattern) {
	case VIRTIO_RDMA_PATTERN_ZERO:
		memset(p, 0, len);
		break;
	case VIRTIO_RDMA_PATTERN_INC:
		for (i = 0; i < len; i++)
			p[i] = (u8)i;
		break;
	case VIRTIO_RDMA_PATTERN_RAND: {
		u32 state = 0x12345678;

		for (i = 0; i < len; i++)
			p[i] = (u8)virtio_rdma_rand32(&state);
		break;
	}
	case VIRTIO_RDMA_PATTERN_CONST:
	default:
		memset(p, (u8)pattern_byte, len);
		break;
	}
}

static int virtio_rdma_submit(struct virtio_rdma_dev *vrdev,
			      const struct virtio_rdma_req *req,
			      struct virtio_rdma_resp *resp,
			      size_t req_len, bool map_invalid,
			      bool allow_nonzero_status)
{
	struct scatterlist sg_req, sg_resp;
	struct scatterlist *sgs[2];
	unsigned long timeout;
	int ret;

	vrdev->req = kzalloc(sizeof(*vrdev->req), GFP_KERNEL);
	vrdev->resp = kzalloc(sizeof(*vrdev->resp), GFP_KERNEL);
	if (!vrdev->req || !vrdev->resp) {
		ret = -ENOMEM;
		goto out_free;
	}

	memcpy(vrdev->req, req, sizeof(*vrdev->req));
	vrdev->last_qp_id = le32_to_cpu(req->qp_id);
	vrdev->last_status = 0;
	vrdev->timed_out = false;
	reinit_completion(&vrdev->ctrl_done);
	vrdev->inflight = true;

	sg_init_one(&sg_req, vrdev->req, req_len);
	sg_init_one(&sg_resp, vrdev->resp, sizeof(*vrdev->resp));
	sgs[0] = &sg_req;
	sgs[1] = &sg_resp;

	ret = virtqueue_add_sgs(vrdev->ctrl_vq, sgs, 1, 1, vrdev->req,
				GFP_KERNEL);
	if (ret) {
		ret = -EIO;
		goto out_inflight;
	}

	virtqueue_kick(vrdev->ctrl_vq);

	timeout = wait_for_completion_timeout(&vrdev->ctrl_done,
					      msecs_to_jiffies(ctrl_timeout_ms));
	if (!timeout) {
		void *unused = virtqueue_detach_unused_buf(vrdev->ctrl_vq);

		if (unused) {
			vrdev->inflight = false;
			vrdev->timed_out = false;
			ret = -ETIMEDOUT;
			goto out_inflight;
		}
		vrdev->timed_out = true;
		return -ETIMEDOUT;
	}

	vrdev->inflight = false;
	*resp = vrdev->last_resp;
	if (allow_nonzero_status) {
		ret = 0;
	} else {
		ret = (vrdev->last_status == 0) ?
			      0 :
			      (map_invalid ? -EINVAL : -EIO);
	}

out_inflight:
	if (!vrdev->inflight)
		virtio_rdma_free_buffers(vrdev);
	return ret;

out_free:
	virtio_rdma_free_buffers(vrdev);
	return ret;
}

static void virtio_rdma_free_mrs(struct virtio_rdma_dev *vrdev)
{
	unsigned int i;

	for (i = 0; i < vrdev->mr_count; i++)
		kfree(vrdev->mrs[i].kaddr);
	vrdev->mr_count = 0;
}

static struct virtio_rdma_mr_entry *virtio_rdma_find_mr(
	struct virtio_rdma_dev *vrdev, u32 mr_id)
{
	unsigned int i;

	for (i = 0; i < vrdev->mr_count; i++) {
		if (vrdev->mrs[i].id == mr_id)
			return &vrdev->mrs[i];
	}
	return NULL;
}

static int virtio_rdma_remove_mr(struct virtio_rdma_dev *vrdev, u32 mr_id)
{
	unsigned int i;

	for (i = 0; i < vrdev->mr_count; i++) {
		if (vrdev->mrs[i].id == mr_id) {
			kfree(vrdev->mrs[i].kaddr);
			vrdev->mrs[i] = vrdev->mrs[vrdev->mr_count - 1];
			vrdev->mr_count--;
			return 0;
		}
	}
	return -EINVAL;
}

static long virtio_rdma_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct virtio_rdma_dev *vrdev = file->private_data;
	struct virtio_rdma_raw raw = { 0 };
	struct virtio_rdma_caps caps = { 0 };
	struct virtio_rdma_mr mr = { 0 };
	struct virtio_rdma_mr_alloc mr_alloc = { 0 };
	struct virtio_rdma_mr_rw mr_rw = { 0 };
	struct virtio_rdma_wr wr = { 0 };
	struct virtio_rdma_cqe cqe = { 0 };
	struct virtio_rdma_req req = { 0 };
	struct virtio_rdma_resp resp = { 0 };
	struct virtio_rdma_mr_entry *entry;
	u32 qp_id;
	int ret;

	switch (cmd) {
	case VIRTIO_RDMA_IOCTL_CREATE_QP:
		if (copy_from_user(&qp_id, (void __user *)arg, sizeof(qp_id)))
			return -EFAULT;
		break;
	case VIRTIO_RDMA_IOCTL_SEND_RAW:
		if (copy_from_user(&raw, (void __user *)arg, sizeof(raw)))
			return -EFAULT;
		break;
	case VIRTIO_RDMA_IOCTL_QUERY_CAPS:
	case VIRTIO_RDMA_IOCTL_POLL_CQ:
		break;
	case VIRTIO_RDMA_IOCTL_REGISTER_MR:
		if (copy_from_user(&mr, (void __user *)arg, sizeof(mr)))
			return -EFAULT;
		break;
	case VIRTIO_RDMA_IOCTL_ALLOC_MR:
		if (copy_from_user(&mr_alloc, (void __user *)arg,
				   sizeof(mr_alloc)))
			return -EFAULT;
		break;
	case VIRTIO_RDMA_IOCTL_READ_MR:
		if (copy_from_user(&mr_rw, (void __user *)arg,
				   sizeof(mr_rw)))
			return -EFAULT;
		break;
	case VIRTIO_RDMA_IOCTL_POST_SEND:
	case VIRTIO_RDMA_IOCTL_POST_RECV:
		if (copy_from_user(&wr, (void __user *)arg, sizeof(wr)))
			return -EFAULT;
		break;
	case VIRTIO_RDMA_IOCTL_DEREGISTER_MR:
	case VIRTIO_RDMA_IOCTL_DESTROY_QP:
		if (copy_from_user(&qp_id, (void __user *)arg, sizeof(qp_id)))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	if (cmd == VIRTIO_RDMA_IOCTL_SEND_RAW &&
	    (raw.flags & VIRTIO_RDMA_RAW_F_TRUNCATE)) {
		dev_info(&vrdev->vdev->dev,
			 "virtio-rdma: RAW opcode=0x%x qp_id=%u -> status=EINVAL\n",
			 raw.opcode, raw.qp_id);
		return -EINVAL;
	}

	if (cmd == VIRTIO_RDMA_IOCTL_SEND_RAW)
		qp_id = raw.qp_id;

	mutex_lock(&vrdev->ioctl_lock);
	if (vrdev->inflight) {
		mutex_unlock(&vrdev->ioctl_lock);
		return -EBUSY;
	}

	if (cmd == VIRTIO_RDMA_IOCTL_CREATE_QP) {
		req.opcode = cpu_to_le32(RDMA_OPCODE_CREATE_QP);
		req.qp_id = cpu_to_le32(qp_id);
		ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req), false,
					 false);
		if (ret == 0)
			vrdev->qp_created = true;
		dev_info(&vrdev->vdev->dev,
			 "virtio-rdma: CREATE_QP qp_id=%u -> status=%u\n",
			 qp_id, le32_to_cpu(resp.status));
	} else {
		switch (cmd) {
		case VIRTIO_RDMA_IOCTL_SEND_RAW:
			req.opcode = cpu_to_le32(raw.opcode);
			req.qp_id = cpu_to_le32(raw.qp_id);
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 true, false);
			dev_info(&vrdev->vdev->dev,
				 "virtio-rdma: RAW opcode=0x%x qp_id=%u -> status=%u\n",
				 raw.opcode, raw.qp_id,
				 le32_to_cpu(resp.status));
			break;
		case VIRTIO_RDMA_IOCTL_QUERY_CAPS:
			req.opcode = cpu_to_le32(RDMA_OPCODE_QUERY_CAPS);
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, false);
			if (!ret) {
				caps.version_major = le32_to_cpu(resp.value0);
				caps.version_minor = le32_to_cpu(resp.value1);
				caps.max_qp = le32_to_cpu(resp.value2);
				caps.max_mr = le32_to_cpu(resp.value3);
				caps.max_cq = le32_to_cpu(resp.bytes);
				caps.max_wr = le32_to_cpu(resp.value4);
				if (copy_to_user((void __user *)arg, &caps,
						 sizeof(caps)))
					ret = -EFAULT;
			}
			break;
		case VIRTIO_RDMA_IOCTL_REGISTER_MR:
			if (mr.len == 0 || mr.len > RDMA_MAX_MR_LEN ||
			    vrdev->mr_count >= RDMA_MAX_MR) {
				ret = -EINVAL;
				break;
			}
			entry = &vrdev->mrs[vrdev->mr_count];
			entry->kaddr = kmalloc(mr.len, GFP_KERNEL);
			if (!entry->kaddr) {
				ret = -ENOMEM;
				break;
			}
			memset(entry->kaddr, 0, mr.len);
			entry->addr = virt_to_phys(entry->kaddr);
			entry->len = mr.len;
			entry->pattern = VIRTIO_RDMA_PATTERN_ZERO;
			entry->pattern_byte = 0;

			req.opcode = cpu_to_le32(RDMA_OPCODE_REGISTER_MR);
			req.addr = cpu_to_le64(entry->addr);
			req.len = cpu_to_le32(entry->len);
			req.mr_id = 0;
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, false);
			if (ret) {
				kfree(entry->kaddr);
				break;
			}
			entry->id = le32_to_cpu(resp.value0);
			if (!entry->id) {
				kfree(entry->kaddr);
				ret = -EIO;
				break;
			}
			mr.mr_id = entry->id;
			mr.addr = entry->addr;
			vrdev->mr_count++;
			if (copy_to_user((void __user *)arg, &mr, sizeof(mr)))
				ret = -EFAULT;
			dev_info(&vrdev->vdev->dev,
				 "virtio-rdma: REGISTER_MR id=%u len=%u\n",
				 entry->id, entry->len);
			break;
		case VIRTIO_RDMA_IOCTL_DEREGISTER_MR:
			req.opcode = cpu_to_le32(RDMA_OPCODE_DEREGISTER_MR);
			req.mr_id = cpu_to_le32(qp_id);
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, true);
			if (!ret) {
				if (le32_to_cpu(resp.status) != RDMA_STATUS_OK) {
					ret = -EIO;
					break;
				}
				if (virtio_rdma_remove_mr(vrdev, qp_id) < 0)
					ret = 0;
				dev_info(&vrdev->vdev->dev,
					 "virtio-rdma: DEREGISTER_MR id=%u\n",
					 qp_id);
			}
			break;
		case VIRTIO_RDMA_IOCTL_DESTROY_QP:
			req.opcode = cpu_to_le32(RDMA_OPCODE_DESTROY_QP);
			req.qp_id = cpu_to_le32(qp_id);
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, true);
			if (!ret) {
				if (le32_to_cpu(resp.status) != RDMA_STATUS_OK) {
					ret = -EIO;
					break;
				}
				vrdev->qp_created = false;
				dev_info(&vrdev->vdev->dev,
					 "virtio-rdma: DESTROY_QP qp_id=%u\n",
					 qp_id);
			}
			break;
		case VIRTIO_RDMA_IOCTL_ALLOC_MR:
			if (mr_alloc.len == 0 ||
			    mr_alloc.len > RDMA_MAX_MR_LEN ||
			    vrdev->mr_count >= RDMA_MAX_MR) {
				ret = -EINVAL;
				break;
			}
			if (mr_alloc.pattern > VIRTIO_RDMA_PATTERN_CONST) {
				ret = -EINVAL;
				break;
			}
			entry = &vrdev->mrs[vrdev->mr_count];
			entry->kaddr = kmalloc(mr_alloc.len, GFP_KERNEL);
			if (!entry->kaddr) {
				ret = -ENOMEM;
				break;
			}
			virtio_rdma_fill_pattern(entry->kaddr, mr_alloc.len,
						 mr_alloc.pattern,
						 mr_alloc.pattern_byte);
			entry->addr = virt_to_phys(entry->kaddr);
			entry->len = mr_alloc.len;
			entry->pattern = mr_alloc.pattern;
			entry->pattern_byte = mr_alloc.pattern_byte;

			req.opcode = cpu_to_le32(RDMA_OPCODE_REGISTER_MR);
			req.addr = cpu_to_le64(entry->addr);
			req.len = cpu_to_le32(entry->len);
			req.mr_id = 0;
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, false);
			if (ret) {
				kfree(entry->kaddr);
				break;
			}
			entry->id = le32_to_cpu(resp.value0);
			if (!entry->id) {
				kfree(entry->kaddr);
				ret = -EIO;
				break;
			}
			mr_alloc.mr_id = entry->id;
			mr_alloc.addr = entry->addr;
			vrdev->mr_count++;
			if (copy_to_user((void __user *)arg, &mr_alloc,
					 sizeof(mr_alloc)))
				ret = -EFAULT;
			dev_info(&vrdev->vdev->dev,
				 "virtio-rdma: ALLOC_MR id=%u len=%u pattern=%u\n",
				 entry->id, entry->len, entry->pattern);
			break;
		case VIRTIO_RDMA_IOCTL_READ_MR:
			entry = virtio_rdma_find_mr(vrdev, mr_rw.mr_id);
			if (!entry) {
				ret = -EINVAL;
				break;
			}
			if (mr_rw.len == 0 ||
			    mr_rw.offset > entry->len ||
			    mr_rw.len > entry->len - mr_rw.offset) {
				ret = -EINVAL;
				break;
			}
			if (copy_to_user((void __user *)(unsigned long)mr_rw.user_ptr,
					 (u8 *)entry->kaddr + mr_rw.offset,
					 mr_rw.len)) {
				ret = -EFAULT;
				break;
			}
			ret = 0;
			break;
		case VIRTIO_RDMA_IOCTL_POST_SEND:
		case VIRTIO_RDMA_IOCTL_POST_RECV:
			entry = virtio_rdma_find_mr(vrdev, wr.mr_id);
			if (!entry) {
				ret = -EINVAL;
				break;
			}
			if (wr.len == 0 || wr.len > entry->len)
				wr.len = entry->len;

			req.opcode = cpu_to_le32(
				cmd == VIRTIO_RDMA_IOCTL_POST_SEND ?
					RDMA_OPCODE_POST_SEND :
					RDMA_OPCODE_POST_RECV);
			req.qp_id = cpu_to_le32(wr.qp_id);
			req.mr_id = cpu_to_le32(wr.mr_id);
			req.addr = cpu_to_le64(entry->addr);
			req.len = cpu_to_le32(wr.len);
			req.wr_id = cpu_to_le64(wr.wr_id);
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, false);
			if (!ret) {
				dev_info(&vrdev->vdev->dev,
					 "virtio-rdma: %s qp_id=%u mr_id=%u wr_id=%llu len=%u\n",
					 cmd == VIRTIO_RDMA_IOCTL_POST_SEND ?
						 "POST_SEND" :
						 "POST_RECV",
					 wr.qp_id, wr.mr_id,
					 (unsigned long long)wr.wr_id, wr.len);
			}
			break;
		case VIRTIO_RDMA_IOCTL_POLL_CQ:
			req.opcode = cpu_to_le32(RDMA_OPCODE_POLL_CQ);
			ret = virtio_rdma_submit(vrdev, &req, &resp, sizeof(req),
						 false, true);
			if (!ret) {
				if (le32_to_cpu(resp.status) ==
				    RDMA_STATUS_EMPTY) {
					ret = -EAGAIN;
					break;
				}
				if (le32_to_cpu(resp.status) != RDMA_STATUS_OK) {
					ret = -EIO;
					break;
				}
				cqe.wr_id = le64_to_cpu(resp.wr_id);
				cqe.status = le32_to_cpu(resp.status);
				cqe.bytes = le32_to_cpu(resp.bytes);
				cqe.opcode = le32_to_cpu(resp.opcode);
				if (copy_to_user((void __user *)arg, &cqe,
						 sizeof(cqe)))
					ret = -EFAULT;
			}
			break;
		default:
			ret = -ENOTTY;
			break;
		}
	}

	mutex_unlock(&vrdev->ioctl_lock);
	return ret;
}

static int virtio_rdma_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;
	struct virtio_rdma_dev *vrdev =
		container_of(mdev, struct virtio_rdma_dev, miscdev);

	file->private_data = vrdev;
	return 0;
}

static int virtio_rdma_release(struct inode *inode, struct file *file)
{
	struct virtio_rdma_dev *vrdev = file->private_data;
	unsigned int i;
	struct virtio_rdma_req req = { 0 };
	struct virtio_rdma_resp resp = { 0 };

	mutex_lock(&vrdev->ioctl_lock);
	for (i = 0; i < vrdev->mr_count; i++) {
		req.opcode = cpu_to_le32(RDMA_OPCODE_DEREGISTER_MR);
		req.mr_id = cpu_to_le32(vrdev->mrs[i].id);
		virtio_rdma_submit(vrdev, &req, &resp, sizeof(req), false,
				   true);
		kfree(vrdev->mrs[i].kaddr);
	}
	vrdev->mr_count = 0;

	if (vrdev->qp_created) {
		req.opcode = cpu_to_le32(RDMA_OPCODE_DESTROY_QP);
		req.qp_id = cpu_to_le32(0);
		virtio_rdma_submit(vrdev, &req, &resp, sizeof(req), false,
				   true);
		vrdev->qp_created = false;
	}
	mutex_unlock(&vrdev->ioctl_lock);
	return 0;
}

static const struct file_operations virtio_rdma_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = virtio_rdma_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = virtio_rdma_ioctl,
#endif
	.open = virtio_rdma_open,
	.release = virtio_rdma_release,
	.llseek = no_llseek,
};

static int virtio_rdma_probe(struct virtio_device *vdev)
{
	struct virtio_rdma_dev *vrdev;
	struct virtqueue *vqs[1];
	vq_callback_t *cbs[1] = { virtio_rdma_ctrl_cb };
	const char *names[1] = { "ctrl" };
	int ret;

	vrdev = devm_kzalloc(&vdev->dev, sizeof(*vrdev), GFP_KERNEL);
	if (!vrdev)
		return -ENOMEM;

	vdev->priv = vrdev;
	vrdev->vdev = vdev;
	mutex_init(&vrdev->ioctl_lock);
	init_completion(&vrdev->ctrl_done);

	ret = virtio_find_vqs(vdev, 1, vqs, cbs, names, NULL);
	if (ret) {
		dev_err(&vdev->dev, "virtio-rdma: failed to find vq: %d\n", ret);
		return ret;
	}

	vrdev->ctrl_vq = vqs[0];
	vrdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	vrdev->miscdev.name = "virtio-rdma0";
	vrdev->miscdev.fops = &virtio_rdma_fops;

	ret = misc_register(&vrdev->miscdev);
	if (ret) {
		dev_err(&vdev->dev, "virtio-rdma: misc_register failed: %d\n",
			ret);
		vdev->config->del_vqs(vdev);
		return ret;
	}

	dev_info(&vdev->dev, "virtio-rdma: probed\n");
	dev_info(&vdev->dev,
		 "virtio-rdma: features=0x%llx vq_size=%u\n",
		 vdev->features,
		 virtqueue_get_vring_size(vrdev->ctrl_vq));

	return 0;
}

static void virtio_rdma_remove(struct virtio_device *vdev)
{
	struct virtio_rdma_dev *vrdev = vdev->priv;

	dev_info(&vdev->dev, "virtio-rdma: removed\n");
	virtio_rdma_free_mrs(vrdev);
	misc_deregister(&vrdev->miscdev);
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
}

static const struct virtio_device_id virtio_rdma_id_table[] = {
	{ VIRTIO_ID_RDMA, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_rdma_driver = {
	.feature_table = NULL,
	.feature_table_size = 0,
	.driver.name = "virtio_rdma",
	.driver.owner = THIS_MODULE,
	.id_table = virtio_rdma_id_table,
	.probe = virtio_rdma_probe,
	.remove = virtio_rdma_remove,
};

module_virtio_driver(virtio_rdma_driver);
MODULE_DEVICE_TABLE(virtio, virtio_rdma_id_table);

MODULE_AUTHOR("Firecracker contributors");
MODULE_DESCRIPTION("Minimal virtio-rdma guest driver for Firecracker");
MODULE_LICENSE("GPL");
