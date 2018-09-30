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

#include "bdev_raid.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/conf.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"

static bool g_shutdown_started = false;

/* raid bdev config as read from config file */
struct raid_config          g_spdk_raid_config = {
	.raid_bdev_config_head = TAILQ_HEAD_INITIALIZER(g_spdk_raid_config.raid_bdev_config_head),
};

/*
 * List of raid bdev in configured list, these raid bdevs are registered with
 * bdev layer
 */
struct spdk_raid_configured_tailq       g_spdk_raid_bdev_configured_list;

/* List of raid bdev in configuring list */
struct spdk_raid_configuring_tailq      g_spdk_raid_bdev_configuring_list;

/* List of all raid bdevs */
struct spdk_raid_all_tailq              g_spdk_raid_bdev_list;

/* List of all raid bdevs that are offline */
struct spdk_raid_offline_tailq          g_spdk_raid_bdev_offline_list;

/* Function declarations */
static void   raid_bdev_examine(struct spdk_bdev *bdev);
static int    raid_bdev_init(void);
static void   raid_bdev_waitq_io_process(void *ctx);
static void   raid_bdev_deconfigure(struct raid_bdev *raid_bdev);


/*
 * brief:
 * raid_bdev_create_cb function is a cb function for raid bdev which creates the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev            *raid_bdev = io_device;
	struct raid_bdev_io_channel *raid_ch = ctx_buf;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_create_cb, %p\n", raid_ch);

	assert(raid_bdev != NULL);
	assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);

	raid_ch->base_channel = calloc(raid_bdev->num_base_bdevs,
				       sizeof(struct spdk_io_channel *));
	if (!raid_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}
	for (uint32_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 */
		raid_ch->base_channel[i] = spdk_bdev_get_io_channel(
						   raid_bdev->base_bdev_info[i].desc);
		if (!raid_ch->base_channel[i]) {
			for (uint32_t j = 0; j < i; j++) {
				spdk_put_io_channel(raid_ch->base_channel[j]);
			}
			free(raid_ch->base_channel);
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			return -ENOMEM;
		}
	}

	return 0;
}

/*
 * brief:
 * raid_bdev_destroy_cb function is a cb function for raid bdev which deletes the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * none
 */
static void
raid_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev_io_channel *raid_ch = ctx_buf;
	struct raid_bdev            *raid_bdev = io_device;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_destroy_cb\n");

	assert(raid_bdev != NULL);
	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);
	for (uint32_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* Free base bdev channels */
		assert(raid_ch->base_channel[i] != NULL);
		spdk_put_io_channel(raid_ch->base_channel[i]);
		raid_ch->base_channel[i] = NULL;
	}
	free(raid_ch->base_channel);
	raid_ch->base_channel = NULL;
}

/*
 * brief:
 * raid_bdev_cleanup is used to cleanup and free raid_bdev related data
 * structures.
 * params:
 * raid_bdev - pointer to raid_bdev
 * returns:
 * none
 */
void
raid_bdev_cleanup(struct raid_bdev *raid_bdev)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_cleanup, %p name %s, state %u, config %p\n",
		      raid_bdev,
		      raid_bdev->bdev.name, raid_bdev->state, raid_bdev->config);
	if (raid_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
		TAILQ_REMOVE(&g_spdk_raid_bdev_configuring_list, raid_bdev, state_link);
	} else if (raid_bdev->state == RAID_BDEV_STATE_OFFLINE) {
		TAILQ_REMOVE(&g_spdk_raid_bdev_offline_list, raid_bdev, state_link);
	} else {
		assert(0);
	}
	TAILQ_REMOVE(&g_spdk_raid_bdev_list, raid_bdev, global_link);
	free(raid_bdev->bdev.name);
	raid_bdev->bdev.name = NULL;
	assert(raid_bdev->base_bdev_info);
	free(raid_bdev->base_bdev_info);
	raid_bdev->base_bdev_info = NULL;
	if (raid_bdev->config) {
		raid_bdev->config->raid_bdev = NULL;
	}
	free(raid_bdev);
}

/*
 * brief:
 * free resource of base bdev for raid bdev
 * params:
 * raid_bdev - pointer to raid bdev
 * base_bdev_slot - position to base bdev in raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
void
raid_bdev_free_base_bdev_resource(struct raid_bdev *raid_bdev, uint32_t base_bdev_slot)
{
	struct raid_base_bdev_info *info;

	info = &raid_bdev->base_bdev_info[base_bdev_slot];

	spdk_bdev_module_release_bdev(info->bdev);
	spdk_bdev_close(info->desc);
	info->desc = NULL;
	info->bdev = NULL;

	assert(raid_bdev->num_base_bdevs_discovered);
	raid_bdev->num_base_bdevs_discovered--;
}

/*
 * brief:
 * raid_bdev_destruct is the destruct function table pointer for raid bdev
 * params:
 * ctxt - pointer to raid_bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_destruct(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_destruct\n");

	raid_bdev->destruct_called = true;
	for (uint16_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started ||
		    ((raid_bdev->base_bdev_info[i].remove_scheduled == true) &&
		     (raid_bdev->base_bdev_info[i].bdev != NULL))) {
			raid_bdev_free_base_bdev_resource(raid_bdev, i);
		}
	}

	if (g_shutdown_started) {
		TAILQ_REMOVE(&g_spdk_raid_bdev_configured_list, raid_bdev, state_link);
		raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
		TAILQ_INSERT_TAIL(&g_spdk_raid_bdev_offline_list, raid_bdev, state_link);
		spdk_io_device_unregister(raid_bdev, NULL);
	}

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* Free raid_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid bdev base bdevs is 0, going to free all in destruct\n");
		raid_bdev_cleanup(raid_bdev);
	}

	return 0;
}

/*
 * brief:
 * raid_bdev_io_completion function is called by lower layers to notify raid
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context, like parent io pointer
 * returns:
 * none
 */
