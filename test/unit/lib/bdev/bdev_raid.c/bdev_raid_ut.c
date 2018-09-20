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
#include "spdk_cunit.h"
#include "spdk/env.h"
#include "spdk_internal/mock.h"
#include "bdev/raid/bdev_raid.c"
#include "bdev/raid/bdev_raid_rpc.c"

#define MAX_BASE_DRIVES 255
#define MAX_RAIDS 31
#define INVALID_IO_SUBMIT 0xFFFF

/* Data structure to capture the output of IO for verification */
struct io_output {
	struct spdk_bdev_desc       *desc;
	struct spdk_io_channel      *ch;
	uint64_t                    offset_blocks;
	uint64_t                    num_blocks;
	spdk_bdev_io_completion_cb  cb;
	void                        *cb_arg;
	enum spdk_bdev_io_type      iotype;
};

/* Different test options, more options to test can be added here */
uint32_t g_blklen_opts[] = {512, 4096};
uint32_t g_strip_opts[] = {64, 128, 256, 512, 1024, 2048};
uint32_t g_iosize_opts[] = {256, 512, 1024};
uint32_t g_max_qd_opts[] = {64, 128, 256, 512, 1024, 2048};

/* Globals */
int g_bdev_io_submit_status;
struct io_output *g_io_output = NULL;
uint32_t g_io_output_index;
uint32_t g_io_comp_status;
bool g_child_io_status_flag;
void *rpc_req;
uint32_t rpc_req_size;
TAILQ_HEAD(bdev, spdk_bdev);
struct bdev g_bdev_list;
TAILQ_HEAD(waitq, spdk_bdev_io_wait_entry);
struct waitq g_io_waitq;
uint32_t g_block_len;
uint32_t g_strip_size;
uint32_t g_max_io_size;
uint32_t g_max_qd;
uint8_t g_max_base_drives;
uint8_t g_max_raids;
uint8_t g_ignore_io_output;
uint8_t g_rpc_err;
char *g_get_raids_output[MAX_RAIDS];
uint32_t g_get_raids_count;
uint8_t g_json_beg_res_ret_err;
uint8_t g_json_decode_obj_err;
uint8_t g_json_decode_obj_construct;
uint8_t g_config_level_create = 0;
uint8_t g_test_multi_raids;

/* Set randomly test options, in every run it is different */
static void
set_test_opts(void)
{
	uint32_t seed = time(0);

	/* Generate random test options */
	srand(seed);
	g_max_base_drives = (rand() % MAX_BASE_DRIVES) + 1;
	g_max_raids = (rand() % MAX_RAIDS) + 1;
	g_block_len = g_blklen_opts[rand() % SPDK_COUNTOF(g_blklen_opts)];
	g_strip_size = g_strip_opts[rand() % SPDK_COUNTOF(g_strip_opts)];
	g_max_io_size = g_iosize_opts[rand() % SPDK_COUNTOF(g_iosize_opts)];
	g_max_qd = g_max_qd_opts[rand() % SPDK_COUNTOF(g_max_qd_opts)];

	printf("Test Options, seed = %u\n", seed);
	printf("blocklen = %u, strip_size = %u, max_io_size = %u, max_qd = %u, g_max_base_drives = %u, g_max_raids = %u\n",
	       g_block_len, g_strip_size, g_max_io_size, g_max_qd, g_max_base_drives, g_max_raids);
}

/* Set globals before every test run */
static void
set_globals(void)
{
	uint32_t max_splits;

	g_bdev_io_submit_status = 0;
	if (g_max_io_size < g_strip_size) {
		max_splits = 2;
	} else {
		max_splits = (g_max_io_size / g_strip_size) + 1;
	}
	g_io_output = calloc(max_splits, sizeof(struct io_output));
	SPDK_CU_ASSERT_FATAL(g_io_output != NULL);
	g_io_output_index = 0;
	memset(g_get_raids_output, 0, sizeof(g_get_raids_output));
	g_get_raids_count = 0;
	g_io_comp_status = 0;
	g_ignore_io_output = 0;
	g_config_level_create = 0;
	g_rpc_err = 0;
	g_test_multi_raids = 0;
	g_child_io_status_flag = true;
	TAILQ_INIT(&g_bdev_list);
	TAILQ_INIT(&g_io_waitq);
	rpc_req = NULL;
	rpc_req_size = 0;
	g_json_beg_res_ret_err = 0;
	g_json_decode_obj_err = 0;
	g_json_decode_obj_construct = 0;
}

static void
base_bdevs_cleanup(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *bdev_next;

	if (!TAILQ_EMPTY(&g_bdev_list)) {
		TAILQ_FOREACH_SAFE(bdev, &g_bdev_list, internal.link, bdev_next) {
			free(bdev->name);
			TAILQ_REMOVE(&g_bdev_list, bdev, internal.link);
			free(bdev);
		}
	}
}

static void
check_and_remove_raid_bdev(struct raid_bdev_config *raid_cfg)
{
	struct raid_bdev       *raid_bdev;

	/* Get the raid structured allocated if exists */
	raid_bdev = raid_cfg->raid_bdev;
	if (raid_bdev == NULL) {
		return;
	}

	for (uint32_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
		assert(raid_bdev->base_bdev_info != NULL);
		if (raid_bdev->base_bdev_info[i].bdev) {
			raid_bdev_free_base_bdev_resource(raid_bdev, i);
		}
	}
	assert(raid_bdev->num_base_bdevs_discovered == 0);
	raid_bdev_cleanup(raid_bdev);
}

/* Reset globals */
static void
reset_globals(void)
{
	if (g_io_output) {
		free(g_io_output);
		g_io_output = NULL;
	}
	rpc_req = NULL;
	rpc_req_size = 0;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
		     uint64_t len)
{
	CU_ASSERT(false);
}

/* Store the IO completion status in global variable to verify by various tests */
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_io_comp_status = ((status == SPDK_BDEV_IO_STATUS_SUCCESS) ? true : false);
}

