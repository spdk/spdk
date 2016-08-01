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

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/scsi.h"
#include "spdk/scsi_spec.h"
#include "spdk/trace.h"

enum {
	SPDK_SCSI_TASK_UNKNOWN = -1,
	SPDK_SCSI_TASK_COMPLETE,
	SPDK_SCSI_TASK_PENDING,
};

/*
 * SAM does not define the value for these service responses.  Each transport
 *  (i.e. SAS, FC, iSCSI) will map these value to transport-specific codes,
 *  and may add their own.
 */
enum spdk_scsi_task_mgmt_resp {
	SPDK_SCSI_TASK_MGMT_RESP_COMPLETE,
	SPDK_SCSI_TASK_MGMT_RESP_SUCCESS,
	SPDK_SCSI_TASK_MGMT_RESP_REJECT,
	SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN,
	SPDK_SCSI_TASK_MGMT_RESP_TARGET_FAILURE,
	SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED
};

#define OWNER_SCSI_DEV		0x10

#define OBJECT_SCSI_TASK	0x10

#define TRACE_GROUP_SCSI	0x2
#define TRACE_SCSI_TASK_DONE	SPDK_TPOINT_ID(TRACE_GROUP_SCSI, 0x0)
#define TRACE_SCSI_TASK_START	SPDK_TPOINT_ID(TRACE_GROUP_SCSI, 0x1)

/**

\brief Represents a SCSI LUN.

LUN modules will implement the function pointers specifically for the LUN
type.  For example, NVMe LUNs will implement scsi_execute to translate
the SCSI task to an NVMe command and post it to the NVMe controller.
malloc LUNs will implement scsi_execute to translate the SCSI task and
copy the task's data into or out of the allocated memory buffer.

*/
struct spdk_scsi_lun {
	/** LUN id for this logical unit. */
	int id;

	/** Pointer to the SCSI device containing this LUN. */
	struct spdk_scsi_dev *dev;

	/** The blockdev associated with this LUN. */
	struct spdk_bdev *bdev;

	/** Name for this LUN. */
	char name[SPDK_SCSI_LUN_MAX_NAME_LENGTH];

	TAILQ_HEAD(tasks, spdk_scsi_task) tasks;			/* submitted tasks */
	TAILQ_HEAD(pending_tasks, spdk_scsi_task) pending_tasks;	/* pending tasks */
};

struct spdk_lun_db_entry {
	struct spdk_scsi_lun *lun;
	int claimed;
	struct spdk_lun_db_entry *next;
};

extern struct spdk_lun_db_entry *spdk_scsi_lun_list_head;

/* This typedef exists to work around an astyle 2.05 bug.
 * Remove it when astyle is fixed.
 */
typedef struct spdk_scsi_lun _spdk_scsi_lun;

_spdk_scsi_lun *spdk_scsi_lun_construct(const char *name, struct spdk_bdev *bdev);

void spdk_scsi_lun_clear_all(struct spdk_scsi_lun *lun);
void spdk_scsi_lun_append_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
void spdk_scsi_lun_execute_tasks(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_task_mgmt_execute(struct spdk_scsi_task *task);
void spdk_scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
int spdk_scsi_lun_claim(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_unclaim(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_deletable(const char *name);
void spdk_scsi_lun_delete(const char *lun_name);

int spdk_scsi_lun_db_add(struct spdk_scsi_lun *lun);
int spdk_scsi_lun_db_delete(struct spdk_scsi_lun *lun);

struct spdk_scsi_lun *spdk_lun_db_get_lun(const char *lun_name, int claim_flag);
void spdk_lun_db_put_lun(const char *lun_name);

struct spdk_scsi_dev *spdk_scsi_dev_get_list(void);

int spdk_bdev_scsi_execute(struct spdk_bdev *bdev, struct spdk_scsi_task *task);
int spdk_bdev_scsi_reset(struct spdk_bdev *bdev, struct spdk_scsi_task *task);

static inline uint16_t
from_be16(void *ptr)
{
	uint8_t *tmp = (uint8_t *)ptr;
	return (((uint16_t)tmp[0] << 8) | tmp[1]);
}

static inline void
to_be16(void *out, uint16_t in)
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[0] = (in >> 8) & 0xFF;
	tmp[1] = in & 0xFF;
}

static inline uint32_t
from_be32(void *ptr)
{
	uint8_t *tmp = (uint8_t *)ptr;
	return (((uint32_t)tmp[0] << 24) |
		((uint32_t)tmp[1] << 16) |
		((uint32_t)tmp[2] << 8) |
		((uint32_t)tmp[3]));
}

static inline void
to_be32(void *out, uint32_t in)
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[0] = (in >> 24) & 0xFF;
	tmp[1] = (in >> 16) & 0xFF;
	tmp[2] = (in >> 8) & 0xFF;
	tmp[3] = in & 0xFF;
}

static inline uint64_t
from_be64(void *ptr)
{
	uint8_t *tmp = (uint8_t *)ptr;
	return (((uint64_t)tmp[0] << 56) |
		((uint64_t)tmp[1] << 48) |
		((uint64_t)tmp[2] << 40) |
		((uint64_t)tmp[3] << 32) |
		((uint64_t)tmp[4] << 24) |
		((uint64_t)tmp[5] << 16) |
		((uint64_t)tmp[6] << 8) |
		((uint64_t)tmp[7]));
}

static inline void
to_be64(void *out, uint64_t in)
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[0] = (in >> 56) & 0xFF;
	tmp[1] = (in >> 48) & 0xFF;
	tmp[2] = (in >> 40) & 0xFF;
	tmp[3] = (in >> 32) & 0xFF;
	tmp[4] = (in >> 24) & 0xFF;
	tmp[5] = (in >> 16) & 0xFF;
	tmp[6] = (in >> 8) & 0xFF;
	tmp[7] = in & 0xFF;
};

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
