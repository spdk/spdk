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

#define MAX_BASE_DRIVES 8
#define MAX_QUEUE_DEPTH 128
#define MAX_IO_SIZE 256
#define BLOCK_LEN 512
#define STRIP_SIZE 128
#define MAX_CONF_FILE_ENTRIES 16
#define MAX_PVOLS 16

struct io_output {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	void *buf;
	uint64_t offset_blocks;
	uint64_t num_blocks;
	spdk_bdev_io_completion_cb cb;
	void *cb_arg;
};

uint8_t g_max_base_drives;
uint8_t g_max_queue_depth;
uint64_t g_max_io_size;
uint32_t g_block_len;
uint32_t g_strip_size;
int g_bdev_io_submit_status;
uint32_t g_block_len_shift;
uint32_t g_strip_size_shift;
struct io_output *g_io_output = NULL;
uint32_t g_io_output_index;
uint32_t g_io_comp_status;
bool g_child_io_status_flag;
uint32_t g_max_conf_file_entries;
uint32_t g_max_pvols;

static uint32_t
count_zeros(uint32_t num)
{
	uint32_t count = 0;

	while (num) {
		count++;
		num = num >> 1;
	}

	return count - 1;
}

static void
set_globals(void)
{
	g_max_base_drives = MAX_BASE_DRIVES;
	g_max_queue_depth = MAX_QUEUE_DEPTH;
	g_max_conf_file_entries = MAX_CONF_FILE_ENTRIES;
	g_max_pvols = MAX_PVOLS;
	g_max_io_size = MAX_IO_SIZE;
	g_block_len = BLOCK_LEN;
	g_bdev_io_submit_status = 0;
	g_strip_size = STRIP_SIZE;
	g_strip_size_shift = count_zeros(g_strip_size);
	g_block_len_shift = count_zeros(g_block_len);
	srand(time(0));
	g_io_output = calloc((g_max_io_size / g_strip_size) + 1, sizeof(struct io_output));
	SPDK_CU_ASSERT_FATAL(g_io_output != NULL);
	g_io_output_index = 0;
	g_io_comp_status = 0;
	g_child_io_status_flag = true;
}

static void
reset_globals(void)
{
	if (g_io_output) {
		free(g_io_output);
		g_io_output = NULL;
	}
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_io_comp_status = ((status == SPDK_BDEV_IO_STATUS_SUCCESS) ? true : false);
}

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *p = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	SPDK_CU_ASSERT_FATAL(g_io_output_index < (g_max_io_size / g_strip_size) + 1);
	if (g_bdev_io_submit_status == 0) {
		p->desc = desc;
		p->ch = ch;
		p->buf = buf;
		p->offset_blocks = offset_blocks;
		p->num_blocks = num_blocks;
		p->cb = cb;
		p->cb_arg = cb_arg;
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
	return NULL;
}

void
spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb)
{
}

char *
spdk_sprintf_alloc(const char *format, ...)
{
	return NULL;
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

int
spdk_json_write_object_begin(struct spdk_json_write_ctx *w)
{
	return 0;
}

int
spdk_json_write_uint32(struct spdk_json_write_ctx *w, uint32_t val)
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
spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val)
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
	return NULL;
}

void
spdk_for_each_thread(spdk_thread_fn fn, void *ctx, spdk_thread_fn cpl)
{
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

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *p = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	SPDK_CU_ASSERT_FATAL(g_io_output_index < (g_max_io_size / g_strip_size) + 1);
	if (g_bdev_io_submit_status == 0) {
		p->desc = desc;
		p->ch = ch;
		p->buf = buf;
		p->offset_blocks = offset_blocks;
		p->num_blocks = num_blocks;
		p->cb = cb;
		p->cb_arg = cb_arg;
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
}


char *
spdk_str_trim(char *s)
{
	return NULL;
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module *module)
{
}

struct spdk_conf_section *
spdk_conf_first_section(struct spdk_conf *cp)
{
	return NULL;
}

bool
spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix)
{
	return false;
}

char *
spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key)
{
	return NULL;
}

int
spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key)
{
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
	return 0;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	return 0;
}

uint32_t
spdk_env_get_first_core(void)
{
	return 0;
}

