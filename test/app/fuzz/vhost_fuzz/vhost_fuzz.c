/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

#include <linux/virtio_blk.h>
#include <linux/virtio_scsi.h>

#define DEFAULT_RUNTIME 30 /* seconds */
#define MAX_RUNTIME_S 86400 /* 24 hours */
#define IO_TIMEOUT_S 5

/* Features desired/implemented by virtio blk. */
#define VIRTIO_BLK_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_BLK_F_BLK_SIZE		|	\
	 1ULL << VIRTIO_BLK_F_TOPOLOGY		|	\
	 1ULL << VIRTIO_BLK_F_MQ		|	\
	 1ULL << VIRTIO_BLK_F_RO		|	\
	 1ULL << VIRTIO_BLK_F_DISCARD		|	\
	 1ULL << VIRTIO_RING_F_EVENT_IDX	|	\
	 1ULL << VHOST_USER_F_PROTOCOL_FEATURES)

/* Features desired/implemented by virtio scsi. */
#define VIRTIO_SCSI_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_SCSI_F_INOUT		|	\
	 1ULL << VIRTIO_SCSI_F_HOTPLUG		|	\
	 1ULL << VIRTIO_RING_F_EVENT_IDX	|	\
	 1ULL << VHOST_USER_F_PROTOCOL_FEATURES)

#define VIRTIO_DEV_FIXED_QUEUES	2
#define VIRTIO_SCSI_CONTROLQ	0
#define VIRTIO_SCSI_EVENTQ	1
#define VIRTIO_REQUESTQ		2

struct vhost_fuzz_iov_ctx {
	struct iovec			iov_req;
	struct iovec			iov_data;
	struct iovec			iov_resp;
};

struct vhost_fuzz_io_ctx {
	struct vhost_fuzz_iov_ctx		iovs;
	union {
		struct virtio_blk_outhdr	blk_req;
		struct virtio_scsi_cmd_req	scsi_req;
		struct virtio_scsi_ctrl_tmf_req	scsi_tmf_req;
	} req;
	union {
		uint8_t					blk_resp;
		struct virtio_scsi_cmd_resp		scsi_resp;
		struct virtio_scsi_ctrl_tmf_resp	scsi_tmf_resp;
	} resp;
};

struct hugepage_file_info {
	uint64_t addr;            /**< virtual addr */
	size_t   size;            /**< the file size */
	char     path[PATH_MAX];  /**< path to backing file */
};

struct hugepage_file_info	g_huge_regions[VHOST_USER_MEMORY_MAX_NREGIONS];
int				g_num_mem_regions;

char				g_socket_path[PATH_MAX + 1] = {0};
bool				g_socket_is_blk = false;
bool				g_test_scsi_tmf = false;

time_t				g_seed_value = 0;
int				g_num_io = 0;
int				g_successful_io = 0;
int				g_runtime;

struct virtio_dev		*g_virtio_dev;
struct vhost_fuzz_io_ctx	*g_io_ctx;

