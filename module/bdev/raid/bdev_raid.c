/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_raid.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"

static bool g_shutdown_started = false;

/* List of all raid bdevs */
struct raid_all_tailq g_raid_bdev_list = TAILQ_HEAD_INITIALIZER(g_raid_bdev_list);

static TAILQ_HEAD(, raid_bdev_module) g_raid_modules = TAILQ_HEAD_INITIALIZER(g_raid_modules);

static struct raid_bdev_module *
raid_bdev_module_find(enum raid_level level)
{
	struct raid_bdev_module *raid_module;

	TAILQ_FOREACH(raid_module, &g_raid_modules, link) {
		if (raid_module->level == level) {
			return raid_module;
		}
	}

	return NULL;
}

void
raid_bdev_module_list_add(struct raid_bdev_module *raid_module)
{
	if (raid_bdev_module_find(raid_module->level) != NULL) {
		SPDK_ERRLOG("module for raid level '%s' already registered.\n",
			    raid_bdev_level_to_str(raid_module->level));
		assert(false);
	} else {
		TAILQ_INSERT_TAIL(&g_raid_modules, raid_module, link);
	}
}

/* Function declarations */
static void	raid_bdev_examine(struct spdk_bdev *bdev);
static int	raid_bdev_init(void);
static void	raid_bdev_deconfigure(struct raid_bdev *raid_bdev,
				      raid_bdev_destruct_cb cb_fn, void *cb_arg);

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
	uint8_t i;
	int ret = 0;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_create_cb, %p\n", raid_ch);

	assert(raid_bdev != NULL);
	assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);

	raid_ch->num_channels = raid_bdev->num_base_bdevs;

	raid_ch->base_channel = calloc(raid_ch->num_channels,
				       sizeof(struct spdk_io_channel *));
	if (!raid_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}
	for (i = 0; i < raid_ch->num_channels; i++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 */
		raid_ch->base_channel[i] = spdk_bdev_get_io_channel(
						   raid_bdev->base_bdev_info[i].desc);
		if (!raid_ch->base_channel[i]) {
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			ret = -ENOMEM;
			break;
		}
	}

	if (!ret && raid_bdev->module->get_io_channel) {
		raid_ch->module_channel = raid_bdev->module->get_io_channel(raid_bdev);
		if (!raid_ch->module_channel) {
			SPDK_ERRLOG("Unable to create io channel for raid module\n");
			ret = -ENOMEM;
		}
	}

	if (ret) {
		uint8_t j;

		for (j = 0; j < i; j++) {
			spdk_put_io_channel(raid_ch->base_channel[j]);
		}
		free(raid_ch->base_channel);
		raid_ch->base_channel = NULL;
	}
	return ret;
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
	uint8_t i;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destroy_cb\n");

	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);

	if (raid_ch->module_channel) {
		spdk_put_io_channel(raid_ch->module_channel);
	}

	for (i = 0; i < raid_ch->num_channels; i++) {
		/* Free base bdev channels */
		assert(raid_ch->base_channel[i] != NULL);
		spdk_put_io_channel(raid_ch->base_channel[i]);
	}
	free(raid_ch->base_channel);
	raid_ch->base_channel = NULL;
}

/*
 * brief:
 * raid_bdev_cleanup is used to cleanup raid_bdev related data
 * structures.
 * params:
 * raid_bdev - pointer to raid_bdev
 * returns:
 * none
 */
static void
raid_bdev_cleanup(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_cleanup, %p name %s, state %s\n",
		      raid_bdev, raid_bdev->bdev.name, raid_bdev_state_to_str(raid_bdev->state));
	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		assert(base_info->bdev == NULL);
		assert(base_info->desc == NULL);
		free(base_info->name);
	}

	TAILQ_REMOVE(&g_raid_bdev_list, raid_bdev, global_link);
	free(raid_bdev->base_bdev_info);
}

static void
raid_bdev_free(struct raid_bdev *raid_bdev)
{
	free(raid_bdev->bdev.name);
	free(raid_bdev);
}

static void
raid_bdev_cleanup_and_free(struct raid_bdev *raid_bdev)
{
	raid_bdev_cleanup(raid_bdev);
	raid_bdev_free(raid_bdev);
}

