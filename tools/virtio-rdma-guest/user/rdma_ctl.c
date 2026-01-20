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
		"  %s query-caps\n"
		"  %s register-mr <len>\n"
		"  %s alloc-mr <len> [--pattern=inc|rand|0xaa]\n"
		"  %s dump-mr <mr_id> <offset> <len>\n"
		"  %s check-mr <mr_id> <offset> <len> --expect=inc|rand|0xaa\n"
		"  %s deregister-mr <mr_id>\n"
		"  %s destroy-qp <qp_id>\n"
		"  %s stress --iters <n> --outstanding <n>\n"
		"  %s post-send <qp_id> <mr_id> <len> <wr_id>\n"
		"  %s post-recv <qp_id> <mr_id> <len> <wr_id>\n"
		"  %s poll-cq\n"
		"  %s send-raw --opcode <hex> --qp <id> [--truncate]\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog);
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

static int parse_u32(const char *arg, uint32_t *out)
{
	char *end = NULL;

	*out = (uint32_t)strtoul(arg, &end, 0);
	return (end && *end == '\0') ? 0 : -1;
}

static int parse_u64(const char *arg, uint64_t *out)
{
	char *end = NULL;

	*out = (uint64_t)strtoull(arg, &end, 0);
	return (end && *end == '\0') ? 0 : -1;
}

static int parse_pattern(const char *arg, uint32_t *pattern,
			 uint32_t *pattern_byte)
{
	if (strcmp(arg, "inc") == 0) {
		*pattern = VIRTIO_RDMA_PATTERN_INC;
		*pattern_byte = 0;
		return 0;
	}
	if (strcmp(arg, "rand") == 0) {
		*pattern = VIRTIO_RDMA_PATTERN_RAND;
		*pattern_byte = 0;
		return 0;
	}
	if (strncmp(arg, "0x", 2) == 0 || strncmp(arg, "0X", 2) == 0) {
		uint32_t value;
		char *end = NULL;

		value = (uint32_t)strtoul(arg, &end, 16);
		if (!end || *end != '\0' || value > 0xFF)
			return -1;
		*pattern = VIRTIO_RDMA_PATTERN_CONST;
		*pattern_byte = value;
		return 0;
	}
	if (strcmp(arg, "zero") == 0) {
		*pattern = VIRTIO_RDMA_PATTERN_ZERO;
		*pattern_byte = 0;
		return 0;
	}
	return -1;
}

static const char *opcode_name(uint32_t opcode)
{
	switch (opcode) {
	case 4:
		return "SEND";
	case 5:
		return "RECV";
	default:
		return "UNKNOWN";
	}
}

