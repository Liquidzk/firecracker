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
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/wait.h>

#include "virtio_rdma_uapi.h"

/* Must match Firecracker's VirtioDeviceType::Rdma value. */
#define VIRTIO_ID_RDMA 42

struct virtio_rdma_req {
	__le32 opcode;
	__le32 qp_id;
};

struct virtio_rdma_resp {
	__le32 status;
};

struct virtio_rdma_dev {
	struct virtio_device *vdev;
	struct virtqueue *ctrl_vq;
	struct miscdevice miscdev;
	struct mutex ioctl_lock;
	struct completion ctrl_done;
	struct virtio_rdma_req *req;
	struct virtio_rdma_resp *resp;
	u32 last_status;
	u32 last_qp_id;
	bool inflight;
	bool timed_out;
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
		vrdev->last_status = le32_to_cpu(vrdev->resp->status);
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

static int virtio_rdma_submit(
	struct virtio_rdma_dev *vrdev, u32 opcode, u32 qp_id, u32 *status_out,
	bool map_invalid)
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

	vrdev->req->opcode = cpu_to_le32(opcode);
	vrdev->req->qp_id = cpu_to_le32(qp_id);
	vrdev->last_qp_id = qp_id;
	vrdev->last_status = 0;
	vrdev->timed_out = false;
	reinit_completion(&vrdev->ctrl_done);
	vrdev->inflight = true;

	sg_init_one(&sg_req, vrdev->req, sizeof(*vrdev->req));
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
	*status_out = vrdev->last_status;
	ret = (vrdev->last_status == 0) ? 0 : (map_invalid ? -EINVAL : -EIO);

out_inflight:
	if (!vrdev->inflight)
		virtio_rdma_free_buffers(vrdev);
	return ret;

out_free:
	virtio_rdma_free_buffers(vrdev);
	return ret;
}

static long virtio_rdma_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct miscdevice *mdev = file->private_data;
	struct virtio_rdma_dev *vrdev =
		container_of(mdev, struct virtio_rdma_dev, miscdev);
	struct virtio_rdma_raw raw = { 0 };
	u32 qp_id;
	u32 status = 0;
	int ret;

	if (cmd != VIRTIO_RDMA_IOCTL_CREATE_QP &&
	    cmd != VIRTIO_RDMA_IOCTL_SEND_RAW)
		return -ENOTTY;

	if (cmd == VIRTIO_RDMA_IOCTL_CREATE_QP) {
		if (copy_from_user(&qp_id, (void __user *)arg, sizeof(qp_id)))
			return -EFAULT;
	} else {
		if (copy_from_user(&raw, (void __user *)arg, sizeof(raw)))
			return -EFAULT;
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
		ret = virtio_rdma_submit(vrdev, 1, qp_id, &status, false);
		dev_info(&vrdev->vdev->dev,
			 "virtio-rdma: CREATE_QP qp_id=%u -> status=%u\n",
			 qp_id, status);
	} else {
		ret = virtio_rdma_submit(vrdev, raw.opcode, raw.qp_id, &status,
					 true);
		dev_info(&vrdev->vdev->dev,
			 "virtio-rdma: RAW opcode=0x%x qp_id=%u -> status=%u\n",
			 raw.opcode, raw.qp_id, status);
	}

	mutex_unlock(&vrdev->ioctl_lock);
	return ret;
}

static const struct file_operations virtio_rdma_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = virtio_rdma_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = virtio_rdma_ioctl,
#endif
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