/*
 * brief:
 * free resource of base bdev for raid bdev
 * params:
 * raid_bdev - pointer to raid bdev
 * base_info - raid base bdev info
 * returns:
 * 0 - success
 * non zero - failure
 */
static void
raid_bdev_free_base_bdev_resource(struct raid_bdev *raid_bdev,
				  struct raid_base_bdev_info *base_info)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	free(base_info->name);
	base_info->name = NULL;

	if (base_info->bdev == NULL) {
		return;
	}

	assert(base_info->desc);
	spdk_bdev_module_release_bdev(base_info->bdev);
	spdk_bdev_close(base_info->desc);
	base_info->desc = NULL;
	base_info->bdev = NULL;

	assert(raid_bdev->num_base_bdevs_discovered);
	raid_bdev->num_base_bdevs_discovered--;
}

static void
raid_bdev_io_device_unregister_cb(void *io_device)
{
	struct raid_bdev *raid_bdev = io_device;

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* Free raid_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(bdev_raid, "raid bdev base bdevs is 0, going to free all in destruct\n");
		raid_bdev_cleanup(raid_bdev);
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);
		raid_bdev_free(raid_bdev);
	} else {
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);
	}
}

void
raid_bdev_module_stop_done(struct raid_bdev *raid_bdev)
{
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_io_device_unregister(raid_bdev, raid_bdev_io_device_unregister_cb);
	}
}

static void
_raid_bdev_destruct(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destruct\n");

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started || base_info->remove_scheduled == true) {
			raid_bdev_free_base_bdev_resource(raid_bdev, base_info);
		}
	}

	if (g_shutdown_started) {
		raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	}

	if (raid_bdev->module->stop != NULL) {
		if (raid_bdev->module->stop(raid_bdev) == false) {
			return;
		}
	}

	raid_bdev_module_stop_done(raid_bdev);
}

static int
raid_bdev_destruct(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_destruct, ctx);

	return 1;
}

void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);

	spdk_bdev_io_complete(bdev_io, status);
}

/*
 * brief:
 * raid_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the raid_io if this is the final expected IO.
 * The caller should first set raid_io->base_bdev_io_remaining. This function
 * will decrement this counter by the value of the 'completed' parameter and
 * complete the raid_io if the counter reaches 0. The caller is free to
 * interpret the 'base_bdev_io_remaining' and 'completed' values as needed,
 * it can represent e.g. blocks or IOs.
 * params:
 * raid_io - pointer to raid_bdev_io
 * completed - the part of the raid_io that has been completed
 * status - status of the base IO
 * returns:
 * true - if the raid_io is completed
 * false - otherwise
 */
bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	assert(raid_io->base_bdev_io_remaining >= completed);
	raid_io->base_bdev_io_remaining -= completed;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		raid_io->base_bdev_io_status = status;
	}

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

/*
 * brief:
 * raid_bdev_queue_io_wait function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * raid_io - pointer to raid_bdev_io
 * bdev - the block device that the IO is submitted to
 * ch - io channel
 * cb_fn - callback when the spdk_bdev_io for bdev becomes available
 * returns:
 * none
 */
void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = raid_io;
	spdk_bdev_queue_io_wait(bdev, ch, &raid_io->waitq_entry);
}

static void
raid_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io);

static void
_raid_bdev_submit_reset_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid_bdev_submit_reset_request(raid_io);
}

/*
 * brief:
 * raid_bdev_submit_reset_request function submits reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * raid_io
 * returns:
 * none
 */