/* It will cache the split IOs for verification */
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *p = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	if (g_ignore_io_output) {
		return 0;
	}

	if (g_max_io_size < g_strip_size) {
		SPDK_CU_ASSERT_FATAL(g_io_output_index < 2);
	} else {
		SPDK_CU_ASSERT_FATAL(g_io_output_index < (g_max_io_size / g_strip_size) + 1);
	}
	if (g_bdev_io_submit_status == 0) {
		p->desc = desc;
		p->ch = ch;
		p->offset_blocks = offset_blocks;
		p->num_blocks = num_blocks;
		p->cb = cb;
		p->cb_arg = cb_arg;
		p->iotype = SPDK_BDEV_IO_TYPE_WRITE;
		g_io_output_index++;
		child_io = calloc(1, sizeof(struct spdk_bdev_io));
		SPDK_CU_ASSERT_FATAL(child_io != NULL);
		cb(child_io, g_child_io_status_flag, cb_arg);
	}

	return g_bdev_io_submit_status;
}

int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	bdev->fn_table->destruct(bdev->ctxt);
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
	*_desc = (void *)0x1;
	return 0;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	CU_ASSERT(ch == (void *)1);
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	return NULL;
}

void
spdk_poller_unregister(struct spdk_poller **ppoller)
{
}

struct spdk_poller *
spdk_poller_register(spdk_poller_fn fn,
		     void *arg,
		     uint64_t period_microseconds)
{
	return (void *)1;
}

void
spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb)
{
}

char *
spdk_sprintf_alloc(const char *format, ...)
{
	return strdup(format);
}

void
spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size,
			const char *name)
{
}

int
spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int spdk_json_write_named_uint32(struct spdk_json_write_ctx *w, const char *name, uint32_t val)
{
	struct rpc_construct_raid_bdev *req = rpc_req;
	if (strcmp(name, "strip_size") == 0) {
		CU_ASSERT(req->strip_size * 1024 / g_block_len == val);
	} else if (strcmp(name, "blocklen_shift") == 0) {
		CU_ASSERT(spdk_u32log2(g_block_len) == val);
	} else if (strcmp(name, "raid_level") == 0) {
		CU_ASSERT(req->raid_level == val);
	} else if (strcmp(name, "num_base_bdevs") == 0) {
		CU_ASSERT(req->base_bdevs.num_base_bdevs == val);
	} else if (strcmp(name, "state") == 0) {
		CU_ASSERT(val == RAID_BDEV_STATE_ONLINE);
	} else if (strcmp(name, "destruct_called") == 0) {
		CU_ASSERT(val == 0);
	} else if (strcmp(name, "num_base_bdevs_discovered") == 0) {
		CU_ASSERT(req->base_bdevs.num_base_bdevs == val);
	}
	return 0;
}

int spdk_json_write_named_string(struct spdk_json_write_ctx *w, const char *name, const char *val)
{
	return 0;
}

int
spdk_json_write_object_begin(struct spdk_json_write_ctx *w)
{
	return 0;
}

int
spdk_json_write_named_object_begin(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int
spdk_json_write_named_array_begin(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int
spdk_json_write_array_end(struct spdk_json_write_ctx *w)
{
	return 0;
}

int
spdk_json_write_object_end(struct spdk_json_write_ctx *w)
{
	return 0;
}

int
spdk_json_write_bool(struct spdk_json_write_ctx *w, bool val)
{
	return 0;
}

int spdk_json_write_null(struct spdk_json_write_ctx *w)
{
	return 0;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return (void *)1;
}

void
spdk_for_each_thread(spdk_thread_fn fn, void *ctx, spdk_thread_fn cpl)
{
	fn(ctx);
	cpl(ctx);
}

struct spdk_thread *
spdk_get_thread(void)
{
	return NULL;
}

void
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx)
{
	fn(ctx);
}

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io) {
		free(bdev_io);
	}
}

/* It will cache split IOs for verification */
int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *p = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	if (g_ignore_io_output) {
		return 0;
	}

	SPDK_CU_ASSERT_FATAL(g_io_output_index <= (g_max_io_size / g_strip_size) + 1);
	if (g_bdev_io_submit_status == 0) {
		p->desc = desc;
		p->ch = ch;
		p->offset_blocks = offset_blocks;
		p->num_blocks = num_blocks;
		p->cb = cb;
		p->cb_arg = cb_arg;
		p->iotype = SPDK_BDEV_IO_TYPE_READ;
		g_io_output_index++;
		child_io = calloc(1, sizeof(struct spdk_bdev_io));
		SPDK_CU_ASSERT_FATAL(child_io != NULL);
		cb(child_io, g_child_io_status_flag, cb_arg);
	}

	return g_bdev_io_submit_status;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	CU_ASSERT(bdev->internal.claim_module != NULL);
	bdev->internal.claim_module = NULL;
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module *module)
{
}

struct spdk_conf_section *
spdk_conf_first_section(struct spdk_conf *cp)
{
	if (g_config_level_create) {
		return (void *) 0x1;
	}

	return NULL;
}

bool
spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix)
{
	if (g_config_level_create) {
		return true;
	}

	return false;
}

char *
spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key)
{
	struct rpc_construct_raid_bdev  *req = rpc_req;

	if (g_config_level_create) {
		if (strcmp(key, "Name") == 0) {
			return req->name;
		}
	}

	return NULL;
}

int
spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key)
{
	struct rpc_construct_raid_bdev  *req = rpc_req;

	if (g_config_level_create) {
		if (strcmp(key, "StripSize") == 0) {
			return req->strip_size;
		} else if (strcmp(key, "NumDevices") == 0) {
			return req->base_bdevs.num_base_bdevs;
		} else if (strcmp(key, "RaidLevel") == 0) {
			return req->raid_level;
		}
	}

	return 0;
}

struct spdk_conf_section *
spdk_conf_next_section(struct spdk_conf_section *sp)
{
	return NULL;
}

