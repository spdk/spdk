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

#include "spdk/bdev.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

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
#define SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH	255

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

	struct spdk_scsi_lun		*lun;
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
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

struct spdk_scsi_port;
struct spdk_scsi_dev;
struct spdk_scsi_lun;
struct spdk_scsi_lun_desc;

typedef void (*spdk_scsi_lun_remove_cb_t)(struct spdk_scsi_lun *, void *);
typedef void (*spdk_scsi_dev_destruct_cb_t)(void *cb_arg, int rc);

/**
 * Initialize SCSI layer.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_scsi_init(void);

/**
 * Stop and clean the SCSI layer.
 */
void spdk_scsi_fini(void);

/**
 * Get the LUN id of the given logical unit.
 *
 * \param lun Logical unit.
 *
 * \return LUN id of the logical unit.
 */
int spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun);

/**
 * Get the name of the bdev associated with the given logical unit.
 *
 * \param lun Logical unit.
 *
 * \return the name of the bdev associated with the logical unit.
 */
const char *spdk_scsi_lun_get_bdev_name(const struct spdk_scsi_lun *lun);

/**
 * Get the SCSI device associated with the given logical unit.
 *
 * \param lun Logical unit.
 *
 * \return the SCSI device associated with the logical unit.
 */
const struct spdk_scsi_dev *spdk_scsi_lun_get_dev(const struct spdk_scsi_lun *lun);

/**
 * Check if the logical unit is hot removing.
 *
 * \param lun Logical unit
 *
 * \return true if removing, false otherwise.
 */
bool spdk_scsi_lun_is_removing(const struct spdk_scsi_lun *lun);

/**
 * Get the name of the given SCSI device.
 *
 * \param dev SCSI device.
 *
 * \return the name of the SCSI device on success, or NULL on failure.
 */
const char *spdk_scsi_dev_get_name(const struct spdk_scsi_dev *dev);

/**
 * Get the id of the given SCSI device.
 *
 * \param dev SCSI device.
 *
 * \return the id of the SCSI device.
 */
int spdk_scsi_dev_get_id(const struct spdk_scsi_dev *dev);

/**
 * Get the logical unit of the given SCSI device whose id is lun_id.
 *
 * \param dev SCSI device.
 * \param lun_id Id of the logical unit.
 *
 * \return the logical unit on success, or NULL on failure.
 */
struct spdk_scsi_lun *spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id);

/**
 * Check whether the SCSI device has any pending task.
 *
 * \param dev SCSI device.
 * \param initiator_port Check tasks only from the initiator if specified, or
 * all all tasks otherwise.
 *
 * \return true if the SCSI device has any pending task, or false otherwise.
 */
bool spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev,
				     const struct spdk_scsi_port *initiator_port);

/**
 * Destruct the SCSI decice.
 *
 * \param dev SCSI device.
 * \param cb_fn Callback function.
 * \param cb_arg Argument to callback function.
 */
void spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev,
			    spdk_scsi_dev_destruct_cb_t cb_fn, void *cb_arg);

/**
 * Execute the SCSI management task.
 *
 * The task can be constructed by the function spdk_scsi_task_construct().
 * Code of task management function to be executed is set before calling this API.
 *
 * \param dev SCSI device.
 * \param task SCSI task to be executed.
 */
void spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);

/**
 * Execute the SCSI task.
 *
 * The task can be constructed by the function spdk_scsi_task_construct().
 *
 * \param dev SCSI device.
 * \param task Task to be executed.
 */
void spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);

/**
 * Add a new port to the given SCSI device.
 *
 * \param dev SCSI device.
 * \param id Port id.
 * \param name Port name.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name);

/**
 * Delete a specified port of the given SCSI device.
 *
 * \param dev SCSI device.
 * \param id Port id.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_scsi_dev_delete_port(struct spdk_scsi_dev *dev, uint64_t id);

/**
 * Get the port of the given SCSI device whose port ID is id.
 *
 * \param dev SCSI device.
 * \param id Port id.
 *
 * \return the port of the SCSI device on success, or NULL on failure.
 */
struct spdk_scsi_port *spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id);

/**
 * Allocate I/O channels for all LUNs of the given SCSI device.
 *
 * \param dev SCSI device.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_scsi_dev_allocate_io_channels(struct spdk_scsi_dev *dev);

/**
 * Free I/O channels from all LUNs of the given SCSI device.
 */
void spdk_scsi_dev_free_io_channels(struct spdk_scsi_dev *dev);

