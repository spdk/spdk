/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2019, Peng Yu <yupeng0921@gmail.com>.
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

#include "bdev_linear.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/conf.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"

#define LINEAR_IO_BOUNDARY_BLOCKCNT (1024)
#define LINEAR_IO_BOUNDARY_SHIFT (10)

static bool g_shutdown_started = false;

/* linear bdev confg as read from config file */
struct linear_config g_linear_config = {
	.linear_bdev_config_head = TAILQ_HEAD_INITIALIZER(g_linear_config.linear_bdev_config_head),
};

/*
 * List of linear bdev in configured list, these linear bdevs are registered with
 * bdev layer
 */
struct linear_configured_tailq g_linear_bdev_configured_list = TAILQ_HEAD_INITIALIZER(
			g_linear_bdev_configured_list);

/* List of linear bdev in configuring list */
struct linear_configuring_tailq g_linear_bdev_configuring_list = TAILQ_HEAD_INITIALIZER(
			g_linear_bdev_configuring_list);

/* List of all linear bdevs */
struct linear_all_tailq g_linear_bdev_list = TAILQ_HEAD_INITIALIZER(g_linear_bdev_list);

/* List of all linear bdevs that are offline */
struct linear_offline_tailq g_linear_bdev_offline_list = TAILQ_HEAD_INITIALIZER(
			g_linear_bdev_offline_list);

/* Function declarations */
static int linear_bdev_init(void);
static void linear_waitq_io_process(void *ctx);
static void linear_bdev_deconfigure(struct linear_bdev *linear_bdev,
				    linear_bdev_destruct_cb cb_fn, void *cb_arg);
static void linear_bdev_remove_base_bdev(void *ctx);

/*
 * brief:
 * linear_bdev_create_cb function is a cb function for linear bdev which creates the
 * hierarchy from linear bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to linear bdev io device represented by linear_bdev
 * ctx_buf - pointer to context buffer for linear bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
linear_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct linear_bdev *linear_bdev = io_device;
	struct linear_bdev_io_channel *linear_ch = ctx_buf;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_create_cb, %p\n", linear_ch);

	assert(linear_bdev != NULL);
	assert(linear_bdev->state == LINEAR_BDEV_STATE_ONLINE);

	linear_ch->num_channels = linear_bdev->num_base_bdevs;

	linear_ch->base_channel = calloc(linear_ch->num_channels,
					 sizeof(struct spdk_io_channel *));
	if (!linear_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}
	for (uint8_t i = 0; i < linear_ch->num_channels; i++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 */
		linear_ch->base_channel[i] = spdk_bdev_get_io_channel(
						     linear_bdev->base_bdev_info[i].desc);
		if (!linear_ch->base_channel[i]) {
			for (uint8_t j = 0; j < i; j++) {
				spdk_put_io_channel(linear_ch->base_channel[j]);
			}
			free(linear_ch->base_channel);
			linear_ch->base_channel = NULL;
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			return -ENOMEM;
		}
	}

	return 0;
}

/*
 * brief:
 * linear_bdev_destroy_cb function is a cb function for linear bdev which deletes the
 * hierarchy from linear bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to linear bdev io device represented by linear_bdev
 * ctx_buf - pointer to context buffer for linear bdev io channel
 * returns:
 * none
 */
static void
linear_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct linear_bdev_io_channel *linear_ch = ctx_buf;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_destroy_cb\n");

	assert(linear_ch != NULL);
	assert(linear_ch->base_channel);
	for (uint8_t i = 0; i < linear_ch->num_channels; i++) {
		/* Free base bdev channels */
		assert(linear_ch->base_channel[i] != NULL);
		spdk_put_io_channel(linear_ch->base_channel[i]);
	}
	free(linear_ch->base_channel);
	linear_ch->base_channel = NULL;
}

/*
 * brief:
 * linear_bdev_cleanup is used to cleanup and free linear_bdev related data
 * structures.
 * params:
 * linear_bdev - pointer to linear_bdev
 * returns:
 * none
 */
static void
linear_bdev_cleanup(struct linear_bdev *linear_bdev)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_cleanup, %pname %s, state %u config %p\n",
		      linear_bdev,
		      linear_bdev->bdev.name, linear_bdev->state, linear_bdev->config);
	if (linear_bdev->state == LINEAR_BDEV_STATE_CONFIGURING) {
		TAILQ_REMOVE(&g_linear_bdev_configuring_list, linear_bdev, state_link);
	} else if (linear_bdev->state == LINEAR_BDEV_STATE_OFFLINE) {
		TAILQ_REMOVE(&g_linear_bdev_offline_list, linear_bdev, state_link);
	} else {
		assert(0);
	}
	TAILQ_REMOVE(&g_linear_bdev_list, linear_bdev, global_link);
	free(linear_bdev->bdev.name);
	free(linear_bdev->offsets);
	free(linear_bdev->base_bdev_info);
	if (linear_bdev->config) {
		linear_bdev->config->linear_bdev = NULL;
	}
	free(linear_bdev);
}

