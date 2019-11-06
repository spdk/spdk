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
#include "bdev_merge.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/conf.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/config.h"


/* merge bdev config as read from config file */
struct merge_config	g_merge_config = {
	.merge_bdev_config_head = TAILQ_HEAD_INITIALIZER(g_merge_config.merge_bdev_config_head),
	.total_merge_bdev = 0,
};

/* List of all merge bdevs */
TAILQ_HEAD(, merge_bdev) g_merge_bdev_list = TAILQ_HEAD_INITIALIZER(g_merge_bdev_list);

int merge_bdev_config_add(const char *merge_name, uint32_t master_strip_size,
			  uint32_t slave_strip_size,
			  uint8_t buff_cnt, struct merge_bdev_config **_merge_cfg);
int merge_bdev_config_add_base_bdev(struct merge_bdev_config *merge_cfg, const char *base_bdev_name,
				    enum merge_bdev_type base_type);
int merge_bdev_create(struct merge_bdev_config *merge_bdev_config);
int merge_bdev_add_base_devices(struct merge_bdev_config *merge_bdev_config);

static int merge_bdev_init(void);
static void merge_bdev_exit(void);
static int merge_bdev_get_ctx_size(void);
static void merge_bdev_get_running_config(FILE *fp);
static void merge_bdev_examine(struct spdk_bdev *bdev);

static int merge_bdev_destruct(void *ctxt);
static void merge_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool merge_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *merge_bdev_get_io_channel(void *ctx);
static int merge_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
static void merge_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);
static void merge_bdev_write_slave(struct merge_bdev *mbdev, struct merge_bdev_io_channel *merge_ch,
				   struct merge_base_bdev_config *master_cfg, struct merge_base_bdev_config *slave_cfg,
				   int buf_sumbit);
static void merge_bdev_master_write_io_completion(struct spdk_bdev_io *bdev_io,
		bool success, void *cb_arg);

static struct spdk_bdev_module g_merge_module = {
	.name = "merge",
	.module_init = merge_bdev_init,
	.module_fini = merge_bdev_exit,
	.get_ctx_size = merge_bdev_get_ctx_size,
	.examine_config = merge_bdev_examine,
	.config_text = merge_bdev_get_running_config,
	.async_init = false,
	.async_fini = false,
};



/* g_merge_bdev_fn_table is the function table for merge bdev */
static const struct spdk_bdev_fn_table g_merge_bdev_fn_table = {
	.destruct		= merge_bdev_destruct,
	.submit_request		= merge_bdev_submit_request,
	.io_type_supported	= merge_bdev_io_type_supported,
	.get_io_channel		= merge_bdev_get_io_channel,
	.dump_info_json		= merge_bdev_dump_info_json,
	.write_config_json	= merge_bdev_write_config_json,
};

/*
 * brief:
 * simply do nothing in this phase
 *
 * params:
 * ch - pointer to merge bdev io channel
 * bdev_io - pointer to parent bdev_io on merge bdev device
 * returns:
 * none
 */
static void
_merge_bdev_submit_reset_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	/* TODO: simply do nothing in this phase, maybe add some new operations in the future */
	return;
}


static void
_merge_bdev_submit_null_payload_request(void *_bdev_io)
{
	/* TODO: simply do nothing in this phase, maybe add some new operations in the future */
	return;
}

static void
merge_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct merge_bdev *merge_bdev = bdev->ctxt;
	struct spdk_bdev *base;
	struct merge_base_bdev_config *base_cfg;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_merge_bdev");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "master_strip_size", merge_bdev->config->master_strip_size);
	spdk_json_write_named_uint32(w, "slave_strip_size", merge_bdev->config->slave_strip_size);

	spdk_json_write_named_array_begin(w, "base_bdevs");
	base_cfg = merge_bdev->config->master_bdev_config;
	base = base_cfg->base_bdev_info.bdev;
	if (base) {
		spdk_json_write_named_string(w, "master bdev", base->name);
	}

	base_cfg = merge_bdev->config->slave_bdev_config;
	base = base_cfg->base_bdev_info.bdev;
	if (base) {
		spdk_json_write_named_string(w, "slave bdev", base->name);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}


static int
merge_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct merge_bdev *merge_bdev = ctx;
	struct merge_base_bdev_config *base_cfg;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_dump_config_json\n");
	assert(merge_bdev != NULL);

	/* Dump the merge bdev configuration related information */
	spdk_json_write_named_object_begin(w, "merge");
	spdk_json_write_named_uint32(w, "master_strip_size", merge_bdev->config->master_strip_size);
	spdk_json_write_named_uint32(w, "slave_strip_size", merge_bdev->config->slave_strip_size);
	spdk_json_write_named_uint32(w, "state", merge_bdev->state);
	spdk_json_write_named_uint32(w, "destruct_called", merge_bdev->destruct_called);
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);

	base_cfg = merge_bdev->config->master_bdev_config;
	if (base_cfg->base_bdev_info.bdev) {
		spdk_json_write_named_string(w, "master bdev", base_cfg->base_bdev_info.bdev->name);
	} else {
		spdk_json_write_null(w);
	}

	base_cfg = merge_bdev->config->slave_bdev_config;
	if (base_cfg->base_bdev_info.bdev) {
		spdk_json_write_named_string(w, "slave bdev", base_cfg->base_bdev_info.bdev->name);
	} else {
		spdk_json_write_null(w);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	return 0;
}