static void
raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	int				ret;
	uint8_t				i;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	raid_bdev = raid_io->raid_bdev;

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
	}

	while (raid_io->base_bdev_io_submitted < raid_bdev->num_base_bdevs) {
		i = raid_io->base_bdev_io_submitted;
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_io->raid_ch->base_channel[i];
		ret = spdk_bdev_reset(base_info->desc, base_ch,
				      raid_base_bdev_reset_complete, raid_io);
		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, base_info->bdev, base_ch,
						_raid_bdev_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
static void
raid_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	if (!success) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	raid_io->raid_bdev->module->submit_rw_request(raid_io);
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
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	raid_io->raid_bdev = bdev_io->bdev->ctxt;
	raid_io->raid_ch = spdk_io_channel_get_ctx(ch);
	raid_io->base_bdev_io_remaining = 0;
	raid_io->base_bdev_io_submitted = 0;
	raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, raid_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		raid_io->raid_bdev->module->submit_rw_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		raid_bdev_submit_reset_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		raid_io->raid_bdev->module->submit_null_payload_request(raid_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
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
_raid_bdev_io_type_supported(struct raid_bdev *raid_bdev, enum spdk_bdev_io_type io_type)
{
	struct raid_base_bdev_info *base_info;

	if (io_type == SPDK_BDEV_IO_TYPE_FLUSH ||
	    io_type == SPDK_BDEV_IO_TYPE_UNMAP) {
		if (raid_bdev->module->submit_null_payload_request == NULL) {
			return false;
		}
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->bdev == NULL) {
			assert(false);
			continue;
		}

		if (spdk_bdev_io_type_supported(base_info->bdev, io_type) == false) {
			return false;
		}
	}

	return true;
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
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return _raid_bdev_io_type_supported(ctx, io_type);

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

void
raid_bdev_write_info_json(struct raid_bdev *raid_bdev, struct spdk_json_write_ctx *w)
{
	struct raid_base_bdev_info *base_info;

	assert(raid_bdev != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "state", raid_bdev_state_to_str(raid_bdev->state));
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));
	spdk_json_write_named_uint32(w, "num_base_bdevs", raid_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", raid_bdev->num_base_bdevs_discovered);
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->bdev) {
			spdk_json_write_string(w, base_info->bdev->name);
		} else {
			spdk_json_write_null(w);
		}
	}
	spdk_json_write_array_end(w);
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

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_dump_config_json\n");

	/* Dump the raid bdev configuration related information */
	spdk_json_write_named_object_begin(w, "raid");
	raid_bdev_write_info_json(raid_bdev, w);
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
	struct raid_base_bdev_info *base_info;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_raid_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));

	spdk_json_write_named_array_begin(w, "base_bdevs");
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->bdev) {
			spdk_json_write_string(w, base_info->bdev->name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
raid_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct raid_bdev *raid_bdev = ctx;
	struct spdk_bdev *base_bdev;
	uint32_t i;
	int domains_count = 0, rc;

	/* First loop to get the number of memory domains */
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		base_bdev = raid_bdev->base_bdev_info[i].bdev;
		rc = spdk_bdev_get_memory_domains(base_bdev, NULL, 0);
		if (rc < 0) {
			return rc;
		}
		domains_count += rc;
	}

	if (!domains || array_size < domains_count) {
		return domains_count;
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		base_bdev = raid_bdev->base_bdev_info[i].bdev;
		rc = spdk_bdev_get_memory_domains(base_bdev, domains, array_size);
		if (rc < 0) {
			return rc;
		}
		domains += rc;
		array_size -= rc;
	}

	return domains_count;
}

/* g_raid_bdev_fn_table is the function table for raid bdev */
static const struct spdk_bdev_fn_table g_raid_bdev_fn_table = {
	.destruct		= raid_bdev_destruct,
	.submit_request		= raid_bdev_submit_request,
	.io_type_supported	= raid_bdev_io_type_supported,
	.get_io_channel		= raid_bdev_get_io_channel,
	.dump_info_json		= raid_bdev_dump_info_json,
	.write_config_json	= raid_bdev_write_config_json,
	.get_memory_domains	= raid_bdev_get_memory_domains,
};

struct raid_bdev *
raid_bdev_find_by_name(const char *name)
{
	struct raid_bdev *raid_bdev;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (strcmp(raid_bdev->bdev.name, name) == 0) {
			return raid_bdev;
		}
	}

	return NULL;
}

static struct {
	const char *name;
	enum raid_level value;
} g_raid_level_names[] = {
	{ "raid0", RAID0 },
	{ "0", RAID0 },
	{ "raid1", RAID1 },
	{ "1", RAID1 },
	{ "raid5f", RAID5F },
	{ "5f", RAID5F },
	{ "concat", CONCAT },
	{ }
};