/* Copied some code from vhost_user.c to make finding valid memory regions easier. */
static int
get_hugepage_file_info(struct hugepage_file_info huges[], int max)
{
	int idx, rc;
	FILE *f;
	char buf[BUFSIZ], *tmp, *tail;
	char *str_underline, *str_start;
	int huge_index;
	uint64_t v_start, v_end;

	f = fopen("/proc/self/maps", "r");
	if (!f) {
		SPDK_ERRLOG("cannot open /proc/self/maps\n");
		rc = -errno;
		assert(rc < 0); /* scan-build hack */
		return rc;
	}

	idx = 0;
	while (fgets(buf, sizeof(buf), f) != NULL) {
		if (sscanf(buf, "%" PRIx64 "-%" PRIx64, &v_start, &v_end) < 2) {
			SPDK_ERRLOG("Failed to parse address\n");
			rc = -EIO;
			goto out;
		}

		tmp = strchr(buf, ' ') + 1; /** skip address */
		tmp = strchr(tmp, ' ') + 1; /** skip perm */
		tmp = strchr(tmp, ' ') + 1; /** skip offset */
		tmp = strchr(tmp, ' ') + 1; /** skip dev */
		tmp = strchr(tmp, ' ') + 1; /** skip inode */
		while (*tmp == ' ') {       /** skip spaces */
			tmp++;
		}
		tail = strrchr(tmp, '\n');  /** remove newline if exists */
		if (tail) {
			*tail = '\0';
		}

		/* Match HUGEFILE_FMT, aka "%s/%smap_%d",
		 * which is defined in eal_filesystem.h
		 */
		str_underline = strrchr(tmp, '_');
		if (!str_underline) {
			continue;
		}

		str_start = str_underline - strlen("map");
		if (str_start < tmp) {
			continue;
		}

		if (sscanf(str_start, "map_%d", &huge_index) != 1) {
			continue;
		}

		if (idx >= max) {
			SPDK_ERRLOG("Exceed maximum of %d\n", max);
			rc = -ENOSPC;
			goto out;
		}

		if (idx > 0 &&
		    strncmp(tmp, huges[idx - 1].path, PATH_MAX) == 0 &&
		    v_start == huges[idx - 1].addr + huges[idx - 1].size) {
			huges[idx - 1].size += (v_end - v_start);
			continue;
		}

		huges[idx].addr = v_start;
		huges[idx].size = v_end - v_start;
		snprintf(huges[idx].path, PATH_MAX, "%s", tmp);
		idx++;
	}

	rc = idx;
out:
	fclose(f);
	return rc;
}

/* end vhost_user code. */

static int virtio_dev_init(struct virtio_dev *vdev, uint64_t flags, uint16_t max_queues)
{
	int rc;

	rc = virtio_user_dev_init(vdev, "fuzz_device", (const char *)&g_socket_path, 1024);
	if (rc != 0) {
		fprintf(stderr, "Failed to initialize virtual bdev\n");
		return rc;
	}

	rc = virtio_dev_reset(vdev, flags);
	if (rc != 0) {
		return rc;
	}

	rc = virtio_dev_start(vdev, max_queues, VIRTIO_DEV_FIXED_QUEUES);
	if (rc != 0) {
		return rc;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_REQUESTQ);
	if (rc < 0) {
		fprintf(stderr, "Couldn't get an unused queue for the io_channel.\n");
		virtio_dev_stop(vdev);
		return -1;
	}
	return 0;
}

static int
blk_dev_init(struct virtio_dev *vdev, uint16_t max_queues)
{
	uint16_t host_max_queues;
	int rc;

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_MQ)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, num_queues),
						&host_max_queues, sizeof(host_max_queues));
		if (rc) {
			fprintf(stderr, "%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			return rc;
		}
	} else {
		host_max_queues = 1;
	}

	if (max_queues == 0) {
		fprintf(stderr, "%s: requested 0 request queues (%"PRIu16" available).\n",
			vdev->name, host_max_queues);
		return -EINVAL;
	}

	if (max_queues > host_max_queues) {
		fprintf(stderr, "%s: requested %"PRIu16" request queues "
			"but only %"PRIu16" available.\n",
			vdev->name, max_queues, host_max_queues);
		max_queues = host_max_queues;
	}

	return virtio_dev_init(vdev, VIRTIO_BLK_DEV_SUPPORTED_FEATURES, max_queues);

	return 0;
}

static int
scsi_dev_init(struct virtio_dev *vdev, uint16_t max_queues)
{
	int rc;

	rc = virtio_dev_init(vdev, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES, max_queues);
	if (rc != 0) {
		return rc;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_CONTROLQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the controlq.\n");
		return -1;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_EVENTQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the eventq.\n");
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_CONTROLQ);
		return -1;
	}

	return 0;
}