static void
raid_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io         *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		spdk_bdev_io_complete(parent_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(parent_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * brief:
 * raid_bdev_submit_rw_request function is used to submit I/O to the correct
 * member disk
 * params:
 * bdev_io - parent bdev io
 * start_strip - start strip number of this io
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_submit_rw_request(struct spdk_bdev_io *bdev_io, uint64_t start_strip)
{
	struct raid_bdev_io		*raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	struct raid_bdev_io_channel	*raid_ch = spdk_io_channel_get_ctx(raid_io->ch);
	struct raid_bdev		*raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	uint64_t			pd_strip;
	uint32_t			offset_in_strip;
	uint64_t			pd_lba;
	uint64_t			pd_blocks;
	uint32_t			pd_idx;
	int				ret = 0;

	pd_strip = start_strip / raid_bdev->num_base_bdevs;
	pd_idx = start_strip % raid_bdev->num_base_bdevs;
	offset_in_strip = bdev_io->u.bdev.offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;
	pd_blocks = bdev_io->u.bdev.num_blocks;
	if (raid_bdev->base_bdev_info[pd_idx].desc == NULL) {
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and function callback context
	 */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_readv_blocks(raid_bdev->base_bdev_info[pd_idx].desc,
					     raid_ch->base_channel[pd_idx],
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     pd_lba, pd_blocks, raid_bdev_io_completion,
					     bdev_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = spdk_bdev_writev_blocks(raid_bdev->base_bdev_info[pd_idx].desc,
					      raid_ch->base_channel[pd_idx],
					      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					      pd_lba, pd_blocks, raid_bdev_io_completion,
					      bdev_io);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
		assert(0);
	}

	return ret;
}

/*
 * brief:
 * get_curr_base_bdev_index function calculates the base bdev index
 * params:
 * raid_bdev - pointer to pooled bdev
 * raid_io - pointer to parent io context
 * returns:
 * base bdev index
 */
static uint8_t
get_curr_base_bdev_index(struct raid_bdev *raid_bdev, struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io	*bdev_io;
	uint64_t		start_strip;

	bdev_io = SPDK_CONTAINEROF(raid_io, struct spdk_bdev_io, driver_ctx);
	start_strip = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;

	return (start_strip % raid_bdev->num_base_bdevs);
}

/*
 * brief:
 * raid_bdev_io_submit_fail_process function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * bdev_io - pointer to bdev_io
 * raid_io - pointer to raid bdev io
 * ret - return code
 * returns:
 * none
 */
static void
raid_bdev_io_submit_fail_process(struct raid_bdev *raid_bdev, struct spdk_bdev_io *bdev_io,
				 struct raid_bdev_io *raid_io, int ret)
{
	struct   raid_bdev_io_channel *raid_ch;
	uint8_t pd_idx;

	if (ret != -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		/* Queue the IO to bdev layer wait queue */
		pd_idx = get_curr_base_bdev_index(raid_bdev, raid_io);
		raid_io->waitq_entry.bdev = raid_bdev->base_bdev_info[pd_idx].bdev;
		raid_io->waitq_entry.cb_fn = raid_bdev_waitq_io_process;
		raid_io->waitq_entry.cb_arg = raid_io;
		raid_ch = spdk_io_channel_get_ctx(raid_io->ch);
		if (spdk_bdev_queue_io_wait(raid_bdev->base_bdev_info[pd_idx].bdev,
					    raid_ch->base_channel[pd_idx],
					    &raid_io->waitq_entry) != 0) {
			SPDK_ERRLOG("bdev io waitq error, it should not happen\n");
			assert(0);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/*
 * brief:
 * raid_bdev_waitq_io_process function is the callback function
 * registered by raid bdev module to bdev when bdev_io was unavailable.
 * params:
 * ctx - pointer to raid_bdev_io
 * returns:
 * none
 */
static void
raid_bdev_waitq_io_process(void *ctx)
{
	struct   raid_bdev_io         *raid_io = ctx;
	struct   spdk_bdev_io         *bdev_io;
	struct   raid_bdev            *raid_bdev;
	int                           ret;
	uint64_t                      start_strip;

	bdev_io = SPDK_CONTAINEROF(raid_io, struct spdk_bdev_io, driver_ctx);
	/*
	 * Try to submit childs of parent bdev io. If failed due to resource
	 * crunch then break the loop and don't try to process other queued IOs.
	 */
	raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	start_strip = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	ret = raid_bdev_submit_rw_request(bdev_io, start_strip);
	if (ret != 0) {
		raid_bdev_io_submit_fail_process(raid_bdev, bdev_io, raid_io, ret);
	}
}

/*
 * brief:
 * raid_bdev_start_rw_request function is the submit_request function for
 * read/write requests
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
raid_bdev_start_rw_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid_bdev_io		*raid_io;
	struct raid_bdev		*raid_bdev;
	uint64_t			start_strip = 0;
	uint64_t			end_strip = 0;
	int				ret;

	raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	raid_io->ch = ch;
	start_strip = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		assert(false);
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	ret = raid_bdev_submit_rw_request(bdev_io, start_strip);
	if (ret != 0) {
		raid_bdev_io_submit_fail_process(raid_bdev, bdev_io, raid_io, ret);
	}
}

/*
 * brief:
 * raid_bdev_reset_completion is the completion callback for member disk resets
 * params:
 * bdev_io - pointer to member disk reset bdev_io
 * success - true if reset was successful, false if unsuccessful
 * cb_arg - callback argument (parent reset bdev_io)
 * returns:
 * none
 */
static void
raid_bdev_reset_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;
	struct raid_bdev *raid_bdev = (struct raid_bdev *)parent_io->bdev->ctxt;
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)parent_io->driver_ctx;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		raid_io->base_bdev_reset_status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	raid_io->base_bdev_reset_completed++;
	if (raid_io->base_bdev_reset_completed == raid_bdev->num_base_bdevs) {
		spdk_bdev_io_complete(parent_io, raid_io->base_bdev_reset_status);
	}
}

/*
 * brief:
 * _raid_bdev_submit_reset_request_next function submits the next batch of reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
_raid_bdev_submit_reset_request_next(void *_bdev_io)
{
	struct spdk_bdev_io		*bdev_io = _bdev_io;
	struct raid_bdev_io		*raid_io;
	struct raid_bdev		*raid_bdev;
	struct raid_bdev_io_channel	*raid_ch;
	int				ret;
	uint8_t				i;

	raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	raid_ch = spdk_io_channel_get_ctx(raid_io->ch);

	while (raid_io->base_bdev_reset_submitted < raid_bdev->num_base_bdevs) {
		i = raid_io->base_bdev_reset_submitted;
		ret = spdk_bdev_reset(raid_bdev->base_bdev_info[i].desc,
				      raid_ch->base_channel[i],
				      raid_bdev_reset_completion, bdev_io);
		if (ret == 0) {
			raid_io->base_bdev_reset_submitted++;
		} else if (ret == -ENOMEM) {
			raid_io->waitq_entry.bdev = raid_bdev->base_bdev_info[i].bdev;
			raid_io->waitq_entry.cb_fn = _raid_bdev_submit_reset_request_next;
			raid_io->waitq_entry.cb_arg = bdev_io;
			spdk_bdev_queue_io_wait(raid_bdev->base_bdev_info[i].bdev,
						raid_ch->base_channel[i],
						&raid_io->waitq_entry);
			return;
		} else {
			assert(false);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * brief:
 * _raid_bdev_submit_reset_request function is the submit_request function for
 * reset requests
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
_raid_bdev_submit_reset_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid_bdev_io		*raid_io;

	raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	raid_io->ch = ch;
	raid_io->base_bdev_reset_submitted = 0;
	raid_io->base_bdev_reset_completed = 0;
	raid_io->base_bdev_reset_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	_raid_bdev_submit_reset_request_next(bdev_io);
}

/*
 * brief:
 * raid_bdev_submit_request function is the submit_request function pointer of
 * raid bdev function table. This is used to submit the io on raid_bdev to below
 * layers.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
raid_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			spdk_bdev_io_get_buf(bdev_io, raid_bdev_start_rw_request,
					     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		} else {
			/* Just call it directly if iov_base is already populated. */
			raid_bdev_start_rw_request(ch, bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		raid_bdev_start_rw_request(ch, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		// TODO: support flush if requirement comes
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		_raid_bdev_submit_reset_request(ch, bdev_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}

}

/*
 * brief:
 * raid_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * raid bdev module
 * params:
 * ctx - pointer to raid bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
static bool
raid_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * raid_bdev_get_io_channel is the get_io_channel function table pointer for
 * raid bdev. This is used to return the io channel for this raid bdev
 * params:
 * ctxt - pointer to raid_bdev
 * returns:
 * pointer to io channel for raid bdev
 */
static struct spdk_io_channel *
raid_bdev_get_io_channel(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;

	return spdk_get_io_channel(raid_bdev);
}

/*
 * brief:
 * raid_bdev_dump_info_json is the function table pointer for raid bdev
 * params:
 * ctx - pointer to raid_bdev
 * w - pointer to json context
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_dump_config_json\n");
	assert(raid_bdev != NULL);

	/* Dump the raid bdev configuration related information */
	spdk_json_write_name(w, "raid");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "strip_size", raid_bdev->strip_size);
	spdk_json_write_named_uint32(w, "state", raid_bdev->state);
	spdk_json_write_named_uint32(w, "raid_level", raid_bdev->raid_level);
	spdk_json_write_named_uint32(w, "destruct_called", raid_bdev->destruct_called);
	spdk_json_write_named_uint32(w, "num_base_bdevs", raid_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", raid_bdev->num_base_bdevs_discovered);
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	for (uint16_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
		if (raid_bdev->base_bdev_info[i].bdev) {
			spdk_json_write_string(w, raid_bdev->base_bdev_info[i].bdev->name);
		} else {
			spdk_json_write_null(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	return 0;
}

/*
 * brief:
 * raid_bdev_write_config_json is the function table pointer for raid bdev
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
static void
raid_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = bdev->ctxt;
	struct spdk_bdev *base;
	uint16_t i;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_raid_bdev");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "strip_size", raid_bdev->strip_size);
	spdk_json_write_named_uint32(w, "raid_level", raid_bdev->raid_level);

	spdk_json_write_named_array_begin(w, "base_bdevs");
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		base = raid_bdev->base_bdev_info[i].bdev;
		if (base) {
			spdk_json_write_string(w, base->name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/* g_raid_bdev_fn_table is the function table for raid bdev */
static const struct spdk_bdev_fn_table g_raid_bdev_fn_table = {
	.destruct		= raid_bdev_destruct,
	.submit_request		= raid_bdev_submit_request,
	.io_type_supported	= raid_bdev_io_type_supported,
	.get_io_channel		= raid_bdev_get_io_channel,
	.dump_info_json		= raid_bdev_dump_info_json,
	.write_config_json	= raid_bdev_write_config_json,
};

/*
 * brief:
 * raid_bdev_config_cleanup function is used to free memory for one raid_bdev in configuration
 * params:
 * raid_cfg - pointer to raid_bdev_config structure
 * returns:
 * none
 */
void
raid_bdev_config_cleanup(struct raid_bdev_config *raid_cfg)
{
	uint32_t i;

	TAILQ_REMOVE(&g_spdk_raid_config.raid_bdev_config_head, raid_cfg, link);
	g_spdk_raid_config.total_raid_bdev--;

	if (raid_cfg->base_bdev) {
		for (i = 0; i < raid_cfg->num_base_bdevs; i++) {
			free(raid_cfg->base_bdev[i].name);
		}
		free(raid_cfg->base_bdev);
	}
	free(raid_cfg->name);
	free(raid_cfg);
}

/*
 * brief:
 * raid_bdev_free is the raid bdev function table function pointer. This is
 * called on bdev free path
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_free(void)
{
	struct raid_bdev_config *raid_cfg, *tmp;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_free\n");
	TAILQ_FOREACH_SAFE(raid_cfg, &g_spdk_raid_config.raid_bdev_config_head, link, tmp) {
		raid_bdev_config_cleanup(raid_cfg);
	}
}

/* brief
 * raid_bdev_config_find_by_name is a helper function to find raid bdev config
 * by name as key.
 *
 * params:
 * raid_name - name for raid bdev.
 */
struct raid_bdev_config *
raid_bdev_config_find_by_name(const char *raid_name)
{
	struct raid_bdev_config *raid_cfg;

	TAILQ_FOREACH(raid_cfg, &g_spdk_raid_config.raid_bdev_config_head, link) {
		if (!strcmp(raid_cfg->name, raid_name)) {
			return raid_cfg;
		}
	}

	return raid_cfg;
}

/*
 * brief
 * raid_bdev_config_add function adds config for newly created raid bdev.
 *
 * params:
 * raid_name - name for raid bdev.
 * strip_size - strip size in KB
 * num_base_bdevs - number of base bdevs.
 * raid_level - raid level, only raid level 0 is supported.
 * _raid_cfg - Pointer to newly added configuration
 */
int
raid_bdev_config_add(const char *raid_name, int strip_size, int num_base_bdevs,
		     int raid_level, struct raid_bdev_config **_raid_cfg)
{
	struct raid_bdev_config *raid_cfg;

	raid_cfg = raid_bdev_config_find_by_name(raid_name);
	if (raid_cfg != NULL) {
		SPDK_ERRLOG("Duplicate raid bdev name found in config file %s\n",
			    raid_name);
		return -EEXIST;
	}

	if (spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %d\n", strip_size);
		return -EINVAL;
	}

	if (num_base_bdevs <= 0) {
		SPDK_ERRLOG("Invalid base device count %d\n", num_base_bdevs);
		return -EINVAL;
	}

	if (raid_level != 0) {
		SPDK_ERRLOG("invalid raid level %d, only raid level 0 is supported\n",
			    raid_level);
		return -EINVAL;
	}

	raid_cfg = calloc(1, sizeof(*raid_cfg));
	if (raid_cfg == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	raid_cfg->name = strdup(raid_name);
	if (!raid_cfg->name) {
		free(raid_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}
	raid_cfg->strip_size = strip_size;
	raid_cfg->num_base_bdevs = num_base_bdevs;
	raid_cfg->raid_level = raid_level;

	raid_cfg->base_bdev = calloc(num_base_bdevs, sizeof(*raid_cfg->base_bdev));
	if (raid_cfg->base_bdev == NULL) {
		free(raid_cfg->name);
		free(raid_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_spdk_raid_config.raid_bdev_config_head, raid_cfg, link);
	g_spdk_raid_config.total_raid_bdev++;

	*_raid_cfg = raid_cfg;
	return 0;
}

/*
 * brief:
 * raid_bdev_config_add_base_bdev function add base bdev to raid bdev config.
 *
 * params:
 * raid_cfg - pointer to raid bdev configuration
 * base_bdev_name - name of base bdev
 * slot - Position to add base bdev
 */
int
raid_bdev_config_add_base_bdev(struct raid_bdev_config *raid_cfg, const char *base_bdev_name,
			       uint32_t slot)
{
	uint32_t i;
	struct raid_bdev_config *tmp;

	if (slot >= raid_cfg->num_base_bdevs) {
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &g_spdk_raid_config.raid_bdev_config_head, link) {
		for (i = 0; i < tmp->num_base_bdevs; i++) {
			if (tmp->base_bdev[i].name != NULL) {
				if (!strcmp(tmp->base_bdev[i].name, base_bdev_name)) {
					SPDK_ERRLOG("duplicate base bdev name %s mentioned\n",
						    base_bdev_name);
					return -EEXIST;
				}
			}
		}
	}

	raid_cfg->base_bdev[slot].name = strdup(base_bdev_name);
	if (raid_cfg->base_bdev[slot].name == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}
/*
 * brief:
 * raid_bdev_parse_raid is used to parse the raid bdev from config file based on
 * pre-defined raid bdev format in config file.
 * Format of config file:
 *   [RAID1]
 *   Name raid1
 *   StripSize 64
 *   NumDevices 2
 *   RaidLevel 0
 *   Devices Nvme0n1 Nvme1n1
 *
 *   [RAID2]
 *   Name raid2
 *   StripSize 64
 *   NumDevices 3
 *   RaidLevel 0
 *   Devices Nvme2n1 Nvme3n1 Nvme4n1
 *
 * params:
 * conf_section - pointer to config section
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_parse_raid(struct spdk_conf_section *conf_section)
{
	const char *raid_name;
	int strip_size;
	int i, num_base_bdevs;
	int raid_level;
	const char *base_bdev_name;
	struct raid_bdev_config *raid_cfg;
	int rc;

	raid_name = spdk_conf_section_get_val(conf_section, "Name");
	if (raid_name == NULL) {
		SPDK_ERRLOG("raid_name %s is null\n", raid_name);
		return -EINVAL;
	}

	strip_size = spdk_conf_section_get_intval(conf_section, "StripSize");
	num_base_bdevs = spdk_conf_section_get_intval(conf_section, "NumDevices");
	raid_level = spdk_conf_section_get_intval(conf_section, "RaidLevel");

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "%s %d %d %d\n", raid_name, strip_size, num_base_bdevs,
		      raid_level);

	rc = raid_bdev_config_add(raid_name, strip_size, num_base_bdevs, raid_level,
				  &raid_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add raid bdev config\n");
		return rc;
	}

	for (i = 0; true; i++) {
		base_bdev_name = spdk_conf_section_get_nmval(conf_section, "Devices", 0, i);
		if (base_bdev_name == NULL) {
			break;
		}
		if (i >= num_base_bdevs) {
			raid_bdev_config_cleanup(raid_cfg);
			SPDK_ERRLOG("Number of devices mentioned is more than count\n");
			return -EINVAL;
		}

		rc = raid_bdev_config_add_base_bdev(raid_cfg, base_bdev_name, i);
		if (rc != 0) {
			raid_bdev_config_cleanup(raid_cfg);
			SPDK_ERRLOG("Failed to add base bdev to raid bdev config\n");
			return rc;
		}
	}

	if (i != raid_cfg->num_base_bdevs) {
		raid_bdev_config_cleanup(raid_cfg);
		SPDK_ERRLOG("Number of devices mentioned is less than count\n");
		return -EINVAL;
	}

	rc = raid_bdev_create(raid_cfg);
	if (rc != 0) {
		raid_bdev_config_cleanup(raid_cfg);
		SPDK_ERRLOG("Failed to create raid bdev\n");
		return rc;
	}

	rc = raid_bdev_add_base_devices(raid_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add any base bdev to raid bdev\n");
		/* Config is not removed in this case. */
	}

	return 0;
}

/*
 * brief:
 * raid_bdev_parse_config is used to find the raid bdev config section and parse it
 * Format of config file:
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_parse_config(void)
{
	int                      ret;
	struct spdk_conf_section *conf_section;

	conf_section = spdk_conf_first_section(NULL);
	while (conf_section != NULL) {
		if (spdk_conf_section_match_prefix(conf_section, "RAID")) {
			ret = raid_bdev_parse_raid(conf_section);
			if (ret < 0) {
				SPDK_ERRLOG("Unable to parse raid bdev section\n");
				return ret;
			}
		}
		conf_section = spdk_conf_next_section(conf_section);
	}

	return 0;
}

/*
 * brief:
 * raid_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_fini_start(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_fini_start\n");
	g_shutdown_started = true;
}

/*
 * brief:
 * raid_bdev_exit is called on raid bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_exit(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_exit\n");
	raid_bdev_free();
}

/*
 * brief:
 * raid_bdev_get_ctx_size is used to return the context size of bdev_io for raid
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for raid
 */
static int
raid_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_get_ctx_size\n");
	return sizeof(struct raid_bdev_io);
}

/*
 * brief:
 * raid_bdev_get_running_config is used to get the configuration options.
 *
 * params:
 * fp - The pointer to a file that will be written to the configuration options.
 * returns:
 * none
 */
static void
raid_bdev_get_running_config(FILE *fp)
{
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *base;
	int index = 1;
	uint16_t i;

	TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_configured_list, state_link) {
		fprintf(fp,
			"\n"
			"[RAID%d]\n"
			"  Name %s\n"
			"  StripSize %" PRIu32 "\n"
			"  NumDevices %hu\n"
			"  RaidLevel %hhu\n",
			index, raid_bdev->bdev.name, raid_bdev->strip_size,
			raid_bdev->num_base_bdevs, raid_bdev->raid_level);
		fprintf(fp,
			"  Devices ");
		for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
			base = raid_bdev->base_bdev_info[i].bdev;
			if (base) {
				fprintf(fp,
					"%s ",
					base->name);
			}
		}
		fprintf(fp,
			"\n");
		index++;
	}
}

/*
 * brief:
 * raid_bdev_can_claim_bdev is the function to check if this base_bdev can be
 * claimed by raid bdev or not.
 * params:
 * bdev_name - represents base bdev name
 * _raid_cfg - pointer to raid bdev config parsed from config file
 * base_bdev_slot - if bdev can be claimed, it represents the base_bdev correct
 * slot. This field is only valid if return value of this function is true
 * returns:
 * true - if bdev can be claimed
 * false - if bdev can't be claimed
 */
static bool
raid_bdev_can_claim_bdev(const char *bdev_name, struct raid_bdev_config **_raid_cfg,
			 uint32_t *base_bdev_slot)
{
	struct raid_bdev_config *raid_cfg;
	uint32_t i;

	TAILQ_FOREACH(raid_cfg, &g_spdk_raid_config.raid_bdev_config_head, link) {
		for (i = 0; i < raid_cfg->num_base_bdevs; i++) {
			/*
			 * Check if the base bdev name is part of raid bdev configuration.
			 * If match is found then return true and the slot information where
			 * this base bdev should be inserted in raid bdev
			 */
			if (!strcmp(bdev_name, raid_cfg->base_bdev[i].name)) {
				*_raid_cfg = raid_cfg;
				*base_bdev_slot = i;
				return true;
			}
		}
	}

	return false;
}


static struct spdk_bdev_module g_raid_if = {
	.name = "raid",
	.module_init = raid_bdev_init,
	.fini_start = raid_bdev_fini_start,
	.module_fini = raid_bdev_exit,
	.get_ctx_size = raid_bdev_get_ctx_size,
	.examine_config = raid_bdev_examine,
	.config_text = raid_bdev_get_running_config,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(&g_raid_if)

/*
 * brief:
 * raid_bdev_init is the initialization function for raid bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_init(void)
{
	int ret;

	TAILQ_INIT(&g_spdk_raid_bdev_configured_list);
	TAILQ_INIT(&g_spdk_raid_bdev_configuring_list);
	TAILQ_INIT(&g_spdk_raid_bdev_list);
	TAILQ_INIT(&g_spdk_raid_bdev_offline_list);

	/* Parse config file for raids */
	ret = raid_bdev_parse_config();
	if (ret < 0) {
		SPDK_ERRLOG("raid bdev init failed parsing\n");
		raid_bdev_free();
		return ret;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_init completed successfully\n");

	return 0;
}

/*
 * brief:
 * raid_bdev_create allocates raid bdev based on passed configuration
 * params:
 * raid_cfg - configuration of raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_create(struct raid_bdev_config *raid_cfg)
{
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *raid_bdev_gen;

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	if (!raid_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for raid bdev\n");
		return -ENOMEM;
	}

	assert(raid_cfg->num_base_bdevs != 0);
	raid_bdev->num_base_bdevs = raid_cfg->num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	if (!raid_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable able to allocate base bdev info\n");
		free(raid_bdev);
		return -ENOMEM;
	}

	raid_bdev->strip_size = raid_cfg->strip_size;
	raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	raid_bdev->config = raid_cfg;

	raid_bdev_gen = &raid_bdev->bdev;

	raid_bdev_gen->name = strdup(raid_cfg->name);
	if (!raid_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for raid\n");
		free(raid_bdev->base_bdev_info);
		free(raid_bdev);
		return -ENOMEM;
	}

	raid_bdev_gen->product_name = "Pooled Device";
	raid_bdev_gen->ctxt = raid_bdev;
	raid_bdev_gen->fn_table = &g_raid_bdev_fn_table;
	raid_bdev_gen->module = &g_raid_if;

	TAILQ_INSERT_TAIL(&g_spdk_raid_bdev_configuring_list, raid_bdev, state_link);
	TAILQ_INSERT_TAIL(&g_spdk_raid_bdev_list, raid_bdev, global_link);

	raid_cfg->raid_bdev = raid_bdev;

	return 0;
}

/*
 * brief
 * raid_bdev_alloc_base_bdev_resource allocates resource of base bdev.
 * params:
 * raid_bdev - pointer to raid bdev
 * bdev - pointer to base bdev
 * base_bdev_slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_alloc_base_bdev_resource(struct raid_bdev *raid_bdev, struct spdk_bdev *bdev,
				   uint32_t base_bdev_slot)
{
	struct spdk_bdev_desc *desc;
	int rc;

	rc = spdk_bdev_open(bdev, true, raid_bdev_remove_base_bdev, bdev, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", bdev->name);
		return rc;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_raid_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "bdev %s is claimed\n", bdev->name);

	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);
	assert(base_bdev_slot < raid_bdev->num_base_bdevs);

	raid_bdev->base_bdev_info[base_bdev_slot].bdev = bdev;
	raid_bdev->base_bdev_info[base_bdev_slot].desc = desc;
	raid_bdev->num_base_bdevs_discovered++;
	assert(raid_bdev->num_base_bdevs_discovered <= raid_bdev->num_base_bdevs);

	return 0;
}

/*
 * brief:
 * If raid bdev config is complete, then only register the raid bdev to
 * bdev layer and remove this raid bdev from configuring list and
 * insert the raid bdev to configured list
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_configure(struct raid_bdev *raid_bdev)
{
	uint32_t		blocklen;
	uint64_t		min_blockcnt;
	struct spdk_bdev	*raid_bdev_gen;
	int rc = 0;

	blocklen = raid_bdev->base_bdev_info[0].bdev->blocklen;
	min_blockcnt = raid_bdev->base_bdev_info[0].bdev->blockcnt;
	for (uint32_t i = 1; i < raid_bdev->num_base_bdevs; i++) {
		/* Calculate minimum block count from all base bdevs */
		if (raid_bdev->base_bdev_info[i].bdev->blockcnt < min_blockcnt) {
			min_blockcnt = raid_bdev->base_bdev_info[i].bdev->blockcnt;
		}

		/* Check blocklen for all base bdevs that it should be same */
		if (blocklen != raid_bdev->base_bdev_info[i].bdev->blocklen) {
			/*
			 * Assumption is that all the base bdevs for any raid bdev should
			 * have same blocklen
			 */
			SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
			return -EINVAL;
		}
	}

	raid_bdev_gen = &raid_bdev->bdev;
	raid_bdev_gen->write_cache = 0;
	raid_bdev_gen->blocklen = blocklen;
	raid_bdev_gen->ctxt = raid_bdev;
	raid_bdev_gen->fn_table = &g_raid_bdev_fn_table;
	raid_bdev_gen->module = &g_raid_if;
	raid_bdev->strip_size = (raid_bdev->strip_size * 1024) / blocklen;
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);
	raid_bdev->blocklen_shift = spdk_u32log2(blocklen);
	if (raid_bdev->num_base_bdevs > 1) {
		raid_bdev_gen->optimal_io_boundary = raid_bdev->strip_size;
		raid_bdev_gen->split_on_optimal_io_boundary = true;
	} else {
		/* Do not need to split reads/writes on single bdev RAID modules. */
		raid_bdev_gen->optimal_io_boundary = 0;
		raid_bdev_gen->split_on_optimal_io_boundary = false;
	}

	/*
	 * RAID bdev logic is for striping so take the minimum block count based
	 * approach where total block count of raid bdev is the number of base
	 * bdev times the minimum block count of any base bdev
	 */
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "min blockcount %lu,  numbasedev %u, strip size shift %u\n",
		      min_blockcnt,
		      raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);
	raid_bdev_gen->blockcnt = ((min_blockcnt >> raid_bdev->strip_size_shift) <<
				   raid_bdev->strip_size_shift)  * raid_bdev->num_base_bdevs;
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "io device register %p\n", raid_bdev);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "blockcnt %lu, blocklen %u\n", raid_bdev_gen->blockcnt,
		      raid_bdev_gen->blocklen);
	if (raid_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
		raid_bdev->state = RAID_BDEV_STATE_ONLINE;
		spdk_io_device_register(raid_bdev, raid_bdev_create_cb, raid_bdev_destroy_cb,
					sizeof(struct raid_bdev_io_channel),
					raid_bdev->bdev.name);
		rc = spdk_bdev_register(raid_bdev_gen);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to register pooled bdev and stay at configuring state\n");
			spdk_io_device_unregister(raid_bdev, NULL);
			raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
			return rc;
		}
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid bdev generic %p\n", raid_bdev_gen);
		TAILQ_REMOVE(&g_spdk_raid_bdev_configuring_list, raid_bdev, state_link);
		TAILQ_INSERT_TAIL(&g_spdk_raid_bdev_configured_list, raid_bdev, state_link);
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid bdev is created with name %s, raid_bdev %p\n",
			      raid_bdev_gen->name, raid_bdev);
	}

	return 0;
}

