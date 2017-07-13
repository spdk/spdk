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
#include "spdk/util.h"

void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
	if (!task) {
		return;
	}

	task->ref--;

	if (task->ref == 0) {
		struct spdk_bdev_io *bdev_io = task->bdev_io;

		if (task->parent) {
			spdk_scsi_task_put(task->parent);
			task->parent = NULL;
		}

		if (bdev_io) {
			spdk_bdev_free_io(bdev_io);
		}

		spdk_scsi_task_free_data(task);

		task->free_fn(task);
	}
}

void
spdk_scsi_task_construct(struct spdk_scsi_task *task,
			 spdk_scsi_task_cpl cpl_fn,
			 spdk_scsi_task_free free_fn,
			 struct spdk_scsi_task *parent)
{
	assert(task != NULL);
	assert(cpl_fn != NULL);
	assert(free_fn != NULL);

	task->cpl_fn = cpl_fn;
	task->free_fn = free_fn;

	task->ref++;

	/*
	 * Pre-fill the iov_buffers to point to the embedded iov
	 */
	assert(task->iov.iov_base == NULL);
	task->iovs = &task->iov;
	task->iovcnt = 1;

	if (parent != NULL) {
		parent->ref++;
		task->parent = parent;
		task->dxfer_dir = parent->dxfer_dir;
		task->transfer_len = parent->transfer_len;
		task->lun = parent->lun;
		task->cdb = parent->cdb;
		task->target_port = parent->target_port;
		task->initiator_port = parent->initiator_port;
	}
}

void
spdk_scsi_task_free_data(struct spdk_scsi_task *task)
{
	if (task->alloc_len != 0) {
		spdk_dma_free(task->iov.iov_base);
		task->alloc_len = 0;
	}

	task->iov.iov_base = NULL;
	task->iov.iov_len = 0;
}

void *
spdk_scsi_task_alloc_data(struct spdk_scsi_task *task, uint32_t alloc_len)
{
	assert(task->alloc_len == 0);

	task->iov.iov_base = spdk_dma_zmalloc(alloc_len, 0, NULL);
	task->iov.iov_len = alloc_len;
	task->alloc_len = alloc_len;

	return task->iov.iov_base;
}

int
spdk_scsi_task_scatter_data(struct spdk_scsi_task *task, const void *src, size_t buf_len)
{
	size_t len = 0;
	size_t buf_left = buf_len;
	int i;
	struct iovec *iovs = task->iovs;
	const uint8_t *pos;

	if (buf_len == 0)
		return 0;

	if (task->iovcnt == 1 && iovs[0].iov_base == NULL) {
		spdk_scsi_task_alloc_data(task, buf_len);
		iovs[0] = task->iov;
	}

	for (i = 0; i < task->iovcnt; i++) {
		assert(iovs[i].iov_base != NULL);
		len += iovs[i].iov_len;
	}

	if (len < buf_len) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -1;
	}

	pos = src;

	for (i = 0; i < task->iovcnt; i++) {
		len = spdk_min(iovs[i].iov_len, buf_left);
		buf_left -= len;
		memcpy(iovs[i].iov_base, pos, len);
		pos += len;
	}

	return buf_len;
}

void *
spdk_scsi_task_gather_data(struct spdk_scsi_task *task, int *len)
{
	int i;
	struct iovec *iovs = task->iovs;
	size_t buf_len = 0;
	uint8_t *buf, *pos;

	for (i = 0; i < task->iovcnt; i++) {
		assert(iovs[i].iov_base != NULL);
		buf_len += iovs[i].iov_len;
	}

	if (buf_len == 0) {
		*len = 0;
		return NULL;
	}

	buf = spdk_dma_malloc(buf_len, 0, NULL);
	if (buf == NULL) {
		*len = -1;
		return NULL;
	}

	pos = buf;
	for (i = 0; i < task->iovcnt; i++) {
		memcpy(pos, iovs[i].iov_base, iovs[i].iov_len);
		pos += iovs[i].iov_len;
	}

	*len = buf_len;
	return buf;
}

void
spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len)
{
	assert(task->iovcnt == 1);
	assert(task->alloc_len == 0);

	task->iovs[0].iov_base = data;
	task->iovs[0].iov_len = len;
}

void
spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc, int ascq)
{
	uint8_t *cp;
	int resp_code;

	resp_code = 0x70; /* Current + Fixed format */

	/* Sense Data */
	cp = task->sense_data;

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
	task->sense_data_len = 18;
}

void
spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk,
			  int asc, int ascq)
{
	if (sc == SPDK_SCSI_STATUS_CHECK_CONDITION) {
		spdk_scsi_task_build_sense_data(task, sk, asc, ascq);
	}
	task->status = sc;
}