char *
spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key, int idx1, int idx2)
{
	struct rpc_construct_raid_bdev  *req = rpc_req;

	if (g_config_level_create) {
		if (strcmp(key, "Devices") == 0) {
			if (idx2 >= g_max_base_drives) {
				return NULL;
			}
			return req->base_bdevs.base_bdevs[idx2];
		}
	}

	return NULL;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module *module)
{
	if (bdev->internal.claim_module != NULL) {
		return -1;
	}
	bdev->internal.claim_module = module;
	return 0;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	return 0;
}

uint32_t
spdk_env_get_last_core(void)
{
	return 0;
}

int
spdk_json_decode_string(const struct spdk_json_val *val, void *out)
{
	return 0;
}

int
spdk_json_decode_object(const struct spdk_json_val *values,
			const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out)
{
	struct rpc_construct_raid_bdev *req, *_out;
	size_t i;

	if (g_json_decode_obj_err) {
		return -1;
	} else if (g_json_decode_obj_construct) {
		req = rpc_req;
		_out = out;

		_out->name = strdup(req->name);
		SPDK_CU_ASSERT_FATAL(_out->name != NULL);
		_out->strip_size = req->strip_size;
		_out->raid_level = req->raid_level;
		_out->base_bdevs.num_base_bdevs = req->base_bdevs.num_base_bdevs;
		for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
			_out->base_bdevs.base_bdevs[i] = strdup(req->base_bdevs.base_bdevs[i]);
			SPDK_CU_ASSERT_FATAL(_out->base_bdevs.base_bdevs[i]);
		}
	} else {
		memcpy(out, rpc_req, rpc_req_size);
	}

	return 0;
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	if (g_json_beg_res_ret_err) {
		return NULL;
	} else {
		return (void *)1;
	}
}

int
spdk_json_write_array_begin(struct spdk_json_write_ctx *w)
{
	return 0;
}

int
spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val)
{
	if (g_test_multi_raids) {
		g_get_raids_output[g_get_raids_count] = strdup(val);
		SPDK_CU_ASSERT_FATAL(g_get_raids_output[g_get_raids_count] != NULL);
		g_get_raids_count++;
	}

	return 0;
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
{
	g_rpc_err = 1;
}

void
spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
				     int error_code, const char *fmt, ...)
{
	g_rpc_err = 1;
}

void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev;

	if (!TAILQ_EMPTY(&g_bdev_list)) {
		TAILQ_FOREACH(bdev, &g_bdev_list, internal.link) {
			if (strcmp(bdev_name, bdev->name) == 0) {
				return bdev;
			}
		}
	}

	return NULL;
}

const char *
spdk_strerror(int errnum)
{
	return NULL;
}

int
spdk_json_decode_array(const struct spdk_json_val *values, spdk_json_decode_fn decode_func,
		       void *out, size_t max_size, size_t *out_size, size_t stride)
{
	return 0;
}

void
spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func, uint32_t state_mask)
{
}

int
spdk_json_decode_uint32(const struct spdk_json_val *val, void *out)
{
	return 0;
}


void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{
}

static void
bdev_io_cleanup(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io->u.bdev.iovs) {
		if (bdev_io->u.bdev.iovs->iov_base) {
			free(bdev_io->u.bdev.iovs->iov_base);
			bdev_io->u.bdev.iovs->iov_base = NULL;
		}
		free(bdev_io->u.bdev.iovs);
		bdev_io->u.bdev.iovs = NULL;
	}
}

static void
bdev_io_initialize(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev,
		   uint64_t lba, uint64_t blocks, int16_t iotype)
{
	bdev_io->bdev = bdev;
	bdev_io->u.bdev.offset_blocks = lba;
	bdev_io->u.bdev.num_blocks = blocks;
	bdev_io->type = iotype;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.iovs = calloc(1, sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iovs != NULL);
	bdev_io->u.bdev.iovs->iov_base = calloc(1, bdev_io->u.bdev.num_blocks * g_block_len);
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iovs->iov_base != NULL);
	bdev_io->u.bdev.iovs->iov_len = bdev_io->u.bdev.num_blocks * g_block_len;
	bdev_io->u.bdev.iovs = bdev_io->u.bdev.iovs;
}

static void
verify_io(struct spdk_bdev_io *bdev_io, uint8_t num_base_drives,
	  struct raid_bdev_io_channel *ch_ctx, struct raid_bdev *raid_bdev, uint32_t io_status)
{
	uint32_t strip_shift = spdk_u32log2(g_strip_size);
	uint64_t start_strip = bdev_io->u.bdev.offset_blocks >> strip_shift;
	uint64_t end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
			     strip_shift;
	uint32_t splits_reqd = (end_strip - start_strip + 1);
	uint32_t strip;
	uint64_t pd_strip;
	uint64_t pd_idx;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint64_t pd_blocks;
	uint32_t index = 0;
	uint8_t *buf = bdev_io->u.bdev.iovs->iov_base;

	if (io_status == INVALID_IO_SUBMIT) {
		CU_ASSERT(g_io_comp_status == false);
		return;
	}
	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);
	SPDK_CU_ASSERT_FATAL(num_base_drives != 0);

	CU_ASSERT(splits_reqd == g_io_output_index);
	for (strip = start_strip; strip <= end_strip; strip++, index++) {
		pd_strip = strip / num_base_drives;
		pd_idx = strip % num_base_drives;
		if (strip == start_strip) {
			offset_in_strip = bdev_io->u.bdev.offset_blocks & (g_strip_size - 1);
			pd_lba = (pd_strip << strip_shift) + offset_in_strip;
			if (strip == end_strip) {
				pd_blocks = bdev_io->u.bdev.num_blocks;
			} else {
				pd_blocks = g_strip_size - offset_in_strip;
			}
		} else if (strip == end_strip) {
			pd_lba = pd_strip << strip_shift;
			pd_blocks = ((bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) &
				     (g_strip_size - 1)) + 1;
		} else {
			pd_lba = pd_strip << raid_bdev->strip_size_shift;
			pd_blocks = raid_bdev->strip_size;
		}
		CU_ASSERT(pd_lba == g_io_output[index].offset_blocks);
		CU_ASSERT(pd_blocks == g_io_output[index].num_blocks);
		CU_ASSERT(ch_ctx->base_channel[pd_idx] == g_io_output[index].ch);
		CU_ASSERT(raid_bdev->base_bdev_info[pd_idx].desc == g_io_output[index].desc);
		CU_ASSERT(bdev_io->type == g_io_output[index].iotype);
		buf += (pd_blocks << spdk_u32log2(g_block_len));
	}
	CU_ASSERT(g_io_comp_status == io_status);
}