/*
 * brief:
 * free resource of base bdev for linear bdev
 * params:
 * linear_bdev - pointer to linear bdev
 * base_bdev_slot - position to base bdev in linear bdev
 * returns:
 * 0 - success
 * non zero -failure
 */
static void
linear_bdev_free_base_bdev_resource(struct linear_bdev *linear_bdev, uint8_t base_bdev_slot)
{
	struct linear_base_bdev_info *info;

	info = &linear_bdev->base_bdev_info[base_bdev_slot];

	spdk_bdev_module_release_bdev(info->bdev);
	spdk_bdev_close(info->desc);
	info->desc = NULL;
	info->bdev = NULL;

	assert(linear_bdev->num_base_bdevs_discovered);
	linear_bdev->num_base_bdevs_discovered--;
}

/*
 * brief:
 * linear_bdev_destruct is the destruct function table pointer for linear bdev
 * params:
 * ctxt - pointer to linear_bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
linear_bdev_destruct(void *ctxt)
{
	struct linear_bdev *linear_bdev = ctxt;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_destruct\n");

	linear_bdev->destruct_called = true;
	for (uint8_t i = 0; i < linear_bdev->num_base_bdevs; i++) {
		/*
		 * Close all base bdev descriptors for which call has come form below
		 * layers. Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started ||
		    ((linear_bdev->base_bdev_info[i].remove_scheduled == true) &&
		     (linear_bdev->base_bdev_info[i].bdev != NULL))) {
			linear_bdev_free_base_bdev_resource(linear_bdev, i);
		}
	}

	if (g_shutdown_started) {
		TAILQ_REMOVE(&g_linear_bdev_configured_list, linear_bdev, state_link);
		linear_bdev->state = LINEAR_BDEV_STATE_OFFLINE;
		TAILQ_INSERT_TAIL(&g_linear_bdev_offline_list, linear_bdev, state_link);
	}

	spdk_io_device_unregister(linear_bdev, NULL);

	if (linear_bdev->num_base_bdevs_discovered == 0) {
		/* Free linear_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear bdev base bdevs is 0, going to free all in destruct\n");
		linear_bdev_cleanup(linear_bdev);
	}

	return 0;
}

/*
 * brief:
 * linear_bdev_io_completion function is called by lower layers to notify linear
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdve io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context, like parent io pointer
 * returns:
 * none
 */
