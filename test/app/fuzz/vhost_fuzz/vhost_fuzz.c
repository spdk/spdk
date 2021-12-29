/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

#include "fuzz_common.h"
#include "vhost_fuzz.h"

#include <linux/virtio_blk.h>
#include <linux/virtio_scsi.h>

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
#define FUZZ_MAX_QUEUES		3

#define FUZZ_QUEUE_DEPTH	128

#define BLK_IO_NAME		"vhost_blk_cmd"
#define SCSI_IO_NAME		"vhost_scsi_cmd"
#define SCSI_MGMT_NAME		"vhost_scsi_mgmt_cmd"

struct fuzz_vhost_iov_ctx {
	struct iovec			iov_req;
	struct iovec			iov_data;
	struct iovec			iov_resp;
};

struct fuzz_vhost_io_ctx {
	struct fuzz_vhost_iov_ctx		iovs;
	union {
		struct virtio_blk_outhdr	blk_req;
		struct virtio_scsi_cmd_req	scsi_req;
		struct virtio_scsi_ctrl_tmf_req	scsi_tmf_req;
	} req;
	union {
		uint8_t					blk_resp;
		struct virtio_scsi_cmd_resp		scsi_resp;
		union {
			struct virtio_scsi_ctrl_tmf_resp	scsi_tmf_resp;
			struct virtio_scsi_ctrl_an_resp		an_resp;
		} scsi_tmf_resp;
	} resp;

	TAILQ_ENTRY(fuzz_vhost_io_ctx) link;
};

struct fuzz_vhost_dev_ctx {
	struct virtio_dev			virtio_dev;
	struct spdk_thread			*thread;
	struct spdk_poller			*poller;

	struct fuzz_vhost_io_ctx		*io_ctx_array;
	TAILQ_HEAD(, fuzz_vhost_io_ctx)		free_io_ctx;
	TAILQ_HEAD(, fuzz_vhost_io_ctx)		outstanding_io_ctx;

	unsigned int				random_seed;

	uint64_t				submitted_io;
	uint64_t				completed_io;
	uint64_t				successful_io;
	uint64_t				timeout_tsc;

	bool					socket_is_blk;
	bool					test_scsi_tmf;
	bool					valid_lun;
	bool					use_bogus_buffer;
	bool					use_valid_buffer;
	bool					timed_out;

	TAILQ_ENTRY(fuzz_vhost_dev_ctx)	link;
};

/* Global run state */
uint64_t				g_runtime_ticks;
int					g_runtime;
int					g_num_active_threads;
bool					g_run = true;
bool					g_verbose_mode = false;

/* Global resources */
TAILQ_HEAD(, fuzz_vhost_dev_ctx)	g_dev_list = TAILQ_HEAD_INITIALIZER(g_dev_list);
struct spdk_poller			*g_run_poller;
void					*g_valid_buffer;
unsigned int				g_random_seed;


/* Global parameters and resources for parsed commands */
bool					g_keep_iov_pointers = false;
char					*g_json_file = NULL;
struct fuzz_vhost_io_ctx		*g_blk_cmd_array = NULL;
struct fuzz_vhost_io_ctx		*g_scsi_cmd_array = NULL;
struct fuzz_vhost_io_ctx		*g_scsi_mgmt_cmd_array = NULL;

size_t					g_blk_cmd_array_size;
size_t					g_scsi_cmd_array_size;
size_t					g_scsi_mgmt_cmd_array_size;

static void
cleanup(void)
{
	struct fuzz_vhost_dev_ctx *dev_ctx, *tmp;
	printf("Fuzzing completed.\n");
	TAILQ_FOREACH_SAFE(dev_ctx, &g_dev_list, link, tmp) {
		printf("device %p stats: Completed I/O: %lu, Successful I/O: %lu\n", dev_ctx,
		       dev_ctx->completed_io, dev_ctx->successful_io);
		virtio_dev_release_queue(&dev_ctx->virtio_dev, VIRTIO_REQUESTQ);
		if (!dev_ctx->socket_is_blk) {
			virtio_dev_release_queue(&dev_ctx->virtio_dev, VIRTIO_SCSI_EVENTQ);
			virtio_dev_release_queue(&dev_ctx->virtio_dev, VIRTIO_SCSI_CONTROLQ);
		}
		virtio_dev_stop(&dev_ctx->virtio_dev);
		virtio_dev_destruct(&dev_ctx->virtio_dev);
		if (dev_ctx->io_ctx_array) {
			spdk_free(dev_ctx->io_ctx_array);
		}
		free(dev_ctx);
	}

	spdk_free(g_valid_buffer);

	if (g_blk_cmd_array) {
		free(g_blk_cmd_array);
	}
	if (g_scsi_cmd_array) {
		free(g_scsi_cmd_array);
	}
	if (g_scsi_mgmt_cmd_array) {
		free(g_scsi_mgmt_cmd_array);
	}
}