static void
merge_bdev_get_running_config(FILE *fp)
{
	struct spdk_bdev *base;
	struct merge_base_bdev_config *base_cfg;
	struct merge_bdev	*mbdev;

	TAILQ_FOREACH(mbdev, &g_merge_bdev_list, link) {
		fprintf(fp, \
			"\n" \
			"[MERGE%d]\n" \
			"  Name	%s\n" \
			"  MasterStripSize %" PRIu32 "\n" \
			"  SlaveStripSize %" PRIu32 "\n" \
			"  NumDevices %u\n", \
			0, mbdev->bdev.name, mbdev->config->master_strip_size, \
			mbdev->config->slave_strip_size, 2);
		fprintf(fp,
			"	Devices ");
		base_cfg = mbdev->config->master_bdev_config;
		base = base_cfg->base_bdev_info.bdev;
		if (base) {
			fprintf(fp, \
				"%s ", \
				base->name);
		}
		fprintf(fp, \
			"\n");
		base_cfg = mbdev->config->slave_bdev_config;
		base = base_cfg->base_bdev_info.bdev;
		if (base) {
			fprintf(fp, \
				"%s ", \
				base->name);
		}
		fprintf(fp, \
			"\n");
	}

}

static struct spdk_io_channel *
merge_bdev_get_io_channel(void *ctx)
{
	struct merge_bdev *mg_bdev = ctx;

	return spdk_get_io_channel(mg_bdev);
}

static bool
get_slave_master_config(const struct merge_bdev *mbdev, struct merge_base_bdev_config **master_cfg,
			struct merge_base_bdev_config **slave_cfg)
{
	*master_cfg = mbdev->config->master_bdev_config;
	*slave_cfg = mbdev->config->slave_bdev_config;

	if (mbdev->base_bdev_discovered != 2 || *master_cfg == NULL || *slave_cfg == NULL) {
		return false;
	}
	return true;
}

struct write_ctxt {
	struct merge_bdev_io_channel			*merge_ch;
	struct merge_bdev			*mbdev;
	struct merge_base_bdev_config			*master_cfg;
	struct merge_base_bdev_config			*slave_cfg;
	union {
		/* for large I/O */
		uint8_t			buff_number;
		/* for small I/O */
		struct spdk_bdev_io			*parent_io;
	};
};

static void merge_bdev_submit_queued_request(struct merge_bdev *mbdev,
		struct merge_bdev_io_channel *merge_ch,
		struct merge_base_bdev_config *master_cfg, struct merge_base_bdev_config *slave_cfg)
{
	struct merge_master_io_queue_ele			*queue_io = NULL;
	struct merge_slave_io_queue_ele				*buf_io = NULL;
	struct spdk_bdev_io			*bdev_io = NULL;
	struct merge_bdev_config			*merge_cfg = mbdev->config;
	struct write_ctxt			*comple_cb_arg = NULL;
	uint64_t			offset  = 0;
	int			rc = 0;

	if (mbdev->queue == true || STAILQ_EMPTY(&mbdev->queued_req)) {
		return;
	}