uint32_t
spdk_env_get_next_core(uint32_t prev_core)
{
	return UINT32_MAX;
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{
}

static void
pvol_bdev_base_bdev_cleanup(struct pvol_bdev *pvol_bdev)
{
	uint8_t i;

	for (i = 0; i < pvol_bdev->num_base_bdevs; i++) {
		if (pvol_bdev->base_bdev_info[i].base_bdev) {
			free(pvol_bdev->base_bdev_info[i].base_bdev);
			pvol_bdev->base_bdev_info[i].base_bdev = NULL;
		}
		if (pvol_bdev->base_bdev_info[i].base_bdev_desc) {
			free(pvol_bdev->base_bdev_info[i].base_bdev_desc);
			pvol_bdev->base_bdev_info[i].base_bdev_desc = NULL;
		}
	}
	if (pvol_bdev->base_bdev_info) {
		free(pvol_bdev->base_bdev_info);
		pvol_bdev->base_bdev_info = NULL;
	}
}

static void
pvol_bdev_initialize(struct pvol_bdev *pvol_bdev, uint8_t num_base_drives)
{
	uint8_t i;

	pvol_bdev->strip_size = g_strip_size;
	pvol_bdev->strip_size_shift = g_strip_size_shift;
	pvol_bdev->blocklen_shift = g_block_len_shift;
	pvol_bdev->state = PVOL_BDEV_STATE_ONLINE;
	pvol_bdev->num_base_bdevs = num_base_drives;
	pvol_bdev->num_base_bdevs_discovered = num_base_drives;
	pvol_bdev->base_bdev_info = calloc(pvol_bdev->num_base_bdevs, sizeof(struct pvol_base_bdev_info));
	SPDK_CU_ASSERT_FATAL(pvol_bdev->base_bdev_info != NULL);
	for (i = 0; i < pvol_bdev->num_base_bdevs; i++) {
		pvol_bdev->base_bdev_info[i].base_bdev = calloc(1, sizeof(struct spdk_bdev));
		SPDK_CU_ASSERT_FATAL(pvol_bdev->base_bdev_info[i].base_bdev != NULL);
		pvol_bdev->base_bdev_info[i].base_bdev_desc = calloc(1, sizeof(void *));
		SPDK_CU_ASSERT_FATAL(pvol_bdev->base_bdev_info[i].base_bdev_desc != NULL);
	}
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

static void
io_waitq_initialize(struct pvol_bdev_io_waitq *io_waitq)
{
	TAILQ_INIT(&io_waitq->io_waitq);
}

static uint32_t
get_num_elts_in_waitq(struct pvol_bdev_io_waitq *waitq)
{
	struct pvol_bdev_io *b1, *b2;
	uint32_t count = 0;
	TAILQ_FOREACH_SAFE(b1, &waitq->io_waitq, link, b2) {
		count++;
	}

	return count;
}

static void
test_io_waitq_insert_on_q_empty(void)
{
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io;
	struct pvol_bdev *pvol_bdev;

	set_globals();
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);

	ch_ctx = spdk_io_channel_get_ctx(ch);
	ch_ctx->pvol_bdev_ctxt = calloc(1, sizeof(struct pvol_bdev_ctxt));
	SPDK_CU_ASSERT_FATAL(ch_ctx->pvol_bdev_ctxt != NULL);
	ch_ctx->base_bdevs_io_channel = calloc(2, sizeof(struct spdk_io_channel *));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel != NULL);
	ch_ctx->base_bdevs_io_channel[0] = calloc(1, sizeof(struct spdk_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[0] != NULL);
	ch_ctx->base_bdevs_io_channel[1] = calloc(1, sizeof(struct spdk_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[1] != NULL);

	g_pvol_bdev_io_waitq = calloc(1, sizeof(struct pvol_bdev_io_waitq));
	SPDK_CU_ASSERT_FATAL(g_pvol_bdev_io_waitq != NULL);

	pvol_bdev = &ch_ctx->pvol_bdev_ctxt->pvol_bdev;
	pvol_bdev_initialize(pvol_bdev, 2);
	bdev_io_initialize(bdev_io, 0, 1, SPDK_BDEV_IO_TYPE_WRITE);
	io_waitq_initialize(g_pvol_bdev_io_waitq);
	g_bdev_io_submit_status = -ENOMEM;
	pvol_bdev_submit_request(ch, bdev_io);
	CU_ASSERT(!TAILQ_EMPTY(&g_pvol_bdev_io_waitq->io_waitq));
	g_bdev_io_submit_status = 0;
	pvol_bdev_poll_io_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(TAILQ_EMPTY(&g_pvol_bdev_io_waitq->io_waitq));
	bdev_io_cleanup(bdev_io);
	pvol_bdev_base_bdev_cleanup(pvol_bdev);
	free(bdev_io);
	free(ch_ctx->pvol_bdev_ctxt);
	free(g_pvol_bdev_io_waitq);
	free(ch_ctx->base_bdevs_io_channel[0]);
	free(ch_ctx->base_bdevs_io_channel[1]);
	free(ch_ctx->base_bdevs_io_channel);
	free(ch);
	reset_globals();
	g_bdev_io_submit_status = 0;
}

static void
test_io_waitq_insert_on_q_not_empty(void)
{
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io1, *bdev_io2;
	struct pvol_bdev *pvol_bdev;
	uint32_t count;

	set_globals();
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	bdev_io1 = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io1 != NULL);
	bdev_io2 = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io2 != NULL);

	ch_ctx = spdk_io_channel_get_ctx(ch);
	ch_ctx->pvol_bdev_ctxt = calloc(1, sizeof(struct pvol_bdev_ctxt));
	SPDK_CU_ASSERT_FATAL(ch_ctx->pvol_bdev_ctxt != NULL);
	ch_ctx->base_bdevs_io_channel = calloc(2, sizeof(struct spdk_io_channel *));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel != NULL);
	ch_ctx->base_bdevs_io_channel[0] = calloc(1, sizeof(struct spdk_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[0] != NULL);
	ch_ctx->base_bdevs_io_channel[1] = calloc(1, sizeof(struct spdk_io_channel));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[1] != NULL);

	g_pvol_bdev_io_waitq = calloc(1, sizeof(struct pvol_bdev_io_waitq));
	SPDK_CU_ASSERT_FATAL(g_pvol_bdev_io_waitq != NULL);

	pvol_bdev = &ch_ctx->pvol_bdev_ctxt->pvol_bdev;
	pvol_bdev_initialize(pvol_bdev, 2);
	bdev_io_initialize(bdev_io1, 0, 1, SPDK_BDEV_IO_TYPE_WRITE);
	bdev_io_initialize(bdev_io2, 2, 1, SPDK_BDEV_IO_TYPE_WRITE);
	io_waitq_initialize(g_pvol_bdev_io_waitq);
	g_bdev_io_submit_status = -ENOMEM;
	CU_ASSERT(TAILQ_EMPTY(&g_pvol_bdev_io_waitq->io_waitq));
	pvol_bdev_submit_request(ch, bdev_io1);
	count = get_num_elts_in_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(count == 1);
	pvol_bdev_submit_request(ch, bdev_io2);
	count = get_num_elts_in_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(count == 2);
	pvol_bdev_poll_io_waitq(g_pvol_bdev_io_waitq);
	count = get_num_elts_in_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(count == 2);
	pvol_bdev_poll_io_waitq(g_pvol_bdev_io_waitq);
	count = get_num_elts_in_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(count == 2);
	g_bdev_io_submit_status = 0;
	pvol_bdev_poll_io_waitq(g_pvol_bdev_io_waitq);
	CU_ASSERT(TAILQ_EMPTY(&g_pvol_bdev_io_waitq->io_waitq));
	bdev_io_cleanup(bdev_io1);
	bdev_io_cleanup(bdev_io2);
	pvol_bdev_base_bdev_cleanup(pvol_bdev);
	free(bdev_io1);
	free(bdev_io2);
	free(ch_ctx->pvol_bdev_ctxt);
	free(g_pvol_bdev_io_waitq);
	free(ch_ctx->base_bdevs_io_channel[0]);
	free(ch_ctx->base_bdevs_io_channel[1]);
	free(ch_ctx->base_bdevs_io_channel);
	free(ch);
	reset_globals();
	g_bdev_io_submit_status = 0;
}