/* Get a memory address that is random and not located in our hugepage memory. */
static void *
get_invalid_mem_address(uint64_t length)
{
	uint64_t chosen_address = 0x0;

	while (true) {
		chosen_address = rand();
		chosen_address = (chosen_address << 32) | rand();
		if (spdk_vtophys((void *)chosen_address, &length) == SPDK_VTOPHYS_ERROR) {
			return (void *)chosen_address;
		}
	}
	return NULL;
}

/* dev initialization code begin. */
static int
virtio_dev_init(struct virtio_dev *vdev, const char *socket_path, uint64_t flags,
		uint16_t max_queues)
{
	int rc;

	rc = virtio_user_dev_init(vdev, "dev_ctx", socket_path, 1024);
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
		return rc;
	}
	return 0;
}

static int
blk_dev_init(struct virtio_dev *vdev, const char *socket_path, uint16_t max_queues)
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

	return virtio_dev_init(vdev, socket_path, VIRTIO_BLK_DEV_SUPPORTED_FEATURES, max_queues);
}

static int
scsi_dev_init(struct virtio_dev *vdev, const char *socket_path, uint16_t max_queues)
{
	int rc;

	rc = virtio_dev_init(vdev, socket_path, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES, max_queues);
	if (rc != 0) {
		return rc;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_CONTROLQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the controlq.\n");
		return rc;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_EVENTQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the eventq.\n");
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_CONTROLQ);
		return rc;
	}

	return 0;
}

int
fuzz_vhost_dev_init(const char *socket_path, bool is_blk_dev, bool use_bogus_buffer,
		    bool use_valid_buffer, bool valid_lun, bool test_scsi_tmf)
{
	struct fuzz_vhost_dev_ctx *dev_ctx;
	int rc = 0, i;

	dev_ctx = calloc(1, sizeof(*dev_ctx));
	if (dev_ctx == NULL) {
		return -ENOMEM;
	}

	dev_ctx->socket_is_blk = is_blk_dev;
	dev_ctx->use_bogus_buffer = use_bogus_buffer;
	dev_ctx->use_valid_buffer = use_valid_buffer;
	dev_ctx->valid_lun = valid_lun;
	dev_ctx->test_scsi_tmf = test_scsi_tmf;

	TAILQ_INIT(&dev_ctx->free_io_ctx);
	TAILQ_INIT(&dev_ctx->outstanding_io_ctx);

	assert(sizeof(*dev_ctx->io_ctx_array) <= UINT64_MAX / FUZZ_QUEUE_DEPTH);
	dev_ctx->io_ctx_array = spdk_malloc(sizeof(*dev_ctx->io_ctx_array) * FUZZ_QUEUE_DEPTH, 0x0, NULL,
					    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (dev_ctx->io_ctx_array == NULL) {
		free(dev_ctx);
		return -ENOMEM;
	}

	for (i = 0; i < FUZZ_QUEUE_DEPTH; i++) {
		TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, &dev_ctx->io_ctx_array[i], link);
	}

	dev_ctx->thread = spdk_thread_create(NULL, NULL);
	if (dev_ctx->thread == NULL) {
		fprintf(stderr, "Unable to allocate a thread for a fuzz device.\n");
		rc = -ENOMEM;
		goto error_out;
	}

	if (is_blk_dev) {
		rc = blk_dev_init(&dev_ctx->virtio_dev, socket_path, FUZZ_MAX_QUEUES);
	} else {
		rc = scsi_dev_init(&dev_ctx->virtio_dev, socket_path, FUZZ_MAX_QUEUES);
	}

	if (rc) {
		fprintf(stderr, "Unable to prepare the device to perform I/O.\n");
		goto error_out;
	}

	TAILQ_INSERT_TAIL(&g_dev_list, dev_ctx, link);
	return 0;

error_out:
	spdk_free(dev_ctx->io_ctx_array);
	free(dev_ctx);
	return rc;
}
/* dev initialization code end */

/* data dumping functions begin */
static int
dump_virtio_cmd(void *ctx, const void *data, size_t size)
{
	fprintf(stderr, "%s\n", (const char *)data);
	return 0;
}