	comple_cb_arg = calloc(1, sizeof(struct write_ctxt));
	if (comple_cb_arg == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for write context.\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	comple_cb_arg->merge_ch = merge_ch;
	comple_cb_arg->mbdev = mbdev;
	comple_cb_arg->master_cfg = master_cfg;
	comple_cb_arg->slave_cfg = slave_cfg;

	queue_io = STAILQ_FIRST(&mbdev->queued_req);
	bdev_io = queue_io->bdev_io;
	comple_cb_arg->parent_io = bdev_io;
	STAILQ_REMOVE(&mbdev->queued_req, queue_io, merge_master_io_queue_ele, link);
	free(queue_io);
	spdk_memcpy((char *)mbdev->buff_group[mbdev->buff_number] + mbdev->big_buff_size,
		    bdev_io->u.bdev.iovs[0].iov_base,
		    merge_cfg->master_strip_size);
	mbdev->big_buff_size += merge_cfg->master_strip_size;
	offset = bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks > mbdev->master_blockcnt ?
		 (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks) % mbdev->master_blockcnt :
		 bdev_io->u.bdev.offset_blocks;
	rc = spdk_bdev_writev_blocks(master_cfg->base_bdev_info.desc,
				     merge_ch->master_channel,
				     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     offset, bdev_io->u.bdev.num_blocks,
				     merge_bdev_master_write_io_completion,
				     comple_cb_arg);
	if (rc != 0) {
		SPDK_ERRLOG("Bad IO write request. error code : %d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}


	if (mbdev->big_buff_size >= merge_cfg->slave_strip_size) {
		/*
		 * Set the correspond point of buff_map as 0, and then change to an empty buffer,
		 * and if no buffer is empty, ready to queue subsequent I/Os. Besides, put each
		 * slave io into a queue, which is same as what we do for the master io. Each time
		 * we get the first slave io in the queue and submit it.
		 */
		BUF_USE(mbdev->buff_map, mbdev->buff_number);
		if (mbdev->submit_large_io) {
			buf_io = calloc(1, sizeof(struct merge_slave_io_queue_ele));
			if (buf_io == NULL) {
				SPDK_ERRLOG("Failed to allocate memory for write context.\n");
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
			buf_io->buffer_no = mbdev->buff_number;
			STAILQ_INSERT_TAIL(&mbdev->queued_buf, buf_io, link);
		} else {
			merge_bdev_write_slave(mbdev, merge_ch, master_cfg, slave_cfg, mbdev->buff_number);
		}
		mbdev->big_buff_size = 0;
		if (mbdev->buff_map != BUFFER_FILLED) {
			SWITCH_TO_EMPTY_BUFFER(mbdev->buff_map, mbdev->buff_number, rc);
		} else {
			mbdev->queue = true;
		}
	}


}

static void
merge_bdev_master_write_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct write_ctxt		*ctxt = cb_arg;

	spdk_bdev_free_io(bdev_io);
	spdk_bdev_io_complete(ctxt->parent_io,
			      success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
	if (!STAILQ_EMPTY(&ctxt->mbdev->queued_req)) {
		merge_bdev_submit_queued_request(ctxt->mbdev, ctxt->merge_ch, ctxt->master_cfg,
						 ctxt->slave_cfg);
	}
	free(ctxt);
}

static void
merge_bdev_slave_read_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io         *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);
	spdk_bdev_io_complete(parent_io,
			      success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
merge_bdev_slave_write_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct write_ctxt			*ctxt = cb_arg;
	struct merge_slave_io_queue_ele		*buf_io = NULL;

	spdk_bdev_free_io(bdev_io);

	/* After finishing writing salve bdev, set slave_state to "wait" */
	BUF_RELEASE(ctxt->mbdev->buff_map, ctxt->buff_number);
	ctxt->merge_ch->outstanding_large_io -= 1;
	if (ctxt->mbdev->queue) {
		/* If queueing, assign the buffer just released to I/Os in queue */
		ctxt->mbdev->buff_number = ctxt->buff_number;
		ctxt->mbdev->queue = false;
		merge_bdev_submit_queued_request(ctxt->mbdev, ctxt->merge_ch, ctxt->master_cfg, ctxt->slave_cfg);
	}
	if (!STAILQ_EMPTY(&ctxt->mbdev->queued_buf)) {
		buf_io = STAILQ_FIRST(&ctxt->mbdev->queued_buf);
		STAILQ_REMOVE(&ctxt->mbdev->queued_buf, buf_io, merge_slave_io_queue_ele, link);
		merge_bdev_write_slave(ctxt->mbdev, ctxt->merge_ch, ctxt->master_cfg, ctxt->slave_cfg,
				       buf_io->buffer_no);
		free(buf_io);
	}
	if (STAILQ_EMPTY(&ctxt->mbdev->queued_buf)) {
		ctxt->mbdev->submit_large_io = false;
	}
	free(ctxt);
}

static bool _test = true;

static void merge_bdev_write_slave(struct merge_bdev *mbdev, struct merge_bdev_io_channel *merge_ch,
				   struct merge_base_bdev_config *master_cfg, struct merge_base_bdev_config *slave_cfg,
				   int buf_submit)
{
	struct merge_bdev_config			*merge_cfg = mbdev->config;
	int			number_block = 0;
	int			rc  = 0;
	struct write_ctxt			*ctxt = NULL;

	ctxt = (struct write_ctxt *)calloc(1,
					   sizeof(struct write_ctxt));
	if (ctxt == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for write context.\n");
		return;
	}
	ctxt->merge_ch = merge_ch;
	ctxt->mbdev = mbdev;
	ctxt->master_cfg = master_cfg;
	ctxt->slave_cfg = slave_cfg;
	ctxt->buff_number = buf_submit;

	/* Calculate the offset of slave bdev */
	number_block = merge_cfg->slave_strip_size / mbdev->slave_blocklen;
	mbdev->big_buff_iov.iov_base = mbdev->buff_group[buf_submit];
	mbdev->big_buff_iov.iov_len = merge_cfg->slave_strip_size;
	if (_test) {
		mbdev->slave_offset = __rand64(&mbdev->max_io_rand_state) % mbdev->slave_blockcnt;
	}
	if (mbdev->slave_offset + number_block > mbdev->slave_blockcnt) {
		mbdev->slave_offset = (mbdev->slave_offset + number_block) % mbdev->slave_blockcnt;
	}

	merge_ch->outstanding_large_io += 1;
	mbdev->submit_large_io = true;
	rc = spdk_bdev_writev_blocks(slave_cfg->base_bdev_info.desc,
				     merge_ch->slave_channel,
				     &mbdev->big_buff_iov, 1,
				     mbdev->slave_offset, number_block, merge_bdev_slave_write_io_completion,
				     ctxt);
	if (rc != 0) {
		SPDK_ERRLOG("Bad IO write request submit to slave bdev. error code : %d\n", rc);
		return;
	}

	/* TODO: Currently just add up the offset. When using FTL need complex mapping progress */
	mbdev->slave_offset += mbdev->big_buff_size / mbdev->slave_blocklen;

}

static void merge_bdev_write(struct merge_bdev *mbdev, struct merge_bdev_io_channel *merge_ch,
			     struct merge_base_bdev_config *master_cfg, struct merge_base_bdev_config *slave_cfg,
			     struct spdk_bdev_io *bdev_io)
{
	struct merge_master_io_queue_ele			*queue_io;

	/* We should put the io into queue anyhow */
	queue_io = (struct merge_master_io_queue_ele *)calloc(1,
			sizeof(struct merge_master_io_queue_ele));
	queue_io->bdev_io = bdev_io;
	STAILQ_INSERT_TAIL(&mbdev->queued_req, queue_io, link);

	if (!mbdev->queue) {
		/*  Directly write I/O to master bdev */
		merge_bdev_submit_queued_request(mbdev, merge_ch, master_cfg, slave_cfg);
	}
}

static void
merge_bdev_start_rw_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct merge_bdev_io			*merge_io;
	struct merge_bdev_io_channel	*merge_ch;
	struct merge_bdev		*merge_bdev;
	struct merge_base_bdev_config			*master_bdev_config = NULL;
	struct merge_base_bdev_config			*slave_bdev_config = NULL;
	int ret = 0;

	merge_bdev = (struct merge_bdev *)bdev_io->bdev->ctxt;
	merge_io = (struct merge_bdev_io *)bdev_io->driver_ctx;
	merge_io->ch = ch;
	merge_ch =  spdk_io_channel_get_ctx(merge_io->ch);

	if (!get_slave_master_config(merge_bdev, &master_bdev_config, &slave_bdev_config)) {
		SPDK_ERRLOG("Base bdev error\n");
		return;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_readv_blocks(slave_bdev_config->base_bdev_info.desc,
					     merge_ch->slave_channel,
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, merge_bdev_slave_read_io_completion,
					     bdev_io);
		if (ret != 0) {
			SPDK_ERRLOG("Bad IO read request. error code : %d\n", ret);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		if (bdev_io->u.bdev.iovcnt != 1 ||
		    merge_bdev->config->master_strip_size != bdev_io->u.bdev.iovs[0].iov_len) {
			SPDK_ERRLOG("Bad IO write request ,iovcnt must be 1 , io size must be %d\n"
				    , merge_bdev->config->master_strip_size);
			return;
		}
		merge_bdev_write(merge_bdev, merge_ch, master_bdev_config, slave_bdev_config, bdev_io);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
	}

}



/*
 * brief:
 * _merge_bdev_io_type_supported checks whether io_type is supported in
 * all base bdev modules of merge bdev module. If anyone among the base_bdevs
 * doesn't support, the merge device doesn't supports.
 *
 * params:
 * merge_bdev - pointer to merge bdev context
 * io_type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
inline static bool
_merge_bdev_io_type_supported(struct merge_bdev *merge_bdev, enum spdk_bdev_io_type io_type)
{
	struct merge_base_bdev_config	*base_cfg;
	base_cfg = merge_bdev->config->master_bdev_config;
	if (base_cfg->base_bdev_info.bdev == NULL) {
		assert(false);
	}
	if (spdk_bdev_io_type_supported(base_cfg->base_bdev_info.bdev, io_type) == false) {
		return false;
	}
	base_cfg = merge_bdev->config->slave_bdev_config;
	if (base_cfg->base_bdev_info.bdev == NULL) {
		assert(false);
	}
	if (spdk_bdev_io_type_supported(base_cfg->base_bdev_info.bdev, io_type) == false) {
		return false;
	}

	return true;
}


static bool
merge_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP: {
		return _merge_bdev_io_type_supported(ctx, io_type);
	}
	default:
		return false;
	}

	return false;
}


static void
_merge_bdev_null_payload_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	_merge_bdev_submit_null_payload_request(bdev_io);
	return;
}

static void
merge_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		      bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	merge_bdev_start_rw_request(ch, bdev_io);
}

static void
merge_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs == NULL || bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			spdk_bdev_io_get_buf(bdev_io, merge_bdev_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		} else {
			merge_bdev_start_rw_request(ch, bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		merge_bdev_start_rw_request(ch, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		_merge_bdev_submit_reset_request(ch, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		_merge_bdev_null_payload_request(ch, bdev_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}

}



static int
merge_bdev_create_io_channel(void *io_device, void *ctx_buf)
{
	struct merge_bdev            *merge_bdev = io_device;
	struct merge_bdev_io_channel *merge_ch = ctx_buf;
	struct merge_base_bdev_config *base_cfg;

	merge_ch->outstanding_large_io = 0;
	base_cfg = merge_bdev->config->master_bdev_config;
	merge_ch->master_channel = spdk_bdev_get_io_channel(base_cfg->base_bdev_info.desc);
	if (!merge_ch->master_channel) {
		goto error;
	}
	base_cfg = merge_bdev->config->slave_bdev_config;
	merge_ch->slave_channel = spdk_bdev_get_io_channel(base_cfg->base_bdev_info.desc);
	if (!merge_ch->slave_channel) {
		goto error;
	}
	return 0;

error:
	spdk_put_io_channel(merge_ch->master_channel);
	spdk_put_io_channel(merge_ch->slave_channel);
	merge_ch->master_channel = NULL;
	merge_ch->slave_channel = NULL;
	SPDK_ERRLOG("Unable to create io channel for base bdev\n");
	return -ENOMEM;
}


/* brief: */
static void
merge_bdev_cleanup(struct merge_bdev *merge_bdev)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_cleanup, %p name %s, state %u, config %p\n", \
		      merge_bdev, \
		      merge_bdev->bdev.name, merge_bdev->state, merge_bdev->config);
	if (merge_bdev->state != MERGE_BDEV_STATE_CONFIGURING \
	    && merge_bdev->state != MERGE_BDEV_STATE_OFFLINE) {
		assert(0);
	}
	TAILQ_REMOVE(&g_merge_bdev_list, merge_bdev, link);
	spdk_free(merge_bdev->big_buff);
	free(merge_bdev->bdev.name);
	free(merge_bdev->buff_group);
	if (merge_bdev->config) {
		merge_bdev->config->merge_bdev = NULL;
	}
	free(merge_bdev);
}

/* brief: */
static void
merge_bdev_config_cleanup(struct merge_bdev_config *merge_cfg)
{
	struct merge_base_bdev_config *base_cfg;

	if (merge_cfg->slave_bdev_config) {
		base_cfg = merge_cfg->slave_bdev_config;
		free(base_cfg->name);
		base_cfg->merge_bdev = NULL;
		free(base_cfg);
	}
	if (merge_cfg->master_bdev_config) {
		base_cfg = merge_cfg->master_bdev_config;
		free(base_cfg->name);
		base_cfg->merge_bdev = NULL;
		free(base_cfg);
	}
	free(merge_cfg->name);
	free(merge_cfg);
}

/* brief: */
static void
merge_bdev_free(void)
{
	struct merge_bdev_config	*merge_cfg,	*tmp;
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_free\n");
	TAILQ_FOREACH_SAFE(merge_cfg, &g_merge_config.merge_bdev_config_head, link, tmp) {
		merge_bdev_config_cleanup(merge_cfg);
	}
}

/* brief: */
static void
merge_bdev_deconfigure(struct merge_bdev *merge_bdev, merge_bdev_destruct_cb cb_fn, \
		       void *cb_arg)
{
	if (merge_bdev->state != MERGE_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}
	assert(merge_bdev->base_bdev_discovered == 2);
	merge_bdev->state = MERGE_BDEV_STATE_OFFLINE;
	assert(merge_bdev->base_bdev_discovered);

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge bdev state changing from online to offline\n");

	spdk_bdev_unregister(&merge_bdev->bdev, cb_fn, cb_arg);
}

/* brief: */
static bool
merge_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev, struct merge_bdev **_merge_bdev, \
			     struct merge_base_bdev_config **_base_cfg)
{
	struct merge_base_bdev_config *base_cfg = NULL;
	struct merge_bdev	*merge_bdev = NULL;