static uint32_t rand_step(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static int check_pattern(const uint8_t *buf, uint32_t offset, uint32_t len,
			 uint32_t pattern, uint32_t pattern_byte)
{
	uint32_t i;

	switch (pattern) {
	case VIRTIO_RDMA_PATTERN_ZERO:
		for (i = 0; i < len; i++) {
			if (buf[i] != 0)
				return -1;
		}
		return 0;
	case VIRTIO_RDMA_PATTERN_INC:
		for (i = 0; i < len; i++) {
			if (buf[i] != (uint8_t)(offset + i))
				return -1;
		}
		return 0;
	case VIRTIO_RDMA_PATTERN_RAND: {
		uint32_t state = 0x12345678;
		uint32_t index;

		for (index = 0; index < offset + len; index++) {
			uint8_t value = (uint8_t)rand_step(&state);
			if (index >= offset && value != buf[index - offset])
				return -1;
		}
		return 0;
	}
	case VIRTIO_RDMA_PATTERN_CONST:
	default:
		for (i = 0; i < len; i++) {
			if (buf[i] != (uint8_t)pattern_byte)
				return -1;
		}
		return 0;
	}
}

static void dump_hex(const uint8_t *buf, uint32_t offset, uint32_t len)
{
	const uint32_t max_dump = 256;
	uint32_t i;
	uint32_t dump_len = len > max_dump ? max_dump : len;

	for (i = 0; i < dump_len; i++) {
		if ((i % 16) == 0)
			printf("%08x: ", offset + i);
		printf("%02x ", buf[i]);
		if ((i % 16) == 15 || i + 1 == dump_len)
			printf("\n");
	}
	if (len > max_dump)
		printf("... truncated (%u bytes total)\n", len);
}

int main(int argc, char **argv)
{
	struct virtio_rdma_raw raw;
	struct virtio_rdma_caps caps;
	struct virtio_rdma_mr mr;
	struct virtio_rdma_mr_alloc mr_alloc;
	struct virtio_rdma_mr_rw mr_rw;
	struct virtio_rdma_wr wr;
	struct virtio_rdma_cqe cqe;
	uint32_t qp_id;
	uint32_t pattern = VIRTIO_RDMA_PATTERN_ZERO;
	uint32_t pattern_byte = 0;
	int fd;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	fd = open("/dev/virtio-rdma0", O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (strcmp(argv[1], "create-qp") == 0 && argc == 3) {
		if (parse_u32(argv[2], &qp_id) != 0) {
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
		uint32_t start;
		uint32_t count;

		if (parse_u32(argv[2], &start) != 0) {
			fprintf(stderr, "Invalid start: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		if (parse_u32(argv[3], &count) != 0 || count == 0) {
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

	if (strcmp(argv[1], "query-caps") == 0 && argc == 2) {
		memset(&caps, 0, sizeof(caps));
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_QUERY_CAPS, &caps);
		if (ret == 0) {
			printf("version=%u.%u max_qp=%u max_mr=%u max_cq=%u max_wr=%u\n",
			       caps.version_major, caps.version_minor, caps.max_qp,
			       caps.max_mr, caps.max_cq, caps.max_wr);
		}
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "register-mr") == 0 && argc == 3) {
		memset(&mr, 0, sizeof(mr));
		if (parse_u32(argv[2], &mr.len) != 0 || mr.len == 0) {
			fprintf(stderr, "Invalid len: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_REGISTER_MR, &mr);
		if (ret == 0)
			printf("mr_id=%u addr=0x%llx len=%u\n",
			       mr.mr_id, (unsigned long long)mr.addr, mr.len);
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "alloc-mr") == 0 &&
	    (argc == 3 || argc == 4)) {
		memset(&mr_alloc, 0, sizeof(mr_alloc));
		if (parse_u32(argv[2], &mr_alloc.len) != 0 ||
		    mr_alloc.len == 0) {
			fprintf(stderr, "Invalid len: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		mr_alloc.pattern = VIRTIO_RDMA_PATTERN_INC;
		mr_alloc.pattern_byte = 0;
		if (argc == 4) {
			const char *arg = argv[3];

			if (strncmp(arg, "--pattern=", 10) != 0 ||
			    parse_pattern(arg + 10, &pattern, &pattern_byte) !=
				    0) {
				fprintf(stderr, "Invalid pattern: %s\n", arg);
				close(fd);
				return 1;
			}
			mr_alloc.pattern = pattern;
			mr_alloc.pattern_byte = pattern_byte;
		}

		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_ALLOC_MR, &mr_alloc);
		if (ret == 0)
			printf("mr_id=%u addr=0x%llx len=%u\n",
			       mr_alloc.mr_id,
			       (unsigned long long)mr_alloc.addr,
			       mr_alloc.len);
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "dump-mr") == 0 && argc == 5) {
		uint32_t mr_id;
		uint32_t offset;
		uint32_t len;
		uint8_t *buf;

		if (parse_u32(argv[2], &mr_id) != 0 ||
		    parse_u32(argv[3], &offset) != 0 ||
		    parse_u32(argv[4], &len) != 0 || len == 0) {
			fprintf(stderr, "Invalid args\n");
			close(fd);
			return 1;
		}
		buf = malloc(len);
		if (!buf) {
			fprintf(stderr, "malloc failed\n");
			close(fd);
			return 1;
		}
		memset(&mr_rw, 0, sizeof(mr_rw));
		mr_rw.mr_id = mr_id;
		mr_rw.offset = offset;
		mr_rw.len = len;
		mr_rw.user_ptr = (uintptr_t)buf;
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_READ_MR, &mr_rw);
		if (ret == 0)
			dump_hex(buf, offset, len);
		free(buf);
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "check-mr") == 0 && argc == 6) {
		uint32_t mr_id;
		uint32_t offset;
		uint32_t len;
		uint8_t *buf;

		if (strncmp(argv[5], "--expect=", 9) != 0 ||
		    parse_pattern(argv[5] + 9, &pattern, &pattern_byte) != 0) {
			fprintf(stderr, "Invalid expect: %s\n", argv[5]);
			close(fd);
			return 1;
		}
		if (parse_u32(argv[2], &mr_id) != 0 ||
		    parse_u32(argv[3], &offset) != 0 ||
		    parse_u32(argv[4], &len) != 0 || len == 0) {
			fprintf(stderr, "Invalid args\n");
			close(fd);
			return 1;
		}
		buf = malloc(len);
		if (!buf) {
			fprintf(stderr, "malloc failed\n");
			close(fd);
			return 1;
		}
		memset(&mr_rw, 0, sizeof(mr_rw));
		mr_rw.mr_id = mr_id;
		mr_rw.offset = offset;
		mr_rw.len = len;
		mr_rw.user_ptr = (uintptr_t)buf;
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_READ_MR, &mr_rw);
		if (ret == 0) {
			if (check_pattern(buf, offset, len, pattern,
					  pattern_byte) == 0) {
				printf("OK\n");
				ret = 0;
			} else {
				printf("MISMATCH\n");
				ret = 1;
			}
		}
		free(buf);
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "deregister-mr") == 0 && argc == 3) {
		uint32_t mr_id;

		if (parse_u32(argv[2], &mr_id) != 0) {
			fprintf(stderr, "Invalid mr_id: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_DEREGISTER_MR, &mr_id);
		if (ret == 0)
			printf("OK\n");
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "destroy-qp") == 0 && argc == 3) {
		if (parse_u32(argv[2], &qp_id) != 0) {
			fprintf(stderr, "Invalid qp_id: %s\n", argv[2]);
			close(fd);
			return 1;
		}
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_DESTROY_QP, &qp_id);
		if (ret == 0)
			printf("OK\n");
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "stress") == 0) {
		uint32_t iters = 0;
		uint32_t outstanding = 0;
		uint32_t send_mr = 0;
		uint32_t recv_mr = 0;
		uint32_t len = 256;

		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
				if (parse_u32(argv[++i], &iters) != 0)
					iters = 0;
			} else if (strcmp(argv[i], "--outstanding") == 0 &&
				   i + 1 < argc) {
				if (parse_u32(argv[++i], &outstanding) != 0)
					outstanding = 0;
			} else {
				usage(argv[0]);
				close(fd);
				return 1;
			}
		}
		if (iters == 0 || outstanding == 0) {
			fprintf(stderr,
				"stress requires --iters and --outstanding\n");
			close(fd);
			return 1;
		}

		qp_id = 1;
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_CREATE_QP, &qp_id);
		if (ret != 0) {
			close(fd);
			return 1;
		}

		memset(&mr_alloc, 0, sizeof(mr_alloc));
		mr_alloc.len = 4096;
		mr_alloc.pattern = VIRTIO_RDMA_PATTERN_INC;
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_ALLOC_MR, &mr_alloc);
		if (ret != 0) {
			close(fd);
			return 1;
		}
		send_mr = mr_alloc.mr_id;

		memset(&mr_alloc, 0, sizeof(mr_alloc));
		mr_alloc.len = 4096;
		mr_alloc.pattern = VIRTIO_RDMA_PATTERN_ZERO;
		ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_ALLOC_MR, &mr_alloc);
		if (ret != 0) {
			close(fd);
			return 1;
		}
		recv_mr = mr_alloc.mr_id;

		for (uint32_t iter = 0; iter < iters; iter++) {
			uint64_t recv_base = ((uint64_t)iter << 32);
			uint64_t send_base = recv_base + 0x10000000ULL;
			uint32_t total = outstanding * 2;
			uint8_t *seen = calloc(total, 1);

			if (!seen) {
				fprintf(stderr, "calloc failed\n");
				close(fd);
				return 1;
			}
			for (uint32_t i = 0; i < outstanding; i++) {
				wr.qp_id = 1;
				wr.mr_id = recv_mr;
				wr.len = len;
				wr.wr_id = recv_base | i;
				ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_POST_RECV,
					       &wr);
				if (ret != 0) {
					fprintf(stderr,
						"POST_RECV failed at iter=%u i=%u\n",
						iter, i);
					free(seen);
					close(fd);
					return 1;
				}
			}

			for (uint32_t i = 0; i < outstanding; i++) {
				wr.qp_id = 1;
				wr.mr_id = send_mr;
				wr.len = len;
				wr.wr_id = send_base | i;
				ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_POST_SEND,
					       &wr);
				if (ret != 0) {
					fprintf(stderr,
						"POST_SEND failed at iter=%u i=%u\n",
						iter, i);
					free(seen);
					close(fd);
					return 1;
				}
			}

			uint32_t completed = 0;
			while (completed < outstanding * 2) {
				ret = ioctl(fd, VIRTIO_RDMA_IOCTL_POLL_CQ,
					    &cqe);
				if (ret < 0) {
					if (errno == EAGAIN)
						continue;
					fprintf(stderr,
						"poll-cq failed: %s\n",
						strerror(errno));
					free(seen);
					close(fd);
					return 1;
				}
				if (cqe.status != 0 || cqe.bytes != len) {
					fprintf(stderr,
						"bad cqe: wr_id=%llu status=%u bytes=%u\n",
						(unsigned long long)cqe.wr_id,
						cqe.status, cqe.bytes);
					free(seen);
					close(fd);
					return 1;
				}
				if (cqe.wr_id >= recv_base &&
				    cqe.wr_id < recv_base + outstanding) {
					uint32_t idx =
						(uint32_t)(cqe.wr_id - recv_base);
					if (cqe.opcode != 5 || seen[idx]) {
						fprintf(stderr,
							"bad recv cqe wr_id=%llu opcode=%u\n",
							(unsigned long long)cqe.wr_id,
							cqe.opcode);
						free(seen);
						close(fd);
						return 1;
					}
					seen[idx] = 1;
				} else if (cqe.wr_id >= send_base &&
					   cqe.wr_id < send_base + outstanding) {
					uint32_t idx = outstanding +
						       (uint32_t)(cqe.wr_id -
								  send_base);
					if (cqe.opcode != 4 || seen[idx]) {
						fprintf(stderr,
							"bad send cqe wr_id=%llu opcode=%u\n",
							(unsigned long long)cqe.wr_id,
							cqe.opcode);
						free(seen);
						close(fd);
						return 1;
					}
					seen[idx] = 1;
				} else {
					fprintf(stderr,
						"unexpected wr_id=%llu\n",
						(unsigned long long)cqe.wr_id);
					free(seen);
					close(fd);
					return 1;
				}
				completed++;
			}
			free(seen);

			memset(&mr_rw, 0, sizeof(mr_rw));
			mr_rw.mr_id = recv_mr;
			mr_rw.offset = 0;
			mr_rw.len = len;
			uint8_t buf[256];
			mr_rw.user_ptr = (uintptr_t)buf;
			ret = do_ioctl(fd, VIRTIO_RDMA_IOCTL_READ_MR, &mr_rw);
			if (ret != 0) {
				close(fd);
				return 1;
			}
			if (check_pattern(buf, 0, len,
					  VIRTIO_RDMA_PATTERN_INC, 0) != 0) {
				fprintf(stderr,
					"data mismatch at iter=%u\n", iter);
				close(fd);
				return 1;
			}
		}

		printf("OK\n");
		close(fd);
		return 0;
	}

	if ((strcmp(argv[1], "post-send") == 0 ||
	     strcmp(argv[1], "post-recv") == 0) &&
	    argc == 6) {
		memset(&wr, 0, sizeof(wr));
		if (parse_u32(argv[2], &wr.qp_id) != 0 ||
		    parse_u32(argv[3], &wr.mr_id) != 0 ||
		    parse_u32(argv[4], &wr.len) != 0 ||
		    parse_u64(argv[5], &wr.wr_id) != 0) {
			fprintf(stderr, "Invalid args\n");
			close(fd);
			return 1;
		}
		ret = do_ioctl(fd,
			       strcmp(argv[1], "post-send") == 0 ?
				       VIRTIO_RDMA_IOCTL_POST_SEND :
				       VIRTIO_RDMA_IOCTL_POST_RECV,
			       &wr);
		if (ret == 0)
			printf("OK\n");
		close(fd);
		return ret == 0 ? 0 : 1;
	}

	if (strcmp(argv[1], "poll-cq") == 0 && argc == 2) {
		memset(&cqe, 0, sizeof(cqe));
		ret = ioctl(fd, VIRTIO_RDMA_IOCTL_POLL_CQ, &cqe);
		if (ret < 0) {
			if (errno == EAGAIN) {
				printf("EMPTY\n");
				close(fd);
				return 0;
			}
			fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		printf("wr_id=%llu status=%u bytes=%u type=%s\n",
		       (unsigned long long)cqe.wr_id, cqe.status, cqe.bytes,
		       opcode_name(cqe.opcode));
		close(fd);
		return 0;
	}

	if (strcmp(argv[1], "send-raw") == 0) {
		int have_opcode = 0;
		int have_qp = 0;

		memset(&raw, 0, sizeof(raw));
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--opcode") == 0 && i + 1 < argc) {
				if (parse_u32(argv[++i], &raw.opcode) != 0) {
					fprintf(stderr, "Invalid opcode\n");
					close(fd);
					return 1;
				}
				have_opcode = 1;
			} else if (strcmp(argv[i], "--qp") == 0 && i + 1 < argc) {
				if (parse_u32(argv[++i], &raw.qp_id) != 0) {
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