static void
print_blk_io_data(struct spdk_json_write_ctx *w, struct fuzz_vhost_io_ctx *io_ctx)
{
	spdk_json_write_named_uint32(w, "type", io_ctx->req.blk_req.type);
	spdk_json_write_named_uint32(w, "ioprio", io_ctx->req.blk_req.ioprio);
	spdk_json_write_named_uint64(w, "sector", io_ctx->req.blk_req.sector);
}

static void
print_scsi_tmf_io_data(struct spdk_json_write_ctx *w, struct fuzz_vhost_io_ctx *io_ctx)
{
	char *lun_data;

	lun_data = fuzz_get_value_base_64_buffer(io_ctx->req.scsi_tmf_req.lun,
			sizeof(io_ctx->req.scsi_tmf_req.lun));

	spdk_json_write_named_uint32(w, "type", io_ctx->req.scsi_tmf_req.type);
	spdk_json_write_named_uint32(w, "subtype", io_ctx->req.scsi_tmf_req.subtype);
	spdk_json_write_named_string(w, "lun", lun_data);
	spdk_json_write_named_uint64(w, "tag", io_ctx->req.scsi_tmf_req.tag);

	free(lun_data);
}

static void
print_scsi_io_data(struct spdk_json_write_ctx *w, struct fuzz_vhost_io_ctx *io_ctx)
{
	char *lun_data;
	char *cdb_data;

	lun_data = fuzz_get_value_base_64_buffer(io_ctx->req.scsi_req.lun,
			sizeof(io_ctx->req.scsi_req.lun));
	cdb_data = fuzz_get_value_base_64_buffer(io_ctx->req.scsi_req.cdb,
			sizeof(io_ctx->req.scsi_req.cdb));

	spdk_json_write_named_string(w, "lun", lun_data);
	spdk_json_write_named_uint64(w, "tag", io_ctx->req.scsi_req.tag);
	spdk_json_write_named_uint32(w, "task_attr", io_ctx->req.scsi_req.task_attr);
	spdk_json_write_named_uint32(w, "prio", io_ctx->req.scsi_req.prio);
	spdk_json_write_named_uint32(w, "crn", io_ctx->req.scsi_req.crn);
	spdk_json_write_named_string(w, "cdb", cdb_data);

	free(lun_data);
	free(cdb_data);
}

static void
print_iov_obj(struct spdk_json_write_ctx *w, const char *iov_name, struct iovec *iov)
{
	/* "0x" + up to 16 digits + null terminator */
	char hex_addr[19];
	int rc;

	rc = snprintf(hex_addr, 19, "%lx", (uintptr_t)iov->iov_base);

	/* default to 0. */
	if (rc < 0 || rc >= 19) {
		hex_addr[0] = '0';
		hex_addr[1] = '\0';
	}

	spdk_json_write_named_object_begin(w, iov_name);
	spdk_json_write_named_string(w, "iov_base", hex_addr);
	spdk_json_write_named_uint64(w, "iov_len", iov->iov_len);
	spdk_json_write_object_end(w);
}

static void
print_iovs(struct spdk_json_write_ctx *w, struct fuzz_vhost_io_ctx *io_ctx)
{
	print_iov_obj(w, "req_iov", &io_ctx->iovs.iov_req);
	print_iov_obj(w, "data_iov", &io_ctx->iovs.iov_data);
	print_iov_obj(w, "resp_iov", &io_ctx->iovs.iov_resp);
}

static void
print_req_obj(struct fuzz_vhost_dev_ctx *dev_ctx, struct fuzz_vhost_io_ctx *io_ctx)
{

	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(dump_virtio_cmd, NULL, SPDK_JSON_WRITE_FLAG_FORMATTED);

	if (dev_ctx->socket_is_blk) {
		spdk_json_write_named_object_begin(w, BLK_IO_NAME);
		print_iovs(w, io_ctx);
		print_blk_io_data(w, io_ctx);
	} else if (dev_ctx->test_scsi_tmf) {
		spdk_json_write_named_object_begin(w, SCSI_MGMT_NAME);
		print_iovs(w, io_ctx);
		print_scsi_tmf_io_data(w, io_ctx);
	} else {
		spdk_json_write_named_object_begin(w, SCSI_IO_NAME);
		print_iovs(w, io_ctx);
		print_scsi_io_data(w, io_ctx);
	}
	spdk_json_write_object_end(w);
	spdk_json_write_end(w);
}

static void
dump_outstanding_io(struct fuzz_vhost_dev_ctx *dev_ctx)
{
	struct fuzz_vhost_io_ctx *io_ctx, *tmp;

	TAILQ_FOREACH_SAFE(io_ctx, &dev_ctx->outstanding_io_ctx, link, tmp) {
		print_req_obj(dev_ctx, io_ctx);
		TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
		TAILQ_INSERT_TAIL(&dev_ctx->free_io_ctx, io_ctx, link);
	}
}
/* data dumping functions end */