	TAILQ_FOREACH(merge_bdev, &g_merge_bdev_list, link) {
		base_cfg = merge_bdev->config->master_bdev_config;
		if (base_cfg->base_bdev_info.bdev == base_bdev) {
			*_merge_bdev = merge_bdev;
			*_base_cfg = base_cfg;
			return true;
		}
		base_cfg = merge_bdev->config->slave_bdev_config;
		if (base_cfg->base_bdev_info.bdev == base_bdev) {
			*_merge_bdev = merge_bdev;
			*_base_cfg = base_cfg;
			return true;
		}
	}

	return false;
}

struct merge_bdev_timer_context {
	struct merge_bdev			*mbdev;
	struct merge_bdev_io_channel			*merge_ch;
};

static int
merge_bdev_wait_timer(void *arg)
{
	struct merge_bdev_timer_context			*ctxt = arg;
	/*
	 * Check if any I/O is still in flight before destroying the I/O channel.
	 * For now, just complete after the timer expires.
	 */
	if (ctxt->merge_ch->outstanding_large_io == 0 && STAILQ_EMPTY(&ctxt->mbdev->queued_buf)) {
		spdk_poller_unregister(&ctxt->mbdev->io_timer);
		/* After all I/Os in flight have finished, put the I/O channel back */
		spdk_put_io_channel(ctxt->merge_ch->master_channel);
		ctxt->merge_ch->master_channel = NULL;

		spdk_put_io_channel(ctxt->merge_ch->slave_channel);
		ctxt->merge_ch->slave_channel = NULL;

		free(ctxt);
		return -1;
	}
	return 0;

}

static void
merge_bdev_destroy_io_channel(void *io_device, void *ctx_buf)
{
	struct merge_bdev_io_channel *merge_ch = ctx_buf;
	struct merge_bdev			*mbdev = io_device;
	struct merge_bdev_timer_context			*timer_context = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_destroy_io_channel\n");
	/*
	 * Before clear all I/O channel, we should wait until all I/Os which have been submitted
	 * through this merge I/O channel finish. Need to get a more accurate sleep time
	 * determined by the combination of slave strip size, buff count and the performance of
	 * physical device.
	 */
	if (merge_ch->outstanding_large_io != 0 && mbdev->io_timer == NULL) {
		timer_context = calloc(1, sizeof(struct merge_bdev_timer_context));
		timer_context->merge_ch = merge_ch;
		timer_context->mbdev = mbdev;
		mbdev->io_timer = spdk_poller_register(merge_bdev_wait_timer, timer_context, 1 * 1000 * 1000);
	} else {
		spdk_put_io_channel(merge_ch->master_channel);
		merge_ch->master_channel = NULL;

		spdk_put_io_channel(merge_ch->slave_channel);
		merge_ch->slave_channel = NULL;
	}

}

static void
merge_bdev_free_base_bdev_resource(struct merge_bdev *merge_bdev, \
				   struct merge_base_bdev_config *base_bdev_cfg)
{
	struct merge_base_bdev_info *info;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_free_base_bdev_resource\n");