static void
print_blk_io_data(void)
{
	printf("req type: %u\n", g_io_ctx->req.blk_req.type);
	printf("req ioprio: %u\n", g_io_ctx->req.blk_req.ioprio);
	printf("req sector: %llu\n", g_io_ctx->req.blk_req.sector);
}

static void
print_scsi_tmf_io_data(void)
{
	uint64_t i;

	printf("req type: %u\n", g_io_ctx->req.scsi_tmf_req.type);
	printf("req subtype: %u\n", g_io_ctx->req.scsi_tmf_req.subtype);
	for (i = 0; i < sizeof(g_io_ctx->req.scsi_tmf_req.lun); i++) {
		printf("lun[%lu]: %u\n", i, g_io_ctx->req.scsi_tmf_req.lun[i]);
	}
	printf("req tag: %llu\n", g_io_ctx->req.scsi_tmf_req.tag);
}

static void
print_scsi_io_data(void)
{
	uint64_t i;

	for (i = 0; i < sizeof(g_io_ctx->req.scsi_req.lun); i++) {
		printf("lun[%lu]: %u\n", i, g_io_ctx->req.scsi_req.lun[i]);
	}
	printf("req tag: %llu\n", g_io_ctx->req.scsi_req.tag);
	printf("req task_attr: %u\n", g_io_ctx->req.scsi_req.task_attr);
	printf("req prio: %u\n", g_io_ctx->req.scsi_req.prio);
	printf("req crn: %u\n", g_io_ctx->req.scsi_req.crn);
	for (i = 0; i < sizeof(g_io_ctx->req.scsi_req.cdb); i++) {
		printf("lun[%lu]: %u\n", i, g_io_ctx->req.scsi_req.cdb[i]);
	}
}

static void
print_iov_obj(const char *iov_name, struct iovec *iov)
{
	printf("%s iov base: %p\n", iov_name, iov->iov_base);
	printf("%s iov_len: %lu\n", iov_name, iov->iov_len);
}

static void
print_req_obj(void)
{
	print_iov_obj("req", &g_io_ctx->iovs.iov_req);
	print_iov_obj("data", &g_io_ctx->iovs.iov_data);
	print_iov_obj("resp", &g_io_ctx->iovs.iov_resp);
	if (g_socket_is_blk) {
		print_blk_io_data();
	} else if (g_test_scsi_tmf) {
		print_scsi_tmf_io_data();
	} else {
		print_scsi_io_data();
	}
}

/* Make sure that whatever value we get back is not going to actually get written to and explode on us. */
static void *
get_invalid_mem_address(uint64_t length)
{
	uint64_t chosen_address = 0x0;
	int i;
	bool found_invalid = false;

	while (!found_invalid) {
		chosen_address = rand();
		found_invalid = true;
		for (i = 0; i < VHOST_USER_MEMORY_MAX_NREGIONS; i++) {
			if (chosen_address > g_huge_regions[i].addr &&
			    chosen_address + length < g_huge_regions[i].addr + g_huge_regions[i].size) {
				found_invalid = false;
				break;
			}
		}
	}
	return (void *)chosen_address;
}

static void
fill_random_bytes(char *character_repr, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		character_repr[i] = rand() % UINT8_MAX;
	}
}

static void
craft_virtio_scsi_req(void)
{
	fill_random_bytes((char *)&g_io_ctx->req.scsi_req, sizeof(g_io_ctx->req.scsi_req));
}

static void
craft_virtio_scsi_tmf_req(void)
{
	fill_random_bytes((char *)&g_io_ctx->req.scsi_req, sizeof(g_io_ctx->req.scsi_req));
}

static void
craft_virtio_blk_req(void)
{
	g_io_ctx->req.blk_req.type = rand() % 8;
	g_io_ctx->req.blk_req.sector = rand() & ~0x200ULL;
}

