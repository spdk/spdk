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


#include "bdev_merge.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/conf.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"



int merge_bdev_config_add_master_bdev(struct merge_base_bdev_config *merge_cfg,
				      const char *master_bdev_name);
int merge_bdev_config_add_slave_bdev(struct merge_base_bdev_config *merge_cfg,
				     const char *slave_bdev_name);
int merge_bdev_create(struct merge_config *merge_config);
int merge_bdev_add_base_devices(struct merge_config *merge_config);

static int merge_bdev_init(void);
static void merge_bdev_exit(void);
static int merge_bdev_get_ctx_size(void);
static void merge_bdev_get_running_config(FILE *fp);

static int merge_bdev_destruct(void *ctxt);
static void merge_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool merge_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *merge_bdev_get_io_channel(void *ctx);
static int merge_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
static void merge_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);



static struct spdk_bdev_module g_merge_moudle = {
	.name = "merge",
	.module_init = merge_bdev_init,
	.module_fini = merge_bdev_exit,
	.get_ctx_size = merge_bdev_get_ctx_size,
	/* .examine_config = raid_bdev_examine, */
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



struct merge_config *g_merge_config;



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
	/* simply do nothing in this phase, maybe add some new operations in the future */
	return;
}


static void
_merge_bdev_submit_null_payload_request(void *_bdev_io)
{
	return;
}