/**
 * Construct a SCSI device object using the given parameters.
 *
 * \param name Name for the SCSI device.
 * \param bdev_name_list List of bdev names to attach to the LUNs for this SCSI
 * device.
 * \param lun_id_list List of LUN IDs for the LUN in this SCSI device. Caller is
 * responsible for managing the memory containing this list. lun_id_list[x] is
 * the LUN ID for lun_list[x].
 * \param num_luns Number of entries in lun_list and lun_id_list.
 * \param protocol_id SCSI SPC protocol identifier to report in INQUIRY data
 * \param hotremove_cb Callback to lun hotremoval. Will be called once hotremove
 * is first triggered.
 * \param hotremove_ctx Additional argument to hotremove_cb.
 *
 * \return the constructed spdk_scsi_dev object.
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct(const char *name,
		const char *bdev_name_list[],
		int *lun_id_list,
		int num_luns,
		uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);

/**
 * Construct a SCSI device object using the more given parameters.
 *
 * \param name Name for the SCSI device.
 * \param bdev_name_list List of bdev names to attach to the LUNs for this SCSI
 * device.
 * \param lun_id_list List of LUN IDs for the LUN in this SCSI device. Caller is
 * responsible for managing the memory containing this list. lun_id_list[x] is
 * the LUN ID for lun_list[x].
 * \param num_luns Number of entries in lun_list and lun_id_list.
 * \param protocol_id SCSI SPC protocol identifier to report in INQUIRY data
 * \param resize_cb Callback of lun resize.
 * \param resize_ctx Additional argument to resize_cb.
 * \param hotremove_cb Callback to lun hotremoval. Will be called once hotremove
 * is first triggered.
 * \param hotremove_ctx Additional argument to hotremove_cb.
 *
 * \return the constructed spdk_scsi_dev object.
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct_ext(const char *name,
		const char *bdev_name_list[],
		int *lun_id_list,
		int num_luns,
		uint8_t protocol_id,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);

/**
 * Delete a logical unit of the given SCSI device.
 *
 * \param dev SCSI device.
 * \param lun Logical unit to delete.
 */
void spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev, struct spdk_scsi_lun *lun);

/**
 * Add a new logical unit to the given SCSI device.
 *
 * \param dev SCSI device.
 * \param bdev_name Name of the bdev attached to the logical unit.
 * \param lun_id LUN id for the new logical unit.
 * \param hotremove_cb Callback to lun hotremoval. Will be called once hotremove
 * is first triggered.
 * \param hotremove_ctx Additional argument to hotremove_cb.
 */
int spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
			  void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			  void *hotremove_ctx);

/**
 * Add a new logical unit to the given SCSI device with more callbacks.
 *
 * \param dev SCSI device.
 * \param bdev_name Name of the bdev attached to the logical unit.
 * \param lun_id LUN id for the new logical unit.
 * \param resize_cb Callback of lun resize.
 * \param resize_ctx Additional argument to resize_cb.
 * \param hotremove_cb Callback to lun hotremoval. Will be called once hotremove
 * is first triggered.
 * \param hotremove_ctx Additional argument to hotremove_cb.
 */
int spdk_scsi_dev_add_lun_ext(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
			      void (*resize_cb)(const struct spdk_scsi_lun *, void *),
			      void *resize_ctx,
			      void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			      void *hotremove_ctx);

/**
 * Create a new SCSI port.
 *
 * \param id Port id.
 * \param index Port index.
 * \param name Port Name.
 *
 * \return a pointer to the created SCSI port on success, or NULL on failure.
 */
struct spdk_scsi_port *spdk_scsi_port_create(uint64_t id, uint16_t index, const char *name);

/**
 * Free the SCSI port.
 *
 * \param pport SCSI port to free.
 */
void spdk_scsi_port_free(struct spdk_scsi_port **pport);

/**
 * Get the name of the SCSI port.
 *
 * \param port SCSI port to query.
 *
 * \return the name of the SCSI port.
 */
const char *spdk_scsi_port_get_name(const struct spdk_scsi_port *port);

/**
 * Construct a new SCSI task.
 *
 * \param task SCSI task to consturct.
 * \param cpl_fn Called when the task is completed.
 * \param free_fn Called when the task is freed
 */
void spdk_scsi_task_construct(struct spdk_scsi_task *task,
			      spdk_scsi_task_cpl cpl_fn,
			      spdk_scsi_task_free free_fn);

/**
 * Put the SCSI task.
 *
 * \param task SCSI task to put.
 */
void spdk_scsi_task_put(struct spdk_scsi_task *task);

