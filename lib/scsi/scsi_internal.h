/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#ifndef SPDK_SCSI_INTERNAL_H
#define SPDK_SCSI_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/scsi.h"
#include "spdk/scsi_spec.h"
#include "spdk/trace.h"

#include "spdk_internal/log.h"

enum {
	SPDK_SCSI_TASK_UNKNOWN = -1,
	SPDK_SCSI_TASK_COMPLETE,
	SPDK_SCSI_TASK_PENDING,
};

struct spdk_scsi_port {
	struct spdk_scsi_dev	*dev;
	uint64_t		id;
	uint16_t		index;
	char			name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
};

struct spdk_scsi_dev {
	int			id;
	int			is_allocated;

	char			name[SPDK_SCSI_DEV_MAX_NAME];

	int			maxlun;
	struct spdk_scsi_lun	*lun[SPDK_SCSI_DEV_MAX_LUN];

	int			num_ports;
	struct spdk_scsi_port	port[SPDK_SCSI_DEV_MAX_PORTS];
};

struct spdk_scsi_lun {
	/** LUN id for this logical unit. */
	int id;

	/** Pointer to the SCSI device containing this LUN. */
	struct spdk_scsi_dev *dev;

	/** The blockdev associated with this LUN. */
	struct spdk_bdev *bdev;

	/** I/O channel for the blockdev associated with this LUN. */
	struct spdk_io_channel *io_channel;

	/** Thread ID for the thread that allocated the I/O channel for this
	 *   LUN.  All I/O to this LUN must be performed from this thread.
	 */
	pthread_t thread_id;

	/**  The reference number for this LUN, thus we can correctly free the io_channel */
	uint32_t ref;

	/** Name for this LUN. */
	char name[SPDK_SCSI_LUN_MAX_NAME_LENGTH];

	/** Poller to release the resource of the lun when it is hot removed */
	struct spdk_poller *hotplug_poller;

	/** The core hotplug_poller is assigned */
	uint32_t			lcore;

	/** The LUN is removed */
	bool				removed;

	/** The LUN is clamed */
	bool claimed;

	TAILQ_HEAD(tasks, spdk_scsi_task) tasks;			/* submitted tasks */
	TAILQ_HEAD(pending_tasks, spdk_scsi_task) pending_tasks;	/* pending tasks */
};

struct spdk_lun_db_entry {
	struct spdk_scsi_lun *lun;
	struct spdk_lun_db_entry *next;
};

extern struct spdk_lun_db_entry *spdk_scsi_lun_list_head;

/* This typedef exists to work around an astyle 2.05 bug.
 * Remove it when astyle is fixed.
 */
typedef struct spdk_scsi_lun _spdk_scsi_lun;

_spdk_scsi_lun *spdk_scsi_lun_construct(const char *name, struct spdk_bdev *bdev);
int spdk_scsi_lun_destruct(struct spdk_scsi_lun *lun);

void spdk_scsi_lun_clear_all(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_append_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
void spdk_scsi_lun_execute_tasks(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_task_mgmt_execute(struct spdk_scsi_task *task);
void spdk_scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
int spdk_scsi_lun_claim(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_unclaim(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_delete(const char *lun_name);
int spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun);
void spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun *lun);

int spdk_scsi_lun_db_add(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_db_delete(struct spdk_scsi_lun *lun);

struct spdk_scsi_lun *spdk_lun_db_get_lun(const char *lun_name);

struct spdk_scsi_dev *spdk_scsi_dev_get_list(void);

int spdk_scsi_port_construct(struct spdk_scsi_port *port, uint64_t id,
			     uint16_t index, const char *name);

int spdk_bdev_scsi_execute(struct spdk_bdev *bdev, struct spdk_scsi_task *task);
int spdk_bdev_scsi_reset(struct spdk_bdev *bdev, struct spdk_scsi_task *task);

struct spdk_scsi_parameters {
	uint32_t max_unmap_lba_count;
	uint32_t max_unmap_block_descriptor_count;
	uint32_t optimal_unmap_granularity;
	uint32_t unmap_granularity_alignment;
	uint32_t ugavalid;
	uint64_t max_write_same_length;
};

struct spdk_scsi_globals {
	pthread_mutex_t mutex;
	struct spdk_scsi_parameters scsi_params;
};

extern struct spdk_scsi_globals g_spdk_scsi;

#endif /* SPDK_SCSI_INTERNAL_H */