static struct {
	const char *name;
	enum raid_bdev_state value;
} g_raid_state_names[] = {
	{ "online", RAID_BDEV_STATE_ONLINE },
	{ "configuring", RAID_BDEV_STATE_CONFIGURING },
	{ "offline", RAID_BDEV_STATE_OFFLINE },
	{ }
};

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum raid_level raid_level_t;
typedef enum raid_bdev_state raid_bdev_state_t;

raid_level_t
raid_bdev_str_to_level(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (strcasecmp(g_raid_level_names[i].name, str) == 0) {
			return g_raid_level_names[i].value;
		}
	}

	return INVALID_RAID_LEVEL;
}

const char *
raid_bdev_level_to_str(enum raid_level level)
{
	unsigned int i;

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (g_raid_level_names[i].value == level) {
			return g_raid_level_names[i].name;
		}
	}

	return "";
}

raid_bdev_state_t
raid_bdev_str_to_state(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; g_raid_state_names[i].name != NULL; i++) {
		if (strcasecmp(g_raid_state_names[i].name, str) == 0) {
			return g_raid_state_names[i].value;
		}
	}

	return RAID_BDEV_STATE_MAX;
}

const char *
raid_bdev_state_to_str(enum raid_bdev_state state)
{
	unsigned int i;

	for (i = 0; g_raid_state_names[i].name != NULL; i++) {
		if (g_raid_state_names[i].value == state) {
			return g_raid_state_names[i].name;
		}
	}

	assert(false);
	return "";
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
	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_fini_start\n");
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
	struct raid_bdev *raid_bdev, *tmp;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_exit\n");

	TAILQ_FOREACH_SAFE(raid_bdev, &g_raid_bdev_list, global_link, tmp) {
		raid_bdev_cleanup_and_free(raid_bdev);
	}
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
	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_get_ctx_size\n");
	return sizeof(struct raid_bdev_io);
}

static struct spdk_bdev_module g_raid_if = {
	.name = "raid",
	.module_init = raid_bdev_init,
	.fini_start = raid_bdev_fini_start,
	.module_fini = raid_bdev_exit,
	.get_ctx_size = raid_bdev_get_ctx_size,
	.examine_config = raid_bdev_examine,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(raid, &g_raid_if)

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
	return 0;
}

/*
 * brief:
 * raid_bdev_create allocates raid bdev based on passed configuration
 * params:
 * name - name for raid bdev
 * strip_size - strip size in KB
 * num_base_bdevs - number of base bdevs
 * level - raid level
 * raid_bdev_out - the created raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		 enum raid_level level, struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *raid_bdev_gen;
	struct raid_bdev_module *module;
	uint8_t min_operational;

	if (raid_bdev_find_by_name(name) != NULL) {
		SPDK_ERRLOG("Duplicate raid bdev name found: %s\n", name);
		return -EEXIST;
	}

	if (level == RAID1) {
		if (strip_size != 0) {
			SPDK_ERRLOG("Strip size is not supported by raid1\n");
			return -EINVAL;
		}
	} else if (spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %" PRIu32 "\n", strip_size);
		return -EINVAL;
	}

	module = raid_bdev_module_find(level);
	if (module == NULL) {
		SPDK_ERRLOG("Unsupported raid level '%d'\n", level);
		return -EINVAL;
	}

	assert(module->base_bdevs_min != 0);
	if (num_base_bdevs < module->base_bdevs_min) {
		SPDK_ERRLOG("At least %u base devices required for %s\n",
			    module->base_bdevs_min,
			    raid_bdev_level_to_str(level));
		return -EINVAL;
	}

	switch (module->base_bdevs_constraint.type) {
	case CONSTRAINT_MAX_BASE_BDEVS_REMOVED:
		min_operational = num_base_bdevs - module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL:
		min_operational = module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_UNSET:
		if (module->base_bdevs_constraint.value != 0) {
			SPDK_ERRLOG("Unexpected constraint value '%u' provided for raid bdev '%s'.\n",
				    (uint8_t)module->base_bdevs_constraint.value, name);
			return -EINVAL;
		}
		min_operational = num_base_bdevs;
		break;
	default:
		SPDK_ERRLOG("Unrecognised constraint type '%u' in module for raid level '%s'.\n",
			    (uint8_t)module->base_bdevs_constraint.type,
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	};

	if (min_operational == 0 || min_operational > num_base_bdevs) {
		SPDK_ERRLOG("Wrong constraint value for raid level '%s'.\n",
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	}

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	if (!raid_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for raid bdev\n");
		return -ENOMEM;
	}

	raid_bdev->module = module;
	raid_bdev->num_base_bdevs = num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	if (!raid_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable able to allocate base bdev info\n");
		free(raid_bdev);
		return -ENOMEM;
	}

	/* strip_size_kb is from the rpc param.  strip_size is in blocks and used
	 * internally and set later.
	 */
	raid_bdev->strip_size = 0;
	raid_bdev->strip_size_kb = strip_size;
	raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	raid_bdev->level = level;
	raid_bdev->min_base_bdevs_operational = min_operational;

	raid_bdev_gen = &raid_bdev->bdev;

	raid_bdev_gen->name = strdup(name);
	if (!raid_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for raid\n");
		free(raid_bdev->base_bdev_info);
		free(raid_bdev);
		return -ENOMEM;
	}

	raid_bdev_gen->product_name = "Raid Volume";
	raid_bdev_gen->ctxt = raid_bdev;
	raid_bdev_gen->fn_table = &g_raid_bdev_fn_table;
	raid_bdev_gen->module = &g_raid_if;
	raid_bdev_gen->write_cache = 0;

	TAILQ_INSERT_TAIL(&g_raid_bdev_list, raid_bdev, global_link);

	*raid_bdev_out = raid_bdev;

	return 0;
}