/*
 * brief:
 * If raid bdev is online and registered, change the bdev state to
 * configuring and unregister this raid device. Queue this raid device
 * in configuring list
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * none
 */
static void
raid_bdev_deconfigure(struct raid_bdev *raid_bdev)
{
	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		return;
	}

	assert(raid_bdev->num_base_bdevs == raid_bdev->num_base_bdevs_discovered);
	TAILQ_REMOVE(&g_spdk_raid_bdev_configured_list, raid_bdev, state_link);
	raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	assert(raid_bdev->num_base_bdevs_discovered);
	TAILQ_INSERT_TAIL(&g_spdk_raid_bdev_offline_list, raid_bdev, state_link);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid bdev state chaning from online to offline\n");

	spdk_io_device_unregister(raid_bdev, NULL);
	spdk_bdev_unregister(&raid_bdev->bdev, NULL, NULL);
}

/*
 * brief:
 * raid_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any raid bdev
 * or not. If yes, it takes necessary action on that particular raid bdev.
 * params:
 * ctx - pointer to base bdev pointer which got removed
 * returns:
 * none
 */
void
raid_bdev_remove_base_bdev(void *ctx)
{
	struct    spdk_bdev       *base_bdev = ctx;
	struct    raid_bdev       *raid_bdev;
	uint16_t                  i;
	bool                      found = false;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "raid_bdev_remove_base_bdev\n");

	/* Find the raid_bdev which has claimed this base_bdev */
	TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_list, global_link) {
		for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
			if (raid_bdev->base_bdev_info[i].bdev == base_bdev) {
				found = true;
				break;
			}
		}
		if (found == true) {
			break;
		}
	}

	if (found == false) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return;
	}

	assert(raid_bdev != NULL);
	assert(raid_bdev->base_bdev_info[i].bdev);
	assert(raid_bdev->base_bdev_info[i].desc);
	raid_bdev->base_bdev_info[i].remove_scheduled = true;

	if ((raid_bdev->destruct_called == true ||
	     raid_bdev->state == RAID_BDEV_STATE_CONFIGURING) &&
	    raid_bdev->base_bdev_info[i].bdev != NULL) {
		/*
		 * As raid bdev is not registered yet or already unregistered, so cleanup
		 * should be done here itself
		 */
		raid_bdev_free_base_bdev_resource(raid_bdev, i);
		if (raid_bdev->num_base_bdevs_discovered == 0) {
			/* Since there is no base bdev for this raid, so free the raid device */
			raid_bdev_cleanup(raid_bdev);
			return;
		}
	}

	raid_bdev_deconfigure(raid_bdev);
}