/* data parsing functions begin */
static int
hex_value(uint8_t c)
{
#define V(x, y) [x] = y + 1
	static const int8_t val[256] = {
		V('0', 0), V('1', 1), V('2', 2), V('3', 3), V('4', 4),
		V('5', 5), V('6', 6), V('7', 7), V('8', 8), V('9', 9),
		V('A', 0xA), V('B', 0xB), V('C', 0xC), V('D', 0xD), V('E', 0xE), V('F', 0xF),
		V('a', 0xA), V('b', 0xB), V('c', 0xC), V('d', 0xD), V('e', 0xE), V('f', 0xF),
	};
#undef V

	return val[c] - 1;
}

static int
fuzz_json_decode_hex_uint64(const struct spdk_json_val *val, void *out)
{
	uint64_t *out_val = out;
	size_t i;
	char *val_pointer = val->start;
	int current_val;

	if (val->len > 16) {
		return -EINVAL;
	}

	*out_val = 0;
	for (i = 0; i < val->len; i++) {
		*out_val = *out_val << 4;
		current_val = hex_value(*val_pointer);
		if (current_val < 0) {
			return -EINVAL;
		}
		*out_val += current_val;
		val_pointer++;
	}

	return 0;
}

static const struct spdk_json_object_decoder fuzz_vhost_iov_decoders[] = {
	{"iov_base", offsetof(struct iovec, iov_base), fuzz_json_decode_hex_uint64},
	{"iov_len", offsetof(struct iovec, iov_len), spdk_json_decode_uint64},
};

static size_t
parse_iov_struct(struct iovec *iovec, struct spdk_json_val *value)
{
	int rc;

	if (value->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -1;
	}

	rc = spdk_json_decode_object(value,
				     fuzz_vhost_iov_decoders,
				     SPDK_COUNTOF(fuzz_vhost_iov_decoders),
				     iovec);
	if (rc) {
		return -1;
	}

	while (value->type != SPDK_JSON_VAL_OBJECT_END) {
		value++;
		rc++;
	}

	/* The +1 instructs the calling function to skip over the OBJECT_END function. */
	rc += 1;
	return rc;
}