/*
 * brief:
 * Check underlying block devices against support for metadata. Do not configure
 * md support when parameters from block devices are inconsistent.
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * 0 - The raid bdev md parameters were successfully configured.
 * non zero - Failed to configure md.
 */
static int
raid_bdev_configure_md(struct raid_bdev *raid_bdev)
{
	struct spdk_bdev *base_bdev;
	uint8_t i;

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		base_bdev = raid_bdev->base_bdev_info[i].bdev;

		if (i == 0) {
			raid_bdev->bdev.md_len = spdk_bdev_get_md_size(base_bdev);
			raid_bdev->bdev.md_interleave = spdk_bdev_is_md_interleaved(base_bdev);
			raid_bdev->bdev.dif_type = spdk_bdev_get_dif_type(base_bdev);
			raid_bdev->bdev.dif_is_head_of_md = spdk_bdev_is_dif_head_of_md(base_bdev);
			raid_bdev->bdev.dif_check_flags = base_bdev->dif_check_flags;
			continue;
		}

		if (raid_bdev->bdev.md_len != spdk_bdev_get_md_size(base_bdev) ||
		    raid_bdev->bdev.md_interleave != spdk_bdev_is_md_interleaved(base_bdev) ||
		    raid_bdev->bdev.dif_type != spdk_bdev_get_dif_type(base_bdev) ||
		    raid_bdev->bdev.dif_is_head_of_md != spdk_bdev_is_dif_head_of_md(base_bdev) ||
		    raid_bdev->bdev.dif_check_flags != base_bdev->dif_check_flags) {
			SPDK_ERRLOG("base bdevs are configured with different metadata formats\n");
			return -EPERM;
		}
	}

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
	uint32_t blocklen = 0;
	struct spdk_bdev *raid_bdev_gen;
	struct raid_base_bdev_info *base_info;
	int rc = 0;

	assert(raid_bdev->state == RAID_BDEV_STATE_CONFIGURING);
	assert(raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		assert(base_info->bdev != NULL);
		/* Check blocklen for all base bdevs that it should be same */
		if (blocklen == 0) {
			blocklen = base_info->bdev->blocklen;
		} else if (blocklen != base_info->bdev->blocklen) {
			/*
			 * Assumption is that all the base bdevs for any raid bdev should
			 * have same blocklen
			 */
			SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
			return -EINVAL;
		}
	}
	assert(blocklen > 0);

	/* The strip_size_kb is read in from user in KB. Convert to blocks here for
	 * internal use.
	 */
	raid_bdev->strip_size = (raid_bdev->strip_size_kb * 1024) / blocklen;
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);
	raid_bdev->blocklen_shift = spdk_u32log2(blocklen);

	raid_bdev_gen = &raid_bdev->bdev;
	raid_bdev_gen->blocklen = blocklen;

	rc = raid_bdev_configure_md(raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("raid metadata configuration failed\n");
		return rc;
	}

	rc = raid_bdev->module->start(raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("raid module startup callback failed\n");
		return rc;
	}
	raid_bdev->state = RAID_BDEV_STATE_ONLINE;
	SPDK_DEBUGLOG(bdev_raid, "io device register %p\n", raid_bdev);
	SPDK_DEBUGLOG(bdev_raid, "blockcnt %" PRIu64 ", blocklen %u\n",
		      raid_bdev_gen->blockcnt, raid_bdev_gen->blocklen);
	spdk_io_device_register(raid_bdev, raid_bdev_create_cb, raid_bdev_destroy_cb,
				sizeof(struct raid_bdev_io_channel),
				raid_bdev->bdev.name);
	rc = spdk_bdev_register(raid_bdev_gen);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to register raid bdev and stay at configuring state\n");
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
		spdk_io_device_unregister(raid_bdev, NULL);
		raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
		return rc;
	}
	SPDK_DEBUGLOG(bdev_raid, "raid bdev generic %p\n", raid_bdev_gen);
	SPDK_DEBUGLOG(bdev_raid, "raid bdev is created with name %s, raid_bdev %p\n",
		      raid_bdev_gen->name, raid_bdev);

	return 0;
}

