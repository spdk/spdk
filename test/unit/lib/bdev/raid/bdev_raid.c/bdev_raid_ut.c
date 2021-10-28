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
#include "thread/thread_internal.h"
#include "bdev/raid/bdev_raid.c"
#include "bdev/raid/bdev_raid_rpc.c"
#include "bdev/raid/raid0.c"
#include "common/lib/ut_multithread.c"

#define MAX_BASE_DRIVES 32
#define MAX_RAIDS 2
#define INVALID_IO_SUBMIT 0xFFFF
#define MAX_TEST_IO_RANGE (3 * 3 * 3 * (MAX_BASE_DRIVES + 5))
#define BLOCK_CNT (1024ul * 1024ul * 1024ul * 1024ul)

struct spdk_bdev_channel {
	struct spdk_io_channel *channel;
};

struct spdk_bdev_desc {
	struct spdk_bdev *bdev;
};

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

struct raid_io_ranges {
	uint64_t lba;
	uint64_t nblocks;
};

/* Globals */
int g_bdev_io_submit_status;
struct io_output *g_io_output = NULL;
uint32_t g_io_output_index;
uint32_t g_io_comp_status;
bool g_child_io_status_flag;
void *g_rpc_req;
uint32_t g_rpc_req_size;
TAILQ_HEAD(bdev, spdk_bdev);
struct bdev g_bdev_list;
TAILQ_HEAD(waitq, spdk_bdev_io_wait_entry);
struct waitq g_io_waitq;
uint32_t g_block_len;
uint32_t g_strip_size;
uint32_t g_max_io_size;
uint8_t g_max_base_drives;
uint8_t g_max_raids;
uint8_t g_ignore_io_output;
uint8_t g_rpc_err;
char *g_get_raids_output[MAX_RAIDS];
uint32_t g_get_raids_count;
uint8_t g_json_decode_obj_err;
uint8_t g_json_decode_obj_create;
uint8_t g_config_level_create = 0;
uint8_t g_test_multi_raids;
struct raid_io_ranges g_io_ranges[MAX_TEST_IO_RANGE];
uint32_t g_io_range_idx;
uint64_t g_lba_offset;
struct spdk_io_channel g_io_channel;

DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), true);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_flush_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_conf_next_section, struct spdk_conf_section *, (struct spdk_conf_section *sp),
	    NULL);
DEFINE_STUB_V(spdk_rpc_register_method, (const char *method, spdk_rpc_method_handler func,
		uint32_t state_mask));
DEFINE_STUB_V(spdk_rpc_register_alias_deprecated, (const char *method, const char *alias));
DEFINE_STUB_V(spdk_jsonrpc_end_result, (struct spdk_jsonrpc_request *request,
					struct spdk_json_write_ctx *w));
DEFINE_STUB_V(spdk_jsonrpc_send_bool_response, (struct spdk_jsonrpc_request *request,
		bool value));
DEFINE_STUB(spdk_json_decode_string, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uint32, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_array, int, (const struct spdk_json_val *values,
		spdk_json_decode_fn decode_func,
		void *out, size_t max_size, size_t *out_size, size_t stride), 0);
DEFINE_STUB(spdk_json_write_name, int, (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_named_object_begin, int, (struct spdk_json_write_ctx *w,
		const char *name), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_named_array_begin, int, (struct spdk_json_write_ctx *w,
		const char *name), 0);
DEFINE_STUB(spdk_json_write_bool, int, (struct spdk_json_write_ctx *w, bool val), 0);
DEFINE_STUB(spdk_json_write_null, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_strerror, const char *, (int errnum), NULL);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	g_io_channel.thread = spdk_get_thread();

	return &g_io_channel;
}

static void
set_test_opts(void)
{

	g_max_base_drives = MAX_BASE_DRIVES;
	g_max_raids = MAX_RAIDS;
	g_block_len = 4096;
	g_strip_size = 64;
	g_max_io_size = 1024;

	printf("Test Options\n");
	printf("blocklen = %u, strip_size = %u, max_io_size = %u, g_max_base_drives = %u, "
	       "g_max_raids = %u\n",
	       g_block_len, g_strip_size, g_max_io_size, g_max_base_drives, g_max_raids);
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
	if (max_splits < g_max_base_drives) {
		max_splits = g_max_base_drives;
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
	g_rpc_req = NULL;
	g_rpc_req_size = 0;
	g_json_decode_obj_err = 0;
	g_json_decode_obj_create = 0;
	g_lba_offset = 0;
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
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	/* Get the raid structured allocated if exists */
	raid_bdev = raid_cfg->raid_bdev;
	if (raid_bdev == NULL) {
		return;
	}

	assert(raid_bdev->base_bdev_info != NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->bdev) {
			raid_bdev_free_base_bdev_resource(raid_bdev, base_info);
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
	g_rpc_req = NULL;
	g_rpc_req_size = 0;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
		     uint64_t len)
{
	cb(bdev_io->internal.ch->channel, bdev_io, true);
}

/* Store the IO completion status in global variable to verify by various tests */
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_io_comp_status = ((status == SPDK_BDEV_IO_STATUS_SUCCESS) ? true : false);
}

static void
set_io_output(struct io_output *output,
	      struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	      uint64_t offset_blocks, uint64_t num_blocks,
	      spdk_bdev_io_completion_cb cb, void *cb_arg,
	      enum spdk_bdev_io_type iotype)
{
	output->desc = desc;
	output->ch = ch;
	output->offset_blocks = offset_blocks;
	output->num_blocks = num_blocks;
	output->cb = cb;
	output->cb_arg = cb_arg;
	output->iotype = iotype;
}

/* It will cache the split IOs for verification */
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *output = &g_io_output[g_io_output_index];
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
		set_io_output(output, desc, ch, offset_blocks, num_blocks, cb, cb_arg,
			      SPDK_BDEV_IO_TYPE_WRITE);
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
	struct io_output *output = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	if (g_ignore_io_output) {
		return 0;
	}

	if (g_bdev_io_submit_status == 0) {
		set_io_output(output, desc, ch, 0, 0, cb, cb_arg, SPDK_BDEV_IO_TYPE_RESET);
		g_io_output_index++;

		child_io = calloc(1, sizeof(struct spdk_bdev_io));
		SPDK_CU_ASSERT_FATAL(child_io != NULL);
		cb(child_io, g_child_io_status_flag, cb_arg);
	}

	return g_bdev_io_submit_status;
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *output = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	if (g_ignore_io_output) {
		return 0;
	}

	if (g_bdev_io_submit_status == 0) {
		set_io_output(output, desc, ch, offset_blocks, num_blocks, cb, cb_arg,
			      SPDK_BDEV_IO_TYPE_UNMAP);
		g_io_output_index++;

		child_io = calloc(1, sizeof(struct spdk_bdev_io));
		SPDK_CU_ASSERT_FATAL(child_io != NULL);
		cb(child_io, g_child_io_status_flag, cb_arg);
	}

	return g_bdev_io_submit_status;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	bdev->fn_table->destruct(bdev->ctxt);

	if (cb_fn) {
		cb_fn(cb_arg, 0);
	}
}