/**
 * Set internal buffer to given one. Caller is owner of that buffer.
 *
 * \param task SCSI task.
 * \param data Pointer to buffer.
 * \param len Buffer length.
 */
void spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len);

/**
 * Single buffer -> vector of buffers.
 *
 * \param task SCSI task.
 * \param src A pointer to the data buffer read from.
 * \param len Length of the data buffer read from.
 *
 * \return the total length of the vector of buffers written into on success, or
 * -1 on failure.
 */
int spdk_scsi_task_scatter_data(struct spdk_scsi_task *task, const void *src, size_t len);

/**
 * Vector of buffers -> single buffer.
 *
 * \param task SCSI task,
 * \param len Length of the buffer allocated and written into.
 *
 * \return a pointer to the buffer allocated and written into.
 */
void *spdk_scsi_task_gather_data(struct spdk_scsi_task *task, int *len);

/**
 * Build sense data for the SCSI task.
 *
 * \param task SCSI task.
 * \param sk Sense key.
 * \param asc Additional sense code.
 * \param ascq Additional sense code qualifier.
 */
void spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc,
				     int ascq);

/**
 * Set SCSI status code to the SCSI task. When the status code is CHECK CONDITION,
 * sense data is build too.
 *
 * \param task SCSI task.
 * \param sc Sense code
 * \param sk Sense key.
 * \param asc Additional sense code.
 * \param ascq Additional sense code qualifier.
 */
void spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk, int asc,
			       int ascq);

/**
 * Copy SCSI status.
 *
 * \param dst SCSI task whose status is written to.
 * \param src SCSI task whose status is read from.
 */
void spdk_scsi_task_copy_status(struct spdk_scsi_task *dst, struct spdk_scsi_task *src);

/**
 * Process the SCSI task when no LUN is attached.
 *
 * \param task SCSI task.
 */
void spdk_scsi_task_process_null_lun(struct spdk_scsi_task *task);

/**
 * Process the aborted SCSI task.
 *
 * \param task SCSI task.
 */
void spdk_scsi_task_process_abort(struct spdk_scsi_task *task);

/**
 * Open a logical unit for I/O operations.
 *
 * The registered callback function must get all tasks from the upper layer
 *  (e.g. iSCSI) to the LUN done, free the IO channel of the LUN if allocated,
 *  and then close the LUN.
 *
 * \param lun Logical unit to open.
 * \param hotremove_cb Callback function for hot removal of the logical unit.
 * \param hotremove_ctx Param for hot removal callback function.
 * \param desc Output parameter for the descriptor when operation is successful.
 * \return 0 if operation is successful, suitable errno value otherwise
 */
int spdk_scsi_lun_open(struct spdk_scsi_lun *lun, spdk_scsi_lun_remove_cb_t hotremove_cb,
		       void *hotremove_ctx, struct spdk_scsi_lun_desc **desc);

/**
 * Close an opened logical unit.
 *
 * \param desc Descriptor of the logical unit.
 */
void spdk_scsi_lun_close(struct spdk_scsi_lun_desc *desc);

/**
 * Allocate I/O channel for the LUN
 *
 * \param desc Descriptor of the logical unit.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun_desc *desc);

/**
 * Free I/O channel from the logical unit
 *
 * \param desc Descriptor of the logical unit.
 */
void spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun_desc *desc);

/**
 * Get DIF context for SCSI LUN and SCSI command.
 *
 * \param lun Logical unit.
 * \param task SCSI task which has the payload.
 * \param dif_ctx Output parameter which will contain initialized DIF context.
 *
 * \return true on success or false otherwise.
 */
bool spdk_scsi_lun_get_dif_ctx(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task,
			       struct spdk_dif_ctx *dif_ctx);

/**
 * Set iSCSI Initiator port TransportID
 *
 * \param port SCSI initiator port.
 * \param iscsi_name Initiator name.
 * \param isid Session ID.
 */
void spdk_scsi_port_set_iscsi_transport_id(struct spdk_scsi_port *port,
		char *iscsi_name, uint64_t isid);

/**
 * Convert LUN ID from integer to LUN format
 *
 * \param lun_id Integer LUN ID
 *
 * \return LUN format of LUN ID
 */
uint64_t spdk_scsi_lun_id_int_to_fmt(int lun_id);

/**
 * Convert LUN ID from LUN format to integer
 *
 * \param fmt_lun LUN format of LUN ID
 *
 * \return integer LUN ID
 */
int spdk_scsi_lun_id_fmt_to_int(uint64_t fmt_lun);
#ifdef __cplusplus
}
#endif

#endif /* SPDK_SCSI_H */