static void
craft_virtio_req_rsp_pair(void)
{
	struct vhost_fuzz_iov_ctx *iovs = &g_io_ctx->iovs;

	iovs->iov_req.iov_base = &g_io_ctx->req;
	iovs->iov_data.iov_len = rand();
	iovs->iov_data.iov_base = get_invalid_mem_address(iovs->iov_data.iov_len);
	iovs->iov_resp.iov_base = &g_io_ctx->resp;

	if (g_socket_is_blk) {
		iovs->iov_req.iov_len = sizeof(g_io_ctx->req.blk_req);
		iovs->iov_resp.iov_len = sizeof(g_io_ctx->resp.blk_resp);
		craft_virtio_blk_req();
	} else if (g_test_scsi_tmf) {
		iovs->iov_req.iov_len = sizeof(g_io_ctx->req.scsi_tmf_req);
		iovs->iov_resp.iov_len = sizeof(g_io_ctx->resp.scsi_tmf_resp);
		craft_virtio_scsi_tmf_req();
	} else {
		iovs->iov_req.iov_len = sizeof(g_io_ctx->req.scsi_req);
		iovs->iov_resp.iov_len = sizeof(g_io_ctx->resp.scsi_resp);
		craft_virtio_scsi_req();
	}
}

static void
submit_virtio_req_rsp_pair(struct virtqueue *vq)
{
	struct vhost_fuzz_iov_ctx *iovs = &g_io_ctx->iovs;

	virtqueue_req_start(vq, iovs, 2 + g_num_io % 2);
	virtqueue_req_add_iovs(vq, &iovs->iov_req, 1, SPDK_VIRTIO_DESC_RO);
	/* Having an IOV for data and not represent two distinct test cases. We should test both. */
	if (g_num_io % 2) {
		virtqueue_req_add_iovs(vq, &iovs->iov_data, 1, SPDK_VIRTIO_DESC_RO);
	}
	virtqueue_req_add_iovs(vq, &iovs->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
	virtqueue_req_flush(vq);
}

static int
submit_io(void)
{
	void *io = NULL;
	struct virtqueue *vq = g_virtio_dev->vqs[VIRTIO_REQUESTQ];
	uint64_t runtime_ticks;
	uint64_t timeout_ticks;
	uint64_t ticks_hz = spdk_get_ticks_hz();
	uint32_t len;

	if (!g_socket_is_blk && g_test_scsi_tmf) {
		vq = g_virtio_dev->vqs[VIRTIO_SCSI_CONTROLQ];
	} else {
		vq = g_virtio_dev->vqs[VIRTIO_REQUESTQ];
	}

	runtime_ticks = spdk_get_ticks() + g_runtime * ticks_hz;
	while (spdk_get_ticks() < runtime_ticks) {
		timeout_ticks = spdk_get_ticks() + IO_TIMEOUT_S * ticks_hz;
		craft_virtio_req_rsp_pair();
		submit_virtio_req_rsp_pair(vq);
		while (!virtio_recv_pkts(vq, &io, &len, 1)) {
			if (spdk_get_ticks() > timeout_ticks) {
				fprintf(stderr, "An I/O appears to have caused the target to hang.\n");
				print_req_obj();
				return 0;
			}
		}
		if ((g_socket_is_blk && g_io_ctx->resp.blk_resp == 0) ||
		    (!g_socket_is_blk && g_test_scsi_tmf && g_io_ctx->resp.scsi_tmf_resp.response == 0) ||
		    (!g_socket_is_blk && !g_test_scsi_tmf && g_io_ctx->resp.scsi_resp.status == 0)) {
			printf("An I/O completed without an error status. This could be worth looking into.\n");
			g_successful_io++;
			print_req_obj();
		}
		g_num_io++;
	}

	return 0;
}

static void
seed_rand(void)
{
	time_t seed_value;

	if (g_seed_value != 0) {
		seed_value = g_seed_value;
	} else {
		seed_value = time(0);
	}
	printf("Seed value for this run %lu\n", seed_value);
	srand(seed_value);
}

static void
vhost_fuzz_usage(void)
{

	fprintf(stderr,
		" -b            Open the provided socket path as a blk device (Default is scsi).\n");
	fprintf(stderr, " -h            Print this message.\n");
	fprintf(stderr,
		" -m            Test against scsi task management functions (not compatible with -b).\n");
	fprintf(stderr,
		" -r <integer>  Random seed value for test. If not provided, default to the current time.\n");
	fprintf(stderr,
		" -s <path>     The path to a vhost_user socket on which to create the device for fuzzingg.\n");
	fprintf(stderr, " -t <integer>  Time in seconds to run the fuzz test.\n");
}

static int
vhost_fuzz_parse(int argc, char **argv)
{
	int ch;
	int64_t error_test;

	while ((ch = getopt(argc, argv, "bhms:t:")) != -1) {
		switch (ch) {
		case 'b':
			g_socket_is_blk = true;
			break;
		case 'h':
			vhost_fuzz_usage();
			return -1;
		case 'm':
			g_test_scsi_tmf = true;
			break;
		case 'r':
			error_test = spdk_strtol(argv[optind], 10);
			if (error_test < 0) {
				fprintf(stderr, "Invalid value supplied for the random seed.\n");
				return -1;
			} else {
				g_seed_value = spdk_strtol(argv[optind], 10);
			}
			break;
		case 's':
			snprintf(g_socket_path, PATH_MAX, "%s", optarg);
			break;
		case 't':
			g_runtime = spdk_strtol(optarg, 10);
			if (g_runtime < 0 || g_runtime > MAX_RUNTIME_S) {
				fprintf(stderr, "You must supply a positive runtime value less than 86401\n");
				return -1;
			}
			break;
		case '?':
		default:
			vhost_fuzz_usage();
			return -1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts = {};
	int rc;

	spdk_env_opts_init(&opts);
	opts.name = "nvme_fuzz";
	opts.mem_size = 2048;
	g_runtime = DEFAULT_RUNTIME;

	seed_rand();

	if (vhost_fuzz_parse(argc, argv) < 0) {
		return -1;
	}

	rc = spdk_env_init(&opts);

	if (rc < 0) {
		fprintf(stderr, "Unable to initialize the SPDK environment\n");
		return rc;
	}

	g_io_ctx = spdk_malloc(sizeof(*g_io_ctx), 0x0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (g_io_ctx == NULL) {
		fprintf(stderr, "Failed to allocate global I/O context\n");
		return -ENOMEM;
	}

	g_virtio_dev = calloc(1, sizeof(*g_virtio_dev));
	if (g_virtio_dev == NULL) {
		fprintf(stderr, "failed to allocate global virtio bdev.\n");
		spdk_free(g_io_ctx);
		return -ENOMEM;
	}

	g_num_mem_regions = get_hugepage_file_info(g_huge_regions, VHOST_USER_MEMORY_MAX_NREGIONS);
	if (g_num_mem_regions < 0) {
		fprintf(stderr, "Failed to determine addresses for valid memory mapped regions.\n");
		spdk_free(g_io_ctx);
		return g_num_mem_regions;
	}

	if (g_socket_is_blk) {
		rc = blk_dev_init(g_virtio_dev, 3);
		if (rc != 0) {
			fprintf(stderr, "Failed to initialize virtual bdev\n");
			goto out;
		}
	} else {
		/* controlq, eventq, and requestq */
		rc = scsi_dev_init(g_virtio_dev, 3);
		if (rc != 0) {
			fprintf(stderr, "Failed to initialize scsi device\n");
			goto out;
		}
	}

	printf("properly configured queue and starting I/O\n");
	rc = submit_io();

	printf("Fuzz testing over. Completed %d I/O. Successful I/O %d\n", g_num_io, g_successful_io);

out:
	spdk_free(g_io_ctx);
	if (g_virtio_dev) {
		virtio_dev_stop(g_virtio_dev);
		virtio_dev_destruct(g_virtio_dev);
		free(g_virtio_dev);
	}
	return rc;
}