int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		   void *event_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		return -ENODEV;
	}

	*_desc = (void *)bdev;
	return 0;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return (void *)desc;
}

char *
spdk_sprintf_alloc(const char *format, ...)
{
	return strdup(format);
}

int spdk_json_write_named_uint32(struct spdk_json_write_ctx *w, const char *name, uint32_t val)
{
	struct rpc_bdev_raid_create *req = g_rpc_req;
	if (strcmp(name, "strip_size_kb") == 0) {
		CU_ASSERT(req->strip_size_kb == val);
	} else if (strcmp(name, "blocklen_shift") == 0) {
		CU_ASSERT(spdk_u32log2(g_block_len) == val);
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
	struct rpc_bdev_raid_create *req = g_rpc_req;
	if (strcmp(name, "raid_level") == 0) {
		CU_ASSERT(strcmp(val, raid_bdev_level_to_str(req->level)) == 0);
	}
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
	struct io_output *output = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	if (g_ignore_io_output) {
		return 0;
	}

	SPDK_CU_ASSERT_FATAL(g_io_output_index <= (g_max_io_size / g_strip_size) + 1);
	if (g_bdev_io_submit_status == 0) {
		set_io_output(output, desc, ch, offset_blocks, num_blocks, cb, cb_arg,
			      SPDK_BDEV_IO_TYPE_READ);
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
spdk_json_decode_object(const struct spdk_json_val *values,
			const struct spdk_json_object_decoder *decoders, size_t num_decoders,
			void *out)
{
	struct rpc_bdev_raid_create *req, *_out;
	size_t i;

	if (g_json_decode_obj_err) {
		return -1;
	} else if (g_json_decode_obj_create) {
		req = g_rpc_req;
		_out = out;

		_out->name = strdup(req->name);
		SPDK_CU_ASSERT_FATAL(_out->name != NULL);
		_out->strip_size_kb = req->strip_size_kb;
		_out->level = req->level;
		_out->base_bdevs.num_base_bdevs = req->base_bdevs.num_base_bdevs;
		for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
			_out->base_bdevs.base_bdevs[i] = strdup(req->base_bdevs.base_bdevs[i]);
			SPDK_CU_ASSERT_FATAL(_out->base_bdevs.base_bdevs[i]);
		}
	} else {
		memcpy(out, g_rpc_req, g_rpc_req_size);
	}

	return 0;
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	return (void *)1;
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

static void
bdev_io_cleanup(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io->u.bdev.iovs) {
		if (bdev_io->u.bdev.iovs->iov_base) {
			free(bdev_io->u.bdev.iovs->iov_base);
		}
		free(bdev_io->u.bdev.iovs);
	}
	free(bdev_io);
}

static void
bdev_io_initialize(struct spdk_bdev_io *bdev_io, struct spdk_io_channel *ch, struct spdk_bdev *bdev,
		   uint64_t lba, uint64_t blocks, int16_t iotype)
{
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io->bdev = bdev;
	bdev_io->u.bdev.offset_blocks = lba;
	bdev_io->u.bdev.num_blocks = blocks;
	bdev_io->type = iotype;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_UNMAP || bdev_io->type == SPDK_BDEV_IO_TYPE_FLUSH) {
		return;
	}

	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.iovs = calloc(1, sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iovs != NULL);
	bdev_io->u.bdev.iovs->iov_base = calloc(1, bdev_io->u.bdev.num_blocks * g_block_len);
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iovs->iov_base != NULL);
	bdev_io->u.bdev.iovs->iov_len = bdev_io->u.bdev.num_blocks * g_block_len;
	bdev_io->internal.ch = channel;
}