	info = &base_bdev_cfg->base_bdev_info;

	spdk_bdev_module_release_bdev(info->bdev);
	spdk_bdev_close(info->desc);
	info->desc = NULL;
	info->bdev = NULL;

	assert(merge_bdev->base_bdev_discovered);
	merge_bdev->base_bdev_discovered--;
}

static void
merge_bdev_remove_base_bdev(void *ctx)
{
	struct spdk_bdev	*base_bdev = ctx;
	struct merge_bdev	*merge_bdev = NULL;
	struct merge_base_bdev_config *base_cfg = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_remove_base_bdev\n");

	/* find the merge bdev whic has claimed this base_bdev */
	if (!merge_bdev_find_by_base_bdev(base_bdev, &merge_bdev, &base_cfg)) {
		SPDK_ERRLOG("bdev to remove '%s' not fount\n", base_bdev->name);
		return;
	}

	assert(base_cfg);
	assert(base_cfg->base_bdev_info.desc);

	if (merge_bdev->destruct_called == true ||
	    merge_bdev->state == MERGE_BDEV_STATE_CONFIGURING) {

		/*
		 * As merge bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 */
		merge_bdev_free_base_bdev_resource(merge_bdev, base_cfg);
		if (merge_bdev->base_bdev_discovered == 0) {
			merge_bdev_cleanup(merge_bdev);
			return;
		}
	}

	merge_bdev_deconfigure(merge_bdev, NULL, NULL);
}


static int
merge_bdev_get_ctx_size(void)
{
	return sizeof(struct merge_bdev_io);
}

static int
merge_bdev_destruct(void *ctxt)
{
	struct merge_bdev *merge_bdev = ctxt;
	struct merge_base_bdev_config *base_cfg;

	/* Before destruct, check the I/O queue for small size I/Os. If it is not empty,
	 * wait for them to be finished */
	if (!STAILQ_EMPTY(&merge_bdev->queued_req)) {
		SPDK_ERRLOG("Some master write remain unfinished!\n");
	}
	if (!STAILQ_EMPTY(&merge_bdev->queued_buf)) {
		SPDK_ERRLOG("Some slave write remain unfinished!\n");
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_destruct\n");

	merge_bdev->destruct_called = true;
	base_cfg = merge_bdev->config->master_bdev_config;
	if (base_cfg->base_bdev_info.bdev != NULL) {
		merge_bdev_free_base_bdev_resource(merge_bdev, base_cfg);
	}
	base_cfg = merge_bdev->config->slave_bdev_config;
	if (base_cfg->base_bdev_info.bdev != NULL) {
		merge_bdev_free_base_bdev_resource(merge_bdev, base_cfg);
	}

	merge_bdev->state = MERGE_BDEV_STATE_OFFLINE;
	spdk_io_device_unregister(merge_bdev, NULL);

	if (merge_bdev->base_bdev_discovered == 0) {
		/* free merge_bdev when there is no base bdevs */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge bdev base bdev is 0, going to free all in desturct\n");
		merge_bdev_cleanup(merge_bdev);
	}

	return 0;
}

static int
merge_bdev_configure(struct merge_bdev *merge_bdev)
{
	int rc = 0;

	/* register io_device */
	assert(merge_bdev->state == MERGE_BDEV_STATE_CONFIGURING);
	spdk_io_device_register(merge_bdev,
				merge_bdev_create_io_channel,
				merge_bdev_destroy_io_channel,
				sizeof(struct merge_bdev_io_channel),
				merge_bdev->bdev.name);
	/*
	 * Since we intend to expose the slave bdev, we should set the informatin about block of merge bdev same as
	 * slave block.
	 */
	/* TODO: Maybe need other method to decide the blockcnt and blocklen */
	merge_bdev->bdev.blockcnt = merge_bdev->slave_blockcnt;
	merge_bdev->bdev.blocklen = merge_bdev->slave_blocklen;
	merge_bdev->state = MERGE_BDEV_STATE_ONLINE;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "io device register %p\n", merge_bdev);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "blockcnt %lu, blocklen %u\n",
		      merge_bdev->bdev.blockcnt, merge_bdev->bdev.blocklen);