static void
merge_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct merge_bdev *merge_bdev = bdev->ctxt;
	struct spdk_bdev *base;
	struct merge_base_bdev_config *base_bdev_p;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_merge_bdev");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "master_strip_size", merge_bdev->config->master_strip_size);
	spdk_json_write_named_uint32(w, "slave_strip_size", merge_bdev->config->slave_strip_size);

	spdk_json_write_named_array_begin(w, "base_bdevs");
	TAILQ_FOREACH(base_bdev_p, &merge_bdev->config->merge_base_bdev_config_head, link) {
		base = base_bdev_p->base_bdev_info.bdev;
		if (base) {
			if (base_bdev_p->type == MERGE_BDEV_TYPE_MASTER) {
				spdk_json_write_named_string(w, "master bdev", base->name);
			} else {
				spdk_json_write_named_string(w, "slave bdev", base->name);
			}
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}


static int
merge_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct merge_bdev *merge_bdev = ctx;
	struct merge_base_bdev_config *base_bdev_p;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_dump_config_json\n");
	assert(merge_bdev != NULL);

	/* Dump the merge bdev configuration related information */
	spdk_json_write_named_object_begin(w, "merge");
	spdk_json_write_named_uint32(w, "master_strip_size", merge_bdev->config->master_strip_size);
	spdk_json_write_named_uint32(w, "slave_strip_size", merge_bdev->config->slave_strip_size);
	spdk_json_write_named_uint32(w, "state", merge_bdev->state);
	spdk_json_write_named_uint32(w, "destruct_called", merge_bdev->destruct_called);
	/* TODO: how to deal with the number of master and slave bdev? */
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(base_bdev_p, &merge_bdev->config->merge_base_bdev_config_head, link) {
		if (base_bdev_p->base_bdev_info.bdev) {
			if (base_bdev_p->type == MERGE_BDEV_TYPE_MASTER) {
				spdk_json_write_named_string(w, "master bdev", base_bdev_p->base_bdev_info.bdev->name);
			} else {
				spdk_json_write_named_string(w, "slave bdev", base_bdev_p->base_bdev_info.bdev->name);
			}
		} else {
			spdk_json_write_null(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	return 0;
}


static void
merge_bdev_get_running_config(FILE *fp)
{
	struct spdk_bdev *base;
	struct merge_base_bdev_config *base_bdev_p;

	if (g_merge_config != NULL) {
		fprintf(fp, \
			"\n" \
			"[MERGE%d]\n" \
			"  Name	%s\n" \
			"  MasterStripSize %" PRIu32 "\n" \
			"  SlaveStripSize %" PRIu32 "\n" \
			"  NumDevices %u\n", \
			0, g_merge_config->merge_bdev->bdev.name, g_merge_config->master_strip_size, \
			g_merge_config->slave_strip_size, g_merge_config->total_merge_slave_bdev + 1);
		fprintf(fp,
			"	Devices ");
		TAILQ_FOREACH(base_bdev_p, &g_merge_config->merge_base_bdev_config_head, link) {
			base = base_bdev_p->base_bdev_info.bdev;
			if (base) {
				fprintf(fp, \
					"%s ", \
					base->name);
			}
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


static void
merge_bdev_io_completion_without_clear_pio(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{

	spdk_bdev_free_io(bdev_io);
}


static void
merge_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io         *parent_io = cb_arg;
	spdk_bdev_free_io(bdev_io);

	spdk_bdev_io_complete(parent_io,
			      success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
merge_bdev_slave_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io         *parent_io = cb_arg;
	spdk_bdev_free_io(bdev_io);

	spdk_bdev_io_complete(parent_io,
			      success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}


static __thread unsigned int seed = 0;
static bool _test = true;

static void
merge_bdev_start_rw_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct merge_bdev_io			*merge_io;
	struct merge_bdev_io_channel	*merge_ch;
	struct merge_bdev		*merge_bdev;
	struct merge_config		*merge_config;
	struct merge_base_bdev_config *merge_cfg, *master_bdev_config = NULL, *slave_bdev_config = NULL;
	int i = 2, ret = 0;
	int number_block = 0;
	uint64_t offset;

	merge_bdev = (struct merge_bdev *)bdev_io->bdev->ctxt;
	merge_io = (struct merge_bdev_io *)bdev_io->driver_ctx;
	merge_io->ch = ch;
	merge_ch =  spdk_io_channel_get_ctx(merge_io->ch);

	TAILQ_FOREACH(merge_cfg, &merge_bdev->config->merge_base_bdev_config_head, link) {
		i--;
		if (merge_cfg->type == MERGE_BDEV_TYPE_MASTER) {
			master_bdev_config = merge_cfg;
		} else if (merge_cfg->type == MERGE_BDEV_TYPE_SLAVE) {
			slave_bdev_config = merge_cfg;
		}
	}

	if (i != 0 || master_bdev_config == NULL || slave_bdev_config == NULL) {
		SPDK_ERRLOG("Base bdev error\n");
		return ;
	}

	merge_io->ch = ch;

	/* todo deal with number_block and offset */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_readv_blocks(slave_bdev_config->base_bdev_info.desc,
					     merge_ch->base_channel[1],
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, merge_bdev_slave_io_completion,
					     bdev_io);

	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		if (bdev_io->u.bdev.iovcnt != 1 ||
		    merge_bdev->config->master_strip_size != bdev_io->u.bdev.iovs[0].iov_len) {
			SPDK_ERRLOG("Bad IO write request ,iovcnt must be 1 , io size must be %d\n"
				    , merge_bdev->config->master_strip_size);
			return     ;
		}

		merge_config = merge_bdev->config;
		spdk_memcpy(merge_bdev->big_buff + merge_bdev->big_buff_size, bdev_io->u.bdev.iovs[0].iov_base,
			    merge_config->master_strip_size);
		merge_bdev->big_buff_size += merge_config->master_strip_size;

		offset = bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks > merge_bdev->bdev.blockcnt ?
			 (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks) % merge_bdev->bdev.blockcnt :
			 bdev_io->u.bdev.offset_blocks;
		if (merge_bdev->big_buff_size >= merge_config->slave_strip_size) {
			ret = spdk_bdev_writev_blocks(master_bdev_config->base_bdev_info.desc,
						      merge_ch->base_channel[0],
						      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						      offset % merge_bdev->bdev.blockcnt, bdev_io->u.bdev.num_blocks,
						      merge_bdev_io_completion_without_clear_pio,
						      NULL);
		} else {

			ret = spdk_bdev_writev_blocks(master_bdev_config->base_bdev_info.desc,
						      merge_ch->base_channel[0],
						      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						      offset % merge_bdev->bdev.blockcnt, bdev_io->u.bdev.num_blocks,
						      merge_bdev_io_completion,
						      bdev_io);
		}


		if (ret != 0) {
			SPDK_ERRLOG("Bad IO write request. error code : %d\n", ret);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		if (merge_bdev->big_buff_size >= merge_config->slave_strip_size) {
			number_block = merge_config->slave_strip_size / merge_config->master_strip_size *
				       (merge_config->master_strip_size / spdk_bdev_get_block_size(&merge_bdev->bdev));
			merge_bdev->big_buff_iov.iov_base = merge_bdev->big_buff;
			merge_bdev->big_buff_iov.iov_len = merge_config->slave_strip_size;
			if (_test) {
				merge_bdev->slave_offset = rand_r(&seed) % merge_bdev->bdev.blockcnt;
			}
			if (merge_bdev->slave_offset + number_block > merge_bdev->bdev.blockcnt) {
				merge_bdev->slave_offset = (merge_bdev->slave_offset + number_block) % merge_bdev->bdev.blockcnt;
			}
			ret = spdk_bdev_writev_blocks(slave_bdev_config->base_bdev_info.desc,
						      merge_ch->base_channel[1],
						      &merge_bdev->big_buff_iov, 1,
						      merge_bdev->slave_offset % merge_bdev->bdev.blockcnt, number_block, merge_bdev_slave_io_completion,
						      bdev_io);

			if (ret != 0) {
				SPDK_ERRLOG("Bad IO write request submit to slave bdev. error code : %d\n", ret);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			}
			merge_bdev->slave_offset += merge_bdev->big_buff_size / spdk_bdev_get_block_size(&merge_bdev->bdev);
			merge_bdev->big_buff_size = 0;
		}

	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
	}

}



/*
 * brief:
 * _raid_bdev_io_type_supported checks whether io_type is supported in
 * all base bdev modules of raid bdev module. If anyone among the base_bdevs
 * doesn't support, the raid device doesn't supports.
 *
 * params:
 * raid_bdev - pointer to raid bdev context
 * io_type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
inline static bool
_merge_bdev_io_type_supported(struct merge_bdev *merge_bdev, enum spdk_bdev_io_type io_type)
{
	struct merge_base_bdev_config	*conf;
	TAILQ_FOREACH(conf, &merge_bdev->config->merge_base_bdev_config_head, link) {
		if (conf->base_bdev_info.bdev == NULL) {
			assert(false);
			continue;
		}

		if (spdk_bdev_io_type_supported(conf->base_bdev_info.bdev \
						, io_type) == false) {
			return false;
		}
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
	struct merge_base_bdev_config *merge_cfg;
	int i = 0;

	merge_ch->num_channels = 2; /* fix for test */
	merge_ch->base_channel = calloc(merge_ch->num_channels, sizeof(struct spdk_io_channel *));
	if (!merge_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}
	i = merge_ch->num_channels;

	TAILQ_FOREACH(merge_cfg, &merge_bdev->config->merge_base_bdev_config_head, link) {
		i--;
		merge_ch->base_channel[i] = spdk_bdev_get_io_channel(merge_cfg->base_bdev_info.desc);

		if (!merge_ch->base_channel[i]) {
			for (int j = merge_ch->num_channels; j > i; j--) {
				spdk_put_io_channel(merge_ch->base_channel[j]);
			}
			free(merge_ch->base_channel);
			merge_ch->base_channel = NULL;
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			return -ENOMEM;
		}
	}

	return i != 0 ? -1 : 0 ;

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
	spdk_free(merge_bdev->big_buff);
	free(merge_bdev->bdev.name);
	if (merge_bdev->config) {
		merge_bdev->config->merge_bdev = NULL;
	}
	free(merge_bdev);
}

/* brief: */
static void
merge_bdev_config_cleanup(struct merge_config *merge_cfg)
{
	struct merge_base_bdev_config *base_bdev_cfg;

	while (!TAILQ_EMPTY(&merge_cfg->merge_base_bdev_config_head)) {
		base_bdev_cfg = TAILQ_FIRST(&merge_cfg->merge_base_bdev_config_head);
		if (base_bdev_cfg != NULL) {
			TAILQ_REMOVE(&merge_cfg->merge_base_bdev_config_head, base_bdev_cfg, link);
			free(base_bdev_cfg->name);
			base_bdev_cfg->merge_bdev = NULL;
			free(base_bdev_cfg);
		}
	}
	free(merge_cfg->name);
	free(merge_cfg);
}

/* brief: */
static void
merge_bdev_free(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_free\n");
	if (g_merge_config == NULL) {
		return ;
	}
	/* free config of base bdevs and then free config of merge_bdev */
	merge_bdev_config_cleanup(g_merge_config);
}

/* brief: */
static void
merge_bdev_deconfigure(struct merge_bdev *merge_bdev, merge_bdev_destruct_cb cb_fn, \
		       void *cb_arg)
{
	uint8_t	i = 0;
	struct merge_base_bdev_config *base_bdev_cfg;
	if (merge_bdev->state != MERGE_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	merge_bdev->state = MERGE_BDEV_STATE_OFFLINE;
	TAILQ_FOREACH(base_bdev_cfg, &merge_bdev->config->merge_base_bdev_config_head, link) {
		if (base_bdev_cfg->base_bdev_info.bdev != NULL && \
		    base_bdev_cfg->base_bdev_info.desc != NULL) {
			i++;
		}
	}
	assert(i);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge bdev state changing from online to offline\n");

	spdk_bdev_unregister(&merge_bdev->bdev, cb_fn, cb_arg);
}

/* brief: */
static bool
merge_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev, struct merge_bdev **_merge_bdev, \
			     struct merge_base_bdev_config **_base_bdev_p)
{
	struct merge_base_bdev_config *base_bdev_p = NULL;

	/* there is only one merge bdev for now, so only need to
	 * so only need to check the pointer for non-NULL.
	 */
	assert(g_merge_config != NULL);
	if (g_merge_config == NULL) {
		SPDK_ERRLOG("find base bdev failed, there is no merge bdev.\n");
		return false;
	}
	TAILQ_FOREACH(base_bdev_p, &g_merge_config->merge_base_bdev_config_head, link) {
		if (base_bdev_p->base_bdev_info.bdev == base_bdev) {
			*_merge_bdev = base_bdev_p->merge_bdev;
			*_base_bdev_p = base_bdev_p;
			return true;
		}
	}

	return false;
}


static void
merge_bdev_destroy_io_channel(void *io_device, void *ctx_buf)
{
	struct merge_bdev_io_channel *merge_ch = ctx_buf;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_destroy_io_channel\n");

	for (int i = 0; i < merge_ch->num_channels; i++) {
		spdk_put_io_channel(merge_ch->base_channel[i]);
	}
	free(merge_ch->base_channel);
	merge_ch->base_channel = NULL;
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
}

static void
merge_bdev_remove_base_bdev(void *ctx)
{
	struct spdk_bdev	*base_bdev = ctx;
	struct merge_bdev	*merge_bdev = NULL;
	struct merge_base_bdev_config *base_bdev_cfg = NULL;
	uint8_t		i = 0;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_remove_base_bdev\n");

	/* find the merge bdev whic has claimed this base_bdev */
	if (!merge_bdev_find_by_base_bdev(base_bdev, &merge_bdev, &base_bdev_cfg)) {
		SPDK_ERRLOG("bdev to remove '%s' not fount\n", base_bdev->name);
		return;
	}

	assert(base_bdev_cfg);

	if (merge_bdev->destruct_called == true ||
	    merge_bdev->state == MERGE_BDEV_STATE_CONFIGURING) {

		/*
		 * As merge bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 */
		merge_bdev_free_base_bdev_resource(merge_bdev, base_bdev_cfg);
		TAILQ_FOREACH(base_bdev_cfg, &merge_bdev->config->merge_base_bdev_config_head, link) {
			if (base_bdev_cfg->base_bdev_info.bdev != NULL && \
			    base_bdev_cfg->base_bdev_info.desc != NULL) {
				i++;
			}
		}
		if (i == 0) {
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


int
merge_bdev_config_add_master_bdev(struct merge_base_bdev_config *merge_cfg,
				  const char *master_bdev_name)
{
	struct merge_base_bdev_config *tmp;

	/* for rpc method , to check master node exist */
	TAILQ_FOREACH(tmp, &g_merge_config->merge_base_bdev_config_head, link) {
		if (tmp->type == MERGE_BDEV_TYPE_MASTER && tmp->merge_bdev != NULL) {
			SPDK_ERRLOG("Already contain master node : %s\n", tmp->name);
			return -EEXIST;
		}
	}

	merge_cfg->merge_bdev = NULL;
	merge_cfg->type = MERGE_BDEV_TYPE_MASTER;
	merge_cfg->name = strdup(master_bdev_name);
	if (merge_cfg->name == NULL) {
		SPDK_ERRLOG("Unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}


int
merge_bdev_config_add_slave_bdev(struct merge_base_bdev_config *merge_cfg,
				 const char *slave_bdev_name)
{
	/* todo check slave number , now we only need one slave */
	merge_cfg->name = strdup(slave_bdev_name);
	merge_cfg->type = MERGE_BDEV_TYPE_SLAVE;
	if (merge_cfg->name == NULL) {
		SPDK_ERRLOG("Unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}


static int
merge_bdev_destruct(void *ctxt)
{
	/* todo: do we need g_shutdown_started? */
	struct merge_bdev *merge_bdev = ctxt;
	struct merge_base_bdev_config *base_bdev_p;
	uint8_t i = 0, j = 0;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_destruct\n");

	merge_bdev->destruct_called = true;
	TAILQ_FOREACH(base_bdev_p, &merge_bdev->config->merge_base_bdev_config_head, link) {
		/* Close all base bdev descriptors for which call has come from below
		 * layers. Also close the descriptors if we have started shutdown.
		 */
		if (base_bdev_p->base_bdev_info.bdev != NULL) {
			merge_bdev_free_base_bdev_resource(merge_bdev, base_bdev_p);
		}
	}

	merge_bdev->state = MERGE_BDEV_STATE_OFFLINE;

	spdk_io_device_unregister(merge_bdev, NULL);

	TAILQ_FOREACH(base_bdev_p, &merge_bdev->config->merge_base_bdev_config_head, link) {
		if (base_bdev_p->type == MERGE_BDEV_TYPE_MASTER) {
			i++;
		} else {
			j++;
		}
	}
	if (i == 0 || j == 0) {
		/* free merge_bdev when there is no base bdevs or lacking one of
		 * two kinds base bdev type
		 */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge bdev master bdev is 0 or slave bdev is 0, going \
							to free all in desturct\n");
		merge_bdev_cleanup(merge_bdev);
	}

	return 0;
}

int
merge_bdev_create(struct merge_config *merge_config)
{
	struct merge_bdev *merge_bdev;
	struct spdk_bdev *merge_bdev_gen;
	struct merge_base_bdev_config *mb_config;

	merge_bdev = calloc(1, sizeof(*merge_bdev));
	if (!merge_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for merge bdev\n");
		return -ENOMEM;
	}

	merge_bdev->big_buff = spdk_zmalloc(merge_config->slave_strip_size, 8, NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
	merge_bdev->big_buff_size = 0;

	merge_bdev->config = merge_config;
	merge_bdev->state = MERGE_BDEV_STATE_CONFIGURING;
	merge_bdev_gen = &merge_bdev->bdev;

	merge_bdev_gen->name = strdup(merge_config->name);
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
	merge_bdev_gen->module = &g_merge_moudle;
	merge_bdev_gen->write_cache = 0;

	TAILQ_FOREACH(mb_config, &merge_config->merge_base_bdev_config_head, link) {
		mb_config->merge_bdev = merge_bdev;
	}

	merge_config->merge_bdev = merge_bdev;

	return 0;
}


static int
merge_bdev_add_base_device(struct merge_base_bdev_config *mb_config, struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;
	int rc;

	/* register claim */
	rc = spdk_bdev_open(bdev, true, merge_bdev_remove_base_bdev, bdev, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", bdev->name);
		return rc;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_merge_moudle);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "bdev %s is claimed\n", bdev->name);

	mb_config->base_bdev_info.bdev = bdev;
	mb_config->base_bdev_info.desc = desc;

	return rc;
}

int
merge_bdev_add_base_devices(struct merge_config *merge_config)
{
	struct merge_bdev *merge_bdev;
	struct merge_base_bdev_config *mb_config;
	struct spdk_bdev	*base_bdev;
	int			rc = 0, _rc;

	uint64_t		min_blockcnt = UINT64_MAX;
	uint32_t		blocklen = 0;

	TAILQ_FOREACH(mb_config, &merge_config->merge_base_bdev_config_head, link) {
		base_bdev = spdk_bdev_get_by_name(mb_config->name);
		if (base_bdev == NULL) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "base bdev %s doesn't exist now\n", mb_config->name);
			continue;
		}

		/* config merge bdev blockcnt */
		min_blockcnt = min_blockcnt < base_bdev->blockcnt ? min_blockcnt : base_bdev->blockcnt;

		/* todo make sure blocklen */
		blocklen = base_bdev->blocklen;

		_rc = merge_bdev_add_base_device(mb_config, base_bdev);
		if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to MERGE bdev %s: %s\n",
				    mb_config->name, merge_config->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}

	merge_bdev = merge_config->merge_bdev;

	/* register io_device */
	if (merge_bdev->state == MERGE_BDEV_STATE_CONFIGURING) {
		merge_bdev->state = MERGE_BDEV_STATE_ONLINE;
		spdk_io_device_register(merge_bdev,
					merge_bdev_create_io_channel,
					merge_bdev_destroy_io_channel,
					sizeof(struct merge_bdev_io_channel),
					merge_bdev->bdev.name);
		merge_bdev->bdev.blockcnt = min_blockcnt;
		merge_bdev->bdev.blocklen = blocklen;

		rc = spdk_bdev_register(&merge_bdev->bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to register merge bdev and stay at configuring state\n");
			spdk_io_device_unregister(merge_bdev, NULL);
			merge_bdev->state = MERGE_BDEV_STATE_ERROR;
			return rc;
		}

	} else {
		/* Disconncet descriptor and bdev pointer of all base_bdev without removing
		 * configuration of base_bdev. This is not a final method.
		 */
		SPDK_ERRLOG("Merge bdev state error.\n");
		TAILQ_FOREACH(mb_config, &merge_config->merge_base_bdev_config_head, link) {
			merge_bdev_free_base_bdev_resource(merge_bdev, mb_config);
		}
		return -1;
	}

	return rc;
}

/*
 * brief:
 * merge_bdev_parse_config is used to parse the merge bdev from config file based on
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
merge_bdev_parse_config(struct spdk_conf_section *conf_section)
{
	const char *merge_name;
	const char *master_name, *slave_name;
	struct merge_base_bdev_config *merge_cfg;
	int rc, val;

	merge_name = spdk_conf_section_get_val(conf_section, "Name");
	if (merge_name == NULL) {
		SPDK_ERRLOG("merge_name is null\n");
		return -EINVAL;
	}

	g_merge_config = calloc(1, sizeof(*g_merge_config));
	g_merge_config->name =  strdup(merge_name);
	TAILQ_INIT(&g_merge_config->merge_base_bdev_config_head);

	/* now , ali only need one slave , and i think most of situation is one */
	g_merge_config->total_merge_slave_bdev = 1;

	/* parse the strip size */
	val = spdk_conf_section_get_intval(conf_section, "MasterStripSize");
	if (val < 0) {
		SPDK_ERRLOG("MasterStripSize must bigger than 0\n");
		return -EINVAL;
	}
	g_merge_config->master_strip_size = val;

	val = spdk_conf_section_get_intval(conf_section, "SlaveStripSize");
	if (val < 0) {
		SPDK_ERRLOG("SlaveStripSize must bigger than 0\n");
		return -EINVAL;
	}
	g_merge_config->slave_strip_size = val;

	if (g_merge_config->slave_strip_size <= g_merge_config->master_strip_size) {
		SPDK_ERRLOG("SlaveStripSize must bigger than MasterStripSize\n");
		return -EINVAL;
	}

	if (g_merge_config->slave_strip_size % g_merge_config->master_strip_size != 0) {
		SPDK_ERRLOG("SlaveStripSize must be a multiple of MasterStripSize\n");
		return -EINVAL;
	}

	/* parse the master bdev */
	master_name = spdk_conf_section_get_val(conf_section, "Master");
	if (master_name == NULL) {
		SPDK_ERRLOG("Master name is null\n");
		return -EINVAL;
	}

	merge_cfg = calloc(1, sizeof(*merge_cfg));
	merge_cfg->strip_size = g_merge_config->master_strip_size;
	rc = merge_bdev_config_add_master_bdev(merge_cfg, master_name);
	if (rc != 0) {
		/* Free the config */
		free(merge_cfg);
		SPDK_ERRLOG("Failed to add base bdev to merge bdev config\n");
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_merge_config->merge_base_bdev_config_head, merge_cfg, link);

	/* parse the slave bdev */
	slave_name = spdk_conf_section_get_val(conf_section, "Slave");
	if (slave_name == NULL) {
		SPDK_ERRLOG("Slave name is null\n");
		return -EINVAL;
	}

	merge_cfg = calloc(1, sizeof(*merge_cfg));
	merge_cfg->strip_size = g_merge_config->slave_strip_size;
	merge_bdev_config_add_slave_bdev(merge_cfg, slave_name);
	if (rc != 0) {
		/* Free the config  */
		free(merge_cfg);
		SPDK_ERRLOG("Failed to add base bdev to merge bdev config\n");
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_merge_config->merge_base_bdev_config_head, merge_cfg, link);

	/* create bdevs */
	rc = merge_bdev_create(g_merge_config);
	if (rc != 0) {
		merge_bdev_config_cleanup(g_merge_config);
		SPDK_ERRLOG("Failed to create merge bdev\n");
		return rc;
	}

	rc = merge_bdev_add_base_devices(g_merge_config);
	if (rc != 0) {
		/* Pending: do we really need to clean up config */
		merge_bdev_config_cleanup(g_merge_config);
		SPDK_ERRLOG("Failed to add any base bdev to merge bdev\n");
	}

	return 0;
}


/* todo deal [Merge1] ... [Merge2] now , only support one section */
static int
merge_bdev_parse_config_root(void)
{
	int ret;
	struct spdk_conf_section *conf_section;
	/* multi [Merge]
	conf_section = spdk_conf_first_section(NULL);
	while (conf_section != NULL) {
		if (spdk_conf_section_match_prefix(conf_section, "Merge")) {
			ret = merge_bdev_parse_config(conf_section);
			if (ret < 0) {
				SPDK_ERRLOG("Unable to parse merge bdev section\n");
				return ret;
			}
		}
		conf_section = spdk_conf_next_section(conf_section);
	} */

	conf_section = spdk_conf_find_section(NULL, "Merge");
	if (conf_section != NULL) {
		ret = merge_bdev_parse_config(conf_section);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse merge bdev section\n");
			return ret;
		}
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

	ret = merge_bdev_parse_config_root();
	if (ret < 0) {
		SPDK_ERRLOG("merge bdev init failed parsing\n");
		merge_bdev_exit();
		return ret;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_init completed successfully\n");

	return 0;

}

SPDK_BDEV_MODULE_REGISTER(merge, &g_merge_moudle)
SPDK_LOG_REGISTER_COMPONENT("bdev_merge", SPDK_LOG_BDEV_MERGE)
