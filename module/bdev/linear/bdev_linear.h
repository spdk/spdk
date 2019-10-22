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

#ifndef SPDK_BDEV_LINEAR_INTERNAL_H
#define SPDK_BDEV_LINEAR_INTERNAL_H

#include "spdk/bdev_module.h"

/*
 * Linear state describes the state of the linear. This linear bdev can be either in
 * configured list or configuring list
 */
enum linear_bdev_state {
	/* linear bdev is ready and is seen by upper layers */
	LINEAR_BDEV_STATE_ONLINE,

	/*
	 * linear bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	LINEAR_BDEV_STATE_CONFIGURING,

	/*
	 * In offline state, linear bdev layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	LINEAR_BDEV_STATE_OFFLINE,

	/* linear bdev max, new states should be added before this */
	LINEAR_BDEV_MAX
};

/*
 * linear_base_bdev_info contains information for the base bdevs which are part of some
 * linear. This structure contains the per base bdev information. Whatever is
 * required per base device for linear bdev will be kept here
 */
struct linear_base_bdev_info {
	/* pointer to base spdk bdev */
	struct spdk_bdev *bdev;

	/* pointer to base bdev descriptor opened by linear bdev */
	struct spdk_bdev_desc *desc;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool remove_scheduled;
};

/*
 * linear_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for a linear bdev
 */
struct linear_bdev_io {
	/* WaitQ entry, used only in waitq logic */
	struct spdk_bdev_io_wait_entry waitq_entry;

	/* Original channel for this IO, used in queuing logic */
	struct spdk_io_channel *ch;

	/* Used for tracking progress on io requests sent to member disks. */
	uint8_t base_bdev_io_submitted;
	uint8_t base_bdev_io_completed;
	uint8_t base_bdev_io_expected;
	uint8_t base_bdev_io_status;
};

/*
 * linear_bdev is the single entity structure which contains SPDK block device
 * and the information related to any linear bdev eitehr configured or
 * in configuring list. io device is created on this.
 */
struct linear_bdev {
	/* linear bdev device, this will get registered in bdev layer */
	struct spdk_bdev bdev;

	/* link of linear bdev to link it to configured, configuring or offline list */
	TAILQ_ENTRY(linear_bdev) state_link;

	/* link of linear bdev to link it to global linear bdev list */
	TAILQ_ENTRY(linear_bdev) global_link;

	/* pointer to config file entry */
	struct linear_bdev_config *config;

	/* array of base bdev info */
	struct linear_base_bdev_info *base_bdev_info;

	/* state of linear bdev */
	enum linear_bdev_state state;

	/*
	 * offset of each base devices
	 * e.g., there are 3 base devices, their sizes are 10G, 15G, 20G
	 * then offsets[0] = 0, offsets[1] = 10G, offsets[2] = 25G
	 */
	uint64_t *offsets;

	/* number of base bdevs comprising linear bdev */
	uint8_t num_base_bdevs;

	/* number of base bdevs discovered */
	uint8_t num_base_bdevs_discovered;

	/* Set to true if desctruct is called for this linear bdev */
	bool destruct_called;

	/* Set to true if destroy of this linear bdev is started. */
	bool destroy_started;
};

/*
 * linear_base_bdev_config is the per base bdev data structure which contains
 * information w.r.t to per base bdev during parsing config
 */
struct linear_base_bdev_config {
	/* base bdev name from config file */
	char *name;
};

/*
 * linear_bdev_config contains the linear bdev config related information after
 * parsing the config file
 */
struct linear_bdev_config {
	/* base bdev config per underlying bdev */
	struct linear_base_bdev_config *base_bdev;

	/* Points to already created linear bdev */
	struct linear_bdev *linear_bdev;

	char *name;

	/* number of base bdevs */
	uint8_t num_base_bdevs;

	TAILQ_ENTRY(linear_bdev_config) link;
};

/*
 * linear_config is the top level structure representing the linear bdev config as read
 * from config file for all linears
 */
struct linear_config {
	/* linear bdev context from config file */
	TAILQ_HEAD(, linear_bdev_config) linear_bdev_config_head;

	/* total linear bdev from config file */
	uint8_t total_linear_bdev;
};

/*
 * linear_bdev_io_channel is the context of spdk_io_channel for linear bdev device. It
 * contains the relationship of linear bdev io channel with base bdev io channels.
 */
struct linear_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel **base_channel;

	/* Number of IO channels */
	uint8_t num_channels;
};

/* TAIL heads for various linear bdev lists */
TAILQ_HEAD(linear_configured_tailq, linear_bdev);
TAILQ_HEAD(linear_configuring_tailq, linear_bdev);
TAILQ_HEAD(linear_all_tailq, linear_bdev);
TAILQ_HEAD(linear_offline_tailq, linear_bdev);

extern struct linear_configured_tailq g_linear_bdev_configured_list;
extern struct linear_configuring_tailq g_linear_bdev_configuring_list;
extern struct linear_all_tailq g_linear_bdev_list;
extern struct linear_offline_tailq g_linear_bdev_offline_list;
extern struct linear_config g_linear_config;

typedef void (*linear_bdev_destruct_cb)(void *cb_ctx, int rc);

int linear_bdev_create(struct linear_bdev_config *linear_cfg);
int linear_bdev_add_base_devices(struct linear_bdev_config *linear_cfg);
void linear_bdev_remove_base_devices(struct linear_bdev_config *linear_cfg,
				     linear_bdev_destruct_cb cb_fn, void *cb_ctx);
int linear_bdev_config_add(const char *linear_name, uint8_t num_base_bdevs,
			   struct linear_bdev_config **_linear_cfg);
int linear_bdev_config_add_base_bdev(struct linear_bdev_config *linear_cfg,
				     const char *base_bdev_name, uint8_t slot);
void linear_bdev_config_cleanup(struct linear_bdev_config *linear_cfg);
struct linear_bdev_config *linear_bdev_config_find_by_name(const char *linear_name);

#endif  /* SPDK_BDEV_LINEAR_INTERNAL_H */