static void
verify_reset_io(struct spdk_bdev_io *bdev_io, uint8_t num_base_drives,
		struct raid_bdev_io_channel *ch_ctx, struct raid_bdev *raid_bdev, uint32_t io_status)
{
	uint8_t index = 0;
	struct io_output *output;

	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);
	SPDK_CU_ASSERT_FATAL(num_base_drives != 0);
	SPDK_CU_ASSERT_FATAL(io_status != INVALID_IO_SUBMIT);
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_channel != NULL);

	CU_ASSERT(g_io_output_index == num_base_drives);
	for (index = 0; index < g_io_output_index; index++) {
		output = &g_io_output[index];
		CU_ASSERT(ch_ctx->base_channel[index] == output->ch);
		CU_ASSERT(raid_bdev->base_bdev_info[index].desc == output->desc);
		CU_ASSERT(bdev_io->type == output->iotype);
	}
	CU_ASSERT(g_io_comp_status == io_status);
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
	uint8_t pd_idx;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint64_t pd_blocks;
	uint32_t index = 0;
	struct io_output *output;

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
		output = &g_io_output[index];
		CU_ASSERT(pd_lba == output->offset_blocks);
		CU_ASSERT(pd_blocks == output->num_blocks);
		CU_ASSERT(ch_ctx->base_channel[pd_idx] == output->ch);
		CU_ASSERT(raid_bdev->base_bdev_info[pd_idx].desc == output->desc);
		CU_ASSERT(bdev_io->type == output->iotype);
	}
	CU_ASSERT(g_io_comp_status == io_status);
}

static void
verify_io_without_payload(struct spdk_bdev_io *bdev_io, uint8_t num_base_drives,
			  struct raid_bdev_io_channel *ch_ctx, struct raid_bdev *raid_bdev,
			  uint32_t io_status)
{
	uint32_t strip_shift = spdk_u32log2(g_strip_size);
	uint64_t start_offset_in_strip = bdev_io->u.bdev.offset_blocks % g_strip_size;
	uint64_t end_offset_in_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) %
				       g_strip_size;
	uint64_t start_strip = bdev_io->u.bdev.offset_blocks >> strip_shift;
	uint64_t end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
			     strip_shift;
	uint8_t n_disks_involved;
	uint64_t start_strip_disk_idx;
	uint64_t end_strip_disk_idx;
	uint64_t nblocks_in_start_disk;
	uint64_t offset_in_start_disk;
	uint8_t disk_idx;
	uint64_t base_io_idx;
	uint64_t sum_nblocks = 0;
	struct io_output *output;

	if (io_status == INVALID_IO_SUBMIT) {
		CU_ASSERT(g_io_comp_status == false);
		return;
	}
	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);
	SPDK_CU_ASSERT_FATAL(num_base_drives != 0);
	SPDK_CU_ASSERT_FATAL(bdev_io->type != SPDK_BDEV_IO_TYPE_READ);
	SPDK_CU_ASSERT_FATAL(bdev_io->type != SPDK_BDEV_IO_TYPE_WRITE);

	n_disks_involved = spdk_min(end_strip - start_strip + 1, num_base_drives);
	CU_ASSERT(n_disks_involved == g_io_output_index);

	start_strip_disk_idx = start_strip % num_base_drives;
	end_strip_disk_idx = end_strip % num_base_drives;

	offset_in_start_disk = g_io_output[0].offset_blocks;
	nblocks_in_start_disk = g_io_output[0].num_blocks;

	for (base_io_idx = 0, disk_idx = start_strip_disk_idx; base_io_idx < n_disks_involved;
	     base_io_idx++, disk_idx++) {
		uint64_t start_offset_in_disk;
		uint64_t end_offset_in_disk;

		output = &g_io_output[base_io_idx];

		/* round disk_idx */
		if (disk_idx >= num_base_drives) {
			disk_idx %= num_base_drives;
		}

		/* start_offset_in_disk aligned in strip check:
		 * The first base io has a same start_offset_in_strip with the whole raid io.
		 * Other base io should have aligned start_offset_in_strip which is 0.
		 */
		start_offset_in_disk = output->offset_blocks;
		if (base_io_idx == 0) {
			CU_ASSERT(start_offset_in_disk % g_strip_size == start_offset_in_strip);
		} else {
			CU_ASSERT(start_offset_in_disk % g_strip_size == 0);
		}

		/* end_offset_in_disk aligned in strip check:
		 * Base io on disk at which end_strip is located, has a same end_offset_in_strip
		 * with the whole raid io.
		 * Other base io should have aligned end_offset_in_strip.
		 */
		end_offset_in_disk = output->offset_blocks + output->num_blocks - 1;
		if (disk_idx == end_strip_disk_idx) {
			CU_ASSERT(end_offset_in_disk % g_strip_size == end_offset_in_strip);
		} else {
			CU_ASSERT(end_offset_in_disk % g_strip_size == g_strip_size - 1);
		}

		/* start_offset_in_disk compared with start_disk.
		 * 1. For disk_idx which is larger than start_strip_disk_idx: Its start_offset_in_disk
		 *    mustn't be larger than the start offset of start_offset_in_disk; And the gap
		 *    must be less than strip size.
		 * 2. For disk_idx which is less than start_strip_disk_idx, Its start_offset_in_disk
		 *    must be larger than the start offset of start_offset_in_disk; And the gap mustn't
		 *    be less than strip size.
		 */
		if (disk_idx > start_strip_disk_idx) {
			CU_ASSERT(start_offset_in_disk <= offset_in_start_disk);
			CU_ASSERT(offset_in_start_disk - start_offset_in_disk < g_strip_size);
		} else if (disk_idx < start_strip_disk_idx) {
			CU_ASSERT(start_offset_in_disk > offset_in_start_disk);
			CU_ASSERT(output->offset_blocks - offset_in_start_disk <= g_strip_size);
		}

		/* nblocks compared with start_disk:
		 * The gap between them must be within a strip size.
		 */
		if (output->num_blocks <= nblocks_in_start_disk) {
			CU_ASSERT(nblocks_in_start_disk - output->num_blocks <= g_strip_size);
		} else {
			CU_ASSERT(output->num_blocks - nblocks_in_start_disk < g_strip_size);
		}

		sum_nblocks += output->num_blocks;

		CU_ASSERT(ch_ctx->base_channel[disk_idx] == output->ch);
		CU_ASSERT(raid_bdev->base_bdev_info[disk_idx].desc == output->desc);
		CU_ASSERT(bdev_io->type == output->iotype);
	}

	/* Sum of each nblocks should be same with raid bdev_io */
	CU_ASSERT(bdev_io->u.bdev.num_blocks == sum_nblocks);

	CU_ASSERT(g_io_comp_status == io_status);
}