static void
linear_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		spdk_bdev_io_complete(parent_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(parent_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * brief:
 * linear_io_mapping function is called to find the base dev index and
 * map the linear offset to base bdev offset
 * use binary search instead of the O(n) in the further
 * params:
 * linear_bdev - pointer to linear_bdev
 * ori_offset - the io offset of the linear bdev
 * lba - the address to store the base bdev offset
 * idx - the address to store the base bdev index
 * returns:
 * none
 */
static inline void
linear_io_mapping(struct linear_bdev *linear_bdev, uint64_t ori_offset,
		  uint64_t *lba, uint8_t *idx)
{
	uint64_t offset;
	uint8_t target_idx = linear_bdev->num_base_bdevs - 1;
	for (uint8_t i = 1; i < linear_bdev->num_base_bdevs; i++) {
		offset = linear_bdev->offsets[i];
		if (offset > ori_offset) {
			target_idx = i - 1;
			break;
		}
	}
	*lba = ori_offset - linear_bdev->offsets[target_idx];
	*idx = target_idx;
}


/*
 * brief:
 * linear_submit_rw_request function is used to submit I/O to the correct
 * member disk for linear bdevs.
 * params:
 * bdev_io - parent bdev io
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
linear_submit_rw_request(struct spdk_bdev_io *bdev_io)
{
	struct linear_bdev_io *linear_io = (struct linear_bdev_io *)bdev_io->driver_ctx;
	struct linear_bdev_io_channel *linear_ch = spdk_io_channel_get_ctx(linear_io->ch);
	struct linear_bdev *linear_bdev = (struct linear_bdev *)bdev_io->bdev->ctxt;
	uint64_t pd_lba;
	uint64_t pd_blocks;
	uint8_t pd_idx;
	int ret = 0;

	linear_io_mapping(linear_bdev, bdev_io->u.bdev.offset_blocks, &pd_lba, &pd_idx);
	pd_blocks = bdev_io->u.bdev.num_blocks;
	if (linear_bdev->base_bdev_info[pd_idx].desc == NULL) {
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and funciton callback context
	 */
	assert(linear_ch != NULL);
	assert(linear_ch->base_channel);
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_readv_blocks(linear_bdev->base_bdev_info[pd_idx].desc,
					     linear_ch->base_channel[pd_idx],
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     pd_lba, pd_blocks, linear_bdev_io_completion,
					     bdev_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = spdk_bdev_writev_blocks(linear_bdev->base_bdev_info[pd_idx].desc,
					      linear_ch->base_channel[pd_idx],
					      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					      pd_lba, pd_blocks, linear_bdev_io_completion,
					      bdev_io);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
		assert(0);
	}

	return ret;
}

/*
 * brief:
 * linear_get_curr_base_bdev_index function calculates the base bdev index
 * for linear bdevs.
 * params:
 * linear_bdev - pointer to linear bdev
 * linear_io - pointer to parent io context
 * returns:
 * base bdev index
 */
static uint8_t
linear_get_curr_base_bdev_index(struct linear_bdev *linear_bdev, struct linear_bdev_io *linear_io)
{
	struct spdk_bdev_io *bdev_io;
	uint64_t pd_lba;
	uint8_t pd_idx;

	bdev_io = SPDK_CONTAINEROF(linear_io, struct spdk_bdev_io, driver_ctx);
	linear_io_mapping(linear_bdev, bdev_io->u.bdev.offset_blocks, &pd_lba, &pd_idx);

	return pd_idx;
}

/*
 * brief:
 * linear_bdev_io_submit_fail_process function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * bdev_io - pointer to bdev_io
 * linear_io - pointer to linear_bdev_io
 * ret - return code
 * returns:
 * none
 */
static void
linear_bdev_io_submit_fail_process(struct linear_bdev *linear_bdev, struct spdk_bdev_io *bdev_io,
				   struct linear_bdev_io *linear_io, int ret)
{
	struct linear_bdev_io_channel *linear_ch;
	uint8_t pd_idx;
	if (ret != -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		/* Queue the IO to bdev layer wait queue */
		pd_idx = linear_get_curr_base_bdev_index(linear_bdev, linear_io);
		linear_io->waitq_entry.bdev = linear_bdev->base_bdev_info[pd_idx].bdev;
		linear_io->waitq_entry.cb_fn = linear_waitq_io_process;
		linear_io->waitq_entry.cb_arg = linear_io;
		linear_ch = spdk_io_channel_get_ctx(linear_io->ch);
		if (spdk_bdev_queue_io_wait(linear_bdev->base_bdev_info[pd_idx].bdev,
					    linear_ch->base_channel[pd_idx],
					    &linear_io->waitq_entry) != 0) {
			SPDK_ERRLOG("bdev io waitq error, it should not happen\n");
			assert(0);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/*
 * brief:
 * linear_waitq_io_process funciton is the callback function
 * registered by linear bdev module to bdev when bdev_io was unavailable
 * for linear bdevs.
 * params:
 * ctx - pointer to linear_bdev_io
 * returns:
 * none
 */
static void
linear_waitq_io_process(void *ctx)
{
	struct linear_bdev_io *linear_io = ctx;
	struct spdk_bdev_io *bdev_io;
	struct linear_bdev *linear_bdev;
	int ret;

	bdev_io = SPDK_CONTAINEROF(linear_io, struct spdk_bdev_io, driver_ctx);
	/*
	 * Try to submit childs of parent bdev io. If failed due to resource
	 * crunch then break the loop and don't try to rpocess other queued IOs.
	 */
	linear_bdev = (struct linear_bdev *)bdev_io->bdev->ctxt;
	ret = linear_submit_rw_request(bdev_io);
	if (ret != 0) {
		linear_bdev_io_submit_fail_process(linear_bdev, bdev_io, linear_io, ret);
	}
}

/*
 * brief:
 * linear-start_rw_request function is the submit_request function for
 * read/write requests for linear bdevs.
 * params:
 * ch - pointer to linear bdev io channel
 * bdev_io - pointer to parent bdev_io on linear bdev device
 * returns:
 * none
 */
static void
linear_start_rw_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct linear_bdev_io *linear_io;
	struct linear_bdev *linear_bdev;
	int ret;

	linear_bdev = (struct linear_bdev *)bdev_io->bdev->ctxt;
	linear_io = (struct linear_bdev_io *)bdev_io->driver_ctx;
	linear_io->ch = ch;
	ret = linear_submit_rw_request(bdev_io);
	if (ret != 0) {
		linear_bdev_io_submit_fail_process(linear_bdev, bdev_io, linear_io, ret);
	}
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to linear bdev io channel
 * bdev_io - pointer to parent bdev_io on linear bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
static void
linear_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	linear_start_rw_request(ch, bdev_io);
}

/*
 * brief:
 * linear_bdev_submit_request_function is the submit_request function pointer of
 * linear bdev function table. This is used to submit the io on linear_bev to below
 * layers.
 * params:
 * ch - pointer to linear bdev io channel
 * bdev_io - pointer to parent bdev_io on linear bdev device
 * returns
 * none
 */
static void
linear_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, linear_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		linear_start_rw_request(ch, bdev_io);
		break;

	default:
		SPDK_ERRLOG("submit requests, invalid io type %u\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}

}

/*
 * brief:
 * linear_bdev_io_type_supported is the io_supported function for bdev function
 * tabel which returns whether the particular io type is supported or not by
 * linear bdev module
 * params:
 * ctx - pointer to linear bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
static bool
linear_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * linear_bdev_get_io_channel is the get_io_channel function table pointer for
 * linear bdev. This is used to return the io channel for this linear bev
 * params:
 * ctxt - poiner to linear_bdev
 * returns:
 * pointer to io channel for linear bdev
 */
static struct spdk_io_channel *
linear_bdev_get_io_channel(void *ctxt)
{
	struct linear_bdev *linear_bdev = ctxt;

	return spdk_get_io_channel(linear_bdev);
}

/* g_linear_bdev_fn_table is the function table for linear bdev */
static const struct spdk_bdev_fn_table g_linear_bdev_fn_table = {
	.destruct = linear_bdev_destruct,
	.submit_request = linear_bdev_submit_request,
	.io_type_supported = linear_bdev_io_type_supported,
	.get_io_channel = linear_bdev_get_io_channel,
	.dump_info_json = NULL,
	.write_config_json = NULL,
};

/*
 * brief:
 * linear_bdev_config_cleanup function i sused to free memory for one linear_bdev in configuring
 * params:
 * linear_cfg - pointer to linear_bdev_config structure
 * returns:
 * none
 */
void
linear_bdev_config_cleanup(struct linear_bdev_config *linear_cfg)
{
	uint8_t i;

	TAILQ_REMOVE(&g_linear_config.linear_bdev_config_head, linear_cfg, link);
	g_linear_config.total_linear_bdev--;

	if (linear_cfg->base_bdev) {
		for (i = 0; i < linear_cfg->num_base_bdevs; i++) {
			free(linear_cfg->base_bdev[i].name);
		}
		free(linear_cfg->base_bdev);
	}
	free(linear_cfg->name);
	free(linear_cfg);
}

/*
 * brief:
 * linear_bdev_free is the linear bdev function table function pointer. This is
 * called on bdev free path
 * params:
 * none
 * returns:
 * none
 */
static void
linear_bdev_free(void)
{
	struct linear_bdev_config *linear_cfg, *tmp;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_free\n");
	TAILQ_FOREACH_SAFE(linear_cfg, &g_linear_config.linear_bdev_config_head, link, tmp) {
		linear_bdev_config_cleanup(linear_cfg);
	}
}

/* brief
 * linear_bdev_cofig_find_by_name is a helper function to find linear bdev config
 * by name as key.
 *
 * params:
 * linear_name - name for linear bdev.
 */
struct linear_bdev_config *
linear_bdev_config_find_by_name(const char *linear_name)
{
	struct linear_bdev_config *linear_cfg;

	TAILQ_FOREACH(linear_cfg, &g_linear_config.linear_bdev_config_head, link) {
		if (!strcmp(linear_cfg->name, linear_name)) {
			return linear_cfg;
		}
	}

	return linear_cfg;
}

/*
 * brief
 * linear_bdev_config_add function adds config for newly created linear bdev.
 *
 * params:
 * linear_name - name for linear bdev.
 * num_base_bdevs - numer of base bdevs.
 * _linear_cfg - Pointer to newly added configuration
 */
int
linear_bdev_config_add(const char *linear_name, uint8_t num_base_bdevs,
		       struct linear_bdev_config **_linear_cfg)
{
	struct linear_bdev_config *linear_cfg;

	linear_cfg = linear_bdev_config_find_by_name(linear_name);
	if (linear_cfg != NULL) {
		SPDK_ERRLOG("Duplicate linear bdev name found in config file %s\n",
			    linear_name);
		return -EEXIST;
	}

	if (num_base_bdevs == 0) {
		SPDK_ERRLOG("Invalid base device count %u\n", num_base_bdevs);
		return -EINVAL;
	}

	linear_cfg = calloc(1, sizeof(*linear_cfg));
	if (linear_cfg == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	linear_cfg->name = strdup(linear_name);
	if (!linear_cfg->name) {
		free(linear_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}
	linear_cfg->num_base_bdevs = num_base_bdevs;

	linear_cfg->base_bdev = calloc(num_base_bdevs, sizeof(*linear_cfg->base_bdev));
	if (linear_cfg->base_bdev == NULL) {
		free(linear_cfg->name);
		free(linear_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_linear_config.linear_bdev_config_head, linear_cfg, link);
	g_linear_config.total_linear_bdev++;

	*_linear_cfg = linear_cfg;
	return 0;
}

/*
 * brief:
 * linear_bdev_config_add_base_bdev function add base bdev to linear bdev config.
 *
 * params:
 * linear_cfg - pointer to linear bdev configuration
 * base_bdev_name - name of base bdev
 * slot - Position to add base bdev
 */
int
linear_bdev_config_add_base_bdev(struct linear_bdev_config *linear_cfg, const char *base_bdev_name,
				 uint8_t slot)
{
	uint8_t i;
	struct linear_bdev_config *tmp;

	if (slot >= linear_cfg->num_base_bdevs) {
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &g_linear_config.linear_bdev_config_head, link) {
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

	linear_cfg->base_bdev[slot].name = strdup(base_bdev_name);
	if (linear_cfg->base_bdev[slot].name == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}

/*
 * brief:
 * linear_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
static void
linear_bdev_fini_start(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_fini_start\n");
	g_shutdown_started = true;
}

/*
 * brief:
 * linear_bdev_exit is called on linear bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
linear_bdev_exit(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_exit\n");
	linear_bdev_free();
}

/*
 * brief:
 * linear_bdev_get_ctx_size is used to return the context size of bdev_io for linear
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for linear
 */
static int
linear_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_get_ctx_size\n");
	return sizeof(struct linear_bdev_io);
}

static struct spdk_bdev_module g_linear_if = {
	.name = "linear",
	.module_init = linear_bdev_init,
	.fini_start = linear_bdev_fini_start,
	.module_fini = linear_bdev_exit,
	.get_ctx_size = linear_bdev_get_ctx_size,
	.examine_config = NULL,
	.config_text = NULL,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(linear, &g_linear_if)

/*
 * brief:
 * linear_bdev_init is the initialization function for linear bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
linear_bdev_init(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_init completed successful\n");

	return 0;
}

/*
 * brief:
 * linear_bdev_create allocates linear bdev based on passed configuration
 * parmas:
 * linear_cfg - configuration of linear bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
linear_bdev_create(struct linear_bdev_config *linear_cfg)
{
	struct linear_bdev *linear_bdev;
	struct spdk_bdev *linear_bdev_gen;

	linear_bdev = calloc(1, sizeof(*linear_bdev));
	if (!linear_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for linear bdev\n");
		return -ENOMEM;
	}

	assert(linear_cfg->num_base_bdevs != 0);
	linear_bdev->num_base_bdevs = linear_cfg->num_base_bdevs;
	linear_bdev->base_bdev_info = calloc(linear_bdev->num_base_bdevs,
					     sizeof(struct linear_base_bdev_info));
	if (!linear_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable to allocate base bdev info\n");
		free(linear_bdev);
		return -ENOMEM;
	}

	linear_bdev->offsets = malloc(
				       linear_bdev->num_base_bdevs * sizeof(linear_bdev->offsets));
	if (!linear_bdev->offsets) {
		SPDK_ERRLOG("Unable to allocate offsets\n");
		free(linear_bdev->base_bdev_info);
		free(linear_bdev);
	}

	linear_bdev->state = LINEAR_BDEV_STATE_CONFIGURING;
	linear_bdev->config = linear_cfg;

	linear_bdev_gen = &linear_bdev->bdev;

	linear_bdev_gen->name = strdup(linear_cfg->name);
	if (!linear_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for linear\n");
		free(linear_bdev->offsets);
		free(linear_bdev->base_bdev_info);
		free(linear_bdev);
		return -ENOMEM;
	}

	linear_bdev_gen->product_name = "Linear Volume";
	linear_bdev_gen->ctxt = linear_bdev_gen;
	linear_bdev_gen->fn_table = &g_linear_bdev_fn_table;
	linear_bdev_gen->module = &g_linear_if;
	linear_bdev_gen->write_cache = 0;

	TAILQ_INSERT_TAIL(&g_linear_bdev_configuring_list, linear_bdev, state_link);
	TAILQ_INSERT_TAIL(&g_linear_bdev_list, linear_bdev, global_link);

	linear_cfg->linear_bdev = linear_bdev;

	return 0;
}

/*
 * brief
 * linear_bdev_alloc_base_bdev_resource allocates resource of base bdev.
 * params:
 * linear_bdev - pointer to linear bdev
 * bdev - pointer to base bdev
 * base_bdev_slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
linear_bdev_alloc_base_bdev_resource(struct linear_bdev *linear_bdev, struct spdk_bdev *bdev,
				     uint8_t base_bdev_slot)
{
	struct spdk_bdev_desc *desc;
	int rc;

	rc = spdk_bdev_open(bdev, true, linear_bdev_remove_base_bdev, bdev, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to access desc on bdev '%s'\n", bdev->name);
		return rc;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_linear_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "bdev %s is claimed\n", bdev->name);

	assert(linear_bdev->state != LINEAR_BDEV_STATE_ONLINE);
	assert(base_bdev_slot < linear_bdev->num_base_bdevs);

	linear_bdev->base_bdev_info[base_bdev_slot].bdev = bdev;
	linear_bdev->base_bdev_info[base_bdev_slot].desc = desc;
	linear_bdev->num_base_bdevs_discovered++;
	assert(linear_bdev->num_base_bdevs_discovered <= linear_bdev->num_base_bdevs);

	return 0;
}

/*
 * brief:
 * If linear bdev config is complete, then only register the linear bdev to
 * bdev layer and remvoe this linear bdev from configuring list and
 * insert the linear bdev to configured list
 * params:
 * linear_bdev - pointer to linear_bdev
 * returs:
 * 0 - success
 * non zero - failure
 */
static int
linear_bdev_configure(struct linear_bdev *linear_bdev)
{
	uint32_t blocklen;
	uint64_t total_blockcnt, blockcnt;
	struct spdk_bdev *linear_bdev_gen;
	int rc = 0;

	blocklen = linear_bdev->base_bdev_info[0].bdev->blocklen;
	linear_bdev->offsets[0] = 0;
	blockcnt = linear_bdev->base_bdev_info[0].bdev->blockcnt >> LINEAR_IO_BOUNDARY_SHIFT;
	blockcnt = blockcnt << LINEAR_IO_BOUNDARY_SHIFT;
	if (blockcnt == 0) {
		SPDK_ERRLOG("Blockcnt is smaller than %u\n", LINEAR_IO_BOUNDARY_BLOCKCNT);
		return -EINVAL;
	}
	total_blockcnt = blockcnt;
	for (uint8_t i = 1; i < linear_bdev->num_base_bdevs; i++) {
		/* Check blocklen for all base bdevs that it should be same */
		if (blocklen != linear_bdev->base_bdev_info[i].bdev->blocklen) {
			/*
			 * Assumption is that all the base bdevs for any linear bdev should
			 * have same blocklen
			 */
			SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
			return -EINVAL;
		}

		blockcnt = linear_bdev->base_bdev_info[i].bdev->blockcnt >> LINEAR_IO_BOUNDARY_SHIFT;
		blockcnt = blockcnt << LINEAR_IO_BOUNDARY_SHIFT;
		if (blockcnt == 0) {
			SPDK_ERRLOG("Blockcnt is smaller than %u\n", LINEAR_IO_BOUNDARY_BLOCKCNT);
			return -EINVAL;
		}
		linear_bdev->offsets[i] = total_blockcnt;
		total_blockcnt += blockcnt;
	}


	linear_bdev_gen = &linear_bdev->bdev;
	linear_bdev_gen->blocklen = blocklen;
	if (linear_bdev->num_base_bdevs > 1) {
		linear_bdev_gen->optimal_io_boundary = LINEAR_IO_BOUNDARY_BLOCKCNT;
		linear_bdev_gen->split_on_optimal_io_boundary = true;
	} else {
		/* Do not need to split reads/writes on single bdev LINEAR modules. */
		linear_bdev_gen->optimal_io_boundary = 0;
		linear_bdev_gen->split_on_optimal_io_boundary = false;
	}

	linear_bdev_gen->blockcnt = total_blockcnt;
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "io device register %p\n", linear_bdev);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "blockcnt %lu, blocklen %u\n", linear_bdev_gen->blockcnt,
		      linear_bdev_gen->blocklen);
	if (linear_bdev->state == LINEAR_BDEV_STATE_CONFIGURING) {
		linear_bdev->state = LINEAR_BDEV_STATE_ONLINE;
		spdk_io_device_register(linear_bdev, linear_bdev_create_cb, linear_bdev_destroy_cb,
					sizeof(struct linear_bdev_io_channel),
					linear_bdev->bdev.name);
		rc = spdk_bdev_register(linear_bdev_gen);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to register linear bdev and stay at configuring state\n");
			spdk_io_device_unregister(linear_bdev, NULL);
			linear_bdev->state = LINEAR_BDEV_STATE_CONFIGURING;
			return rc;
		}
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear bdev %p\n", linear_bdev_gen);
		TAILQ_REMOVE(&g_linear_bdev_configuring_list, linear_bdev, state_link);
		TAILQ_INSERT_TAIL(&g_linear_bdev_configured_list, linear_bdev, state_link);
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear bdev is created with name %s, linear_bdev %p\n",
			      linear_bdev_gen->name, linear_bdev);
	}

	return 0;
}

/*
 * brief:
 * If linear bdev is online and registered, change the bdev state to
 * configuring and unregister this linear device. Queue this linear device
 * in configuring list
 * params:
 * linear_bdev - pointer to linear bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
linear_bdev_deconfigure(struct linear_bdev *linear_bdev, linear_bdev_destruct_cb cb_fn,
			void *cb_arg)
{
	if (linear_bdev->state != LINEAR_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	assert(linear_bdev->num_base_bdevs == linear_bdev->num_base_bdevs_discovered);
	TAILQ_REMOVE(&g_linear_bdev_configured_list, linear_bdev, state_link);
	linear_bdev->state = LINEAR_BDEV_STATE_OFFLINE;
	assert(linear_bdev->num_base_bdevs_discovered);
	TAILQ_INSERT_TAIL(&g_linear_bdev_offline_list, linear_bdev, state_link);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear bdev state changing from online to offline\n");

	spdk_bdev_unregister(&linear_bdev->bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * linear_bdev_find_by_base_bdev function finds the linear bdev which has
 * claimed the base bdev.
 * params:
 * base_bdev - poiner to base bdev pointer
 * _linear_bdev - Referenct to pointer to linear bdev
 * _base_bdev_slot - Reference to the slot of the base bdev.
 * returns:
 * true - if the linear bdev is found
 * false - if the linear bdev is not found
 */
static bool
linear_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev, struct linear_bdev **_linear_bdev,
			      uint8_t *_base_bdev_slot)
{
	struct linear_bdev *linear_bdev;
	uint8_t i;

	TAILQ_FOREACH(linear_bdev, &g_linear_bdev_list, global_link) {
		for (i = 0; i < linear_bdev->num_base_bdevs; i++) {
			if (linear_bdev->base_bdev_info[i].bdev == base_bdev) {
				*_linear_bdev = linear_bdev;
				*_base_bdev_slot = i;
				return true;
			}
		}
	}

	return false;
}

/*
 * brief:
 * linear_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function  checks if this base bdev is part of any linear bdev
 * or not. If yes, it takes necessary action on that particular linear bdev.
 * params:
 * ctx - pointer to base bdev pointer which got removed
 * returns:
 * none
 */
static void
linear_bdev_remove_base_bdev(void *ctx)
{
	struct spdk_bdev *base_bdev = ctx;
	struct linear_bdev *linear_bdev = NULL;
	uint8_t base_bdev_slot = 0;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_remove_base_bdev\n");

	/* Find the linear_bdev which has claimed this base_bdev */
	if (!linear_bdev_find_by_base_bdev(base_bdev, &linear_bdev, &base_bdev_slot)) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return;
	}

	assert(linear_bdev->base_bdev_info[base_bdev_slot].desc);
	linear_bdev->base_bdev_info[base_bdev_slot].remove_scheduled = true;

	if (linear_bdev->destruct_called == true ||
	    linear_bdev->state == LINEAR_BDEV_STATE_CONFIGURING) {
		/*
		 * As linear bdev is not registered yet or alreayd uregistered,
		 * so cleanup should be done here itself.
		 */
		linear_bdev_free_base_bdev_resource(linear_bdev, base_bdev_slot);
		if (linear_bdev->num_base_bdevs_discovered == 0) {
			/* There is no base bdev for this linear, so free the linear device. */
			linear_bdev_cleanup(linear_bdev);
			return;
		}
	}

	linear_bdev_deconfigure(linear_bdev, NULL, NULL);
}

/*
 * brief:
 * Remove base bdevs from the linear bdev one by one. Skip any base bdev which
 * doesn't exist.
 * params:
 * linear_cfg - pointer to linear bdev config.
 * cb_fn - callback function
 * cb_ctx - argument to callback function
 */
void
linear_bdev_remove_base_devices(struct linear_bdev_config *linear_cfg,
				linear_bdev_destruct_cb cb_fn, void *cb_arg)
{
	struct linear_bdev *linear_bdev;
	struct linear_base_bdev_info *info;
	uint8_t i;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear_bdev_remove_base_devices\n");

	linear_bdev = linear_cfg->linear_bdev;
	if (linear_bdev == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "linear bdev %s doesn't exist now\n", linear_cfg->name);
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	if (linear_bdev->destroy_started) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "destroying linear bdev %s is already started\n",
			      linear_cfg->name);
		if (cb_fn) {
			cb_fn(cb_arg, -EALREADY);
		}
		return;
	}

	linear_bdev->destroy_started = true;

	for (i = 0; i < linear_bdev->num_base_bdevs; i++) {
		info = &linear_bdev->base_bdev_info[i];

		if (info->bdev == NULL) {
			continue;
		}

		assert(info->desc);
		info->remove_scheduled = true;

		if (linear_bdev->destruct_called == true ||
		    linear_bdev->state == LINEAR_BDEV_STATE_CONFIGURING) {
			/*
			 * As linear bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 */
			linear_bdev_free_base_bdev_resource(linear_bdev, i);
			if (linear_bdev->num_base_bdevs_discovered == 0) {
				/* There is no base bdev for this linear, so free linear device. */
				linear_bdev_cleanup(linear_bdev);
				if (cb_fn) {
					cb_fn(cb_arg, 0);
				}
				return;
			}
		}
	}

	linear_bdev_deconfigure(linear_bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * linear_bdev_add_base_device function is the actual function which either adds
 * the nvme base device to existing linear bdev or create a new linear bdev. It also claims
 * the base device and keep the open descriptor.
 * params:
 * linear_cfg - pointer to linear bdev config
 * bdev - pointer to base bdev
 * base_bdev_slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
linear_bdev_add_base_device(struct linear_bdev_config *linear_cfg, struct spdk_bdev *bdev,
			    uint8_t base_bdev_slot)
{
	struct linear_bdev *linear_bdev;
	int rc;

	linear_bdev = linear_cfg->linear_bdev;
	if (!linear_bdev) {
		SPDK_ERRLOG("Linear bdev '%s' is not created yet\n", linear_cfg->name);
		return -ENODEV;
	}

	rc = linear_bdev_alloc_base_bdev_resource(linear_bdev, bdev, base_bdev_slot);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to allocate resource for bdev '%s'\n", bdev->name);
		return rc;
	}

	assert(linear_bdev->num_base_bdevs_discovered <= linear_bdev->num_base_bdevs);

	if (linear_bdev->num_base_bdevs_discovered == linear_bdev->num_base_bdevs) {
		rc = linear_bdev_configure(linear_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure linear bdev\n");
			return rc;
		}
	}

	return 0;
}

/*
 * brief:
 * Add base bdevs to the linear bdev one by one. Skip any base bdev which doens't
 * exist or fails to add. If all base bdevs are successfully added, the linear bdev
 * moves to the configured state and becomes available. Otherwise, the linear bdev
 * stays at the configuring state with added base bdevs.
 * params:
 * linear_cfg - pointer to linear bdev config
 * returns:
 * 0 - The linear bdev moves to the configured state or stays at the configuring
 *     state with added base bdevs due to any nonexistent base bdev.
 * non zero - Failed to add any base bdev and stays at the configuring state with
 *            added base bdevs.
 */
int
linear_bdev_add_base_devices(struct linear_bdev_config *linear_cfg)
{
	struct spdk_bdev *base_bdev;
	uint8_t i;
	int rc = 0, _rc;

	for (i = 0; i < linear_cfg->num_base_bdevs; i++) {
		base_bdev = spdk_bdev_get_by_name(linear_cfg->base_bdev[i].name);
		if (base_bdev == NULL) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_LINEAR, "base bdev %s doesn't exist now\n",
				      linear_cfg->base_bdev[i].name);
			continue;
		}

		_rc = linear_bdev_add_base_device(linear_cfg, base_bdev, i);
		if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to LINEAR bdev %s: %s\n",
				    linear_cfg->base_bdev[i].name, linear_cfg->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}

	return rc;
}

/* Log component for bdev linear bdev module */
SPDK_LOG_REGISTER_COMPONENT("bdev_linear", SPDK_LOG_BDEV_LINEAR)