static bool
parse_vhost_blk_cmds(void *item, struct spdk_json_val *value, size_t num_values)
{
	struct fuzz_vhost_io_ctx *io_ctx = item;
	struct spdk_json_val *prev_value;
	int nested_object_size;
	uint64_t tmp_val;
	size_t i = 0;

	while (i < num_values) {
		nested_object_size = 1;
		if (value->type == SPDK_JSON_VAL_NAME) {
			prev_value = value;
			value++;
			i++;
			if (!strncmp(prev_value->start, "req_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_req, value);
			} else if (!strncmp(prev_value->start, "data_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "resp_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "type", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT32_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.blk_req.type = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "ioprio", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT32_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.blk_req.ioprio = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "sector", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT64_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.blk_req.sector = tmp_val;
				}
			}
		}
		if (nested_object_size < 0) {
			fprintf(stderr, "Invalid value supplied for io_ctx->%.*s: %.*s\n", prev_value->len,
				(char *)prev_value->start, value->len, (char *)value->start);
			return false;
		}
		value += nested_object_size;
		i += nested_object_size;
	}
	return true;
}

static bool
parse_vhost_scsi_cmds(void *item, struct spdk_json_val *value, size_t num_values)
{
	struct fuzz_vhost_io_ctx *io_ctx = item;
	struct spdk_json_val *prev_value;
	int nested_object_size;
	uint64_t tmp_val;
	size_t i = 0;

	while (i < num_values) {
		nested_object_size = 1;
		if (value->type == SPDK_JSON_VAL_NAME) {
			prev_value = value;
			value++;
			i++;
			if (!strncmp(prev_value->start, "req_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_req, value);
			} else if (!strncmp(prev_value->start, "data_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "resp_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "lun", prev_value->len)) {
				if (fuzz_get_base_64_buffer_value(&io_ctx->req.scsi_req.lun,
								  sizeof(io_ctx->req.scsi_req.lun),
								  (char *)value->start,
								  value->len)) {
					nested_object_size = -1;
				}
			} else if (!strncmp(prev_value->start, "tag", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT64_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_req.tag = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "task_attr", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT8_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_req.task_attr = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "prio", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT8_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_req.prio = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "crn", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT8_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_req.crn = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "cdb", prev_value->len)) {
				if (fuzz_get_base_64_buffer_value(&io_ctx->req.scsi_req.cdb,
								  sizeof(io_ctx->req.scsi_req.cdb),
								  (char *)value->start,
								  value->len)) {
					nested_object_size = -1;
				}
			}
		}
		if (nested_object_size < 0) {
			fprintf(stderr, "Invalid value supplied for io_ctx->%.*s: %.*s\n", prev_value->len,
				(char *)prev_value->start, value->len, (char *)value->start);
			return false;
		}
		value += nested_object_size;
		i += nested_object_size;
	}
	return true;

}

static bool
parse_vhost_scsi_mgmt_cmds(void *item, struct spdk_json_val *value, size_t num_values)
{
	struct fuzz_vhost_io_ctx *io_ctx = item;
	struct spdk_json_val *prev_value;
	int nested_object_size;
	uint64_t tmp_val;
	size_t i = 0;

	while (i < num_values) {
		nested_object_size = 1;
		if (value->type == SPDK_JSON_VAL_NAME) {
			prev_value = value;
			value++;
			i++;
			if (!strncmp(prev_value->start, "req_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_req, value);
			} else if (!strncmp(prev_value->start, "data_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "resp_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "type", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT32_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_tmf_req.type = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "subtype", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT32_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_tmf_req.subtype = tmp_val;
				}
			}  else if (!strncmp(prev_value->start, "lun", prev_value->len)) {
				if (fuzz_get_base_64_buffer_value(&io_ctx->req.scsi_tmf_req.lun,
								  sizeof(io_ctx->req.scsi_tmf_req.lun),
								  (char *)value->start,
								  value->len)) {
					nested_object_size = -1;
				}
			} else if (!strncmp(prev_value->start, "tag", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT64_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_tmf_req.tag = tmp_val;
				}
			}
		}
		if (nested_object_size < 0) {
			fprintf(stderr, "Invalid value supplied for io_ctx->%.*s: %.*s\n", prev_value->len,
				(char *)prev_value->start, value->len, (char *)value->start);
			return false;
		}
		value += nested_object_size;
		i += nested_object_size;
	}
	return true;
}
/* data parsing functions end */

/* build requests begin */
static void
craft_io_from_array(struct fuzz_vhost_io_ctx *src_ctx, struct fuzz_vhost_io_ctx *dest_ctx)
{
	if (g_keep_iov_pointers) {
		dest_ctx->iovs = src_ctx->iovs;
	}
	dest_ctx->req = src_ctx->req;
}

static void
craft_virtio_scsi_req(struct fuzz_vhost_dev_ctx *dev_ctx, struct fuzz_vhost_io_ctx *io_ctx)
{
	io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.scsi_req);
	io_ctx->iovs.iov_resp.iov_len = sizeof(io_ctx->resp.scsi_resp);
	fuzz_fill_random_bytes((char *)&io_ctx->req.scsi_req, sizeof(io_ctx->req.scsi_req),
			       &dev_ctx->random_seed);
	/* TODO: set up the logic to find all luns on the target. Right now we are just assuming the first is OK. */
	if (dev_ctx->valid_lun) {
		io_ctx->req.scsi_req.lun[0] = 1;
		io_ctx->req.scsi_req.lun[1] = 0;
	}
}

static void
craft_virtio_scsi_tmf_req(struct fuzz_vhost_dev_ctx *dev_ctx, struct fuzz_vhost_io_ctx *io_ctx)
{
	io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.scsi_tmf_req);
	io_ctx->iovs.iov_resp.iov_len = sizeof(io_ctx->resp.scsi_tmf_resp);
	fuzz_fill_random_bytes((char *)&io_ctx->req.scsi_tmf_req, sizeof(io_ctx->req.scsi_tmf_req),
			       &dev_ctx->random_seed);
	/* TODO: set up the logic to find all luns on the target. Right now we are just assuming the first is OK. */
	if (dev_ctx->valid_lun) {
		io_ctx->req.scsi_tmf_req.lun[0] = 1;
		io_ctx->req.scsi_tmf_req.lun[1] = 0;
	}

	/* Valid controlq commands have to be of type 0, 1, or 2. Any others just return immediately from the target. */
	/* Try to only test the opcodes that will exercise extra paths in the target side. But allow for at least one invalid value. */
	io_ctx->req.scsi_tmf_req.type = rand() % 4;
}

static void
craft_virtio_blk_req(struct fuzz_vhost_io_ctx *io_ctx)
{
	io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.blk_req);
	io_ctx->iovs.iov_resp.iov_len = sizeof(io_ctx->resp.blk_resp);
	io_ctx->req.blk_req.type = rand();
	io_ctx->req.blk_req.sector = rand();
}

