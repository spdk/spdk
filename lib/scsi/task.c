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

#include "scsi_internal.h"
#include "spdk/endian.h"
#include "spdk/env.h"

void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
	if (!task) {
		return;
	}

	task->ref--;

	if (task->ref == 0) {
		struct spdk_bdev_io *bdev_io = task->blockdev_io;

		if (task->parent) {
			spdk_scsi_task_put(task->parent);
			task->parent = NULL;
		}

		if (bdev_io) {
			/* due to lun reset, the bdev_io status could be pending */
			if (bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING) {
				bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
			}
			spdk_bdev_free_io(bdev_io);
		} else {
			spdk_free(task->rbuf);
		}

		task->rbuf = NULL;

		assert(task->owner_task_ctr != NULL);
		if (*(task->owner_task_ctr) > 0) {
			*(task->owner_task_ctr) -= 1;
		} else {
			SPDK_ERRLOG("task counter already 0\n");
		}

		task->free_fn(task);
	}
}

void
spdk_scsi_task_construct(struct spdk_scsi_task *task, uint32_t *owner_task_ctr,
			 struct spdk_scsi_task *parent)
{
	task->ref++;

	assert(owner_task_ctr != NULL);
	task->owner_task_ctr = owner_task_ctr;
	*owner_task_ctr += 1;

	/*
	 * Pre-fill the iov_buffers to point to the embedded iov
	 */
	task->iovs = &task->iov;
	task->iovcnt = 1;

	if (parent != NULL) {
		parent->ref++;
		task->parent = parent;
		task->type = parent->type;
		task->dxfer_dir = parent->dxfer_dir;
		task->transfer_len = parent->transfer_len;
		task->lun = parent->lun;
		task->cdb = parent->cdb;
		task->target_port = parent->target_port;
		task->initiator_port = parent->initiator_port;
		task->id = parent->id;
	}
}

void
spdk_scsi_task_alloc_data(struct spdk_scsi_task *task, uint32_t alloc_len,
			  uint8_t **data)
{
	/*
	 * SPDK iSCSI target depends on allocating at least 4096 bytes, even if
	 *  the command requested less.  The individual command code (for
	 *  example, INQUIRY) will fill out up to 4096 bytes of data, ignoring
	 *  the allocation length specified in the command.  After the individual
	 *  command functions are done, spdk_scsi_lun_execute_tasks() takes
	 *  care of only sending back the amount of data specified in the
	 *  allocation length.
	 */
	if (alloc_len < 4096) {
		alloc_len = 4096;
	}

	task->alloc_len = alloc_len;
	if (task->rbuf == NULL) {
		task->rbuf = spdk_zmalloc(alloc_len, 0, NULL);
	}
	*data = task->rbuf;
	memset(task->rbuf, 0, task->alloc_len);
}

void
spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc, int ascq)
{
	uint8_t *data;
	uint8_t *cp;
	int resp_code;

	data = task->sense_data;
	resp_code = 0x70; /* Current + Fixed format */

	/* SenseLength */
	memset(data, 0, 2);

	/* Sense Data */
	cp = &data[2];

	/* VALID(7) RESPONSE CODE(6-0) */
	cp[0] = 0x80 | resp_code;
	/* Obsolete */
	cp[1] = 0;
	/* FILEMARK(7) EOM(6) ILI(5) SENSE KEY(3-0) */
	cp[2] = sk & 0xf;
	/* INFORMATION */
	memset(&cp[3], 0, 4);

	/* ADDITIONAL SENSE LENGTH */
	cp[7] = 10;

	/* COMMAND-SPECIFIC INFORMATION */
	memset(&cp[8], 0, 4);
	/* ADDITIONAL SENSE CODE */
	cp[12] = asc;
	/* ADDITIONAL SENSE CODE QUALIFIER */
	cp[13] = ascq;
	/* FIELD REPLACEABLE UNIT CODE */
	cp[14] = 0;

	/* SKSV(7) SENSE KEY SPECIFIC(6-0,7-0,7-0) */
	cp[15] = 0;
	cp[16] = 0;
	cp[17] = 0;

	/* SenseLength */
	to_be16(data, 18);
	task->sense_data_len = 20;
}

void
spdk_scsi_task_set_check_condition(struct spdk_scsi_task *task, int sk, int asc, int ascq)
{
	spdk_scsi_task_build_sense_data(task, sk, asc, ascq);
	task->status = SPDK_SCSI_STATUS_CHECK_CONDITION;
}
