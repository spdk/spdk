/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_RAID_INTERNAL_H
#define SPDK_BDEV_RAID_INTERNAL_H

#include "spdk/bdev_module.h"
#include "spdk/uuid.h"

#define RAID_BDEV_MIN_DATA_OFFSET_SIZE	(1024*1024) /* 1 MiB */

enum raid_level {
	INVALID_RAID_LEVEL	= -1,
	RAID0			= 0,
	RAID1			= 1,
	RAID5F			= 95, /* 0x5f */
	CONCAT			= 99,
};

/*
 * Raid state describes the state of the raid. This raid bdev can be either in
 * configured list or configuring list
 */
enum raid_bdev_state {
	/* raid bdev is ready and is seen by upper layers */
	RAID_BDEV_STATE_ONLINE,

	/*
	 * raid bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	RAID_BDEV_STATE_CONFIGURING,

	/*
	 * In offline state, raid bdev layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	RAID_BDEV_STATE_OFFLINE,

	/* raid bdev state max, new states should be added before this */
	RAID_BDEV_STATE_MAX
};

typedef void (*raid_bdev_remove_base_bdev_cb)(void *ctx, int status);

/*
 * raid_base_bdev_info contains information for the base bdevs which are part of some
 * raid. This structure contains the per base bdev information. Whatever is
 * required per base device for raid bdev will be kept here
 */
struct raid_base_bdev_info {
	/* The raid bdev that this base bdev belongs to */
	struct raid_bdev	*raid_bdev;

	/* name of the bdev */
	char			*name;

	/* pointer to base bdev descriptor opened by raid bdev */
	struct spdk_bdev_desc	*desc;

	/* offset in blocks from the start of the base bdev to the start of the data region */
	uint64_t		data_offset;

	/* size in blocks of the base bdev's data region */
	uint64_t		data_size;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool			remove_scheduled;

	/* callback for base bdev removal */
	raid_bdev_remove_base_bdev_cb remove_cb;

	/* context of the callback */
	void			*remove_cb_ctx;

	/* Hold the number of blocks to know how large the base bdev is resized. */
	uint64_t		blockcnt;
};

/*
 * raid_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for a raid bdev
 */
struct raid_bdev_io {
	/* The raid bdev associated with this IO */
	struct raid_bdev *raid_bdev;

	/* WaitQ entry, used only in waitq logic */
	struct spdk_bdev_io_wait_entry	waitq_entry;

	/* Context of the original channel for this IO */
	struct raid_bdev_io_channel	*raid_ch;

	/* Used for tracking progress on io requests sent to member disks. */
	uint64_t			base_bdev_io_remaining;
	uint8_t				base_bdev_io_submitted;
	uint8_t				base_bdev_io_status;

	/* Private data for the raid module */
	void				*module_private;
};

/*
 * raid_bdev is the single entity structure which contains SPDK block device
 * and the information related to any raid bdev either configured or
 * in configuring list. io device is created on this.
 */
struct raid_bdev {
	/* raid bdev device, this will get registered in bdev layer */
	struct spdk_bdev		bdev;

	/* link of raid bdev to link it to global raid bdev list */
	TAILQ_ENTRY(raid_bdev)		global_link;

	/* array of base bdev info */
	struct raid_base_bdev_info	*base_bdev_info;

	/* lock to protect the base bdev array */
	struct spdk_spinlock		base_bdev_lock;

	/* strip size of raid bdev in blocks */
	uint32_t			strip_size;

	/* strip size of raid bdev in KB */
	uint32_t			strip_size_kb;

	/* strip size bit shift for optimized calculation */
	uint32_t			strip_size_shift;

	/* block length bit shift for optimized calculation */
	uint32_t			blocklen_shift;

	/* state of raid bdev */
	enum raid_bdev_state		state;

	/* number of base bdevs comprising raid bdev  */
	uint8_t				num_base_bdevs;

	/* number of base bdevs discovered */
	uint8_t				num_base_bdevs_discovered;

	/* minimum number of viable base bdevs that are required by array to operate */
	uint8_t				min_base_bdevs_operational;

	/* Raid Level of this raid bdev */
	enum raid_level			level;

	/* Set to true if destroy of this raid bdev is started. */
	bool				destroy_started;

	/* Set to true if superblock metadata is enabled on this raid bdev */
	bool				superblock_enabled;

	/* Module for RAID-level specific operations */
	struct raid_bdev_module		*module;

	/* Private data for the raid module */
	void				*module_private;
};

#define RAID_FOR_EACH_BASE_BDEV(r, i) \
	for (i = r->base_bdev_info; i < r->base_bdev_info + r->num_base_bdevs; i++)

/*
 * raid_bdev_io_channel is the context of spdk_io_channel for raid bdev device. It
 * contains the relationship of raid bdev io channel with base bdev io channels.
 */
struct raid_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;

	/* Number of IO channels */
	uint8_t			num_channels;

	/* Private raid module IO channel */
	struct spdk_io_channel	*module_channel;
};