	rc = spdk_bdev_register(&merge_bdev->bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to register merge bdev and stay at configuring state\n");
		spdk_io_device_unregister(merge_bdev, NULL);
		merge_bdev->state = MERGE_BDEV_STATE_ERROR;
		return rc;
	}

	TAILQ_INSERT_TAIL(&g_merge_bdev_list, merge_bdev, link);
	return rc;
}

int
merge_bdev_create(struct merge_bdev_config *merge_bdev_config)
{
	struct merge_bdev *merge_bdev;
	struct spdk_bdev *merge_bdev_gen;
	int			i = 0;

	merge_bdev = calloc(1, sizeof(*merge_bdev));
	if (!merge_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for merge bdev\n");
		return -ENOMEM;
	}

	merge_bdev->slave_blockcnt = 0;
	merge_bdev->slave_blocklen = 0;
	merge_bdev->master_blocklen = 0;
	merge_bdev->master_blockcnt = 0;
	merge_bdev->base_bdev_discovered = 0;
	merge_bdev->big_buff = spdk_zmalloc(merge_bdev_config->slave_strip_size *
					    merge_bdev_config->buff_cnt, 8,
					    NULL,
					    SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
	if (merge_bdev->big_buff == NULL) {
		SPDK_ERRLOG("Unable to allocate big buffer for merge bdev\n");
		return -ENOMEM;
	}
	merge_bdev->buff_group = calloc(merge_bdev_config->buff_cnt, sizeof(void *));
	for (i = 0; i < merge_bdev_config->buff_cnt; i++) {
		merge_bdev->buff_group[i] = (char *)(merge_bdev->big_buff) + (i *
					    merge_bdev_config->slave_strip_size);
	}
	merge_bdev->buff_map = ((UINT32_MAX << (merge_bdev_config->buff_cnt)) ^ (UINT32_MAX));
	merge_bdev->buff_number = 0;
	merge_bdev->big_buff_size = 0;
	merge_bdev->queue = false;
	merge_bdev->submit_large_io = false;
	merge_bdev->io_timer = NULL;
	/* Maybe later in the future do not need random */
	__init_rand64(&merge_bdev->max_io_rand_state, getpid());
	merge_bdev->config = merge_bdev_config;
	merge_bdev->state = MERGE_BDEV_STATE_CONFIGURING;
	STAILQ_INIT(&merge_bdev->queued_req);
	STAILQ_INIT(&merge_bdev->queued_buf);
	merge_bdev_gen = &merge_bdev->bdev;

	merge_bdev_gen->name = strdup(merge_bdev_config->name);
	if (!merge_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for merge\n");
		/* Because base_bdev_info is bond with base_bdev_config, so we don't
		 * clear it in this step which tend to clean merge_bdev but not config.
		 */
		free(merge_bdev);
		return -ENOMEM;
	}

	merge_bdev_gen->product_name = "Merge Volume";
	merge_bdev_gen->ctxt = merge_bdev;
	merge_bdev_gen->fn_table = &g_merge_bdev_fn_table;
	merge_bdev_gen->module = &g_merge_module;
	merge_bdev_gen->write_cache = 0;

	merge_bdev_config->master_bdev_config->merge_bdev = merge_bdev;
	merge_bdev_config->slave_bdev_config->merge_bdev = merge_bdev;


	merge_bdev_config->merge_bdev = merge_bdev;

	return 0;
}


static int
merge_bdev_add_base_device(struct merge_base_bdev_config *base_cfg, struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;
	struct merge_bdev	*merge_bdev = base_cfg->merge_bdev;
	int rc;

	/* register claim */
	rc = spdk_bdev_open(bdev, true, merge_bdev_remove_base_bdev, bdev, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", bdev->name);
		return rc;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_merge_module);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "bdev %s is claimed\n", bdev->name);

	assert(base_cfg->merge_bdev->state != MERGE_BDEV_STATE_ONLINE);

	base_cfg->base_bdev_info.bdev = bdev;
	base_cfg->base_bdev_info.desc = desc;
	merge_bdev->base_bdev_discovered++;
	if (base_cfg->type == MERGE_BDEV_TYPE_MASTER) {
		merge_bdev->master_blockcnt = bdev->blockcnt;
		merge_bdev->master_blocklen = bdev->blocklen;
	} else {
		merge_bdev->slave_blockcnt = bdev->blockcnt;
		merge_bdev->slave_blocklen = bdev->blocklen;
	}

	assert(merge_bdev->base_bdev_discovered <= 2);

	if (merge_bdev->base_bdev_discovered == 2) {
		rc = merge_bdev_configure(merge_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure merge bdev\n");
			return rc;
		}
	}

	return 0;
}