static void
verify_raid_config_present(const char *name, bool presence)
{
	struct raid_bdev_config *raid_cfg;
	bool cfg_found;

	cfg_found = false;

	TAILQ_FOREACH(raid_cfg, &g_spdk_raid_config.raid_bdev_config_head, link) {
		if (raid_cfg->name != NULL) {
			if (strcmp(name, raid_cfg->name) == 0) {
				cfg_found = true;
				break;
			}
		}
	}

	if (presence == true) {
		CU_ASSERT(cfg_found == true);
	} else {
		CU_ASSERT(cfg_found == false);
	}
}

static void
verify_raid_bdev_present(const char *name, bool presence)
{
	struct raid_bdev *pbdev;
	bool   pbdev_found;

	pbdev_found = false;
	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, name) == 0) {
			pbdev_found = true;
			break;
		}
	}
	if (presence == true) {
		CU_ASSERT(pbdev_found == true);
	} else {
		CU_ASSERT(pbdev_found == false);
	}
}
static void
verify_raid_config(struct rpc_construct_raid_bdev *r, bool presence)
{
	struct raid_bdev_config *raid_cfg = NULL;
	uint32_t i;
	int val;

	TAILQ_FOREACH(raid_cfg, &g_spdk_raid_config.raid_bdev_config_head, link) {
		if (strcmp(r->name, raid_cfg->name) == 0) {
			if (presence == false) {
				break;
			}
			CU_ASSERT(raid_cfg->raid_bdev != NULL);
			CU_ASSERT(raid_cfg->strip_size == r->strip_size);
			CU_ASSERT(raid_cfg->num_base_bdevs == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(raid_cfg->raid_level == r->raid_level);
			if (raid_cfg->base_bdev != NULL) {
				for (i = 0; i < raid_cfg->num_base_bdevs; i++) {
					val = strcmp(raid_cfg->base_bdev[i].name, r->base_bdevs.base_bdevs[i]);
					CU_ASSERT(val == 0);
				}
			}
			break;
		}
	}

	if (presence == true) {
		CU_ASSERT(raid_cfg != NULL);
	} else {
		CU_ASSERT(raid_cfg == NULL);
	}
}

static void
verify_raid_bdev(struct rpc_construct_raid_bdev *r, bool presence, uint32_t raid_state)
{
	struct raid_bdev *pbdev;
	uint32_t i;
	struct spdk_bdev *bdev = NULL;
	bool   pbdev_found;
	uint64_t min_blockcnt = 0xFFFFFFFFFFFFFFFF;

	pbdev_found = false;
	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, r->name) == 0) {
			pbdev_found = true;
			if (presence == false) {
				break;
			}
			CU_ASSERT(pbdev->config->raid_bdev == pbdev);
			CU_ASSERT(pbdev->base_bdev_info != NULL);
			CU_ASSERT(pbdev->strip_size == ((r->strip_size * 1024) / g_block_len));
			CU_ASSERT(pbdev->strip_size_shift == spdk_u32log2(((r->strip_size * 1024) / g_block_len)));
			CU_ASSERT(pbdev->blocklen_shift == spdk_u32log2(g_block_len));
			CU_ASSERT(pbdev->state == raid_state);
			CU_ASSERT(pbdev->num_base_bdevs == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pbdev->num_base_bdevs_discovered == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pbdev->raid_level == r->raid_level);
			CU_ASSERT(pbdev->destruct_called == false);
			for (i = 0; i < pbdev->num_base_bdevs; i++) {
				if (pbdev->base_bdev_info && pbdev->base_bdev_info[i].bdev) {
					bdev = spdk_bdev_get_by_name(pbdev->base_bdev_info[i].bdev->name);
					CU_ASSERT(bdev != NULL);
					CU_ASSERT(pbdev->base_bdev_info[i].remove_scheduled == false);
				} else {
					CU_ASSERT(0);
				}

				if (bdev && bdev->blockcnt < min_blockcnt) {
					min_blockcnt = bdev->blockcnt;
				}
			}
			CU_ASSERT((((min_blockcnt / (r->strip_size * 1024 / g_block_len)) * (r->strip_size * 1024 /
					g_block_len)) * r->base_bdevs.num_base_bdevs) == pbdev->bdev.blockcnt);
			CU_ASSERT(strcmp(pbdev->bdev.product_name, "Pooled Device") == 0);
			CU_ASSERT(pbdev->bdev.write_cache == 0);
			CU_ASSERT(pbdev->bdev.blocklen == g_block_len);
			if (pbdev->num_base_bdevs > 1) {
				CU_ASSERT(pbdev->bdev.optimal_io_boundary == pbdev->strip_size);
				CU_ASSERT(pbdev->bdev.split_on_optimal_io_boundary == true);
			} else {
				CU_ASSERT(pbdev->bdev.optimal_io_boundary == 0);
				CU_ASSERT(pbdev->bdev.split_on_optimal_io_boundary == false);
			}
			CU_ASSERT(pbdev->bdev.ctxt == pbdev);
			CU_ASSERT(pbdev->bdev.fn_table == &g_raid_bdev_fn_table);
			CU_ASSERT(pbdev->bdev.module == &g_raid_if);
			break;
		}
	}
	if (presence == true) {
		CU_ASSERT(pbdev_found == true);
	} else {
		CU_ASSERT(pbdev_found == false);
	}
	pbdev_found = false;
	if (raid_state == RAID_BDEV_STATE_ONLINE) {
		TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_configured_list, state_link) {
			if (strcmp(pbdev->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	} else if (raid_state == RAID_BDEV_STATE_CONFIGURING) {
		TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_configuring_list, state_link) {
			if (strcmp(pbdev->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	} else if (raid_state == RAID_BDEV_STATE_OFFLINE) {
		TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_offline_list, state_link) {
			if (strcmp(pbdev->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	}
	if (presence == true) {
		CU_ASSERT(pbdev_found == true);
	} else {
		CU_ASSERT(pbdev_found == false);
	}
}

int
spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			struct spdk_bdev_io_wait_entry *entry)
{
	CU_ASSERT(bdev == entry->bdev);
	CU_ASSERT(entry->cb_fn != NULL);
	CU_ASSERT(entry->cb_arg != NULL);
	TAILQ_INSERT_TAIL(&g_io_waitq, entry, link);
	return 0;
}


static uint32_t
get_num_elts_in_waitq(void)
{
	struct spdk_bdev_io_wait_entry *ele;
	uint32_t count = 0;

	TAILQ_FOREACH(ele, &g_io_waitq, link) {
		count++;
	}

	return count;
}

static void
process_io_waitq(void)
{
	struct spdk_bdev_io_wait_entry *ele;
	struct spdk_bdev_io_wait_entry *next_ele;

	TAILQ_FOREACH_SAFE(ele, &g_io_waitq, link, next_ele) {
		TAILQ_REMOVE(&g_io_waitq, ele, link);
		ele->cb_fn(ele->cb_arg);
	}
}

static void
verify_get_raids(struct rpc_construct_raid_bdev *construct_req,
		 uint8_t g_max_raids,
		 char **g_get_raids_output, uint32_t g_get_raids_count)
{
	uint32_t i, j;
	bool found;

	CU_ASSERT(g_max_raids == g_get_raids_count);
	if (g_max_raids == g_get_raids_count) {
		for (i = 0; i < g_max_raids; i++) {
			found = false;
			for (j = 0; j < g_max_raids; j++) {
				if (construct_req[i].name && strcmp(construct_req[i].name, g_get_raids_output[i]) == 0) {
					found = true;
					break;
				}
			}
			CU_ASSERT(found == true);
		}
	}
}

static void
create_base_bdevs(uint32_t bbdev_start_idx)
{
	uint32_t i;
	struct spdk_bdev *base_bdev;
	char name[16];
	uint16_t num_chars;

	for (i = 0; i < g_max_base_drives; i++, bbdev_start_idx++) {
		num_chars = snprintf(name, 16, "%s%u%s", "Nvme", bbdev_start_idx, "n1");
		name[num_chars] = '\0';
		base_bdev = calloc(1, sizeof(struct spdk_bdev));
		SPDK_CU_ASSERT_FATAL(base_bdev != NULL);
		base_bdev->name = strdup(name);
		SPDK_CU_ASSERT_FATAL(base_bdev->name != NULL);
		base_bdev->blocklen = g_block_len;
		base_bdev->blockcnt = (uint64_t)1024 * 1024 * 1024 * 1024;
		TAILQ_INSERT_TAIL(&g_bdev_list, base_bdev, internal.link);
	}
}

static void
create_test_req(struct rpc_construct_raid_bdev *r, const char *raid_name, uint32_t bbdev_start_idx,
		bool create_base_bdev)
{
	uint32_t i;
	char name[16];
	uint16_t num_chars;
	uint32_t bbdev_idx = bbdev_start_idx;

	r->name = strdup(raid_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);
	r->strip_size = (g_strip_size * g_block_len) / 1024;
	r->raid_level = 0;
	r->base_bdevs.num_base_bdevs = g_max_base_drives;
	for (i = 0; i < g_max_base_drives; i++, bbdev_idx++) {
		num_chars = snprintf(name, 16, "%s%u%s", "Nvme", bbdev_idx, "n1");
		name[num_chars] = '\0';
		r->base_bdevs.base_bdevs[i] = strdup(name);
		SPDK_CU_ASSERT_FATAL(r->base_bdevs.base_bdevs[i] != NULL);
	}
	if (create_base_bdev == true) {
		create_base_bdevs(bbdev_start_idx);
	}
}

static void
free_test_req(struct rpc_construct_raid_bdev *r)
{
	uint8_t i;

	free(r->name);
	for (i = 0; i < r->base_bdevs.num_base_bdevs; i++) {
		free(r->base_bdevs.base_bdevs[i]);
	}
}

static void
test_construct_raid(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&req);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_destroy_raid(void)
{
	struct rpc_construct_raid_bdev construct_req;
	struct rpc_destroy_raid_bdev destroy_req;

	set_globals();
	create_test_req(&construct_req, "raid1", 0, true);
	rpc_req = &construct_req;
	rpc_req_size = sizeof(construct_req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(construct_req.name, false);
	verify_raid_bdev_present(construct_req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&construct_req, true);
	verify_raid_bdev(&construct_req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&construct_req);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_construct_raid_invalid_args(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev_config *raid_cfg;

	set_globals();
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);

	create_test_req(&req, "raid1", 0, true);
	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	req.raid_level = 1;
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_err = 1;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	g_json_decode_obj_err = 0;
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	req.strip_size = 1231;
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&req);

	create_test_req(&req, "raid1", 0, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);

	create_test_req(&req, "raid2", 0, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid2", false);
	verify_raid_bdev_present("raid2", false);

	create_test_req(&req, "raid2", g_max_base_drives, true);
	free(req.base_bdevs.base_bdevs[g_max_base_drives - 1]);
	req.base_bdevs.base_bdevs[g_max_base_drives - 1] = strdup("Nvme0n1");
	SPDK_CU_ASSERT_FATAL(req.base_bdevs.base_bdevs[g_max_base_drives - 1] != NULL);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid2", false);
	verify_raid_bdev_present("raid2", false);

	create_test_req(&req, "raid2", g_max_base_drives, true);
	free(req.base_bdevs.base_bdevs[g_max_base_drives - 1]);
	req.base_bdevs.base_bdevs[g_max_base_drives - 1] = strdup("Nvme100000n1");
	SPDK_CU_ASSERT_FATAL(req.base_bdevs.base_bdevs[g_max_base_drives - 1] != NULL);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	free_test_req(&req);
	verify_raid_config_present("raid2", true);
	verify_raid_bdev_present("raid2", true);
	raid_cfg = raid_bdev_config_find_by_name("raid2");
	SPDK_CU_ASSERT_FATAL(raid_cfg != NULL);
	check_and_remove_raid_bdev(raid_cfg);
	raid_bdev_config_cleanup(raid_cfg);

	create_test_req(&req, "raid2", g_max_base_drives, false);
	g_rpc_err = 0;
	g_json_beg_res_ret_err = 1;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	free_test_req(&req);
	verify_raid_config_present("raid2", true);
	verify_raid_bdev_present("raid2", true);
	verify_raid_config_present("raid1", true);
	verify_raid_bdev_present("raid1", true);
	g_json_beg_res_ret_err = 0;

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	destroy_req.name = strdup("raid2");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_destroy_raid_invalid_args(void)
{
	struct rpc_construct_raid_bdev construct_req;
	struct rpc_destroy_raid_bdev destroy_req;

	set_globals();
	create_test_req(&construct_req, "raid1", 0, true);
	rpc_req = &construct_req;
	rpc_req_size = sizeof(construct_req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(construct_req.name, false);
	verify_raid_bdev_present(construct_req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&construct_req, true);
	verify_raid_bdev(&construct_req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&construct_req);

	destroy_req.name = strdup("raid2");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);

	destroy_req.name = strdup("raid1");
	g_rpc_err = 0;
	g_json_decode_obj_err = 1;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	g_json_decode_obj_err = 0;
	g_rpc_err = 0;
	free(destroy_req.name);
	verify_raid_config_present("raid1", true);
	verify_raid_bdev_present("raid1", true);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_io_channel(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev *pbdev;
	struct raid_bdev_io_channel *ch_ctx;
	uint32_t i;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);

	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch_ctx = calloc(1, sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == (void *)0x1);
	}
	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free_test_req(&req);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	free(ch_ctx);
	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_write_io(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint32_t i;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == (void *)0x1);
	}

	lba = 0;
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_strip_size) + 1;
		bdev_io_initialize(bdev_io, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += g_strip_size;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}
	free_test_req(&req);

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_read_io(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint32_t i;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == (void *)0x1);
	}
	free_test_req(&req);

	lba = 0;
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_strip_size) + 1;
		bdev_io_initialize(bdev_io, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_READ);
		lba += g_strip_size;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Test IO failures */
static void
test_io_failure(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint32_t i;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == (void *)0x1);
	}
	free_test_req(&req);

	lba = 0;
	for (count = 0; count < 1; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_strip_size) + 1;
		bdev_io_initialize(bdev_io, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_INVALID);
		lba += g_strip_size;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  INVALID_IO_SUBMIT);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}


	lba = 0;
	g_child_io_status_flag = false;
	for (count = 0; count < 1; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_strip_size) + 1;
		bdev_io_initialize(bdev_io, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += g_strip_size;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Test waitq logic */
static void
test_io_waitq(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint32_t i;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_io *bdev_io_next;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;
	TAILQ_HEAD(, spdk_bdev_io) head_io;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, req.name) == 0) {
			break;
		}
	}
	SPDK_CU_ASSERT_FATAL(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_channel != NULL);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel[i] == (void *)0x1);
	}
	free_test_req(&req);

	lba = 0;
	TAILQ_INIT(&head_io);
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		TAILQ_INSERT_TAIL(&head_io, bdev_io, module_link);
		io_len = (rand() % g_strip_size) + 1;
		bdev_io_initialize(bdev_io, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		g_bdev_io_submit_status = -ENOMEM;
		lba += g_strip_size;
		raid_bdev_submit_request(ch, bdev_io);
	}

	g_ignore_io_output = 1;

	count = get_num_elts_in_waitq();
	CU_ASSERT(count == g_max_qd);
	g_bdev_io_submit_status = 0;
	process_io_waitq();
	CU_ASSERT(TAILQ_EMPTY(&g_io_waitq));

	TAILQ_FOREACH_SAFE(bdev_io, &head_io, module_link, bdev_io_next) {
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	g_ignore_io_output = 0;
	free(ch);
	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Create multiple raids, destroy raids without IO, get_raids related tests */
static void
test_multi_raid_no_io(void)
{
	struct rpc_construct_raid_bdev *construct_req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct rpc_get_raid_bdevs get_raids_req;
	uint32_t i;
	char name[16];
	uint32_t count;
	uint32_t bbdev_idx = 0;

	set_globals();
	construct_req = calloc(MAX_RAIDS, sizeof(struct rpc_construct_raid_bdev));
	SPDK_CU_ASSERT_FATAL(construct_req != NULL);
	CU_ASSERT(raid_bdev_init() == 0);
	for (i = 0; i < g_max_raids; i++) {
		count = snprintf(name, 16, "%s%u", "raid", i);
		name[count] = '\0';
		create_test_req(&construct_req[i], name, bbdev_idx, true);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
		bbdev_idx += g_max_base_drives;
		rpc_req = &construct_req[i];
		rpc_req_size = sizeof(construct_req[0]);
		g_rpc_err = 0;
		g_json_decode_obj_construct = 1;
		spdk_rpc_construct_raid_bdev(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config(&construct_req[i], true);
		verify_raid_bdev(&construct_req[i], true, RAID_BDEV_STATE_ONLINE);
	}

	get_raids_req.category = strdup("all");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_test_multi_raids = 1;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_get_raids(construct_req, g_max_raids, g_get_raids_output, g_get_raids_count);
	for (i = 0; i < g_get_raids_count; i++) {
		free(g_get_raids_output[i]);
	}
	g_get_raids_count = 0;

	get_raids_req.category = strdup("online");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_get_raids(construct_req, g_max_raids, g_get_raids_output, g_get_raids_count);
	for (i = 0; i < g_get_raids_count; i++) {
		free(g_get_raids_output[i]);
	}
	g_get_raids_count = 0;

	get_raids_req.category = strdup("configuring");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_raids_count == 0);

	get_raids_req.category = strdup("offline");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_raids_count == 0);

	get_raids_req.category = strdup("invalid_category");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	CU_ASSERT(g_get_raids_count == 0);

	get_raids_req.category = strdup("all");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_json_decode_obj_err = 1;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	g_json_decode_obj_err = 0;
	free(get_raids_req.category);
	CU_ASSERT(g_get_raids_count == 0);

	get_raids_req.category = strdup("all");
	rpc_req = &get_raids_req;
	rpc_req_size = sizeof(get_raids_req);
	g_rpc_err = 0;
	g_json_beg_res_ret_err = 1;
	g_json_decode_obj_construct = 0;
	spdk_rpc_get_raid_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	g_json_beg_res_ret_err = 0;
	CU_ASSERT(g_get_raids_count == 0);

	for (i = 0; i < g_max_raids; i++) {
		SPDK_CU_ASSERT_FATAL(construct_req[i].name != NULL);
		destroy_req.name = strdup(construct_req[i].name);
		count = snprintf(name, 16, "%s", destroy_req.name);
		name[count] = '\0';
		rpc_req = &destroy_req;
		rpc_req_size = sizeof(destroy_req);
		g_rpc_err = 0;
		g_json_decode_obj_construct = 0;
		spdk_rpc_destroy_raid_bdev(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
	}
	g_test_multi_raids = 0;
	raid_bdev_exit();
	for (i = 0; i < g_max_raids; i++) {
		free_test_req(&construct_req[i]);
	}
	free(construct_req);
	base_bdevs_cleanup();
	reset_globals();
}

/* Create multiple raids, fire IOs randomly on various raids */
static void
test_multi_raid_with_io(void)
{
	struct rpc_construct_raid_bdev *construct_req;
	struct rpc_destroy_raid_bdev destroy_req;
	uint32_t i, j;
	char name[16];
	uint32_t count;
	uint32_t bbdev_idx = 0;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io;
	uint64_t io_len;
	uint64_t lba;
	struct spdk_io_channel *ch_random;
	struct raid_bdev_io_channel *ch_ctx_random;
	int16_t iotype;
	uint32_t raid_random;

	set_globals();
	construct_req = calloc(g_max_raids, sizeof(struct rpc_construct_raid_bdev));
	SPDK_CU_ASSERT_FATAL(construct_req != NULL);
	CU_ASSERT(raid_bdev_init() == 0);
	ch = calloc(g_max_raids, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	for (i = 0; i < g_max_raids; i++) {
		count = snprintf(name, 16, "%s%u", "raid", i);
		name[count] = '\0';
		create_test_req(&construct_req[i], name, bbdev_idx, true);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
		bbdev_idx += g_max_base_drives;
		rpc_req = &construct_req[i];
		rpc_req_size = sizeof(construct_req[0]);
		g_rpc_err = 0;
		g_json_decode_obj_construct = 1;
		spdk_rpc_construct_raid_bdev(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config(&construct_req[i], true);
		verify_raid_bdev(&construct_req[i], true, RAID_BDEV_STATE_ONLINE);
		TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
			if (strcmp(pbdev->bdev.name, construct_req[i].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev != NULL);
		ch_ctx = spdk_io_channel_get_ctx(&ch[i]);
		SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);
		CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
		CU_ASSERT(ch_ctx->base_channel != NULL);
		for (j = 0; j < construct_req[i].base_bdevs.num_base_bdevs; j++) {
			CU_ASSERT(ch_ctx->base_channel[j] == (void *)0x1);
		}
	}

	lba = 0;
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_strip_size) + 1;
		iotype = (rand() % 2) ? SPDK_BDEV_IO_TYPE_WRITE : SPDK_BDEV_IO_TYPE_READ;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_random = rand() % g_max_raids;
		ch_random = &ch[raid_random];
		ch_ctx_random = spdk_io_channel_get_ctx(ch_random);
		TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
			if (strcmp(pbdev->bdev.name, construct_req[raid_random].name) == 0) {
				break;
			}
		}
		bdev_io_initialize(bdev_io, &pbdev->bdev, lba, io_len, iotype);
		lba += g_strip_size;
		CU_ASSERT(pbdev != NULL);
		raid_bdev_submit_request(ch_random, bdev_io);
		verify_io(bdev_io, g_max_base_drives, ch_ctx_random, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	for (i = 0; i < g_max_raids; i++) {
		TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
			if (strcmp(pbdev->bdev.name, construct_req[i].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev != NULL);
		ch_ctx = spdk_io_channel_get_ctx(&ch[i]);
		SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);
		raid_bdev_destroy_cb(pbdev, ch_ctx);
		CU_ASSERT(ch_ctx->base_channel == NULL);
		destroy_req.name = strdup(construct_req[i].name);
		count = snprintf(name, 16, "%s", destroy_req.name);
		name[count] = '\0';
		rpc_req = &destroy_req;
		rpc_req_size = sizeof(destroy_req);
		g_rpc_err = 0;
		g_json_decode_obj_construct = 0;
		spdk_rpc_destroy_raid_bdev(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
	}
	raid_bdev_exit();
	for (i = 0; i < g_max_raids; i++) {
		free_test_req(&construct_req[i]);
	}
	free(construct_req);
	free(ch);
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_io_type_supported(void)
{
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_READ) == true);
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_WRITE) == true);
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_FLUSH) == true);
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_INVALID) == false);
}