/* TAIL head for raid bdev list */
TAILQ_HEAD(raid_all_tailq, raid_bdev);

extern struct raid_all_tailq		g_raid_bdev_list;

typedef void (*raid_bdev_destruct_cb)(void *cb_ctx, int rc);

int raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		     enum raid_level level, bool superblock, const struct spdk_uuid *uuid,
		     struct raid_bdev **raid_bdev_out);
void raid_bdev_delete(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_ctx);
int raid_bdev_add_base_device(struct raid_bdev *raid_bdev, const char *name, uint8_t slot);
struct raid_bdev *raid_bdev_find_by_name(const char *name);
enum raid_level raid_bdev_str_to_level(const char *str);
const char *raid_bdev_level_to_str(enum raid_level level);
enum raid_bdev_state raid_bdev_str_to_state(const char *str);
const char *raid_bdev_state_to_str(enum raid_bdev_state state);
void raid_bdev_write_info_json(struct raid_bdev *raid_bdev, struct spdk_json_write_ctx *w);
int raid_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, raid_bdev_remove_base_bdev_cb cb_fn,
			       void *cb_ctx);

/*
 * RAID module descriptor
 */
struct raid_bdev_module {
	/* RAID level implemented by this module */
	enum raid_level level;

	/* Minimum required number of base bdevs. Must be > 0. */
	uint8_t base_bdevs_min;

	/*
	 * RAID constraint. Determines number of base bdevs that can be removed
	 * without failing the array.
	 */
	struct {
		enum {
			CONSTRAINT_UNSET = 0,
			CONSTRAINT_MAX_BASE_BDEVS_REMOVED,
			CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL,
		} type;
		uint8_t value;
	} base_bdevs_constraint;

	/* Set to true if this module supports memory domains. */
	bool memory_domains_supported;

	/*
	 * Called when the raid is starting, right before changing the state to
	 * online and registering the bdev. Parameters of the bdev like blockcnt
	 * should be set here.
	 *
	 * Non-zero return value will abort the startup process.
	 */
	int (*start)(struct raid_bdev *raid_bdev);

	/*
	 * Called when the raid is stopping, right before changing the state to
	 * offline and unregistering the bdev. Optional.
	 *
	 * The function should return false if it is asynchronous. Then, after
	 * the async operation has completed and the module is fully stopped
	 * raid_bdev_module_stop_done() must be called.
	 */
	bool (*stop)(struct raid_bdev *raid_bdev);

	/* Handler for R/W requests */
	void (*submit_rw_request)(struct raid_bdev_io *raid_io);

	/* Handler for requests without payload (flush, unmap). Optional. */
	void (*submit_null_payload_request)(struct raid_bdev_io *raid_io);

	/*
	 * Called when the bdev's IO channel is created to get the module's private IO channel.
	 * Optional.
	 */
	struct spdk_io_channel *(*get_io_channel)(struct raid_bdev *raid_bdev);

	/*
	 * Called when a base_bdev is resized to resize the raid if the condition
	 * is satisfied.
	 */
	void (*resize)(struct raid_bdev *raid_bdev);

	TAILQ_ENTRY(raid_bdev_module) link;
};

void raid_bdev_module_list_add(struct raid_bdev_module *raid_module);

#define __RAID_MODULE_REGISTER(line) __RAID_MODULE_REGISTER_(line)
#define __RAID_MODULE_REGISTER_(line) raid_module_register_##line

#define RAID_MODULE_REGISTER(_module)					\
__attribute__((constructor)) static void				\
__RAID_MODULE_REGISTER(__LINE__)(void)					\
{									\
    raid_bdev_module_list_add(_module);					\
}

bool raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
				enum spdk_bdev_io_status status);
void raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			     struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn);
void raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status);
void raid_bdev_module_stop_done(struct raid_bdev *raid_bdev);

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_readv_blocks_ext function.
 */
static inline int
raid_bdev_readv_blocks_ext(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			   uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts)
{
	return spdk_bdev_readv_blocks_ext(base_info->desc, ch, iov, iovcnt,
					  base_info->data_offset + offset_blocks, num_blocks, cb, cb_arg, opts);
}

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_writev_blocks_ext function.
 */
static inline int
raid_bdev_writev_blocks_ext(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			    uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	return spdk_bdev_writev_blocks_ext(base_info->desc, ch, iov, iovcnt,
					   base_info->data_offset + offset_blocks, num_blocks, cb, cb_arg, opts);
}

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_unmap_blocks function.
 */
static inline int
raid_bdev_unmap_blocks(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_unmap_blocks(base_info->desc, ch, base_info->data_offset + offset_blocks,
				      num_blocks, cb, cb_arg);
}

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_flush_blocks function.
 */
static inline int
raid_bdev_flush_blocks(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_flush_blocks(base_info->desc, ch, base_info->data_offset + offset_blocks,
				      num_blocks, cb, cb_arg);
}

#endif /* SPDK_BDEV_RAID_INTERNAL_H */