static void
verify_raid_config_present(const char *name, bool presence)
{
	struct raid_bdev_config *raid_cfg;
	bool cfg_found;

	cfg_found = false;

	TAILQ_FOREACH(raid_cfg, &g_raid_config.raid_bdev_config_head, link) {
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
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
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
verify_raid_config(struct rpc_bdev_raid_create *r, bool presence)
{
	struct raid_bdev_config *raid_cfg = NULL;
	uint8_t i;
	int val;

	TAILQ_FOREACH(raid_cfg, &g_raid_config.raid_bdev_config_head, link) {
		if (strcmp(r->name, raid_cfg->name) == 0) {
			if (presence == false) {
				break;
			}
			CU_ASSERT(raid_cfg->raid_bdev != NULL);
			CU_ASSERT(raid_cfg->strip_size == r->strip_size_kb);
			CU_ASSERT(raid_cfg->num_base_bdevs == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(raid_cfg->level == r->level);
			if (raid_cfg->base_bdev != NULL) {
				for (i = 0; i < raid_cfg->num_base_bdevs; i++) {
					val = strcmp(raid_cfg->base_bdev[i].name,
						     r->base_bdevs.base_bdevs[i]);
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
verify_raid_bdev(struct rpc_bdev_raid_create *r, bool presence, uint32_t raid_state)
{
	struct raid_bdev *pbdev;
	struct raid_base_bdev_info *base_info;
	struct spdk_bdev *bdev = NULL;
	bool   pbdev_found;
	uint64_t min_blockcnt = 0xFFFFFFFFFFFFFFFF;

	pbdev_found = false;
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, r->name) == 0) {
			pbdev_found = true;
			if (presence == false) {
				break;
			}
			CU_ASSERT(pbdev->config->raid_bdev == pbdev);
			CU_ASSERT(pbdev->base_bdev_info != NULL);
			CU_ASSERT(pbdev->strip_size == ((r->strip_size_kb * 1024) / g_block_len));
			CU_ASSERT(pbdev->strip_size_shift == spdk_u32log2(((r->strip_size_kb * 1024) /
					g_block_len)));
			CU_ASSERT(pbdev->blocklen_shift == spdk_u32log2(g_block_len));
			CU_ASSERT((uint32_t)pbdev->state == raid_state);
			CU_ASSERT(pbdev->num_base_bdevs == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pbdev->num_base_bdevs_discovered == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pbdev->level == r->level);
			CU_ASSERT(pbdev->destruct_called == false);
			CU_ASSERT(pbdev->base_bdev_info != NULL);
			RAID_FOR_EACH_BASE_BDEV(pbdev, base_info) {
				CU_ASSERT(base_info->bdev != NULL);
				bdev = spdk_bdev_get_by_name(base_info->bdev->name);
				CU_ASSERT(bdev != NULL);
				CU_ASSERT(base_info->remove_scheduled == false);

				if (bdev && bdev->blockcnt < min_blockcnt) {
					min_blockcnt = bdev->blockcnt;
				}
			}
			CU_ASSERT((((min_blockcnt / (r->strip_size_kb * 1024 / g_block_len)) *
				    (r->strip_size_kb * 1024 / g_block_len)) *
				   r->base_bdevs.num_base_bdevs) == pbdev->bdev.blockcnt);
			CU_ASSERT(strcmp(pbdev->bdev.product_name, "Raid Volume") == 0);
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
		TAILQ_FOREACH(pbdev, &g_raid_bdev_configured_list, state_link) {
			if (strcmp(pbdev->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	} else if (raid_state == RAID_BDEV_STATE_CONFIGURING) {
		TAILQ_FOREACH(pbdev, &g_raid_bdev_configuring_list, state_link) {
			if (strcmp(pbdev->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	} else if (raid_state == RAID_BDEV_STATE_OFFLINE) {
		TAILQ_FOREACH(pbdev, &g_raid_bdev_offline_list, state_link) {
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

static void
verify_get_raids(struct rpc_bdev_raid_create *construct_req,
		 uint8_t g_max_raids,
		 char **g_get_raids_output, uint32_t g_get_raids_count)
{
	uint8_t i, j;
	bool found;

	CU_ASSERT(g_max_raids == g_get_raids_count);
	if (g_max_raids == g_get_raids_count) {
		for (i = 0; i < g_max_raids; i++) {
			found = false;
			for (j = 0; j < g_max_raids; j++) {
				if (construct_req[i].name &&
				    strcmp(construct_req[i].name, g_get_raids_output[i]) == 0) {
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
	uint8_t i;
	struct spdk_bdev *base_bdev;
	char name[16];

	for (i = 0; i < g_max_base_drives; i++, bbdev_start_idx++) {
		snprintf(name, 16, "%s%u%s", "Nvme", bbdev_start_idx, "n1");
		base_bdev = calloc(1, sizeof(struct spdk_bdev));
		SPDK_CU_ASSERT_FATAL(base_bdev != NULL);
		base_bdev->name = strdup(name);
		SPDK_CU_ASSERT_FATAL(base_bdev->name != NULL);
		base_bdev->blocklen = g_block_len;
		base_bdev->blockcnt = BLOCK_CNT;
		TAILQ_INSERT_TAIL(&g_bdev_list, base_bdev, internal.link);
	}
}

static void
create_test_req(struct rpc_bdev_raid_create *r, const char *raid_name,
		uint8_t bbdev_start_idx, bool create_base_bdev)
{
	uint8_t i;
	char name[16];
	uint8_t bbdev_idx = bbdev_start_idx;

	r->name = strdup(raid_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);
	r->strip_size_kb = (g_strip_size * g_block_len) / 1024;
	r->level = RAID0;
	r->base_bdevs.num_base_bdevs = g_max_base_drives;
	for (i = 0; i < g_max_base_drives; i++, bbdev_idx++) {
		snprintf(name, 16, "%s%u%s", "Nvme", bbdev_idx, "n1");
		r->base_bdevs.base_bdevs[i] = strdup(name);
		SPDK_CU_ASSERT_FATAL(r->base_bdevs.base_bdevs[i] != NULL);
	}
	if (create_base_bdev == true) {
		create_base_bdevs(bbdev_start_idx);
	}
	g_rpc_req = r;
	g_rpc_req_size = sizeof(*r);
}

static void
create_raid_bdev_create_req(struct rpc_bdev_raid_create *r, const char *raid_name,
			    uint8_t bbdev_start_idx, bool create_base_bdev,
			    uint8_t json_decode_obj_err)
{
	create_test_req(r, raid_name, bbdev_start_idx, create_base_bdev);

	g_rpc_err = 0;
	g_json_decode_obj_create = 1;
	g_json_decode_obj_err = json_decode_obj_err;
	g_config_level_create = 0;
	g_test_multi_raids = 0;
}

static void
free_test_req(struct rpc_bdev_raid_create *r)
{
	uint8_t i;

	free(r->name);
	for (i = 0; i < r->base_bdevs.num_base_bdevs; i++) {
		free(r->base_bdevs.base_bdevs[i]);
	}
}

static void
create_raid_bdev_delete_req(struct rpc_bdev_raid_delete *r, const char *raid_name,
			    uint8_t json_decode_obj_err)
{
	r->name = strdup(raid_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);

	g_rpc_req = r;
	g_rpc_req_size = sizeof(*r);
	g_rpc_err = 0;
	g_json_decode_obj_create = 0;
	g_json_decode_obj_err = json_decode_obj_err;
	g_config_level_create = 0;
	g_test_multi_raids = 0;
}

static void
create_get_raids_req(struct rpc_bdev_raid_get_bdevs *r, const char *category,
		     uint8_t json_decode_obj_err)
{
	r->category = strdup(category);
	SPDK_CU_ASSERT_FATAL(r->category != NULL);

	g_rpc_req = r;
	g_rpc_req_size = sizeof(*r);
	g_rpc_err = 0;
	g_json_decode_obj_create = 0;
	g_json_decode_obj_err = json_decode_obj_err;
	g_config_level_create = 0;
	g_test_multi_raids = 1;
	g_get_raids_count = 0;
}

static void
test_create_raid(void)
{
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete delete_req;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&req);

	create_raid_bdev_delete_req(&delete_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_delete_raid(void)
{
	struct rpc_bdev_raid_create construct_req;
	struct rpc_bdev_raid_delete delete_req;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&construct_req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&construct_req, true);
	verify_raid_bdev(&construct_req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&construct_req);

	create_raid_bdev_delete_req(&delete_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_create_raid_invalid_args(void)
{
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev_config *raid_cfg;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	req.level = INVALID_RAID_LEVEL;
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_raid_bdev_create_req(&req, "raid1", 0, false, 1);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_raid_bdev_create_req(&req, "raid1", 0, false, 0);
	req.strip_size_kb = 1231;
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	create_raid_bdev_create_req(&req, "raid1", 0, false, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&req);

	create_raid_bdev_create_req(&req, "raid1", 0, false, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);

	create_raid_bdev_create_req(&req, "raid2", 0, false, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid2", false);
	verify_raid_bdev_present("raid2", false);

	create_raid_bdev_create_req(&req, "raid2", g_max_base_drives, true, 0);
	free(req.base_bdevs.base_bdevs[g_max_base_drives - 1]);
	req.base_bdevs.base_bdevs[g_max_base_drives - 1] = strdup("Nvme0n1");
	SPDK_CU_ASSERT_FATAL(req.base_bdevs.base_bdevs[g_max_base_drives - 1] != NULL);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free_test_req(&req);
	verify_raid_config_present("raid2", false);
	verify_raid_bdev_present("raid2", false);

	create_raid_bdev_create_req(&req, "raid2", g_max_base_drives, true, 0);
	free(req.base_bdevs.base_bdevs[g_max_base_drives - 1]);
	req.base_bdevs.base_bdevs[g_max_base_drives - 1] = strdup("Nvme100000n1");
	SPDK_CU_ASSERT_FATAL(req.base_bdevs.base_bdevs[g_max_base_drives - 1] != NULL);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	free_test_req(&req);
	verify_raid_config_present("raid2", true);
	verify_raid_bdev_present("raid2", true);
	raid_cfg = raid_bdev_config_find_by_name("raid2");
	SPDK_CU_ASSERT_FATAL(raid_cfg != NULL);
	check_and_remove_raid_bdev(raid_cfg);
	raid_bdev_config_cleanup(raid_cfg);

	create_raid_bdev_create_req(&req, "raid2", g_max_base_drives, false, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	free_test_req(&req);
	verify_raid_config_present("raid2", true);
	verify_raid_bdev_present("raid2", true);
	verify_raid_config_present("raid1", true);
	verify_raid_bdev_present("raid1", true);

	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	create_raid_bdev_delete_req(&destroy_req, "raid2", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_delete_raid_invalid_args(void)
{
	struct rpc_bdev_raid_create construct_req;
	struct rpc_bdev_raid_delete destroy_req;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&construct_req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&construct_req, true);
	verify_raid_bdev(&construct_req, true, RAID_BDEV_STATE_ONLINE);
	free_test_req(&construct_req);

	create_raid_bdev_delete_req(&destroy_req, "raid2", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);

	create_raid_bdev_delete_req(&destroy_req, "raid1", 1);
	rpc_bdev_raid_delete(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free(destroy_req.name);
	verify_raid_config_present("raid1", true);
	verify_raid_bdev_present("raid1", true);

	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
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
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;
	struct raid_bdev_io_channel *ch_ctx;
	uint8_t i;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);

	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, "raid1") == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch_ctx = calloc(1, sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == &g_io_channel);
	}
	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free_test_req(&req);

	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
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
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint8_t i;
	struct spdk_bdev_io *bdev_io;
	uint64_t io_len;
	uint64_t lba = 0;
	struct spdk_io_channel *ch_b;
	struct spdk_bdev_channel *ch_b_ctx;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, "raid1") == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ch_b = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct spdk_bdev_channel));
	SPDK_CU_ASSERT_FATAL(ch_b != NULL);
	ch_b_ctx = spdk_io_channel_get_ctx(ch_b);
	ch_b_ctx->channel = ch;

	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == &g_io_channel);
	}

	/* test 2 IO sizes based on global strip size set earlier */
	for (i = 0; i < 2; i++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (g_strip_size / 2) << i;
		bdev_io_initialize(bdev_io, ch_b, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
	}

	free_test_req(&req);
	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	free(ch_b);
	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
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
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint8_t i;
	struct spdk_bdev_io *bdev_io;
	uint64_t io_len;
	uint64_t lba;
	struct spdk_io_channel *ch_b;
	struct spdk_bdev_channel *ch_b_ctx;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, "raid1") == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ch_b = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct spdk_bdev_channel));
	SPDK_CU_ASSERT_FATAL(ch_b != NULL);
	ch_b_ctx = spdk_io_channel_get_ctx(ch_b);
	ch_b_ctx->channel = ch;

	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == &g_io_channel);
	}
	free_test_req(&req);

	/* test 2 IO sizes based on global strip size set earlier */
	lba = 0;
	for (i = 0; i < 2; i++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (g_strip_size / 2) << i;
		bdev_io_initialize(bdev_io, ch_b, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_READ);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
	}

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	free(ch_b);
	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
raid_bdev_io_generate_by_strips(uint64_t n_strips)
{
	uint64_t lba;
	uint64_t nblocks;
	uint64_t start_offset;
	uint64_t end_offset;
	uint64_t offsets_in_strip[3];
	uint64_t start_bdev_idx;
	uint64_t start_bdev_offset;
	uint64_t start_bdev_idxs[3];
	int i, j, l;

	/* 3 different situations of offset in strip */
	offsets_in_strip[0] = 0;
	offsets_in_strip[1] = g_strip_size >> 1;
	offsets_in_strip[2] = g_strip_size - 1;

	/* 3 different situations of start_bdev_idx */
	start_bdev_idxs[0] = 0;
	start_bdev_idxs[1] = g_max_base_drives >> 1;
	start_bdev_idxs[2] = g_max_base_drives - 1;

	/* consider different offset in strip */
	for (i = 0; i < 3; i++) {
		start_offset = offsets_in_strip[i];
		for (j = 0; j < 3; j++) {
			end_offset = offsets_in_strip[j];
			if (n_strips == 1 && start_offset > end_offset) {
				continue;
			}

			/* consider at which base_bdev lba is started. */
			for (l = 0; l < 3; l++) {
				start_bdev_idx = start_bdev_idxs[l];
				start_bdev_offset = start_bdev_idx * g_strip_size;
				lba = g_lba_offset + start_bdev_offset + start_offset;
				nblocks = (n_strips - 1) * g_strip_size + end_offset - start_offset + 1;

				g_io_ranges[g_io_range_idx].lba = lba;
				g_io_ranges[g_io_range_idx].nblocks = nblocks;

				SPDK_CU_ASSERT_FATAL(g_io_range_idx < MAX_TEST_IO_RANGE);
				g_io_range_idx++;
			}
		}
	}
}

static void
raid_bdev_io_generate(void)
{
	uint64_t n_strips;
	uint64_t n_strips_span = g_max_base_drives;
	uint64_t n_strips_times[5] = {g_max_base_drives + 1, g_max_base_drives * 2 - 1,
				      g_max_base_drives * 2, g_max_base_drives * 3,
				      g_max_base_drives * 4
				     };
	uint32_t i;

	g_io_range_idx = 0;

	/* consider different number of strips from 1 to strips spanned base bdevs,
	 * and even to times of strips spanned base bdevs
	 */
	for (n_strips = 1; n_strips < n_strips_span; n_strips++) {
		raid_bdev_io_generate_by_strips(n_strips);
	}

	for (i = 0; i < SPDK_COUNTOF(n_strips_times); i++) {
		n_strips = n_strips_times[i];
		raid_bdev_io_generate_by_strips(n_strips);
	}
}

static void
test_unmap_io(void)
{
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint8_t i;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, "raid1") == 0) {
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
		SPDK_CU_ASSERT_FATAL(ch_ctx->base_channel && ch_ctx->base_channel[i] == &g_io_channel);
	}

	CU_ASSERT(raid_bdev_io_type_supported(pbdev, SPDK_BDEV_IO_TYPE_UNMAP) == true);
	CU_ASSERT(raid_bdev_io_type_supported(pbdev, SPDK_BDEV_IO_TYPE_FLUSH) == true);

	raid_bdev_io_generate();
	for (count = 0; count < g_io_range_idx; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = g_io_ranges[count].nblocks;
		lba = g_io_ranges[count].lba;
		bdev_io_initialize(bdev_io, ch, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_UNMAP);
		memset(g_io_output, 0, g_max_base_drives * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io_without_payload(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
					  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
	}
	free_test_req(&req);

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
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
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint8_t i;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
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
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == &g_io_channel);
	}
	free_test_req(&req);

	lba = 0;
	for (count = 0; count < 1; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (g_strip_size / 2) << count;
		bdev_io_initialize(bdev_io, ch, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_INVALID);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  INVALID_IO_SUBMIT);
		bdev_io_cleanup(bdev_io);
	}


	lba = 0;
	g_child_io_status_flag = false;
	for (count = 0; count < 1; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (g_strip_size / 2) << count;
		bdev_io_initialize(bdev_io, ch, &pbdev->bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
	}

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);

	raid_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Test reset IO */
static void
test_reset_io(void)
{
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx;
	uint8_t i;
	struct spdk_bdev_io *bdev_io;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_config(&req, true);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, "raid1") == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	SPDK_CU_ASSERT_FATAL(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		CU_ASSERT(ch_ctx->base_channel && ch_ctx->base_channel[i] == &g_io_channel);
	}
	free_test_req(&req);

	g_bdev_io_submit_status = 0;
	g_child_io_status_flag = true;

	CU_ASSERT(raid_bdev_io_type_supported(pbdev, SPDK_BDEV_IO_TYPE_RESET) == true);

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io_initialize(bdev_io, ch, &pbdev->bdev, 0, 1, SPDK_BDEV_IO_TYPE_RESET);
	memset(g_io_output, 0, g_max_base_drives * sizeof(struct io_output));
	g_io_output_index = 0;
	raid_bdev_submit_request(ch, bdev_io);
	verify_reset_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, pbdev,
			true);
	bdev_io_cleanup(bdev_io);

	raid_bdev_destroy_cb(pbdev, ch_ctx);
	CU_ASSERT(ch_ctx->base_channel == NULL);
	free(ch);
	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
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
	struct rpc_bdev_raid_create *construct_req;
	struct rpc_bdev_raid_delete destroy_req;
	struct rpc_bdev_raid_get_bdevs get_raids_req;
	uint8_t i;
	char name[16];
	uint8_t bbdev_idx = 0;

	set_globals();
	construct_req = calloc(MAX_RAIDS, sizeof(struct rpc_bdev_raid_create));
	SPDK_CU_ASSERT_FATAL(construct_req != NULL);
	CU_ASSERT(raid_bdev_init() == 0);
	for (i = 0; i < g_max_raids; i++) {
		snprintf(name, 16, "%s%u", "raid", i);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
		create_raid_bdev_create_req(&construct_req[i], name, bbdev_idx, true, 0);
		bbdev_idx += g_max_base_drives;
		rpc_bdev_raid_create(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config(&construct_req[i], true);
		verify_raid_bdev(&construct_req[i], true, RAID_BDEV_STATE_ONLINE);
	}

	create_get_raids_req(&get_raids_req, "all", 0);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_get_raids(construct_req, g_max_raids, g_get_raids_output, g_get_raids_count);
	for (i = 0; i < g_get_raids_count; i++) {
		free(g_get_raids_output[i]);
	}

	create_get_raids_req(&get_raids_req, "online", 0);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_get_raids(construct_req, g_max_raids, g_get_raids_output, g_get_raids_count);
	for (i = 0; i < g_get_raids_count; i++) {
		free(g_get_raids_output[i]);
	}

	create_get_raids_req(&get_raids_req, "configuring", 0);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_raids_count == 0);

	create_get_raids_req(&get_raids_req, "offline", 0);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_raids_count == 0);

	create_get_raids_req(&get_raids_req, "invalid_category", 0);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	CU_ASSERT(g_get_raids_count == 0);

	create_get_raids_req(&get_raids_req, "all", 1);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	free(get_raids_req.category);
	CU_ASSERT(g_get_raids_count == 0);

	create_get_raids_req(&get_raids_req, "all", 0);
	rpc_bdev_raid_get_bdevs(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_raids_count == g_max_raids);
	for (i = 0; i < g_get_raids_count; i++) {
		free(g_get_raids_output[i]);
	}

	for (i = 0; i < g_max_raids; i++) {
		SPDK_CU_ASSERT_FATAL(construct_req[i].name != NULL);
		snprintf(name, 16, "%s", construct_req[i].name);
		create_raid_bdev_delete_req(&destroy_req, name, 0);
		rpc_bdev_raid_delete(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
	}
	raid_bdev_exit();
	for (i = 0; i < g_max_raids; i++) {
		free_test_req(&construct_req[i]);
	}
	free(construct_req);
	base_bdevs_cleanup();
	reset_globals();
}

/* Create multiple raids, fire IOs on raids */
static void
test_multi_raid_with_io(void)
{
	struct rpc_bdev_raid_create *construct_req;
	struct rpc_bdev_raid_delete destroy_req;
	uint8_t i, j;
	char name[16];
	uint8_t bbdev_idx = 0;
	struct raid_bdev *pbdev;
	struct spdk_io_channel *ch;
	struct raid_bdev_io_channel *ch_ctx = NULL;
	struct spdk_bdev_io *bdev_io;
	uint64_t io_len;
	uint64_t lba = 0;
	int16_t iotype;
	struct spdk_io_channel *ch_b;
	struct spdk_bdev_channel *ch_b_ctx;

	set_globals();
	construct_req = calloc(g_max_raids, sizeof(struct rpc_bdev_raid_create));
	SPDK_CU_ASSERT_FATAL(construct_req != NULL);
	CU_ASSERT(raid_bdev_init() == 0);
	ch = calloc(g_max_raids, sizeof(struct spdk_io_channel) + sizeof(struct raid_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ch_b = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct spdk_bdev_channel));
	SPDK_CU_ASSERT_FATAL(ch_b != NULL);
	ch_b_ctx = spdk_io_channel_get_ctx(ch_b);
	ch_b_ctx->channel = ch;

	for (i = 0; i < g_max_raids; i++) {
		snprintf(name, 16, "%s%u", "raid", i);
		verify_raid_config_present(name, false);
		verify_raid_bdev_present(name, false);
		create_raid_bdev_create_req(&construct_req[i], name, bbdev_idx, true, 0);
		bbdev_idx += g_max_base_drives;
		rpc_bdev_raid_create(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_raid_config(&construct_req[i], true);
		verify_raid_bdev(&construct_req[i], true, RAID_BDEV_STATE_ONLINE);
		TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
			if (strcmp(pbdev->bdev.name, construct_req[i].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev != NULL);
		ch_ctx = spdk_io_channel_get_ctx(&ch[i]);
		SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);
		CU_ASSERT(raid_bdev_create_cb(pbdev, ch_ctx) == 0);
		SPDK_CU_ASSERT_FATAL(ch_ctx->base_channel != NULL);
		for (j = 0; j < construct_req[i].base_bdevs.num_base_bdevs; j++) {
			CU_ASSERT(ch_ctx->base_channel[j] == &g_io_channel);
		}
	}

	/* This will perform a write on the first raid and a read on the second. It can be
	 * expanded in the future to perform r/w on each raid device in the event that
	 * multiple raid levels are supported.
	 */
	for (i = 0; i < g_max_raids; i++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = g_strip_size;
		iotype = (i) ? SPDK_BDEV_IO_TYPE_WRITE : SPDK_BDEV_IO_TYPE_READ;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
			if (strcmp(pbdev->bdev.name, construct_req[i].name) == 0) {
				break;
			}
		}
		bdev_io_initialize(bdev_io, ch_b, &pbdev->bdev, lba, io_len, iotype);
		CU_ASSERT(pbdev != NULL);
		raid_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, g_max_base_drives, ch_ctx, pbdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
	}

	for (i = 0; i < g_max_raids; i++) {
		TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
			if (strcmp(pbdev->bdev.name, construct_req[i].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev != NULL);
		ch_ctx = spdk_io_channel_get_ctx(&ch[i]);
		SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);
		raid_bdev_destroy_cb(pbdev, ch_ctx);
		CU_ASSERT(ch_ctx->base_channel == NULL);
		snprintf(name, 16, "%s", construct_req[i].name);
		create_raid_bdev_delete_req(&destroy_req, name, 0);
		rpc_bdev_raid_delete(NULL, NULL);
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
	free(ch_b);
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_io_type_supported(void)
{
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_READ) == true);
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_WRITE) == true);
	CU_ASSERT(raid_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_INVALID) == false);
}

static void
test_raid_json_dump_info(void)
{
	struct rpc_bdev_raid_create req;
	struct rpc_bdev_raid_delete destroy_req;
	struct raid_bdev *pbdev;

	set_globals();
	CU_ASSERT(raid_bdev_init() == 0);

	verify_raid_config_present("raid1", false);
	verify_raid_bdev_present("raid1", false);
	create_raid_bdev_create_req(&req, "raid1", 0, true, 0);
	rpc_bdev_raid_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_raid_bdev(&req, true, RAID_BDEV_STATE_ONLINE);

	TAILQ_FOREACH(pbdev, &g_raid_bdev_list, global_link) {
		if (strcmp(pbdev->bdev.name, "raid1") == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev != NULL);

	CU_ASSERT(raid_bdev_dump_info_json(pbdev, NULL) == 0);

	free_test_req(&req);

	create_raid_bdev_delete_req(&destroy_req, "raid1", 0);
	rpc_bdev_raid_delete(NULL, NULL);
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
test_raid_level_conversions(void)
{
	const char *raid_str;

	CU_ASSERT(raid_bdev_parse_raid_level("abcd123") == INVALID_RAID_LEVEL);
	CU_ASSERT(raid_bdev_parse_raid_level("0") == RAID0);
	CU_ASSERT(raid_bdev_parse_raid_level("raid0") == RAID0);
	CU_ASSERT(raid_bdev_parse_raid_level("RAID0") == RAID0);

	raid_str = raid_bdev_level_to_str(INVALID_RAID_LEVEL);
	CU_ASSERT(raid_str != NULL && strlen(raid_str) == 0);
	raid_str = raid_bdev_level_to_str(1234);
	CU_ASSERT(raid_str != NULL && strlen(raid_str) == 0);
	raid_str = raid_bdev_level_to_str(RAID0);
	CU_ASSERT(raid_str != NULL && strcmp(raid_str, "raid0") == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite       suite = NULL;
	unsigned int    num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("raid", NULL, NULL);

	CU_ADD_TEST(suite, test_create_raid);
	CU_ADD_TEST(suite, test_delete_raid);
	CU_ADD_TEST(suite, test_create_raid_invalid_args);
	CU_ADD_TEST(suite, test_delete_raid_invalid_args);
	CU_ADD_TEST(suite, test_io_channel);
	CU_ADD_TEST(suite, test_reset_io);
	CU_ADD_TEST(suite, test_write_io);
	CU_ADD_TEST(suite, test_read_io);
	CU_ADD_TEST(suite, test_unmap_io);
	CU_ADD_TEST(suite, test_io_failure);
	CU_ADD_TEST(suite, test_multi_raid_no_io);
	CU_ADD_TEST(suite, test_multi_raid_with_io);
	CU_ADD_TEST(suite, test_io_type_supported);
	CU_ADD_TEST(suite, test_raid_json_dump_info);
	CU_ADD_TEST(suite, test_context_size);
	CU_ADD_TEST(suite, test_raid_level_conversions);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	set_test_opts();
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