/*
 * brief:
 * If raid bdev is online and registered, change the bdev state to
 * configuring and unregister this raid device. Queue this raid device
 * in configuring list
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
raid_bdev_deconfigure(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn,
		      void *cb_arg)
{
	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	assert(raid_bdev->num_base_bdevs == raid_bdev->num_base_bdevs_discovered);
	raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	assert(raid_bdev->num_base_bdevs_discovered);
	SPDK_DEBUGLOG(bdev_raid, "raid bdev state changing from online to offline\n");

	spdk_bdev_unregister(&raid_bdev->bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * raid_bdev_find_by_base_bdev function finds the raid bdev which has
 *  claimed the base bdev.
 * params:
 * base_bdev - pointer to base bdev pointer
 * _raid_bdev - Reference to pointer to raid bdev
 * _base_info - Reference to the raid base bdev info.
 * returns:
 * true - if the raid bdev is found.
 * false - if the raid bdev is not found.
 */
static bool
raid_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev, struct raid_bdev **_raid_bdev,
			    struct raid_base_bdev_info **_base_info)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->bdev == base_bdev) {
				*_raid_bdev = raid_bdev;
				*_base_info = base_info;
				return true;
			}
		}
	}

	return false;
}

/*
 * brief:
 * raid_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any raid bdev
 * or not. If yes, it takes necessary action on that particular raid bdev.
 * params:
 * base_bdev - pointer to base bdev which got removed
 * returns:
 * none
 */
static void
raid_bdev_remove_base_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev	*raid_bdev = NULL;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_remove_base_bdev\n");

	/* Find the raid_bdev which has claimed this base_bdev */
	if (!raid_bdev_find_by_base_bdev(base_bdev, &raid_bdev, &base_info)) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return;
	}

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	assert(base_info->desc);
	base_info->remove_scheduled = true;

	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		/*
		 * As raid bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 */
		raid_bdev_free_base_bdev_resource(raid_bdev, base_info);
		if (raid_bdev->num_base_bdevs_discovered == 0) {
			/* There is no base bdev for this raid, so free the raid device. */
			raid_bdev_cleanup_and_free(raid_bdev);
			return;
		}
	}

	raid_bdev_deconfigure(raid_bdev, NULL, NULL);
}

