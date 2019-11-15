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

#ifndef SPDK_BDEV_RAID_INTERNAL_H
#define SPDK_BDEV_RAID_INTERNAL_H

#include "spdk/bdev_module.h"

enum raid_level {
	INVALID_RAID_LEVEL	= -1,
	RAID0			= 0,
	RAID5			= 5,
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

	/* raid bdev max, new states should be added before this */
	RAID_BDEV_MAX
};

/*
 * raid_base_bdev_info contains information for the base bdevs which are part of some
 * raid. This structure contains the per base bdev information. Whatever is
 * required per base device for raid bdev will be kept here
 */
struct raid_base_bdev_info {
	/* pointer to base spdk bdev */
	struct spdk_bdev	*bdev;

	/* pointer to base bdev descriptor opened by raid bdev */
	struct spdk_bdev_desc	*desc;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool			remove_scheduled;

	/* thread where base device is opened */
	struct spdk_thread	*thread;
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
};

/*
 * raid_bdev is the single entity structure which contains SPDK block device
 * and the information related to any raid bdev either configured or
 * in configuring list. io device is created on this.
 */
struct raid_bdev {
	/* raid bdev device, this will get registered in bdev layer */
	struct spdk_bdev		bdev;

	/* link of raid bdev to link it to configured, configuring or offline list */
	TAILQ_ENTRY(raid_bdev)		state_link;

	/* link of raid bdev to link it to global raid bdev list */
	TAILQ_ENTRY(raid_bdev)		global_link;

	/* pointer to config file entry */
	struct raid_bdev_config		*config;

	/* array of base bdev info */
	struct raid_base_bdev_info	*base_bdev_info;

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

	/* Raid Level of this raid bdev */
	enum raid_level			level;

	/* Set to true if destruct is called for this raid bdev */
	bool				destruct_called;

	/* Set to true if destroy of this raid bdev is started. */
	bool				destroy_started;

	/* Module for RAID-level specific operations */
	struct raid_bdev_module		*module;

	/* Private data for the raid module */
	void				*module_private;
};

#define RAID_FOR_EACH_BASE_BDEV(r, i) \
	for (i = r->base_bdev_info; i < r->base_bdev_info + r->num_base_bdevs; i++)

/*
 * raid_base_bdev_config is the per base bdev data structure which contains
 * information w.r.t to per base bdev during parsing config
 */
struct raid_base_bdev_config {
	/* base bdev name from config file */
	char				*name;
};

/*
 * raid_bdev_config contains the raid bdev config related information after
 * parsing the config file
 */
struct raid_bdev_config {
	/* base bdev config per underlying bdev */
	struct raid_base_bdev_config	*base_bdev;

	/* Points to already created raid bdev  */
	struct raid_bdev		*raid_bdev;

	char				*name;

	/* strip size of this raid bdev  in kilo bytes */
	uint32_t			strip_size;

	/* number of base bdevs */
	uint8_t				num_base_bdevs;

	/* raid level */
	enum raid_level			level;

	TAILQ_ENTRY(raid_bdev_config)	link;
};

/*
 * raid_config is the top level structure representing the raid bdev config as read
 * from config file for all raids
 */
struct raid_config {
	/* raid bdev  context from config file */
	TAILQ_HEAD(, raid_bdev_config) raid_bdev_config_head;

	/* total raid bdev  from config file */
	uint8_t total_raid_bdev;
};

/*
 * raid_bdev_io_channel is the context of spdk_io_channel for raid bdev device. It
 * contains the relationship of raid bdev io channel with base bdev io channels.
 */
struct raid_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;

	/* Number of IO channels */
	uint8_t			num_channels;
};

/* TAIL heads for various raid bdev lists */
TAILQ_HEAD(raid_configured_tailq, raid_bdev);
TAILQ_HEAD(raid_configuring_tailq, raid_bdev);
TAILQ_HEAD(raid_all_tailq, raid_bdev);
TAILQ_HEAD(raid_offline_tailq, raid_bdev);

extern struct raid_configured_tailq	g_raid_bdev_configured_list;
extern struct raid_configuring_tailq	g_raid_bdev_configuring_list;
extern struct raid_all_tailq		g_raid_bdev_list;
extern struct raid_offline_tailq	g_raid_bdev_offline_list;
extern struct raid_config		g_raid_config;

typedef void (*raid_bdev_destruct_cb)(void *cb_ctx, int rc);

int raid_bdev_create(struct raid_bdev_config *raid_cfg);
int raid_bdev_add_base_devices(struct raid_bdev_config *raid_cfg);
void raid_bdev_remove_base_devices(struct raid_bdev_config *raid_cfg,
				   raid_bdev_destruct_cb cb_fn, void *cb_ctx);
int raid_bdev_config_add(const char *raid_name, uint32_t strip_size, uint8_t num_base_bdevs,
			 enum raid_level level, struct raid_bdev_config **_raid_cfg);
int raid_bdev_config_add_base_bdev(struct raid_bdev_config *raid_cfg,
				   const char *base_bdev_name, uint8_t slot);
void raid_bdev_config_cleanup(struct raid_bdev_config *raid_cfg);
struct raid_bdev_config *raid_bdev_config_find_by_name(const char *raid_name);
enum raid_level raid_bdev_parse_raid_level(const char *str);
const char *raid_bdev_level_to_str(enum raid_level level);

/*
 * RAID module descriptor
 */
struct raid_bdev_module {
	/* RAID level implemented by this module */
	enum raid_level level;

	/* Minimum required number of base bdevs. Must be > 0. */
	uint8_t base_bdevs_min;

	/*
	 * Maximum number of base bdevs that can be removed without failing
	 * the array.
	 */
	uint8_t base_bdevs_max_degraded;

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
	 */
	void (*stop)(struct raid_bdev *raid_bdev);

	/* Handler for R/W requests */
	void (*submit_rw_request)(struct raid_bdev_io *raid_io);

	/* Handler for requests without payload (flush, unmap). Optional. */
	void (*submit_null_payload_request)(struct raid_bdev_io *raid_io);

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

bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status);
void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn);
void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status);

#endif /* SPDK_BDEV_RAID_INTERNAL_H */