/*
 * brief:
 * raid_bdev_add_base_device function is the actual function which either adds
 * the nvme base device to existing raid bdev or create a new raid bdev. It also claims
 * the base device and keep the open descriptor.
 * params:
 * raid_cfg - pointer to raid bdev config
 * bdev - pointer to base bdev
 * base_bdev_slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_add_base_device(struct raid_bdev_config *raid_cfg, struct spdk_bdev *bdev,
			  uint32_t base_bdev_slot)
{
	struct raid_bdev	*raid_bdev;
	int			rc;

	raid_bdev = raid_cfg->raid_bdev;
	if (!raid_bdev) {
		SPDK_ERRLOG("Raid bdev is not created yet '%s'\n", bdev->name);
		return -ENODEV;
	}

	rc = raid_bdev_alloc_base_bdev_resource(raid_bdev, bdev, base_bdev_slot);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to allocate resource for bdev '%s'\n", bdev->name);
		return rc;
	}

	assert(raid_bdev->num_base_bdevs_discovered <= raid_bdev->num_base_bdevs);

	if (raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs) {
		rc = raid_bdev_configure(raid_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure raid bdev\n");
			return rc;
		}
	}

	return 0;
}

/*
 * brief:
 * Add base bdevs to the raid bdev one by one.  Skip any base bdev which doesn't
 *  exist or fails to add. If all base bdevs are successfully added, the raid bdev
 *  moves to the configured state and becomes available. Otherwise, the raid bdev
 *  stays at the configuring state with added base bdevs.
 * params:
 * raid_cfg - pointer to raid bdev config
 * returns:
 * 0 - The raid bdev moves to the configured state or stays at the configuring
 *     state with added base bdevs due to any nonexistent base bdev.
 * non zero - Failed to add any base bdev and stays at the configuring state with
 *            added base bdevs.
 */
