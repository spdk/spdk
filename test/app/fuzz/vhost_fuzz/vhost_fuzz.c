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

/* Features desired/implemented by this driver. */
#define VIRTIO_BLK_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_BLK_F_BLK_SIZE		|	\
	 1ULL << VIRTIO_BLK_F_TOPOLOGY		|	\
	 1ULL << VIRTIO_BLK_F_MQ		|	\
	 1ULL << VIRTIO_BLK_F_RO		|	\
	 1ULL << VIRTIO_BLK_F_DISCARD		|	\
	 1ULL << VIRTIO_RING_F_EVENT_IDX	|	\
	 1ULL << VHOST_USER_F_PROTOCOL_FEATURES)

struct vhost_fuzz_blk_io_ctx {
	struct iovec				iov_req;
	struct iovec				iov_data;
	struct iovec				iov_resp;
	struct virtio_blk_outhdr		req;
	uint8_t					resp;
};

struct virtio_blk_dev {
	struct virtio_dev	vdev;
	uint64_t		num_blocks;
	uint32_t		block_size;
	int32_t			queue_idx;
	bool			readonly;
	bool			unmap;
};

struct hugepage_file_info {
	uint64_t addr;            /**< virtual addr */
	size_t   size;            /**< the file size */
	char     path[PATH_MAX];  /**< path to backing file */
};

struct hugepage_file_info	g_huge_regions[VHOST_USER_MEMORY_MAX_NREGIONS];
struct vhost_fuzz_blk_io_ctx	*g_io_ctx;
struct virtio_blk_dev		*g_blk_dev;
time_t				g_seed_value = 0;
int				g_num_mem_regions;
int				g_num_io = 0;
int				g_runtime;
char				g_socket_path[PATH_MAX + 1] = {0};
bool				g_socket_is_blk = false;

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

