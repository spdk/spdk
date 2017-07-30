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
 * SCSI to bdev translation layer
 */

#ifndef SPDK_SCSI_H
#define SPDK_SCSI_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"

/* Defines for SPDK tracing framework */
#define OWNER_SCSI_DEV				0x10
#define OBJECT_SCSI_TASK			0x10
#define TRACE_GROUP_SCSI			0x2
#define TRACE_SCSI_TASK_DONE	SPDK_TPOINT_ID(TRACE_GROUP_SCSI, 0x0)
#define TRACE_SCSI_TASK_START	SPDK_TPOINT_ID(TRACE_GROUP_SCSI, 0x1)

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

struct spdk_scsi_task;
typedef void (*spdk_scsi_task_cpl)(struct spdk_scsi_task *task);
typedef void (*spdk_scsi_task_free)(struct spdk_scsi_task *task);

struct spdk_scsi_task {
	uint8_t				status;
	uint8_t				function; /* task mgmt function */
	uint8_t				response; /* task mgmt response */
	/**
	 * Record the lun id just in case the lun is invalid,
	 * which will happen when hot remove the lun.
	 */
	int				lun_id;
	struct spdk_scsi_lun		*lun;
	struct spdk_bdev_desc		*desc;
	struct spdk_io_channel		*ch;
	struct spdk_scsi_port		*target_port;
	struct spdk_scsi_port		*initiator_port;

	spdk_scsi_task_cpl		cpl_fn;
	spdk_scsi_task_free		free_fn;

	uint32_t ref;
	uint32_t transfer_len;
	uint32_t dxfer_dir;
	uint32_t length;

	/**
	 * Amount of data actually transferred.  Can be less than requested
	 *  transfer_len - i.e. SCSI INQUIRY.
	 */
	uint32_t data_transferred;

	uint64_t offset;
	struct spdk_scsi_task *parent;

	uint8_t *cdb;

	/**
	 * \internal
	 * Size of internal buffer or zero when iov.iov_base is not internally managed.
	 */
	uint32_t alloc_len;
	/**
	 * \internal
	 * iov is internal buffer. Use iovs to access elements of IO.
	 */
	struct iovec iov;
	struct iovec *iovs;
	uint16_t iovcnt;

	uint8_t sense_data[32];
	size_t sense_data_len;

	void *bdev_io;

	TAILQ_ENTRY(spdk_scsi_task) scsi_link;

	uint32_t abort_id;
};

struct spdk_scsi_port;

struct spdk_scsi_dev;

/**
 * \brief Represents a SCSI LUN.
 *
 * LUN modules will implement the function pointers specifically for the LUN
 * type.  For example, NVMe LUNs will implement scsi_execute to translate
 * the SCSI task to an NVMe command and post it to the NVMe controller.
 * malloc LUNs will implement scsi_execute to translate the SCSI task and
 * copy the task's data into or out of the allocated memory buffer.
 */
struct spdk_scsi_lun;

int spdk_scsi_init(void);

int spdk_scsi_fini(void);

int spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun);
const char *spdk_scsi_lun_get_name(const struct spdk_scsi_lun *lun);
const struct spdk_scsi_dev *spdk_scsi_lun_get_dev(const struct spdk_scsi_lun *lun);

const char *spdk_scsi_dev_get_name(const struct spdk_scsi_dev *dev);
int spdk_scsi_dev_get_id(const struct spdk_scsi_dev *dev);
struct spdk_scsi_lun *spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id);
bool spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev);
void spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev);
void spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task,
				   enum spdk_scsi_task_func func);
void spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);
int spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name);
struct spdk_scsi_port *spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id);
void spdk_scsi_dev_print(struct spdk_scsi_dev *dev);
int spdk_scsi_dev_allocate_io_channels(struct spdk_scsi_dev *dev);
void spdk_scsi_dev_free_io_channels(struct spdk_scsi_dev *dev);

/**
 * \brief Constructs a SCSI device object using the given parameters.
 *
 * \param name Name for the SCSI device.
 * \param queue_depth Queue depth for the SCSI device.  This queue depth is
 * 		      a combined queue depth for all LUNs in the device.
 * \param lun_list List of LUN objects for the SCSI device.  Caller is
 * 		   responsible for managing the memory containing this list.
 * \param lun_id_list List of LUN IDs for the LUN in this SCSI device.  Caller is
 *		      responsible for managing the memory containing this list.
 *		      lun_id_list[x] is the LUN ID for lun_list[x].
 * \param num_luns Number of entries in lun_list and lun_id_list.
 * \param hotremove_cb Callback to lun hotremoval. Will be called
 * 		       once hotremove is first triggered.
 * \param hotremove_ctx Additional argument to hotremove_cb
 * \return The constructed spdk_scsi_dev object.
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct(const char *name,
		char *lun_name_list[],
		int *lun_id_list,
		int num_luns,
		uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);

void spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev, struct spdk_scsi_lun *lun);


struct spdk_scsi_port *spdk_scsi_port_create(uint64_t id, uint16_t index, const char *name);
void spdk_scsi_port_free(struct spdk_scsi_port **pport);
const char *spdk_scsi_port_get_name(const struct spdk_scsi_port *port);


void spdk_scsi_task_construct(struct spdk_scsi_task *task,
			      spdk_scsi_task_cpl cpl_fn,
			      spdk_scsi_task_free free_fn,
			      struct spdk_scsi_task *parent);
void spdk_scsi_task_put(struct spdk_scsi_task *task);

void spdk_scsi_task_free_data(struct spdk_scsi_task *task);
/**
 * Set internal buffer to given one. Caller is owner of that buffer.
 *
 * \param task Task struct
 * \param data Pointer to buffer
 * \param len Buffer length
 */
void spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len);

/**
 * Allocate internal buffer of requested size. Caller is not owner of
 * returned buffer and must not free it. Caller is permitted to call
 * spdk_scsi_task_free_data() to free internal buffer if it is not required
 * anymore, but must assert that task is done and not used by library.
 *
 * Allocated buffer is stored in iov field of task object.
 *
 * \param task Task struct
 * \param alloc_len Size of allocated buffer.
 * \return Pointer to buffer or NULL on error.
 */
void *spdk_scsi_task_alloc_data(struct spdk_scsi_task *task, uint32_t alloc_len);

int spdk_scsi_task_scatter_data(struct spdk_scsi_task *task, const void *src, size_t len);
void *spdk_scsi_task_gather_data(struct spdk_scsi_task *task, int *len);
void spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc,
				     int ascq);
void spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk, int asc,
			       int ascq);
void spdk_scsi_task_process_null_lun(struct spdk_scsi_task *task);

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