static void
craft_virtio_req_rsp_pair(struct fuzz_vhost_dev_ctx *dev_ctx, struct fuzz_vhost_io_ctx *io_ctx)
{
	struct fuzz_vhost_iov_ctx *iovs = &io_ctx->iovs;

	/*
	 * Always set these buffer values up front.
	 * If the user wants to override this with the json values,
	 * they can specify -k when starting the app. */
	iovs->iov_req.iov_base = &io_ctx->req;
	if (dev_ctx->use_bogus_buffer) {
		iovs->iov_data.iov_len = rand();
		iovs->iov_data.iov_base = get_invalid_mem_address(iovs->iov_data.iov_len);
	} else if (dev_ctx->use_valid_buffer) {
		iovs->iov_data.iov_len = 1024;
		iovs->iov_data.iov_base = g_valid_buffer;
	}
	iovs->iov_resp.iov_base = &io_ctx->resp;

	if (dev_ctx->socket_is_blk && g_blk_cmd_array) {
		craft_io_from_array(&g_blk_cmd_array[dev_ctx->submitted_io], io_ctx);
		return;
	} else if (dev_ctx->test_scsi_tmf && g_scsi_mgmt_cmd_array) {
		craft_io_from_array(&g_scsi_mgmt_cmd_array[dev_ctx->submitted_io], io_ctx);
		return;
	} else if (g_scsi_cmd_array) {
		craft_io_from_array(&g_scsi_cmd_array[dev_ctx->submitted_io], io_ctx);
		return;
	}

	if (dev_ctx->socket_is_blk) {
		craft_virtio_blk_req(io_ctx);
	} else if (dev_ctx->test_scsi_tmf) {
		craft_virtio_scsi_tmf_req(dev_ctx, io_ctx);
	} else {
		craft_virtio_scsi_req(dev_ctx, io_ctx);
	}
}
/* build requests end */

/* submit requests begin */
static uint64_t
get_max_num_io(struct fuzz_vhost_dev_ctx *dev_ctx)
{
	if (dev_ctx->socket_is_blk) {
		return g_blk_cmd_array_size;
	} else if (dev_ctx->test_scsi_tmf) {
		return g_scsi_mgmt_cmd_array_size;
	} else {
		return g_scsi_cmd_array_size;
	}
}

static int
submit_virtio_req_rsp_pair(struct fuzz_vhost_dev_ctx *dev_ctx, struct virtqueue *vq,
			   struct fuzz_vhost_io_ctx *io_ctx)
{
	struct fuzz_vhost_iov_ctx *iovs = &io_ctx->iovs;
	int num_iovs = 2, rc;

	num_iovs += dev_ctx->use_bogus_buffer || dev_ctx->use_valid_buffer ? 1 : 0;

