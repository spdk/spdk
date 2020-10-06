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
#include "spdk/dif.h"

#include "spdk/log.h"

enum {
	SPDK_SCSI_TASK_UNKNOWN = -1,
	SPDK_SCSI_TASK_COMPLETE,
	SPDK_SCSI_TASK_PENDING,
};

struct spdk_scsi_port {
	uint8_t			is_used;
	uint64_t		id;
	uint16_t		index;
	uint16_t		transport_id_len;
	char			transport_id[SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH];
	char			name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
};

/* Registrant with I_T nextus */
struct spdk_scsi_pr_registrant {
	uint64_t				rkey;
	uint16_t				relative_target_port_id;
	uint16_t				transport_id_len;
	char					transport_id[SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH];
	char					initiator_port_name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
	char					target_port_name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
	struct spdk_scsi_port			*initiator_port;
	struct spdk_scsi_port			*target_port;
	TAILQ_ENTRY(spdk_scsi_pr_registrant)	link;
};

#define SCSI_SPC2_RESERVE			0x00000001U

/* Reservation with LU_SCOPE */
struct spdk_scsi_pr_reservation {
	uint32_t				flags;
	struct spdk_scsi_pr_registrant		*holder;
	enum spdk_scsi_pr_type_code		rtype;
	uint64_t				crkey;
};

struct spdk_scsi_dev {
	int					id;
	int					is_allocated;
	bool					removed;
	spdk_scsi_dev_destruct_cb_t		remove_cb;
	void					*remove_ctx;

	char					name[SPDK_SCSI_DEV_MAX_NAME + 1];

	struct spdk_scsi_lun			*lun[SPDK_SCSI_DEV_MAX_LUN];

	int					num_ports;
	struct spdk_scsi_port			port[SPDK_SCSI_DEV_MAX_PORTS];

	uint8_t					protocol_id;
};

struct spdk_scsi_lun_desc {
	struct spdk_scsi_lun		*lun;
	spdk_scsi_lun_remove_cb_t	hotremove_cb;
	void				*hotremove_ctx;
	TAILQ_ENTRY(spdk_scsi_lun_desc)	link;
};

struct spdk_scsi_lun {
	/** LUN id for this logical unit. */
	int id;

	/** Pointer to the SCSI device containing this LUN. */
	struct spdk_scsi_dev *dev;

	/** The bdev associated with this LUN. */
	struct spdk_bdev *bdev;

	/** Descriptor for opened block device. */
	struct spdk_bdev_desc *bdev_desc;

	/** The thread which opens this LUN. */
	struct spdk_thread *thread;

	/** I/O channel for the bdev associated with this LUN. */
	struct spdk_io_channel *io_channel;

	/**  The reference number for this LUN, thus we can correctly free the io_channel */
	uint32_t ref;

	/** Poller to release the resource of the lun when it is hot removed */
	struct spdk_poller *hotremove_poller;

	/** The LUN is removed */
	bool removed;

	/** Callback to be fired when LUN removal is first triggered. */
	void (*hotremove_cb)(const struct spdk_scsi_lun *lun, void *arg);

	/** Argument for hotremove_cb */
	void *hotremove_ctx;

	/** Callback to be fired when the bdev size of related LUN has changed. */
	void (*resize_cb)(const struct spdk_scsi_lun *, void *);

	/** Argument for resize_cb */
	void *resize_ctx;

	/** Registrant head for I_T nexus */
	TAILQ_HEAD(, spdk_scsi_pr_registrant) reg_head;
	/** Persistent Reservation Generation */
	uint32_t pr_generation;
	/** Reservation for the LUN */
	struct spdk_scsi_pr_reservation reservation;
	/** Reservation holder for SPC2 RESERVE(6) and RESERVE(10) */
	struct spdk_scsi_pr_registrant scsi2_holder;

	/** List of open descriptors for this LUN. */
	TAILQ_HEAD(, spdk_scsi_lun_desc) open_descs;

	/** submitted tasks */
	TAILQ_HEAD(tasks, spdk_scsi_task) tasks;

	/** pending tasks */
	TAILQ_HEAD(pending_tasks, spdk_scsi_task) pending_tasks;

	/** submitted management tasks */
	TAILQ_HEAD(mgmt_tasks, spdk_scsi_task) mgmt_tasks;

	/** pending management tasks */
	TAILQ_HEAD(pending_mgmt_tasks, spdk_scsi_task) pending_mgmt_tasks;

	/** poller to check completion of tasks prior to reset */
	struct spdk_poller *reset_poller;
};

struct spdk_scsi_lun *scsi_lun_construct(const char *bdev_name,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);
void scsi_lun_destruct(struct spdk_scsi_lun *lun);

void scsi_lun_execute_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
void scsi_lun_execute_mgmt_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
bool scsi_lun_has_pending_mgmt_tasks(const struct spdk_scsi_lun *lun,
				     const struct spdk_scsi_port *initiator_port);
void scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
void scsi_lun_complete_reset_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
bool scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun,
				const struct spdk_scsi_port *initiator_port);
int scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun);
void scsi_lun_free_io_channel(struct spdk_scsi_lun *lun);

struct spdk_scsi_dev *scsi_dev_get_list(void);

int scsi_port_construct(struct spdk_scsi_port *port, uint64_t id,
			uint16_t index, const char *name);
void scsi_port_destruct(struct spdk_scsi_port *port);

int bdev_scsi_execute(struct spdk_scsi_task *task);
void bdev_scsi_reset(struct spdk_scsi_task *task);

bool bdev_scsi_get_dif_ctx(struct spdk_bdev *bdev, struct spdk_scsi_task *task,
			   struct spdk_dif_ctx *dif_ctx);

int scsi_pr_out(struct spdk_scsi_task *task, uint8_t *cdb, uint8_t *data, uint16_t data_len);
int scsi_pr_in(struct spdk_scsi_task *task, uint8_t *cdb, uint8_t *data, uint16_t data_len);
int scsi_pr_check(struct spdk_scsi_task *task);

int scsi2_reserve(struct spdk_scsi_task *task, uint8_t *cdb);
int scsi2_release(struct spdk_scsi_task *task);
int scsi2_reserve_check(struct spdk_scsi_task *task);

struct spdk_scsi_globals {
	pthread_mutex_t mutex;
};

extern struct spdk_scsi_globals g_scsi;

#endif /* SPDK_SCSI_INTERNAL_H */