int
raid_bdev_add_base_devices(struct raid_bdev_config *raid_cfg)
{
	struct spdk_bdev	*base_bdev;
	uint8_t			i;
	int			rc = 0, _rc;

	for (i = 0; i < raid_cfg->num_base_bdevs; i++) {
		base_bdev = spdk_bdev_get_by_name(raid_cfg->base_bdev[i].name);
		if (base_bdev == NULL) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "base bdev %s doesn't exist now\n",
				      raid_cfg->base_bdev[i].name);
			continue;
		}

		_rc = raid_bdev_add_base_device(raid_cfg, base_bdev, i);
		if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to RAID bdev %s: %s\n",
				    raid_cfg->base_bdev[i].name, raid_cfg->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}

	return rc;
}

/*
 * brief:
 * raid_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this raid bdev or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
raid_bdev_examine(struct spdk_bdev *bdev)
{
	struct raid_bdev_config	*raid_cfg;
	uint32_t		base_bdev_slot;

	if (raid_bdev_can_claim_bdev(bdev->name, &raid_cfg, &base_bdev_slot)) {
		raid_bdev_add_base_device(raid_cfg, bdev, base_bdev_slot);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID, "bdev %s can't be claimed\n",
			      bdev->name);
	}

	spdk_bdev_module_examine_done(&g_raid_if);
}

/* Log component for bdev raid bdev module */
SPDK_LOG_REGISTER_COMPONENT("bdev_raid", SPDK_LOG_BDEV_RAID)