static int
blk_dev_init(struct virtio_blk_dev *dev, uint16_t max_queues)
{
	struct virtio_dev *vdev = &dev->vdev;
	uint64_t capacity, num_blocks;
	uint32_t block_size;
	uint16_t host_max_queues;
	int32_t queue_idx;
	int rc;

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_BLK_SIZE)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, blk_size),
						&block_size, sizeof(block_size));
		if (rc) {
			fprintf(stderr, "%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			return rc;
		}

		if (block_size == 0 || block_size % 512 != 0) {
			fprintf(stderr, "%s: invalid block size (%"PRIu32"). Must be "
				"a multiple of 512.\n", vdev->name, block_size);
			return -EIO;
		}
	} else {
		block_size = 512;
	}

	rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, capacity),
					&capacity, sizeof(capacity));
	if (rc) {
		fprintf(stderr, "%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
		return rc;
	}

	/* `capacity` is a number of 512-byte sectors. */
	num_blocks = capacity * 512 / block_size;
	if (num_blocks == 0) {
		fprintf(stderr, "%s: size too small (size: %"PRIu64", blocksize: %"PRIu32").\n",
			vdev->name, capacity * 512, block_size);
		return -EIO;
	}

	if ((capacity * 512) % block_size != 0) {
		fprintf(stderr, "%s: size has been rounded down to the nearest block size boundary. "
			"(block size: %"PRIu32", previous size: %"PRIu64", new size: %"PRIu64")\n",
			vdev->name, block_size, capacity * 512, num_blocks * block_size);
	}

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

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_RO)) {
		dev->readonly = true;
	}

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_DISCARD)) {
		dev->unmap = true;
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

	rc = virtio_dev_start(vdev, max_queues, 0);
	if (rc) {
		fprintf(stderr, "Failed to start virtio device %s.\n", vdev->name);
	}

	queue_idx = virtio_dev_find_and_acquire_queue(vdev, 0);
	if (queue_idx < 0) {
		fprintf(stderr, "Couldn't get an unused queue for the io_channel.\n");
		return -1;
	}

	dev->queue_idx = queue_idx;

	return 0;
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
	print_iov_obj("req", &g_io_ctx->iov_req);
	print_iov_obj("data", &g_io_ctx->iov_data);
	print_iov_obj("resp", &g_io_ctx->iov_resp);
	printf("req type: %u\n", g_io_ctx->req.type);
	printf("req ioprio: %u\n", g_io_ctx->req.ioprio);
	printf("req sector: %llu\n", g_io_ctx->req.sector);
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
craft_virtio_req_rsp_pair(void)
{
	struct virtio_blk_outhdr *req = &g_io_ctx->req;

	g_io_ctx->iov_req.iov_base = req;
	g_io_ctx->iov_req.iov_len = sizeof(*req);
	g_io_ctx->iov_data.iov_len = rand();
	g_io_ctx->iov_data.iov_base = get_invalid_mem_address(g_io_ctx->iov_data.iov_len);
	g_io_ctx->iov_resp.iov_base = &g_io_ctx->resp;
	g_io_ctx->iov_resp.iov_len = sizeof(g_io_ctx->resp);

	req->type = rand() % 8;
	req->sector = rand() & ~0x200ULL;
}

static void
submit_virtio_req_rsp_pair(void)
{
	struct virtqueue *vq = g_blk_dev->vdev.vqs[g_blk_dev->queue_idx];

	virtqueue_req_start(vq, g_io_ctx, 2);
	virtqueue_req_add_iovs(vq, &g_io_ctx->iov_req, 1, SPDK_VIRTIO_DESC_RO);
	/* Having an IOV for data and not represent two distinct test cases. We should test both. */
	if (g_num_io % 2) {
		virtqueue_req_add_iovs(vq, &g_io_ctx->iov_data, 1, SPDK_VIRTIO_DESC_RO);
	}
	virtqueue_req_add_iovs(vq, &g_io_ctx->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
	virtqueue_req_flush(vq);
}

static int
submit_io(struct virtio_blk_dev *g_blk_dev)
{
	void *io = NULL;
	struct virtqueue *vq = g_blk_dev->vdev.vqs[g_blk_dev->queue_idx];
	uint64_t runtime_ticks;
	uint64_t timeout_ticks;
	uint64_t ticks_hz = spdk_get_ticks_hz();
	uint32_t len;

	runtime_ticks = spdk_get_ticks() + g_runtime * ticks_hz;
	while (spdk_get_ticks() < runtime_ticks) {
		timeout_ticks = spdk_get_ticks() + IO_TIMEOUT_S * ticks_hz;
		craft_virtio_req_rsp_pair();
		submit_virtio_req_rsp_pair();
		while (!virtio_recv_pkts(vq, &io, &len, 1)) {
			if (spdk_get_ticks() > timeout_ticks) {
				fprintf(stderr, "An I/O appears to have caused the target to hang.\n");
				print_req_obj();
				return 0;
			}
		}
		if (g_io_ctx->resp == 0) {
			printf("An I/O completed without an error status. This could be worth looking into.\n");
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
		" -b                        open the provided socket path as a blk device (Default is scsi).\n");
	fprintf(stderr, " -h                        print this message.\n");
	fprintf(stderr,
		" -r <integer>              random seed value for test. If not provided, default to the current time.\n");
	fprintf(stderr,
		" -s <path>                 The path to a vhost_user socket on which to create the device for fuzzingg.\n");
	fprintf(stderr, " -t <integer>              time in seconds to run the fuzz test.\n");
}

static int
vhost_fuzz_parse(int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "bhs:t:")) != -1) {
		switch (ch) {
		case 'b':
			g_socket_is_blk = true;
			break;
		case 'h':
			vhost_fuzz_usage();
			return -1;
		case 'r':
			g_seed_value = spdk_strtoll(optarg, 10);
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

	if (g_socket_is_blk) {
		g_blk_dev = calloc(1, sizeof(struct virtio_blk_dev));
		if (g_blk_dev == NULL) {
			fprintf(stderr, "Failed to allocate device context.");
			return -ENOMEM;
		}

		rc = virtio_user_dev_init(&g_blk_dev->vdev, "fuzz_device", (const char *)&g_socket_path, 1024);
		if (rc) {
			fprintf(stderr, "Failed to initialize virtual bdev\n");
			goto out;
		}

		rc = virtio_dev_reset(&g_blk_dev->vdev, VIRTIO_BLK_DEV_SUPPORTED_FEATURES);
		if (rc != 0) {
			fprintf(stderr, "Failed to reset virtual bdev\n");
			goto out;
		}

		rc = blk_dev_init(g_blk_dev, 1);
		if (rc != 0) {
			fprintf(stderr, "Failed to initialize virtual bdev\n");
			goto out;
		}

		g_io_ctx = spdk_malloc(sizeof(*g_io_ctx), 0x0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
		if (g_io_ctx == NULL) {
			rc = -ENOMEM;
			virtio_dev_stop(&g_blk_dev->vdev);
			fprintf(stderr, "Failed to allocate global I/O context\n");
			goto out;
		}

		g_num_mem_regions = get_hugepage_file_info(g_huge_regions, VHOST_USER_MEMORY_MAX_NREGIONS);
		if (g_num_mem_regions < 0) {
			rc = g_num_mem_regions;
			virtio_dev_stop(&g_blk_dev->vdev);
			fprintf(stderr, "Failed to determine addresses for valid memory mapped regions.\n");
			goto out;
		}

		printf("properly configured queue and starting I/O\n");
		rc = submit_io(g_blk_dev);
	} else {
		fprintf(stderr, "Not Implemented\n");
		return -1;
	}

	printf("Fuzz testing over. Completed %d I/O\n", g_num_io);
	virtio_dev_stop(&g_blk_dev->vdev);

out:
	virtio_dev_destruct(&g_blk_dev->vdev);
	free(g_blk_dev);
	return rc;
}