static void
test_create_raid_from_config(void)
{
	struct rpc_construct_raid_bdev req;
	struct spdk_bdev *bdev;
	struct rpc_destroy_raid_bdev destroy_req;
	bool can_claim;
	struct raid_bdev_config *raid_cfg;
	uint32_t base_bdev_slot;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	g_config_level_create = 1;
	CU_ASSERT(raid_bdev_init() == 0);
	g_config_level_create = 0;

	verify_raid_config_present("raid1", true);
	verify_raid_bdev_present("raid1", true);

	TAILQ_FOREACH(bdev, &g_bdev_list, internal.link) {
		raid_bdev_examine(bdev);
	}

	can_claim = raid_bdev_can_claim_bdev("Invalid", &raid_cfg, &base_bdev_slot);
	CU_ASSERT(can_claim == false);

	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	free_test_req(&req);
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_create_raid_from_config_invalid_params(void)
{
	struct rpc_construct_raid_bdev req;
	uint8_t count;

	set_globals();
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	g_config_level_create = 1;

	create_test_req(&req, "raid1", 0, true);
	free(req.name);
	req.name = NULL;
	CU_ASSERT(raid_bdev_init() != 0);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	req.strip_size = 1234;
	CU_ASSERT(raid_bdev_init() != 0);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	req.raid_level = 1;
	CU_ASSERT(raid_bdev_init() != 0);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	req.raid_level = 1;
	CU_ASSERT(raid_bdev_init() != 0);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	req.base_bdevs.num_base_bdevs++;
	CU_ASSERT(raid_bdev_init() != 0);
	req.base_bdevs.num_base_bdevs--;
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_test_req(&req, "raid1", 0, false);
	req.base_bdevs.num_base_bdevs--;
	CU_ASSERT(raid_bdev_init() != 0);
	req.base_bdevs.num_base_bdevs++;
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	if (g_max_base_drives > 1) {
		create_test_req(&req, "raid1", 0, false);
		count = snprintf(req.base_bdevs.base_bdevs[g_max_base_drives - 1], 15, "%s", "Nvme0n1");
		req.base_bdevs.base_bdevs[g_max_base_drives - 1][count] = '\0';
		CU_ASSERT(raid_bdev_init() != 0);
		free_test_req(&req);
		verify_raid_config_present("raid1", false);
		verify_raid_bdev_present("raid1", false);
	}

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_raid_json_dump_info(void)
{
	struct rpc_construct_raid_bdev req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct raid_bdev *pbdev;

	set_globals();
	create_test_req(&req, "raid1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present(req.name, false);
	verify_raid_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);

	TAILQ_FOREACH(pbdev, &g_spdk_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);

	CU_ASSERT(raid_bdev_dump_info_json(pbdev, NULL) == 0);

	free_test_req(&req);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_context_size(void)
{
	CU_ASSERT(raid_bdev_get_ctx_size() == sizeof(struct raid_bdev_io));
}

static void
test_asym_base_drives_blockcnt(void)
{
	struct rpc_construct_raid_bdev construct_req;
	struct rpc_destroy_raid_bdev destroy_req;
	struct spdk_bdev *bbdev;
	uint32_t i;

	set_globals();
	create_test_req(&construct_req, "raid1", 0, true);
	rpc_req = &construct_req;
	rpc_req_size = sizeof(construct_req);
	CU_ASSERT(raid_bdev_init() == 0);
	verify_raid_config_present(construct_req.name, false);
	verify_raid_bdev_present(construct_req.name, false);
	g_rpc_err = 0;
	for (i = 0; i < construct_req.base_bdevs.num_base_bdevs; i++) {
		bbdev = spdk_bdev_get_by_name(construct_req.base_bdevs.base_bdevs[i]);
		SPDK_CU_ASSERT_FATAL(bbdev != NULL);
		bbdev->blockcnt = rand() + 1;
	}
	g_json_decode_obj_construct = 1;
	spdk_rpc_construct_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&construct_req, true);
	verify_raid_bdev(&construct_req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&construct_req);

	destroy_req.name = strdup("raid1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
	spdk_rpc_destroy_raid_bdev(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

int main(int argc, char **argv)
{
	CU_pSuite       suite = NULL;
	unsigned int    num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("raid", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_construct_raid", test_construct_raid) == NULL ||
		CU_add_test(suite, "test_destroy_raid", test_destroy_raid) == NULL ||
		CU_add_test(suite, "test_construct_raid_invalid_args", test_construct_raid_invalid_args) == NULL ||
		CU_add_test(suite, "test_destroy_raid_invalid_args", test_destroy_raid_invalid_args) == NULL ||
		CU_add_test(suite, "test_io_channel", test_io_channel) == NULL ||
		CU_add_test(suite, "test_write_io", test_write_io) == NULL    ||
		CU_add_test(suite, "test_read_io", test_read_io) == NULL     ||
		CU_add_test(suite, "test_io_failure", test_io_failure) == NULL ||
		CU_add_test(suite, "test_io_waitq", test_io_waitq) == NULL ||
		CU_add_test(suite, "test_multi_raid_no_io", test_multi_raid_no_io) == NULL ||
		CU_add_test(suite, "test_multi_raid_with_io", test_multi_raid_with_io) == NULL ||
		CU_add_test(suite, "test_io_type_supported", test_io_type_supported) == NULL ||
		CU_add_test(suite, "test_create_raid_from_config", test_create_raid_from_config) == NULL ||
		CU_add_test(suite, "test_create_raid_from_config_invalid_params",
			    test_create_raid_from_config_invalid_params) == NULL ||
		CU_add_test(suite, "test_raid_json_dump_info", test_raid_json_dump_info) == NULL ||
		CU_add_test(suite, "test_context_size", test_context_size) == NULL ||
		CU_add_test(suite, "test_asym_base_drives_blockcnt", test_asym_base_drives_blockcnt) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	set_test_opts();
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