int
merge_bdev_add_base_devices(struct merge_bdev_config *merge_bdev_config)
{
	struct merge_base_bdev_config *base_cfg;
	struct spdk_bdev	*base_bdev;
	int			rc = 0, _rc;

	/* Try to add master bdev */
	base_cfg = merge_bdev_config->master_bdev_config;
	base_bdev = spdk_bdev_get_by_name(base_cfg->name);
	if (base_bdev == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "base bdev %s doesn't exist now\n", base_cfg->name);
	} else {
		_rc = merge_bdev_add_base_device(base_cfg, base_bdev);
		if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to MERGE bdev %s: %s\n",
				    base_cfg->name, merge_bdev_config->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}
	/* Try to add slave bdev */
	base_cfg = merge_bdev_config->slave_bdev_config;
	base_bdev = spdk_bdev_get_by_name(base_cfg->name);
	if (base_bdev == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "base bdev %s doesn't exist now\n", base_cfg->name);
	} else {
		_rc = merge_bdev_add_base_device(base_cfg, base_bdev);
		if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to MERGE bdev %s: %s\n",
				    base_cfg->name, merge_bdev_config->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}

	return rc;
}


static struct merge_bdev_config *
merge_bdev_config_find_by_name(const char *bdev_name)
{
	struct merge_bdev_config	*merge_cfg = NULL;

	TAILQ_FOREACH(merge_cfg, &g_merge_config.merge_bdev_config_head, link) {
		if (!strcmp(bdev_name, merge_cfg->name)) {
			return merge_cfg;
		}
	}
	return NULL;
}

int
merge_bdev_config_add(const char *merge_name, uint32_t master_strip_size, uint32_t slave_strip_size,
		      uint8_t buff_cnt, struct merge_bdev_config **_merge_cfg)
{
	struct merge_bdev_config	*merge_cfg;

	merge_cfg = merge_bdev_config_find_by_name(merge_name);
	if (merge_cfg != NULL) {
		SPDK_ERRLOG("Duplicate merge bdev name found in config file %s\n",
			    merge_name);
		return -EEXIST;
	}

	merge_cfg = calloc(1, sizeof(*merge_cfg));
	if (merge_cfg == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	merge_cfg->name = strdup(merge_name);
	if (!merge_cfg->name) {
		free(merge_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}
	merge_cfg->master_strip_size = master_strip_size;
	merge_cfg->slave_strip_size = slave_strip_size;
	merge_cfg->buff_cnt = buff_cnt;
	merge_cfg->master_bdev_config = NULL;
	merge_cfg->slave_bdev_config = NULL;

	TAILQ_INSERT_TAIL(&g_merge_config.merge_bdev_config_head, merge_cfg, link);
	g_merge_config.total_merge_bdev += 1;

	*_merge_cfg = merge_cfg;
	return 0;
}

int
merge_bdev_config_add_base_bdev(struct merge_bdev_config *merge_cfg,
				const char *base_bdev_name, enum merge_bdev_type base_type)
{
	struct merge_bdev_config	*tmp = NULL;
	struct merge_base_bdev_config	*base_config = NULL;

	/* for rpc method , to check base node exist */
	TAILQ_FOREACH(tmp, &g_merge_config.merge_bdev_config_head, link) {
		if (tmp->master_bdev_config != NULL && tmp->master_bdev_config->name != NULL) {
			if (!strcmp(tmp->master_bdev_config->name, base_bdev_name)) {
				SPDK_ERRLOG("duplicate base bdev name %s mentioned\n",
					    base_bdev_name);
				return -EEXIST;
			}
		}
		if (tmp->slave_bdev_config != NULL && tmp->slave_bdev_config->name != NULL) {
			if (!strcmp(tmp->slave_bdev_config->name, base_bdev_name)) {
				SPDK_ERRLOG("duplicate base bdev name %s mentioned\n",
					    base_bdev_name);
				return -EEXIST;
			}
		}
	}

	base_config = calloc(1, sizeof(*base_config));
	if (base_config == NULL) {
		SPDK_ERRLOG("Unable to allocate memory\n");
		return -ENOMEM;
	}
	base_config->strip_size = (base_type == MERGE_BDEV_TYPE_MASTER ?
				   merge_cfg->master_strip_size : merge_cfg->slave_strip_size);
	base_config->type = base_type;
	base_config->merge_bdev = NULL;
	base_config->name = strdup(base_bdev_name);
	if (base_config->name == NULL) {
		SPDK_ERRLOG("Unable to allocate memory\n");
		return -ENOMEM;
	}
	if (base_type == MERGE_BDEV_TYPE_MASTER) {
		merge_cfg->master_bdev_config = base_config;
	} else {
		merge_cfg->slave_bdev_config = base_config;
	}

	return 0;
}

/*
 * brief:
 * merge_bdev_parse_merge is used to parse the merge bdev from config file based on
 * pre-defined merge bdev format in config file.
 * Format of config file:
 *   [Merge1]
 *   Name merge1
 *   MasterStripSize 4
 *   SlaveStripSize 1096
 *   Master Nvme1n1
 *   Slave Nvme2n1
 *
 *   [Merge2]
 *   Name merge2
 *   MasterStripSize 4
 *   SlaveStripSize 1096
 *   Master Nvme3n1
 *   Slave Nvme4n1
 *
 * params:
 * conf_section - pointer to config section
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
merge_bdev_parse_merge(struct spdk_conf_section	*conf_section)
{
	const char *merge_name;
	uint32_t master_strip_size = 0, slave_strip_size = 0;
	uint8_t	buff_cnt = 0;
	const char *master_name, *slave_name;
	struct merge_bdev_config	*merge_cfg;
	int rc, val;

	merge_name = spdk_conf_section_get_val(conf_section, "Name");
	if (merge_name == NULL) {
		SPDK_ERRLOG("merge_name is null\n");
		return -EINVAL;
	}

	/* parse the strip size */
	val = spdk_conf_section_get_intval(conf_section, "MasterStripSize");
	if (val < 0) {
		SPDK_ERRLOG("MasterStripSize must bigger than 0\n");
		return -EINVAL;
	}
	master_strip_size = val;

	val = spdk_conf_section_get_intval(conf_section, "SlaveStripSize");
	if (val < 0) {
		SPDK_ERRLOG("SlaveStripSize must bigger than 0\n");
		return -EINVAL;
	}
	slave_strip_size = val;

	if (slave_strip_size <= master_strip_size) {
		SPDK_ERRLOG("SlaveStripSize must bigger than MasterStripSize\n");
		return -EINVAL;
	}

	if (slave_strip_size % master_strip_size != 0) {
		SPDK_ERRLOG("SlaveStripSize must be a multiple of MasterStripSize\n");
		return -EINVAL;
	}

	/* Parse the number of buffers supposed to be used in merge bdev */
	val = spdk_conf_section_get_intval(conf_section, "BufferCount");
	if (val <= 0 || val >= 32) {
		SPDK_ERRLOG("BufferCount must bigger than 0 and smaller than 32\n");
		return -EINVAL;
	}
	buff_cnt = val;

	rc = merge_bdev_config_add(merge_name, master_strip_size, slave_strip_size,
				   buff_cnt, &merge_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add merge bdev config\n");
		return rc;
	}

	/* parse the master bdev and add its configuration */
	master_name = spdk_conf_section_get_val(conf_section, "Master");
	if (master_name == NULL) {
		SPDK_ERRLOG("Master name is null\n");
		return -EINVAL;
	}

	rc = merge_bdev_config_add_base_bdev(merge_cfg, master_name, MERGE_BDEV_TYPE_MASTER);
	if (rc != 0) {
		/* Free the config */
		merge_bdev_config_cleanup(merge_cfg);
		SPDK_ERRLOG("Failed to add base bdev to merge bdev config\n");
		return rc;
	}

	/* parse the slave bdev and add its configuration */
	slave_name = spdk_conf_section_get_val(conf_section, "Slave");
	if (slave_name == NULL) {
		SPDK_ERRLOG("Slave name is null\n");
		return -EINVAL;
	}

	merge_bdev_config_add_base_bdev(merge_cfg, slave_name, MERGE_BDEV_TYPE_SLAVE);
	if (rc != 0) {
		/* Free the config  */
		merge_bdev_config_cleanup(merge_cfg);
		SPDK_ERRLOG("Failed to add base bdev to merge bdev config\n");
		return rc;
	}

	/* create bdevs */
	rc = merge_bdev_create(merge_cfg);
	if (rc != 0) {
		merge_bdev_config_cleanup(merge_cfg);
		SPDK_ERRLOG("Failed to create merge bdev\n");
		return rc;
	}

	rc = merge_bdev_add_base_devices(merge_cfg);
	if (rc != 0) {
		/* Pending: do we really need to clean up config */
		merge_bdev_config_cleanup(merge_cfg);
		SPDK_ERRLOG("Failed to add any base bdev to merge bdev\n");
	}

	return 0;
}

static int
merge_bdev_parse_config(void)
{
	int	rc;
	struct spdk_conf_section	*conf_section;

	conf_section = spdk_conf_first_section(NULL);
	while (conf_section != NULL) {
		if (spdk_conf_section_match_prefix(conf_section, "Merge")) {
			rc = merge_bdev_parse_merge(conf_section);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to parse merge bdev section\n");
				return rc;
			}
		}
		conf_section = spdk_conf_next_section(conf_section);
	}

	return 0;
}