	rc = virtqueue_req_start(vq, io_ctx, num_iovs);
	if (rc) {
		return rc;
	}
	virtqueue_req_add_iovs(vq, &iovs->iov_req, 1, SPDK_VIRTIO_DESC_RO);
	/* blk and scsi requests favor different orders for the iov objects. */
	if (dev_ctx->socket_is_blk) {
		if (dev_ctx->use_bogus_buffer || dev_ctx->use_valid_buffer) {
			virtqueue_req_add_iovs(vq, &iovs->iov_data, 1, SPDK_VIRTIO_DESC_WR);
		}
		virtqueue_req_add_iovs(vq, &iovs->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
	} else {
		virtqueue_req_add_iovs(vq, &iovs->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
		if (dev_ctx->use_bogus_buffer || dev_ctx->use_valid_buffer) {
			virtqueue_req_add_iovs(vq, &iovs->iov_data, 1, SPDK_VIRTIO_DESC_WR);
		}
	}
	virtqueue_req_flush(vq);
	return 0;
}

static void
dev_submit_requests(struct fuzz_vhost_dev_ctx *dev_ctx, struct virtqueue *vq,
		    uint64_t max_io_to_submit)
{
	struct fuzz_vhost_io_ctx *io_ctx;
	int rc;

	while (!TAILQ_EMPTY(&dev_ctx->free_io_ctx) && dev_ctx->submitted_io < max_io_to_submit) {
		io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);
		craft_virtio_req_rsp_pair(dev_ctx, io_ctx);
		rc = submit_virtio_req_rsp_pair(dev_ctx, vq, io_ctx);
		if (rc == 0) {
			TAILQ_REMOVE(&dev_ctx->free_io_ctx, io_ctx, link);
			TAILQ_INSERT_TAIL(&dev_ctx->outstanding_io_ctx, io_ctx, link);
			dev_ctx->submitted_io++;
		} else if (rc == -ENOMEM) {
			/* There are just not enough available buffers right now. try later. */
			return;
		} else if (rc == -EINVAL) {
			/* The virtqueue must be broken. We know we can fit at least three descriptors */
			fprintf(stderr, "One of the virtqueues for dev %p is broken. stopping all devices.\n", dev_ctx);
			g_run = 0;
		}
	}
}
/* submit requests end */

/* complete requests begin */
static void
check_successful_op(struct fuzz_vhost_dev_ctx *dev_ctx, struct fuzz_vhost_io_ctx *io_ctx)
{
	bool is_successful = false;

	if (dev_ctx->socket_is_blk) {
		if (io_ctx->resp.blk_resp == 0) {
			is_successful = true;
		}
	} else if (dev_ctx->test_scsi_tmf) {
		if (io_ctx->resp.scsi_tmf_resp.scsi_tmf_resp.response == 0 &&
		    io_ctx->resp.scsi_tmf_resp.an_resp.response == 0) {
			is_successful = true;
		}
	} else {
		if (io_ctx->resp.scsi_resp.status == 0) {
			is_successful = true;
		}
	}

	if (is_successful) {
		fprintf(stderr, "An I/O completed without an error status. This could be worth looking into.\n");
		fprintf(stderr,
			"There is also a good chance that the target just failed before setting a status.\n");
		dev_ctx->successful_io++;
		print_req_obj(dev_ctx, io_ctx);
	} else if (g_verbose_mode) {
		fprintf(stderr, "The following I/O failed as expected.\n");
		print_req_obj(dev_ctx, io_ctx);
	}
}

static void
complete_io(struct fuzz_vhost_dev_ctx *dev_ctx, struct fuzz_vhost_io_ctx *io_ctx)
{
	TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
	TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, io_ctx, link);
	check_successful_op(dev_ctx, io_ctx);
	dev_ctx->completed_io++;
	dev_ctx->timeout_tsc = fuzz_refresh_timeout();
}

static int
poll_dev(void *ctx)
{
	struct fuzz_vhost_dev_ctx *dev_ctx = ctx;
	struct virtqueue *vq;
	struct fuzz_vhost_io_ctx *io_ctx[FUZZ_QUEUE_DEPTH];
	int num_active_threads;
	uint64_t max_io_to_complete = UINT64_MAX;
	uint64_t current_ticks;
	uint32_t len[FUZZ_QUEUE_DEPTH];
	uint16_t num_cpl, i;

	if (g_json_file) {
		max_io_to_complete = get_max_num_io(dev_ctx);
	}

	if (!dev_ctx->socket_is_blk && dev_ctx->test_scsi_tmf) {
		vq = dev_ctx->virtio_dev.vqs[VIRTIO_SCSI_CONTROLQ];
	} else {
		vq = dev_ctx->virtio_dev.vqs[VIRTIO_REQUESTQ];
	}

	num_cpl = virtio_recv_pkts(vq, (void **)io_ctx, len, FUZZ_QUEUE_DEPTH);

	for (i = 0; i < num_cpl; i++) {
		complete_io(dev_ctx, io_ctx[i]);
	}

	current_ticks = spdk_get_ticks();

	if (current_ticks > dev_ctx->timeout_tsc) {
		dev_ctx->timed_out = true;
		g_run = false;
		fprintf(stderr, "The VQ on device %p timed out. Dumping contents now.\n", dev_ctx);
		dump_outstanding_io(dev_ctx);
	}

	if (current_ticks > g_runtime_ticks) {
		g_run = 0;
	}

	if (!g_run || dev_ctx->completed_io >= max_io_to_complete) {
		if (TAILQ_EMPTY(&dev_ctx->outstanding_io_ctx)) {
			spdk_poller_unregister(&dev_ctx->poller);
			num_active_threads = __sync_sub_and_fetch(&g_num_active_threads, 1);
			if (num_active_threads == 0) {
				g_run = 0;
			}
			spdk_thread_exit(dev_ctx->thread);
		}
		return 0;
	}

	dev_submit_requests(dev_ctx, vq, max_io_to_complete);
	return 0;
}
/* complete requests end */

