// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "virtio_rdma_uapi.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s create-qp <qp_id>\n"
		"  %s loop-create-qp <start> <count>\n"
		"  %s send-raw --opcode <hex> --qp <id> [--truncate]\n",
		prog, prog, prog);
}

static int do_ioctl(int fd, unsigned int cmd, void *arg)
{
	int ret;

	ret = ioctl(fd, cmd, arg);
	if (ret < 0) {
		fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct virtio_rdma_raw raw;
	uint32_t qp_id;
	char *end = NULL;
	int fd;
	int ret;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	fd = open("/dev/virtio-rdma0", O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (strcmp(argv[1], "create-qp") == 0 && argc == 3) {
		qp_id = (uint32_t)strtoul(argv[2], &end, 0);
		if (!end || *end != '\0') {
			fprintf(stderr, "Invalid qp_id: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_CREATE_QP, &qp_id);
		if (ret == 0)
			printf("OK\n");
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "loop-create-qp") == 0 && argc == 4) {
		uint32_t start = (uint32_t)strtoul(argv[2], &end, 0);
		uint32_t count;

		if (!end || *end != '\0') {
			fprintf(stderr, "Invalid start: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		count = (uint32_t)strtoul(argv[3], &end, 0);
		if (!end || *end != '\0' || count == 0) {
			fprintf(stderr, "Invalid count: %s\n", argv[3]);
			close(fd);
			return 1;
		}

		for (uint32_t i = 0; i < count; i++) {
			qp_id = start + i;
			ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_CREATE_QP, &qp_id);
			if (ret != 0) {
				fprintf(stderr, "Failed at qp_id=%u\n", qp_id);
				close(fd);
				return 1;
			}
		}
		printf("OK\n");
		close(fd);
		return 0;
	}

	if (strcmp(argv[1], "send-raw") == 0) {
		int have_opcode = 0;
		int have_qp = 0;

		memset(&raw, 0, sizeof(raw));
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--opcode") == 0 && i + 1 < argc) {
				raw.opcode = (uint32_t)strtoul(argv[++i], &end, 0);
				if (!end || *end != '\0') {
					fprintf(stderr, "Invalid opcode\n");
					close(fd);
					return 1;
				}
				have_opcode = 1;
			} else if (strcmp(argv[i], "--qp") == 0 && i + 1 < argc) {
				raw.qp_id = (uint32_t)strtoul(argv[++i], &end, 0);
				if (!end || *end != '\0') {
					fprintf(stderr, "Invalid qp_id\n");
					close(fd);
					return 1;
				}
				have_qp = 1;
			} else if (strcmp(argv[i], "--truncate") == 0) {
				raw.flags |= VIRTIO_RDMA_RAW_F_TRUNCATE;
			} else {
				usage(argv[0]);
				close(fd);
				return 1;
			}
		}
		if (!have_opcode || !have_qp) {
			fprintf(stderr, "Missing --opcode or --qp\n");
			close(fd);
			return 1;
		}
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_SEND_RAW, &raw);
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	usage(argv[0]);
	close(fd);
	return 1;
}
