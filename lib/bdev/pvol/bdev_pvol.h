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

#ifndef SPDK_BDEV_PVOL_INTERNAL_H
#define SPDK_BDEV_PVOL_INTERNAL_H

#include "spdk_internal/bdev.h"

/* Enable this for debugging */
/* #define PVOL_DEBUG */

/*
 * Pvol state describes the state of the pvol. This pvol can be either in
 * configured list or configuring list
 */
enum pvol_bdev_state {
	/* pvol is ready and is seen by upper layers */
	PVOL_BDEV_STATE_ONLINE = 0,

	/* pvol is configuring, not all underlying bdevs are present */
	PVOL_BDEV_STATE_CONFIGURING,

	/*
	 * In offline state, pvol layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	PVOL_BDEV_STATE_OFFLINE,

	/* pvol max, new states should be added before this */
	PVOL_BDEV_MAX,
};

/*
 * pvol_base_bdev_info contains information for the base bdevs which are part of some
 * pvol. This structure contains the per base bdev information. Whatever is
 * required per base device for pvol will be kept here
 */
struct pvol_base_bdev_info {
	/* pointer to base spdk bdev */
	struct spdk_bdev         *base_bdev;

	/* pointer to base bdev descriptor opened by pvol */
	struct spdk_bdev_desc    *base_bdev_desc;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool                     base_bdev_remove_scheduled;
};

/*
 * pvol_bdev contains the information related to any pvol either configured or
 * in configuring list
 */
struct pvol_bdev {
	/* link of pvol bdev to link it to configured, configuring or offline list */
	TAILQ_ENTRY(pvol_bdev)      link_specific_list;

	/* link of pvol bdev to link it to global pvol list */
	TAILQ_ENTRY(pvol_bdev)      link_global_list;

	/* pointer to config file entry */
	struct pvol_bdev_config     *pvol_bdev_config;

	/* array of base bdev info */
	struct pvol_base_bdev_info  *base_bdev_info;

	/* strip size of pvol bdev in blocks */
	uint32_t                    strip_size;

	/* strip size bit shift for optimized calculation */
	uint32_t                    strip_size_shift;

	/* block length bit shift for optimized calculation */
	uint32_t                    blocklen_shift;

	/* state of pvol bdev */
	enum pvol_bdev_state        state;

	/* number of base bdevs comprising pvol bdev */
	uint16_t                    num_base_bdevs;

	/* number of base bdevs discovered */
	uint16_t                    num_base_bdevs_discovered;

	/* Raid Level of this pvol */
	uint8_t                     raid_level;

	/* Set to true if destruct is called for this pvol */
	bool                        destruct_called;
};

/*
 * pvol_bdev_ctxt is the single entity structure for entire bdev which is
 * allocated for any pvol bdev
 */
struct pvol_bdev_ctxt {
	/* pvol bdev device, this will get registered in bdev layer */
	struct spdk_bdev         bdev;

	/* pvol_bdev object, io device will be created on this */
	struct pvol_bdev         pvol_bdev;
};

/*
 * pvol_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for a pooled bdev
 */
struct pvol_bdev_io {
	/* link for wait queue */
	TAILQ_ENTRY(pvol_bdev_io)   link;

	/* Original channel for this IO, used in queuing logic */
	struct spdk_io_channel      *ch;

	/* current buffer location, used in queueing logic */
	uint8_t                     *buf;

	/* outstanding child completions */
	uint16_t                    splits_comp_outstanding;

	/* pending splits yet to happen */
	uint16_t                    splits_pending;

	/* status of parent io */
	bool                        status;
};

/*
 * pvol_base_bdev_config is the per base bdev data structure which contains
 * information w.r.t to per base bdev during parsing config
 */
struct pvol_base_bdev_config {
	/* base bdev name from config file */
	char                        *bdev_name;
};

/*
 * pvol_bdev_config contains the pvol bdev config related information after
 * parsing the config file
 */
struct pvol_bdev_config {
	/* base bdev config per underlying bdev */
	struct pvol_base_bdev_config  *base_bdev;

	/* Points to already created pvol bdev */
	struct pvol_bdev_ctxt         *pvol_bdev_ctxt;

	char                          *name;

	/* strip size of this pvol bdev in kilo bytes */
	uint32_t                      strip_size;

	/* number of base bdevs */
	uint8_t                       num_base_bdevs;

	/* raid level */
	uint8_t                       raid_level;
};

/*
 * pvol_config is the top level structure representing the pvol config as read
 * from config file for all pvols
 */
struct pvol_config {
	/* pvol bdev context from config file */
	struct pvol_bdev_config *pvol_bdev_config;

	/* total pvol bdev from config file */
	uint8_t total_pvol_bdev;
};

/*
 * pvol_bdev_io_channel is the context of spdk_io_channel for pvol device. It
 * contains the relationship of pvol io channel with base bdev io channels.
 */
struct pvol_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel      **base_bdevs_io_channel;

	/* pvol bdev context pointer */
	struct pvol_bdev_ctxt       *pvol_bdev_ctxt;

#ifdef PVOL_DEBUG
	/*
	 * This is for io stats debugging. This will not be enabled in main line
	 * code
	 */
	uint64_t num_p_reads;
	uint64_t num_p_writes;
	uint64_t num_p_reads_completed;
	uint64_t num_p_writes_completed;
	uint64_t num_c_reads;
	uint64_t num_c_writes;
	uint64_t num_c_reads_completed;
	uint64_t num_c_writes_completed;
	uint64_t num_reads_no_split;
	uint64_t num_writes_no_split;
	uint64_t num_waitq_no_resource;
	uint64_t num_waitq_not_empty;
	uint64_t num_c_read_errors;
	uint64_t num_c_write_errors;
	uint64_t num_p_read_errors;
	uint64_t num_p_write_errors;
#endif
};

/*
 * pvol_bdev_io_waitq is the per core waitq which is used to queue parent
 * bdev_io which is scheduled to submit later due to any reason like resource
 * not available etc
 */
struct pvol_bdev_io_waitq {
	/* Wait queue when resources are not sufficient */
	TAILQ_HEAD(, pvol_bdev_io)  io_waitq;

	/* poller to process IOs in waitq */
	struct spdk_poller          *io_waitq_poller;
};

/* TAIL heads for various pvol lists */
TAILQ_HEAD(spdk_pvol_configured, pvol_bdev);
TAILQ_HEAD(spdk_pvol_configuring, pvol_bdev);
TAILQ_HEAD(spdk_pvol_all, pvol_bdev);
TAILQ_HEAD(spdk_pvol_offline, pvol_bdev);

extern struct spdk_pvol_configured    spdk_pvol_bdev_configured_list;
extern struct spdk_pvol_configuring   spdk_pvol_bdev_configuring_list;
extern struct spdk_pvol_all           spdk_pvol_bdev_list;
extern struct spdk_pvol_offline       spdk_pvol_bdev_offline_list;
extern struct pvol_config             spdk_pvol_config;


void pvol_bdev_remove_base_bdev(void *ctx);
int pvol_bdev_add_base_device(struct spdk_bdev *bdev);

#endif // SPDK_BDEV_PVOL_INTERNAL_H
