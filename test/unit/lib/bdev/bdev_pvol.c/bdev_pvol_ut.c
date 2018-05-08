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
#include "spdk_internal/bdev.h"
#include "spdk/bdev.h"
#include "pvol/bdev_pvol.c"
#include "pvol/bdev_pvol_rpc.c"

#define MAX_BASE_DRIVES 255
#define MAX_PVOLS 31
#define INVALID_IO_SUBMIT 0xFFFF

/* Data structure to capture the output of IO for verification */
struct io_output {
	struct spdk_bdev_desc       *desc;
	struct spdk_io_channel      *ch;
	void                        *buf;
	uint64_t                    offset_blocks;
	uint64_t                    num_blocks;
	spdk_bdev_io_completion_cb  cb;
	void                        *cb_arg;
	enum spdk_bdev_io_type      iotype;
};

/* Different test options, more options to test can be added here */
uint32_t g_block_len_opts[] = {512, 4096};
uint32_t g_strip_size_opts[] = {64, 128, 256, 512, 1024, 2048};
uint32_t g_max_io_size_opts[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
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
uint32_t g_block_len;
uint32_t g_strip_size;
uint32_t g_max_io_size;
uint32_t g_max_qd;
uint8_t g_max_base_drives;
uint8_t g_max_pvols;
uint8_t g_ignore_io_output;
uint8_t g_rpc_err;
char *g_get_pvols_output[MAX_PVOLS];
uint32_t g_get_pvols_count;
uint8_t g_json_beg_res_ret_err;
uint8_t g_json_decode_obj_err;
uint8_t g_config_level_create = 0;

/* Set randomly test options, in every run it is different */
static void
set_test_opts(void)
{
	/* Generate random test options */
	srand(time(0));
	g_max_base_drives = (rand() % MAX_BASE_DRIVES) + 1;
	g_max_pvols = (rand() % MAX_PVOLS) + 1;
	g_block_len = g_block_len_opts[rand() % (sizeof(g_block_len_opts) / sizeof(g_block_len_opts[0]))];
	g_strip_size = g_strip_size_opts[rand() % (sizeof(g_strip_size_opts) / sizeof(
			       g_strip_size_opts[0]))];
	g_max_io_size = g_max_io_size_opts[rand() % (sizeof(g_max_io_size_opts) / sizeof(
			       g_max_io_size_opts[0]))];
	g_max_qd = g_max_qd_opts[rand() % (sizeof(g_max_qd_opts) / sizeof(g_max_qd_opts[0]))];

	printf("Test Options:\n");
	printf("blocklen = %u, strip_size = %u, max_io_size = %u, max_qd = %u, g_max_base_drives = %u, g_max_pvols = %u\n",
	       g_block_len, g_strip_size, g_max_io_size, g_max_qd, g_max_base_drives, g_max_pvols);
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
	memset(g_get_pvols_output, 0, sizeof(g_get_pvols_output));
	g_get_pvols_count = 0;
	g_io_comp_status = 0;
	g_ignore_io_output = 0;
	g_config_level_create = 0;
	g_rpc_err = 0;
	g_child_io_status_flag = true;
	TAILQ_INIT(&g_bdev_list);
	rpc_req = NULL;
	rpc_req_size = 0;
	g_json_beg_res_ret_err = 0;
	g_json_decode_obj_err = 0;
}

static void
base_bdevs_cleanup(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *bdev_next;

	if (!TAILQ_EMPTY(&g_bdev_list)) {
		TAILQ_FOREACH_SAFE(bdev, &g_bdev_list, link, bdev_next) {
			free(bdev->name);
			TAILQ_REMOVE(&g_bdev_list, bdev, link);
			free(bdev);
		}
	}
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

/* Store the IO completion status in global variable to verify by various tests */
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_io_comp_status = ((status == SPDK_BDEV_IO_STATUS_SUCCESS) ? true : false);
}

/* It will cache the split IOs for verification */
int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
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
		p->buf = buf;
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

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	bdev->fn_table->destruct(bdev->ctxt);
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
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
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size)
{
}

int
spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int spdk_json_write_named_uint32(struct spdk_json_write_ctx *w, const char *name, uint32_t val)
{
	struct rpc_construct_pvol  *req = rpc_req;
	if (strcmp(name, "strip_size") == 0) {
		CU_ASSERT(req->strip_size * 1024 / g_block_len == val);
	} else if (strcmp(name, "blocklen_shift") == 0) {
		CU_ASSERT(spdk_u32log2(g_block_len) == val);
	} else if (strcmp(name, "raid_level") == 0) {
		CU_ASSERT(req->raid_level == val);
	} else if (strcmp(name, "num_base_bdevs") == 0) {
		CU_ASSERT(req->base_bdevs.num_base_bdevs == val);
	} else if (strcmp(name, "state") == 0) {
		CU_ASSERT(val == PVOL_BDEV_STATE_ONLINE);
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

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

int
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io) {
		free(bdev_io);
	}

	return 0;
}

/* It will cache split IOs for verification */
int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
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
		p->buf = buf;
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
	CU_ASSERT(bdev->claim_module != NULL);
	bdev->claim_module = NULL;
}