static void
verify_output(struct spdk_bdev_io *bdev_io, uint8_t num_base_drives,
	      struct pvol_bdev_io_channel *ch_ctx, struct pvol_bdev *pvol_bdev, uint32_t io_status)
{
	uint64_t start_strip = bdev_io->u.bdev.offset_blocks >> g_strip_size_shift;
	uint64_t end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
			     g_strip_size_shift;
	uint32_t splits_reqd = (end_strip - start_strip + 1);
	uint32_t strip;
	uint64_t pd_strip;
	uint64_t pd_idx;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint64_t pd_blocks;
	uint32_t index = 0;
	uint8_t *buf = bdev_io->u.bdev.iov.iov_base;

	CU_ASSERT(splits_reqd == g_io_output_index);

	for (strip = start_strip; strip <= end_strip; strip++, index++) {
		pd_strip = strip / num_base_drives;
		pd_idx = strip % num_base_drives;
		if (strip == start_strip) {
			offset_in_strip = bdev_io->u.bdev.offset_blocks & (g_strip_size - 1);
			pd_lba = (pd_strip << g_strip_size_shift) + offset_in_strip;
			if (strip == end_strip) {
				pd_blocks = bdev_io->u.bdev.num_blocks;
			} else {
				pd_blocks = g_strip_size - offset_in_strip;
			}
		} else if (strip == end_strip) {
			pd_lba = pd_strip << g_strip_size_shift;
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
		buf += (pd_blocks << g_block_len_shift);
	}
	CU_ASSERT(g_io_comp_status == io_status);
}

static void
test_write_io(void)
{
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io;
	struct pvol_bdev *pvol_bdev;
	uint32_t count;
	uint8_t num_base_drives;
	uint8_t num_ios;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	ch_ctx->pvol_bdev_ctxt = calloc(1, sizeof(struct pvol_bdev_ctxt));
	SPDK_CU_ASSERT_FATAL(ch_ctx->pvol_bdev_ctxt != NULL);
	num_base_drives = (rand() % g_max_base_drives) + 1;
	ch_ctx->base_bdevs_io_channel = calloc(num_base_drives, sizeof(struct spdk_io_channel *));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel != NULL);
	for (count = 0; count < num_base_drives; count++) {
		ch_ctx->base_bdevs_io_channel[count] = calloc(1, sizeof(struct spdk_io_channel));
		SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[count] != NULL);
	}

	g_pvol_bdev_io_waitq = calloc(1, sizeof(struct pvol_bdev_io_waitq));
	SPDK_CU_ASSERT_FATAL(g_pvol_bdev_io_waitq != NULL);
	io_waitq_initialize(g_pvol_bdev_io_waitq);
	pvol_bdev = &ch_ctx->pvol_bdev_ctxt->pvol_bdev;
	pvol_bdev_initialize(pvol_bdev, num_base_drives);

	num_ios = (rand() % g_max_queue_depth) + 1;
	lba = 0;
	for (count = 0; count < num_ios; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_output(bdev_io, num_base_drives, ch_ctx, pvol_bdev, g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_base_bdev_cleanup(pvol_bdev);
	free(ch_ctx->pvol_bdev_ctxt);
	free(g_pvol_bdev_io_waitq);
	for (count = 0; count < num_base_drives; count++) {
		if (ch_ctx->base_bdevs_io_channel[count]) {
			free(ch_ctx->base_bdevs_io_channel[count]);
		}
	}
	free(ch_ctx->base_bdevs_io_channel);
	free(ch);
	reset_globals();
}

static void
test_read_io(void)
{
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io;
	struct pvol_bdev *pvol_bdev;
	uint32_t count;
	uint8_t num_base_drives;
	uint8_t num_ios;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	ch_ctx->pvol_bdev_ctxt = calloc(1, sizeof(struct pvol_bdev_ctxt));
	SPDK_CU_ASSERT_FATAL(ch_ctx->pvol_bdev_ctxt != NULL);
	num_base_drives = (rand() % g_max_base_drives) + 1;
	ch_ctx->base_bdevs_io_channel = calloc(num_base_drives, sizeof(struct spdk_io_channel *));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel != NULL);
	for (count = 0; count < num_base_drives; count++) {
		ch_ctx->base_bdevs_io_channel[count] = calloc(1, sizeof(struct spdk_io_channel));
		SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[count] != NULL);
	}

	g_pvol_bdev_io_waitq = calloc(1, sizeof(struct pvol_bdev_io_waitq));
	SPDK_CU_ASSERT_FATAL(g_pvol_bdev_io_waitq != NULL);
	io_waitq_initialize(g_pvol_bdev_io_waitq);
	pvol_bdev = &ch_ctx->pvol_bdev_ctxt->pvol_bdev;
	pvol_bdev_initialize(pvol_bdev, num_base_drives);

	num_ios = (rand() % g_max_queue_depth) + 1;
	lba = 0;
	for (count = 0; count < num_ios; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_READ);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_output(bdev_io, num_base_drives, ch_ctx, pvol_bdev, g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_base_bdev_cleanup(pvol_bdev);
	free(ch_ctx->pvol_bdev_ctxt);
	free(g_pvol_bdev_io_waitq);
	for (count = 0; count < num_base_drives; count++) {
		if (ch_ctx->base_bdevs_io_channel[count]) {
			free(ch_ctx->base_bdevs_io_channel[count]);
		}
	}
	free(ch_ctx->base_bdevs_io_channel);
	free(ch);
	reset_globals();
}