static void
merge_bdev_exit(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_exit\n");
	merge_bdev_free();
}

static int
merge_bdev_init(void)
{
	int ret;

	ret = merge_bdev_parse_config();
	if (ret < 0) {
		SPDK_ERRLOG("merge bdev init failed parsing\n");
		merge_bdev_exit();
		return ret;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_init completed successfully\n");

	return 0;

}

static bool
merge_bdev_can_claim_bdev(const char *bdev_name, struct merge_bdev_config **_merge_cfg,
			  enum merge_bdev_type *type)
{
	struct merge_bdev_config *merge_cfg;

	TAILQ_FOREACH(merge_cfg, &g_merge_config.merge_bdev_config_head, link) {
		if (!strcmp(bdev_name, merge_cfg->master_bdev_config->name)) {
			*_merge_cfg = merge_cfg;
			*type = MERGE_BDEV_TYPE_MASTER;
			return true;
		}
		if (!strcmp(bdev_name, merge_cfg->slave_bdev_config->name)) {
			*_merge_cfg = merge_cfg;
			*type = MERGE_BDEV_TYPE_SLAVE;
			return true;
		}
	}

	return false;
}

static void
merge_bdev_examine(struct spdk_bdev *bdev)
{
	struct merge_bdev_config	*merge_cfg = NULL;
	enum merge_bdev_type	type;
	if (merge_bdev_can_claim_bdev(bdev->name, &merge_cfg, &type)) {
		if (type == MERGE_BDEV_TYPE_MASTER) {
			merge_bdev_add_base_device(merge_cfg->master_bdev_config, bdev);
		} else {
			merge_bdev_add_base_device(merge_cfg->slave_bdev_config, bdev);
		}
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "bdev %s can't be claimed\n",
			      bdev->name);
	}
	spdk_bdev_module_examine_done(&g_merge_module);
}

SPDK_BDEV_MODULE_REGISTER(merge, &g_merge_module)
SPDK_LOG_REGISTER_COMPONENT("bdev_merge", SPDK_LOG_BDEV_MERGE)