static void
start_io(void *ctx)
{
	struct fuzz_vhost_dev_ctx *dev_ctx = ctx;

	if (g_random_seed) {
		dev_ctx->random_seed = g_random_seed;
	} else {
		dev_ctx->random_seed = spdk_get_ticks();
	}

	dev_ctx->timeout_tsc = fuzz_refresh_timeout();

	dev_ctx->poller = SPDK_POLLER_REGISTER(poll_dev, dev_ctx, 0);
	if (dev_ctx->poller == NULL) {
		return;
	}

}

static int
end_fuzz(void *ctx)
{
	if (!g_run && !g_num_active_threads) {
		spdk_poller_unregister(&g_run_poller);
		cleanup();
		spdk_app_stop(0);
	}
	return 0;
}

static void
begin_fuzz(void *ctx)
{
	struct fuzz_vhost_dev_ctx *dev_ctx;

	g_runtime_ticks = spdk_get_ticks() + spdk_get_ticks_hz() * g_runtime;

	g_valid_buffer = spdk_malloc(0x1000, 0x200, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (g_valid_buffer == NULL) {
		fprintf(stderr, "Failed to allocate a valid buffer for I/O\n");
		goto out;
	}

	g_run_poller = SPDK_POLLER_REGISTER(end_fuzz, NULL, 0);
	if (g_run_poller == NULL) {
		fprintf(stderr, "Failed to register a poller for test completion checking.\n");
	}

	TAILQ_FOREACH(dev_ctx, &g_dev_list, link) {
		assert(dev_ctx->thread != NULL);
		spdk_thread_send_msg(dev_ctx->thread, start_io, dev_ctx);
		__sync_add_and_fetch(&g_num_active_threads, 1);
	}

	return;
out:
	cleanup();
	spdk_app_stop(0);
}

static void
fuzz_vhost_usage(void)
{
	fprintf(stderr, " -j <path>                 Path to a json file containing named objects.\n");
	fprintf(stderr,
		" -k                        Keep the iov pointer addresses from the json file. only valid with -j.\n");
	fprintf(stderr, " -S <integer>              Seed value for test.\n");
	fprintf(stderr, " -t <integer>              Time in seconds to run the fuzz test.\n");
	fprintf(stderr, " -V                        Enable logging of each submitted command.\n");
}

static int
fuzz_vhost_parse(int ch, char *arg)
{
	int64_t error_test;

	switch (ch) {
	case 'j':
		g_json_file = optarg;
		break;
	case 'k':
		g_keep_iov_pointers = true;
		break;
	case 'S':
		error_test = spdk_strtol(arg, 10);
		if (error_test < 0) {
			fprintf(stderr, "Invalid value supplied for the random seed.\n");
			return -1;
		} else {
			g_random_seed = spdk_strtol(arg, 10);
		}
		break;
	case 't':
		g_runtime = spdk_strtol(arg, 10);
		if (g_runtime < 0 || g_runtime > MAX_RUNTIME_S) {
			fprintf(stderr, "You must supply a positive runtime value less than 86401.\n");
			return -1;
		}
		break;
	case 'V':
		g_verbose_mode = true;
		break;
	case '?':
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "vhost_fuzz";
	g_runtime = DEFAULT_RUNTIME;

	rc = spdk_app_parse_args(argc, argv, &opts, "j:kS:t:V", NULL, fuzz_vhost_parse, fuzz_vhost_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		fprintf(stderr, "Unable to parse the application arguments.\n");
		return -1;
	}

	if (g_json_file != NULL) {
		g_blk_cmd_array_size = fuzz_parse_args_into_array(g_json_file,
				       (void **)&g_blk_cmd_array,
				       sizeof(struct fuzz_vhost_io_ctx),
				       BLK_IO_NAME, parse_vhost_blk_cmds);
		g_scsi_cmd_array_size = fuzz_parse_args_into_array(g_json_file,
					(void **)&g_scsi_cmd_array,
					sizeof(struct fuzz_vhost_io_ctx),
					SCSI_IO_NAME, parse_vhost_scsi_cmds);
		g_scsi_mgmt_cmd_array_size = fuzz_parse_args_into_array(g_json_file,
					     (void **)&g_scsi_mgmt_cmd_array,
					     sizeof(struct fuzz_vhost_io_ctx),
					     SCSI_IO_NAME, parse_vhost_scsi_mgmt_cmds);
		if (g_blk_cmd_array_size == 0 && g_scsi_cmd_array_size == 0 && g_scsi_mgmt_cmd_array_size == 0) {
			fprintf(stderr, "The provided json file did not contain any valid commands. Exiting.\n");
			return -EINVAL;
		}
	}

	rc = spdk_app_start(&opts, begin_fuzz, NULL);

	spdk_app_fini();
	return rc;
}