void
spdk_bdev_module_finish_done(void)
{
	CU_ASSERT(g_pvol_bdev_io_waitq == NULL);
}

void
spdk_bdev_module_init_done(struct spdk_bdev_module *module)
{
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
	struct rpc_construct_pvol  *req = rpc_req;

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
	struct rpc_construct_pvol  *req = rpc_req;

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
	struct rpc_construct_pvol  *req = rpc_req;

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
	if (bdev->claim_module != NULL) {
		return -1;
	}
	bdev->claim_module = module;
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
	if (g_json_decode_obj_err) {
		return -1;
	} else {
		memcpy(out, rpc_req, rpc_req_size);
		return 0;
	}
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
	g_get_pvols_output[g_get_pvols_count] = strdup(val);
	SPDK_CU_ASSERT_FATAL(g_get_pvols_output[g_get_pvols_count] != NULL);
	g_get_pvols_count++;

	return 0;
}

struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	return NULL;
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
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
		TAILQ_FOREACH(bdev, &g_bdev_list, link) {
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
spdk_event_call(struct spdk_event *event)
{
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
	if (bdev_io->u.bdev.iovs->iov_base) {
		free(bdev_io->u.bdev.iovs->iov_base);
		bdev_io->u.bdev.iovs->iov_base = NULL;
	}
}

static void
bdev_io_initialize(struct spdk_bdev_io *bdev_io, uint64_t lba, uint64_t blocks, int16_t iotype)
{
	bdev_io->u.bdev.offset_blocks = lba;
	bdev_io->u.bdev.num_blocks = blocks;
	bdev_io->type = iotype;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.iov.iov_base = calloc(1, bdev_io->u.bdev.num_blocks * g_block_len);
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iov.iov_base != NULL);
	bdev_io->u.bdev.iov.iov_len = bdev_io->u.bdev.num_blocks * g_block_len;
	bdev_io->u.bdev.iovs = &bdev_io->u.bdev.iov;
}

static uint32_t
get_num_elts_in_waitq(struct pvol_bdev_io_waitq *waitq)
{
	struct pvol_bdev_io *b1;
	uint32_t count = 0;
	TAILQ_FOREACH(b1, &waitq->io_waitq, link) {
		count++;
	}

	return count;
}

static void
verify_io(struct spdk_bdev_io *bdev_io, uint8_t num_base_drives,
	  struct pvol_bdev_io_channel *ch_ctx, struct pvol_bdev *pvol_bdev, uint32_t io_status)
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
	uint8_t *buf = bdev_io->u.bdev.iov.iov_base;

	if (io_status == INVALID_IO_SUBMIT) {
		CU_ASSERT(g_io_comp_status == false);
		return;
	}

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
			pd_lba = pd_strip << pvol_bdev->strip_size_shift;
			pd_blocks = pvol_bdev->strip_size;
		}
		CU_ASSERT(pd_lba == g_io_output[index].offset_blocks);
		CU_ASSERT(pd_blocks == g_io_output[index].num_blocks);
		CU_ASSERT(ch_ctx->base_bdevs_io_channel[pd_idx] == g_io_output[index].ch);
		CU_ASSERT(pvol_bdev->base_bdev_info[pd_idx].base_bdev_desc == g_io_output[index].desc);
		CU_ASSERT(buf == g_io_output[index].buf);
		CU_ASSERT(bdev_io->type == g_io_output[index].iotype);
		buf += (pd_blocks << spdk_u32log2(g_block_len));
	}
	CU_ASSERT(g_io_comp_status == io_status);
}

static void
verify_pvol_config_present(const char *name, bool presence)
{
	uint32_t iter;
	bool cfg_found;

	cfg_found = false;
	for (iter = 0; iter < spdk_pvol_config.total_pvol_bdev; iter++) {
		if (strcmp(name, spdk_pvol_config.pvol_bdev_config[iter].name) == 0) {
			cfg_found = true;
			break;
		}
	}

	if (presence == true) {
		CU_ASSERT(cfg_found == true);
	} else {
		CU_ASSERT(cfg_found == false);
	}
}