/*
 * brief:
 * raid_bdev_resize_base_bdev function is called by below layers when base_bdev
 * is resized. This function checks if the smallest size of the base_bdevs is changed.
 * If yes, call module handler to resize the raid_bdev if implemented.
 * params:
 * base_bdev - pointer to base bdev which got resized.
 * returns:
 * none
 */
static void
raid_bdev_resize_base_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev	*raid_bdev = NULL;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_resize_base_bdev\n");

	/* Find the raid_bdev which has claimed this base_bdev */
	if (!raid_bdev_find_by_base_bdev(base_bdev, &raid_bdev, &base_info)) {
		SPDK_ERRLOG("raid_bdev whose base_bdev '%s' not found\n", base_bdev->name);
		return;
	}

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	SPDK_NOTICELOG("base_bdev '%s' was resized: old size %" PRIu64 ", new size %" PRIu64 "\n",
		       base_bdev->name, base_info->blockcnt, base_bdev->blockcnt);

	if (raid_bdev->module->resize) {
		raid_bdev->module->resize(raid_bdev);
	}
}

/*
 * brief:
 * raid_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * params:
 * type - event details.
 * bdev - bdev that triggered event.
 * event_ctx - context for event.
 * returns:
 * none
 */
static void
raid_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		raid_bdev_remove_base_bdev(bdev);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		raid_bdev_resize_base_bdev(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * brief:
 * Deletes the specified raid bdev
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 */
void
raid_bdev_delete(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_arg)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "delete raid bdev: %s\n", raid_bdev->bdev.name);

	if (raid_bdev->destroy_started) {
		SPDK_DEBUGLOG(bdev_raid, "destroying raid bdev %s is already started\n",
			      raid_bdev->bdev.name);
		if (cb_fn) {
			cb_fn(cb_arg, -EALREADY);
		}
		return;
	}

	raid_bdev->destroy_started = true;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->remove_scheduled = true;

		if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
			/*
			 * As raid bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 */
			raid_bdev_free_base_bdev_resource(raid_bdev, base_info);
		}
	}

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* There is no base bdev for this raid, so free the raid device. */
		raid_bdev_cleanup_and_free(raid_bdev);
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
	} else {
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_arg);
	}
}

static int
raid_bdev_configure_base_bdev(struct raid_bdev *raid_bdev, struct raid_base_bdev_info *base_info)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(base_info->name != NULL);
	assert(base_info->bdev == NULL);

	rc = spdk_bdev_open_ext(base_info->name, true, raid_bdev_event_base_bdev, NULL, &desc);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_info->name);
		}
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_raid_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_raid, "bdev %s is claimed\n", bdev->name);

	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);

	base_info->bdev = bdev;
	base_info->desc = desc;
	base_info->blockcnt = bdev->blockcnt;
	raid_bdev->num_base_bdevs_discovered++;
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
 * raid_bdev_add_base_device function is the actual function which either adds
 * the nvme base device to existing raid bdev or create a new raid bdev. It also claims
 * the base device and keep the open descriptor.
 * params:
 * raid_bdev - pointer to raid bdev
 * name - name of the base bdev
 * slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_add_base_device(struct raid_bdev *raid_bdev, const char *name, uint8_t slot)
{
	struct raid_base_bdev_info *base_info;
	int rc;

	if (slot >= raid_bdev->num_base_bdevs) {
		return -EINVAL;
	}

	base_info = &raid_bdev->base_bdev_info[slot];

	if (base_info->name != NULL) {
		SPDK_ERRLOG("Slot %u on raid bdev '%s' already assigned to bdev '%s'\n",
			    slot, raid_bdev->bdev.name, base_info->name);
		return -EBUSY;
	}

	base_info->name = strdup(name);
	if (base_info->name == NULL) {
		return -ENOMEM;
	}

	rc = raid_bdev_configure_base_bdev(raid_bdev, base_info);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Failed to allocate resource for bdev '%s'\n", name);
		}
		return rc;
	}

	return 0;
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
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->bdev == NULL && strcmp(bdev->name, base_info->name) == 0) {
				raid_bdev_configure_base_bdev(raid_bdev, base_info);
				break;
			}
		}
	}

	spdk_bdev_module_examine_done(&g_raid_if);
}

/* Log component for bdev raid bdev module */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid)
