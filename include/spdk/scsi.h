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

/**
 * \file
 * SCSI to blockdev translation layer
 */

#ifndef SPDK_SCSI_H
#define SPDK_SCSI_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

#include "spdk/queue.h"
#include "spdk/event.h"

#define SPDK_SCSI_MAX_DEVS			1024
#define SPDK_SCSI_DEV_MAX_LUN			64
#define SPDK_SCSI_DEV_MAX_PORTS			4
#define SPDK_SCSI_DEV_MAX_NAME			255

#define SPDK_SCSI_PORT_MAX_NAME_LENGTH		255

#define SPDK_SCSI_LUN_MAX_NAME_LENGTH		16

enum spdk_scsi_data_dir {
	SPDK_SCSI_DIR_NONE = 0,
	SPDK_SCSI_DIR_TO_DEV = 1,
	SPDK_SCSI_DIR_FROM_DEV = 2,
};

enum spdk_scsi_task_func {
	SPDK_SCSI_TASK_FUNC_ABORT_TASK = 0,
	SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET,
	SPDK_SCSI_TASK_FUNC_CLEAR_TASK_SET,
	SPDK_SCSI_TASK_FUNC_LUN_RESET,
};

enum spdk_scsi_task_type {
	SPDK_SCSI_TASK_TYPE_CMD = 0,
	SPDK_SCSI_TASK_TYPE_MANAGE,
};

struct spdk_scsi_task {
	uint8_t				type;
	uint8_t				status;
	uint8_t				function; /* task mgmt function */
	uint8_t				response; /* task mgmt response */
	struct spdk_scsi_lun		*lun;
	struct spdk_scsi_port		*target_port;
	struct spdk_scsi_port		*initiator_port;
	spdk_event_t			cb_event;

	uint32_t ref;
	uint32_t id;
	uint32_t transfer_len;
	uint32_t data_out_cnt;
	uint32_t dxfer_dir;
	uint32_t desired_data_transfer_length;
	/* Only valid for Read/Write */
	uint32_t bytes_completed;
	uint32_t length;

	/**
	 * Amount of data actually transferred.  Can be less than requested
	 *  transfer_len - i.e. SCSI INQUIRY.
	 */
	uint32_t data_transferred;
	uint32_t alloc_len;

	uint64_t offset;
	struct iovec iov;
	struct spdk_scsi_task *parent;

	void (*free_fn)(struct spdk_scsi_task *);

	uint8_t *cdb;
	uint8_t *iobuf;

	uint8_t sense_data[32];
	size_t sense_data_len;

	uint8_t *rbuf; /* read buffer */
	void *blockdev_io;

	TAILQ_ENTRY(spdk_scsi_task) scsi_link;

	/*
	 * Pointer to scsi task owner's outstanding
	 * task counter. Inc/Dec by get/put task functions.
	 * Note: in the future, we could consider replacing this
	 * with an owner-provided task management fuction that
	 * could perform protocol specific task mangement
	 * operations (such as tracking outstanding tasks).
	 */
	uint32_t *owner_task_ctr;

	uint32_t abort_id;
	TAILQ_HEAD(subtask_list, spdk_scsi_task) subtask_list;
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

void spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev);
void spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);
void spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);
int spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name);
struct spdk_scsi_port *spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id);
void spdk_scsi_dev_print(struct spdk_scsi_dev *dev);

/**

\brief Constructs a SCSI device object using the given parameters.

\param name Name for the SCSI device.
\param queue_depth Queue depth for the SCSI device.  This queue depth is
		   a combined queue depth for all LUNs in the device.
\param lun_list List of LUN objects for the SCSI device.  Caller is
		responsible for managing the memory containing this list.
\param lun_id_list List of LUN IDs for the LUN in this SCSI device.  Caller is
		   responsible for managing the memory containing this list.
		   lun_id_list[x] is the LUN ID for lun_list[x].
\param num_luns Number of entries in lun_list and lun_id_list.
\return The constructed spdk_scsi_dev object.

*/
struct spdk_scsi_dev *spdk_scsi_dev_construct(const char *name,
		char *lun_name_list[],
		int *lun_id_list,
		int num_luns);

void spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev, struct spdk_scsi_lun *lun);


int spdk_scsi_port_construct(struct spdk_scsi_port *port, uint64_t id,
			     uint16_t index, const char *name);


void spdk_scsi_task_construct(struct spdk_scsi_task *task, uint32_t *owner_task_ctr,
			      struct spdk_scsi_task *parent);
void spdk_put_task(struct spdk_scsi_task *task);
void spdk_scsi_task_alloc_data(struct spdk_scsi_task *task, uint32_t alloc_len,
			       uint8_t **data);
int spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc,
				    int ascq);
void spdk_scsi_task_set_check_condition(struct spdk_scsi_task *task, int sk,
					int asc, int ascq);

static inline struct spdk_scsi_task *
spdk_scsi_task_get_primary(struct spdk_scsi_task *task)
{
	if (task->parent) {
		return task->parent;
	} else {
		return task;
	}
}

#endif /* SPDK_SCSI_H */