static void
verify_pvol_bdev_present(const char *name, bool presence)
{
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct pvol_bdev *pbdev;
	bool   pbdev_found;

	pbdev_found = false;
	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, name) == 0) {
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
verify_pvol_config(struct rpc_construct_pvol *r, bool presence)
{
	struct pvol_bdev_config *pvol_cfg = NULL;
	uint32_t iter, iter2;

	for (iter = 0; iter < spdk_pvol_config.total_pvol_bdev; iter++) {
		if (strcmp(r->name, spdk_pvol_config.pvol_bdev_config[iter].name) == 0) {
			pvol_cfg = &spdk_pvol_config.pvol_bdev_config[iter];
			if (presence == false) {
				break;
			}
			CU_ASSERT(pvol_cfg->pvol_bdev_ctxt != NULL);
			CU_ASSERT(pvol_cfg->strip_size == r->strip_size);
			CU_ASSERT(pvol_cfg->num_base_bdevs == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pvol_cfg->raid_level == r->raid_level);
			for (iter2 = 0; iter2 < pvol_cfg->num_base_bdevs; iter2++) {
				CU_ASSERT(strcmp(pvol_cfg->base_bdev[iter2].bdev_name, r->base_bdevs.base_bdevs[iter2]) == 0);
			}
			break;
		}
	}

	if (presence == true) {
		CU_ASSERT(pvol_cfg != NULL);
	} else {
		CU_ASSERT(pvol_cfg == NULL);
	}
}

static void
verify_pvol_bdev(struct rpc_construct_pvol *r, bool presence, uint32_t pvol_state)
{
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct pvol_bdev *pbdev;
	uint32_t iter;
	struct spdk_bdev *bdev = NULL;
	bool   pbdev_found;
	uint64_t min_blockcnt = 0xFFFFFFFFFFFFFFFF;

	pbdev_found = false;
	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, r->name) == 0) {
			pbdev_found = true;
			if (presence == false) {
				break;
			}
			CU_ASSERT(pbdev->pvol_bdev_config->pvol_bdev_ctxt == pbdev_ctxt);
			CU_ASSERT(pbdev->base_bdev_info != NULL);
			CU_ASSERT(pbdev->strip_size == ((r->strip_size * 1024) / g_block_len));
			CU_ASSERT(pbdev->strip_size_shift == spdk_u32log2(((r->strip_size * 1024) / g_block_len)));
			CU_ASSERT(pbdev->blocklen_shift == spdk_u32log2(g_block_len));
			CU_ASSERT(pbdev->state == pvol_state);
			CU_ASSERT(pbdev->num_base_bdevs == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pbdev->num_base_bdevs_discovered == r->base_bdevs.num_base_bdevs);
			CU_ASSERT(pbdev->raid_level == r->raid_level);
			CU_ASSERT(pbdev->destruct_called == false);
			for (iter = 0; iter < pbdev->num_base_bdevs; iter++) {
				if (pbdev->base_bdev_info && pbdev->base_bdev_info[iter].base_bdev) {
					bdev = spdk_bdev_get_by_name(pbdev->base_bdev_info[iter].base_bdev->name);
					CU_ASSERT(bdev != NULL);
					CU_ASSERT(pbdev->base_bdev_info[iter].base_bdev_remove_scheduled == false);
				} else {
					CU_ASSERT(0);
				}

				if (bdev && bdev->blockcnt < min_blockcnt) {
					min_blockcnt = bdev->blockcnt;
				}
			}
			CU_ASSERT((((min_blockcnt / (r->strip_size * 1024 / g_block_len)) * (r->strip_size * 1024 /
					g_block_len)) * r->base_bdevs.num_base_bdevs) == pbdev_ctxt->bdev.blockcnt);
			CU_ASSERT(strcmp(pbdev_ctxt->bdev.product_name, "Pooled Device") == 0);
			CU_ASSERT(pbdev_ctxt->bdev.write_cache == 0);
			CU_ASSERT(pbdev_ctxt->bdev.blocklen == g_block_len);
			CU_ASSERT(pbdev_ctxt->bdev.optimal_io_boundary == 0);
			CU_ASSERT(pbdev_ctxt->bdev.ctxt == pbdev_ctxt);
			CU_ASSERT(pbdev_ctxt->bdev.fn_table == &g_pvol_bdev_fn_table);
			CU_ASSERT(pbdev_ctxt->bdev.module == &g_pvol_if);
			break;
		}
	}
	if (presence == true) {
		CU_ASSERT(pbdev_found == true);
	} else {
		CU_ASSERT(pbdev_found == false);
	}
	pbdev_found = false;
	if (pvol_state == PVOL_BDEV_STATE_ONLINE) {
		TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_configured_list, link_specific_list) {
			pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
			if (strcmp(pbdev_ctxt->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	} else if (pvol_state == PVOL_BDEV_STATE_CONFIGURING) {
		TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_configuring_list, link_specific_list) {
			pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
			if (strcmp(pbdev_ctxt->bdev.name, r->name) == 0) {
				pbdev_found = true;
				break;
			}
		}
	} else if (pvol_state == PVOL_BDEV_STATE_OFFLINE) {
		TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_offline_list, link_specific_list) {
			pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
			if (strcmp(pbdev_ctxt->bdev.name, r->name) == 0) {
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
verify_get_pvols(struct rpc_construct_pvol *construct_req,
		 uint8_t g_max_pvols,
		 char **g_get_pvols_output, uint32_t g_get_pvols_count)
{
	uint32_t iter, iter2;
	bool found;

	CU_ASSERT(g_max_pvols == g_get_pvols_count);
	if (g_max_pvols == g_get_pvols_count) {
		for (iter = 0; iter < g_max_pvols; iter++) {
			found = false;
			for (iter2 = 0; iter2 < g_max_pvols; iter2++) {
				if (construct_req[iter].name && strcmp(construct_req[iter].name, g_get_pvols_output[iter]) == 0) {
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
	uint32_t iter;
	struct spdk_bdev *base_bdev;
	char name[16];
	uint16_t num_chars;

	for (iter = 0; iter < g_max_base_drives; iter++, bbdev_start_idx++) {
		num_chars = snprintf(name, 16, "%s%u%s", "Nvme", bbdev_start_idx, "n1");
		name[num_chars] = '\0';
		base_bdev = calloc(1, sizeof(struct spdk_bdev));
		SPDK_CU_ASSERT_FATAL(base_bdev != NULL);
		base_bdev->name = strdup(name);
		SPDK_CU_ASSERT_FATAL(base_bdev->name != NULL);
		base_bdev->blocklen = g_block_len;
		base_bdev->blockcnt = (uint64_t)1024 * 1024 * 1024 * 1024;
		TAILQ_INSERT_TAIL(&g_bdev_list, base_bdev, link);
	}
}

static void
create_test_req(struct rpc_construct_pvol *r, const char *pvol_name, uint32_t bbdev_start_idx,
		bool create_base_bdev)
{
	uint32_t iter;
	char name[16];
	uint16_t num_chars;
	uint32_t bbdev_idx = bbdev_start_idx;

	r->name = strdup(pvol_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);
	r->strip_size = (g_strip_size * g_block_len) / 1024;
	r->raid_level = 0;
	r->base_bdevs.num_base_bdevs = g_max_base_drives;
	for (iter = 0; iter < g_max_base_drives; iter++, bbdev_idx++) {
		num_chars = snprintf(name, 16, "%s%u%s", "Nvme", bbdev_idx, "n1");
		name[num_chars] = '\0';
		r->base_bdevs.base_bdevs[iter] = strdup(name);
		SPDK_CU_ASSERT_FATAL(r->base_bdevs.base_bdevs[iter] != NULL);
	}
	if (create_base_bdev == true) {
		create_base_bdevs(bbdev_start_idx);
	}
}

static void
free_test_req(struct rpc_construct_pvol *r)
{
	uint8_t iter;

	free(r->name);
	for (iter = 0; iter < r->base_bdevs.num_base_bdevs; iter++) {
		free(r->base_bdevs.base_bdevs[iter]);
	}
}

static void
test_construct_pvol(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);

	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_destroy_pvol(void)
{
	struct rpc_construct_pvol construct_req;
	struct rpc_destroy_pvol destroy_req;

	set_globals();
	create_test_req(&construct_req, "pvol1", 0, true);
	rpc_req = &construct_req;
	rpc_req_size = sizeof(construct_req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(construct_req.name, false);
	verify_pvol_bdev_present(construct_req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&construct_req, true);
	verify_pvol_bdev(&construct_req, true, PVOL_BDEV_STATE_ONLINE);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_construct_pvol_invalid_args(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;

	set_globals();
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);

	create_test_req(&req, "pvol1", 0, true);
	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	req.raid_level = 1;
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	g_json_decode_obj_err = 1;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	g_json_decode_obj_err = 0;
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	req.strip_size = 1231;
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);

	create_test_req(&req, "pvol1", 0, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);

	create_test_req(&req, "pvol2", 0, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	verify_pvol_config_present("pvol2", false);
	verify_pvol_bdev_present("pvol2", false);

	create_test_req(&req, "pvol2", g_max_base_drives, true);
	free(req.base_bdevs.base_bdevs[g_max_base_drives - 1]);
	req.base_bdevs.base_bdevs[g_max_base_drives - 1] = strdup("Nvme0n1");
	SPDK_CU_ASSERT_FATAL(req.base_bdevs.base_bdevs[g_max_base_drives - 1] != NULL);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	verify_pvol_config_present("pvol2", false);
	verify_pvol_bdev_present("pvol2", false);

	create_test_req(&req, "pvol2", g_max_base_drives, true);
	free(req.base_bdevs.base_bdevs[g_max_base_drives - 1]);
	req.base_bdevs.base_bdevs[g_max_base_drives - 1] = strdup("Nvme100000n1");
	SPDK_CU_ASSERT_FATAL(req.base_bdevs.base_bdevs[g_max_base_drives - 1] != NULL);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	verify_pvol_config_present("pvol2", false);
	verify_pvol_bdev_present("pvol2", false);

	create_test_req(&req, "pvol2", g_max_base_drives, false);
	g_rpc_err = 0;
	g_json_beg_res_ret_err = 1;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol2", false);
	verify_pvol_bdev_present("pvol2", false);
	verify_pvol_config_present("pvol1", true);
	verify_pvol_bdev_present("pvol1", true);
	g_json_beg_res_ret_err = 0;

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	spdk_rpc_destroy_pvol(NULL, NULL);
	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_destroy_pvol_invalid_args(void)
{
	struct rpc_construct_pvol construct_req;
	struct rpc_destroy_pvol destroy_req;

	set_globals();
	create_test_req(&construct_req, "pvol1", 0, true);
	rpc_req = &construct_req;
	rpc_req_size = sizeof(construct_req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(construct_req.name, false);
	verify_pvol_bdev_present(construct_req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&construct_req, true);
	verify_pvol_bdev(&construct_req, true, PVOL_BDEV_STATE_ONLINE);

	destroy_req.name = strdup("pvol2");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);

	destroy_req.name = strdup("pvol1");
	g_rpc_err = 0;
	g_json_beg_res_ret_err = 1;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	g_json_beg_res_ret_err = 0;
	g_rpc_err = 0;
	verify_pvol_config_present("pvol1", true);
	verify_pvol_bdev_present("pvol1", true);

	destroy_req.name = strdup("pvol1");
	g_rpc_err = 0;
	g_json_decode_obj_err = 1;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	g_json_decode_obj_err = 0;
	g_rpc_err = 0;
	free(destroy_req.name);
	verify_pvol_config_present("pvol1", true);
	verify_pvol_bdev_present("pvol1", true);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_io_channel(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct pvol_bdev_io_channel *ch_ctx;
	uint32_t iter;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);

	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);

	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev_ctxt != NULL);
	ch_ctx = calloc(1, sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(pvol_bdev_create_cb(&pbdev_ctxt->pvol_bdev, ch_ctx) == 0);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == pbdev_ctxt);
	for (iter = 0; iter < req.base_bdevs.num_base_bdevs; iter++) {
		CU_ASSERT(ch_ctx->base_bdevs_io_channel && ch_ctx->base_bdevs_io_channel[iter] == (void *)0x1);
	}
	pvol_bdev_destroy_cb(&pbdev_ctxt->pvol_bdev, ch_ctx);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == NULL);
	CU_ASSERT(ch_ctx->base_bdevs_io_channel == NULL);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	free(ch_ctx);
	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_write_io(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	uint32_t iter;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev_ctxt != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(pvol_bdev_create_cb(&pbdev_ctxt->pvol_bdev, ch_ctx) == 0);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == pbdev_ctxt);
	for (iter = 0; iter < req.base_bdevs.num_base_bdevs; iter++) {
		CU_ASSERT(ch_ctx->base_bdevs_io_channel && ch_ctx->base_bdevs_io_channel[iter] == (void *)0x1);
	}

	lba = 0;
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, &pbdev_ctxt->pvol_bdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_destroy_cb(&pbdev_ctxt->pvol_bdev, ch_ctx);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == NULL);
	CU_ASSERT(ch_ctx->base_bdevs_io_channel == NULL);
	free(ch);
	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_read_io(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	uint32_t iter;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev_ctxt != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(pvol_bdev_create_cb(&pbdev_ctxt->pvol_bdev, ch_ctx) == 0);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == pbdev_ctxt);
	for (iter = 0; iter < req.base_bdevs.num_base_bdevs; iter++) {
		CU_ASSERT(ch_ctx->base_bdevs_io_channel && ch_ctx->base_bdevs_io_channel[iter] == (void *)0x1);
	}

	lba = 0;
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_READ);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, &pbdev_ctxt->pvol_bdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_destroy_cb(&pbdev_ctxt->pvol_bdev, ch_ctx);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == NULL);
	CU_ASSERT(ch_ctx->base_bdevs_io_channel == NULL);
	free(ch);
	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Test IO failures */
static void
test_io_failure(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	uint32_t iter;
	struct spdk_bdev_io *bdev_io;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev_ctxt != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(pvol_bdev_create_cb(&pbdev_ctxt->pvol_bdev, ch_ctx) == 0);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == pbdev_ctxt);
	for (iter = 0; iter < req.base_bdevs.num_base_bdevs; iter++) {
		CU_ASSERT(ch_ctx->base_bdevs_io_channel && ch_ctx->base_bdevs_io_channel[iter] == (void *)0x1);
	}

	lba = 0;
	for (count = 0; count < 1; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_INVALID);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, &pbdev_ctxt->pvol_bdev,
			  INVALID_IO_SUBMIT);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}


	lba = 0;
	g_child_io_status_flag = false;
	for (count = 0; count < 1; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_io(bdev_io, req.base_bdevs.num_base_bdevs, ch_ctx, &pbdev_ctxt->pvol_bdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_destroy_cb(&pbdev_ctxt->pvol_bdev, ch_ctx);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == NULL);
	CU_ASSERT(ch_ctx->base_bdevs_io_channel == NULL);
	free(ch);
	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Test waitq logic */
static void
test_io_waitq(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	uint32_t iter;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_io *bdev_io_next;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;
	TAILQ_HEAD(, spdk_bdev_io) head_io;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);
	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev_ctxt != NULL);
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);

	CU_ASSERT(pvol_bdev_create_cb(&pbdev_ctxt->pvol_bdev, ch_ctx) == 0);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == pbdev_ctxt);
	CU_ASSERT(ch_ctx->base_bdevs_io_channel != NULL);
	for (iter = 0; iter < req.base_bdevs.num_base_bdevs; iter++) {
		CU_ASSERT(ch_ctx->base_bdevs_io_channel[iter] == (void *)0x1);
	}

	lba = 0;
	TAILQ_INIT(&head_io);
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		TAILQ_INSERT_TAIL(&head_io, bdev_io, module_link);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		g_bdev_io_submit_status = -ENOMEM;
		lba += io_len;
		pvol_bdev_submit_request(ch, bdev_io);
	}

	g_ignore_io_output = 1;

	count = get_num_elts_in_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(count == g_max_qd);
	g_bdev_io_submit_status = 0;
	pvol_bdev_poll_io_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(TAILQ_EMPTY(&g_pvol_bdev_io_waitq->io_waitq));

	TAILQ_FOREACH_SAFE(bdev_io, &head_io, module_link, bdev_io_next) {
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_destroy_cb(&pbdev_ctxt->pvol_bdev, ch_ctx);
	CU_ASSERT(ch_ctx->pvol_bdev_ctxt == NULL);
	CU_ASSERT(ch_ctx->base_bdevs_io_channel == NULL);
	g_ignore_io_output = 0;
	free(ch);
	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

/* Create multiple pvols, destroy pvols without IO, get_pvols related tests */
static void
test_multi_pvol_no_io(void)
{
	struct rpc_construct_pvol *construct_req;
	struct rpc_destroy_pvol destroy_req;
	struct rpc_get_pvols get_pvols_req;
	uint32_t iter;
	char name[16];
	uint32_t count;
	uint32_t bbdev_idx = 0;

	set_globals();
	construct_req = calloc(MAX_PVOLS, sizeof(struct rpc_construct_pvol));
	SPDK_CU_ASSERT_FATAL(construct_req != NULL);
	CU_ASSERT(pvol_bdev_init() == 0);
	for (iter = 0; iter < g_max_pvols; iter++) {
		count = snprintf(name, 16, "%s%u", "pvol", iter);
		name[count] = '\0';
		create_test_req(&construct_req[iter], name, bbdev_idx, true);
		verify_pvol_config_present(name, false);
		verify_pvol_bdev_present(name, false);
		bbdev_idx += g_max_base_drives;
		rpc_req = &construct_req[iter];
		rpc_req_size = sizeof(construct_req[0]);
		g_rpc_err = 0;
		spdk_rpc_construct_pvol(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_pvol_config(&construct_req[iter], true);
		verify_pvol_bdev(&construct_req[iter], true, PVOL_BDEV_STATE_ONLINE);
	}

	get_pvols_req.category = strdup("all");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_get_pvols(construct_req, g_max_pvols, g_get_pvols_output, g_get_pvols_count);
	for (iter = 0; iter < g_get_pvols_count; iter++) {
		free(g_get_pvols_output[iter]);
	}
	g_get_pvols_count = 0;

	get_pvols_req.category = strdup("online");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_get_pvols(construct_req, g_max_pvols, g_get_pvols_output, g_get_pvols_count);
	for (iter = 0; iter < g_get_pvols_count; iter++) {
		free(g_get_pvols_output[iter]);
	}
	g_get_pvols_count = 0;

	get_pvols_req.category = strdup("configuring");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_pvols_count == 0);

	get_pvols_req.category = strdup("offline");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	CU_ASSERT(g_get_pvols_count == 0);

	get_pvols_req.category = strdup("invalid_category");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	CU_ASSERT(g_get_pvols_count == 0);

	get_pvols_req.category = strdup("all");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	g_json_decode_obj_err = 1;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 1);
	g_json_decode_obj_err = 0;
	free(get_pvols_req.category);
	CU_ASSERT(g_get_pvols_count == 0);

	get_pvols_req.category = strdup("all");
	rpc_req = &get_pvols_req;
	rpc_req_size = sizeof(get_pvols_req);
	g_rpc_err = 0;
	g_json_beg_res_ret_err = 1;
	spdk_rpc_get_pvols(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	g_json_beg_res_ret_err = 0;
	CU_ASSERT(g_get_pvols_count == 0);

	for (iter = 0; iter < g_max_pvols; iter++) {
		SPDK_CU_ASSERT_FATAL(construct_req[iter].name != NULL);
		destroy_req.name = strdup(construct_req[iter].name);
		count = snprintf(name, 16, "%s", destroy_req.name);
		name[count] = '\0';
		rpc_req = &destroy_req;
		rpc_req_size = sizeof(destroy_req);
		g_rpc_err = 0;
		spdk_rpc_destroy_pvol(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_pvol_config_present(name, false);
		verify_pvol_bdev_present(name, false);
	}
	pvol_bdev_exit();
	free(construct_req);
	base_bdevs_cleanup();
	reset_globals();
}

/* Create multiple pvols, fire IOs randomly on various pvols */
static void
test_multi_pvol_with_io(void)
{
	struct rpc_construct_pvol *construct_req;
	struct rpc_destroy_pvol destroy_req;
	uint32_t iter, iter2;
	char name[16];
	uint32_t count;
	uint32_t bbdev_idx = 0;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io;
	uint64_t io_len;
	uint64_t lba;
	struct spdk_io_channel *ch_random;
	struct pvol_bdev_io_channel *ch_ctx_random;
	int16_t iotype;
	uint32_t pvol_random;

	set_globals();
	construct_req = calloc(g_max_pvols, sizeof(struct rpc_construct_pvol));
	SPDK_CU_ASSERT_FATAL(construct_req != NULL);
	CU_ASSERT(pvol_bdev_init() == 0);
	ch = calloc(g_max_pvols, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	for (iter = 0; iter < g_max_pvols; iter++) {
		count = snprintf(name, 16, "%s%u", "pvol", iter);
		name[count] = '\0';
		create_test_req(&construct_req[iter], name, bbdev_idx, true);
		verify_pvol_config_present(name, false);
		verify_pvol_bdev_present(name, false);
		bbdev_idx += g_max_base_drives;
		rpc_req = &construct_req[iter];
		rpc_req_size = sizeof(construct_req[0]);
		g_rpc_err = 0;
		spdk_rpc_construct_pvol(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_pvol_config(&construct_req[iter], true);
		verify_pvol_bdev(&construct_req[iter], true, PVOL_BDEV_STATE_ONLINE);
		TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
			pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
			if (strcmp(pbdev_ctxt->bdev.name, construct_req[iter].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev_ctxt != NULL);
		ch_ctx = spdk_io_channel_get_ctx(&ch[iter]);
		SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);
		CU_ASSERT(pvol_bdev_create_cb(&pbdev_ctxt->pvol_bdev, ch_ctx) == 0);
		CU_ASSERT(ch_ctx->pvol_bdev_ctxt == pbdev_ctxt);
		CU_ASSERT(ch_ctx->base_bdevs_io_channel != NULL);
		for (iter2 = 0; iter2 < construct_req[iter].base_bdevs.num_base_bdevs; iter2++) {
			CU_ASSERT(ch_ctx->base_bdevs_io_channel[iter2] == (void *)0x1);
		}
	}

	lba = 0;
	for (count = 0; count < g_max_qd; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		iotype = (rand() % 2) ? SPDK_BDEV_IO_TYPE_WRITE : SPDK_BDEV_IO_TYPE_READ;
		bdev_io_initialize(bdev_io, lba, io_len, iotype);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_random = rand() % g_max_pvols;
		ch_random = &ch[pvol_random];
		ch_ctx_random = spdk_io_channel_get_ctx(ch_random);
		TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
			pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
			if (strcmp(pbdev_ctxt->bdev.name, construct_req[pvol_random].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev_ctxt != NULL);
		pvol_bdev_submit_request(ch_random, bdev_io);
		verify_io(bdev_io, g_max_base_drives, ch_ctx_random, &pbdev_ctxt->pvol_bdev,
			  g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	for (iter = 0; iter < g_max_pvols; iter++) {
		TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
			pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
			if (strcmp(pbdev_ctxt->bdev.name, construct_req[iter].name) == 0) {
				break;
			}
		}
		CU_ASSERT(pbdev_ctxt != NULL);
		ch_ctx = spdk_io_channel_get_ctx(&ch[iter]);
		SPDK_CU_ASSERT_FATAL(ch_ctx != NULL);
		pvol_bdev_destroy_cb(&pbdev_ctxt->pvol_bdev, ch_ctx);
		CU_ASSERT(ch_ctx->pvol_bdev_ctxt == NULL);
		CU_ASSERT(ch_ctx->base_bdevs_io_channel == NULL);
		destroy_req.name = strdup(construct_req[iter].name);
		count = snprintf(name, 16, "%s", destroy_req.name);
		name[count] = '\0';
		rpc_req = &destroy_req;
		rpc_req_size = sizeof(destroy_req);
		g_rpc_err = 0;
		spdk_rpc_destroy_pvol(NULL, NULL);
		CU_ASSERT(g_rpc_err == 0);
		verify_pvol_config_present(name, false);
		verify_pvol_bdev_present(name, false);
	}
	pvol_bdev_exit();
	free(construct_req);
	free(ch);
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_io_type_supported(void)
{
	CU_ASSERT(pvol_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_READ) == true);
	CU_ASSERT(pvol_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_WRITE) == true);
	CU_ASSERT(pvol_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_FLUSH) == true);
	CU_ASSERT(pvol_bdev_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_INVALID) == false);
}

static void
test_create_pvol_from_config(void)
{
	struct rpc_construct_pvol req;
	struct spdk_bdev *bdev;
	struct rpc_destroy_pvol destroy_req;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	g_config_level_create = 1;
	CU_ASSERT(pvol_bdev_init() == 0);
	g_config_level_create = 0;

	verify_pvol_config_present("pvol1", true);
	verify_pvol_bdev_present("pvol1", false);

	TAILQ_FOREACH(bdev, &g_bdev_list, link) {
		pvol_bdev_examine(bdev);
	}

	bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);
	bdev->name = strdup("Invalid");
	SPDK_CU_ASSERT_FATAL(bdev->name != NULL);
	CU_ASSERT(pvol_bdev_add_base_device(bdev) != 0);
	free(bdev->name);
	free(bdev);

	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	free_test_req(&req);
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_create_pvol_from_config_invalid_params(void)
{
	struct rpc_construct_pvol req;
	uint8_t count;

	set_globals();
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	g_config_level_create = 1;

	create_test_req(&req, "pvol1", 0, true);
	free(req.name);
	req.name = NULL;
	CU_ASSERT(pvol_bdev_init() != 0);
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	req.strip_size = 1234;
	CU_ASSERT(pvol_bdev_init() != 0);
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	req.raid_level = 1;
	CU_ASSERT(pvol_bdev_init() != 0);
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	req.raid_level = 1;
	CU_ASSERT(pvol_bdev_init() != 0);
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	req.base_bdevs.num_base_bdevs++;
	CU_ASSERT(pvol_bdev_init() != 0);
	req.base_bdevs.num_base_bdevs--;
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	req.base_bdevs.num_base_bdevs--;
	CU_ASSERT(pvol_bdev_init() != 0);
	req.base_bdevs.num_base_bdevs++;
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	create_test_req(&req, "pvol1", 0, false);
	count = snprintf(req.base_bdevs.base_bdevs[g_max_base_drives - 1], 15, "%s", "Nvme0n1");
	req.base_bdevs.base_bdevs[g_max_base_drives - 1][count] = '\0';
	CU_ASSERT(pvol_bdev_init() != 0);
	free_test_req(&req);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_pvol_json_dump_info(void)
{
	struct rpc_construct_pvol req;
	struct rpc_destroy_pvol destroy_req;
	struct pvol_bdev *pbdev;
	struct pvol_bdev_ctxt *pbdev_ctxt = NULL;

	set_globals();
	create_test_req(&req, "pvol1", 0, true);
	rpc_req = &req;
	rpc_req_size = sizeof(req);
	CU_ASSERT(pvol_bdev_init() == 0);

	verify_pvol_config_present(req.name, false);
	verify_pvol_bdev_present(req.name, false);
	g_rpc_err = 0;
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&req, true);
	verify_pvol_bdev(&req, true, PVOL_BDEV_STATE_ONLINE);

	TAILQ_FOREACH(pbdev, &spdk_pvol_bdev_list, link_global_list) {
		pbdev_ctxt = SPDK_CONTAINEROF(pbdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pbdev_ctxt->bdev.name, req.name) == 0) {
			break;
		}
	}
	CU_ASSERT(pbdev_ctxt != NULL);

	CU_ASSERT(pvol_bdev_dump_info_json(pbdev_ctxt, NULL) == 0);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
	base_bdevs_cleanup();
	reset_globals();
}

static void
test_context_size(void)
{
	CU_ASSERT(pvol_bdev_get_ctx_size() == sizeof(struct pvol_bdev_io));
}

static void
test_asym_base_drives_blockcnt(void)
{
	struct rpc_construct_pvol construct_req;
	struct rpc_destroy_pvol destroy_req;
	struct spdk_bdev *bbdev;
	uint32_t iter;

	set_globals();
	create_test_req(&construct_req, "pvol1", 0, true);
	rpc_req = &construct_req;
	rpc_req_size = sizeof(construct_req);
	CU_ASSERT(pvol_bdev_init() == 0);
	verify_pvol_config_present(construct_req.name, false);
	verify_pvol_bdev_present(construct_req.name, false);
	g_rpc_err = 0;
	for (iter = 0; iter < construct_req.base_bdevs.num_base_bdevs; iter++) {
		bbdev = spdk_bdev_get_by_name(construct_req.base_bdevs.base_bdevs[iter]);
		SPDK_CU_ASSERT_FATAL(bbdev != NULL);
		bbdev->blockcnt = rand() + 1;
	}
	spdk_rpc_construct_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config(&construct_req, true);
	verify_pvol_bdev(&construct_req, true, PVOL_BDEV_STATE_ONLINE);

	destroy_req.name = strdup("pvol1");
	rpc_req = &destroy_req;
	rpc_req_size = sizeof(destroy_req);
	g_rpc_err = 0;
	spdk_rpc_destroy_pvol(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_pvol_config_present("pvol1", false);
	verify_pvol_bdev_present("pvol1", false);

	pvol_bdev_exit();
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

	suite = CU_add_suite("pvol", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_construct_pvol", test_construct_pvol) == NULL ||
		CU_add_test(suite, "test_destroy_pvol", test_destroy_pvol) == NULL ||
		CU_add_test(suite, "test_construct_pvol_invalid_args", test_construct_pvol_invalid_args) == NULL ||
		CU_add_test(suite, "test_destroy_pvol_invalid_args", test_destroy_pvol_invalid_args) == NULL ||
		CU_add_test(suite, "test_io_channel", test_io_channel) == NULL ||
		CU_add_test(suite, "test_write_io", test_write_io) == NULL    ||
		CU_add_test(suite, "test_read_io", test_read_io) == NULL     ||
		CU_add_test(suite, "test_io_failure", test_io_failure) == NULL ||
		CU_add_test(suite, "test_io_waitq", test_io_waitq) == NULL ||
		CU_add_test(suite, "test_multi_pvol_no_io", test_multi_pvol_no_io) == NULL ||
		CU_add_test(suite, "test_multi_pvol_with_io", test_multi_pvol_with_io) == NULL ||
		CU_add_test(suite, "test_io_type_supported", test_io_type_supported) == NULL ||
		CU_add_test(suite, "test_create_pvol_from_config", test_create_pvol_from_config) == NULL ||
		CU_add_test(suite, "test_create_pvol_from_config_invalid_params",
			    test_create_pvol_from_config_invalid_params) == NULL ||
		CU_add_test(suite, "test_pvol_json_dump_info", test_pvol_json_dump_info) == NULL ||
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