static void
test_io_failure(void)
{
	struct spdk_io_channel *ch;
	struct pvol_bdev_io_channel *ch_ctx;
	struct spdk_bdev_io *bdev_io;
	struct pvol_bdev *pvol_bdev;
	uint32_t count;
	uint8_t num_base_drives;
	uint8_t num_ios;
	uint64_t io_len;
	uint64_t lba;

	set_globals();
	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct pvol_bdev_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ch_ctx = spdk_io_channel_get_ctx(ch);
	ch_ctx->pvol_bdev_ctxt = calloc(1, sizeof(struct pvol_bdev_ctxt));
	SPDK_CU_ASSERT_FATAL(ch_ctx->pvol_bdev_ctxt != NULL);
	num_base_drives = (rand() % g_max_base_drives) + 1;
	ch_ctx->base_bdevs_io_channel = calloc(num_base_drives, sizeof(struct spdk_io_channel *));
	SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel != NULL);
	for (count = 0; count < num_base_drives; count++) {
		ch_ctx->base_bdevs_io_channel[count] = calloc(1, sizeof(struct spdk_io_channel));
		SPDK_CU_ASSERT_FATAL(ch_ctx->base_bdevs_io_channel[count] != NULL);
	}

	g_pvol_bdev_io_waitq = calloc(1, sizeof(struct pvol_bdev_io_waitq));
	SPDK_CU_ASSERT_FATAL(g_pvol_bdev_io_waitq != NULL);
	io_waitq_initialize(g_pvol_bdev_io_waitq);
	pvol_bdev = &ch_ctx->pvol_bdev_ctxt->pvol_bdev;
	pvol_bdev_initialize(pvol_bdev, num_base_drives);

	num_ios = 1;
	lba = 0;
	g_child_io_status_flag = false;
	for (count = 0; count < num_ios; count++) {
		bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct pvol_bdev_io));
		SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
		io_len = (rand() % g_max_io_size) + 1;
		bdev_io_initialize(bdev_io, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += io_len;
		memset(g_io_output, 0, (g_max_io_size / g_strip_size) + 1 * sizeof(struct io_output));
		g_io_output_index = 0;
		pvol_bdev_submit_request(ch, bdev_io);
		verify_output(bdev_io, num_base_drives, ch_ctx, pvol_bdev, g_child_io_status_flag);
		bdev_io_cleanup(bdev_io);
		free(bdev_io);
	}

	pvol_bdev_base_bdev_cleanup(pvol_bdev);
	free(ch_ctx->pvol_bdev_ctxt);
	free(g_pvol_bdev_io_waitq);
	for (count = 0; count < num_base_drives; count++) {
		if (ch_ctx->base_bdevs_io_channel[count]) {
			free(ch_ctx->base_bdevs_io_channel[count]);
		}
	}
	free(ch_ctx->base_bdevs_io_channel);
	free(ch);
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

	if (CU_add_test(suite, "test_io_waitq_insert_on_q_empty",
			test_io_waitq_insert_on_q_empty) == NULL ||
	    CU_add_test(suite, "test_io_waitq_insert_on_q_not_empty",
			test_io_waitq_insert_on_q_not_empty) == NULL ||
	    CU_add_test(suite, "test_write_io", test_write_io) == NULL ||
	    CU_add_test(suite, "test_read_io", test_read_io) == NULL ||
	    CU_add_test(suite, "test_io_failure", test_io_failure) == NULL
	   ) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
