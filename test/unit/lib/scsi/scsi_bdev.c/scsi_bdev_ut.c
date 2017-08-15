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

#include "spdk/stdinc.h"

#include "task.c"
#include "scsi_bdev.c"

#include "spdk_cunit.h"

SPDK_LOG_REGISTER_TRACE_FLAG("scsi", SPDK_TRACE_SCSI)

struct spdk_scsi_globals g_spdk_scsi;

static uint64_t g_test_bdev_num_blocks;

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = malloc(size);
	if (phys_addr)
		*phys_addr = (uint64_t)buf;

	return buf;
}

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = calloc(size, 1);
	if (phys_addr)
		*phys_addr = (uint64_t)buf;

	return buf;
}

void
spdk_dma_free(void *buf)
{
	free(buf);
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	abort();
	return false;
}

int
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	CU_ASSERT(0);
	return -1;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return 512;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return g_test_bdev_num_blocks;
}

const char *
spdk_bdev_get_product_name(const struct spdk_bdev *bdev)
{
	return "test product";
}

bool
spdk_bdev_has_write_cache(const struct spdk_bdev *bdev)
{
	return false;
}

void
spdk_scsi_lun_clear_all(struct spdk_scsi_lun *lun)
{
}

void
spdk_scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
}

void
spdk_scsi_lun_complete_mgmt_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
}

static void
spdk_put_task(struct spdk_scsi_task *task)
{
	if (task->alloc_len)
		free(task->iov.iov_base);

	task->iov.iov_base = NULL;
	task->iov.iov_len = 0;
	task->alloc_len = 0;
}


static void
spdk_init_task(struct spdk_scsi_task *task)
{
	memset(task, 0, sizeof(*task));
	task->iovs = &task->iov;
	task->iovcnt = 1;
}

void
spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
			     int *sc, int *sk, int *asc, int *ascq)
{
	switch (bdev_io->status) {
	case SPDK_BDEV_IO_STATUS_SUCCESS:
		*sc = SPDK_SCSI_STATUS_GOOD;
		*sk = SPDK_SCSI_SENSE_NO_SENSE;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
		*sc = bdev_io->error.scsi.sc;
		*sk = bdev_io->error.scsi.sk;
		*asc = bdev_io->error.scsi.asc;
		*ascq = bdev_io->error.scsi.ascq;
		break;
	default:
		*sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		*sk = SPDK_SCSI_SENSE_ABORTED_COMMAND;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	}
}

void
spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp)
{
	*iovp = NULL;
	*iovcntp = 0;
}

int
spdk_bdev_read(struct spdk_bdev_desc *bdev_desc, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		 struct iovec *iov, int iovcnt,
		 uint64_t offset, uint64_t len,
		 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

/*
 * This test specifically tests a mode select 6 command from the
 *  Windows SCSI compliance test that caused SPDK to crash.
 */
static void
mode_select_6_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	char cdb[16];
	char data[24];
	int rc;

	spdk_init_task(&task);

	cdb[0] = 0x15;
	cdb[1] = 0x11;
	cdb[2] = 0x00;
	cdb[3] = 0x00;
	cdb[4] = 0x18;
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	memset(data, 0, sizeof(data));
	data[4] = 0x08;
	data[5] = 0x02;
	spdk_scsi_task_set_data(&task, data, sizeof(data));

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	CU_ASSERT_EQUAL(rc, 0);

	spdk_put_task(&task);
}

/*
 * This test specifically tests a mode select 6 command which
 *  contains no mode pages.
 */
static void
mode_select_6_test2(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	char cdb[16];
	int rc;

	spdk_init_task(&task);

	cdb[0] = 0x15;
	cdb[1] = 0x00;
	cdb[2] = 0x00;
	cdb[3] = 0x00;
	cdb[4] = 0x00;
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	CU_ASSERT_EQUAL(rc, 0);

	spdk_put_task(&task);
}

/*
 * This test specifically tests a mode sense 6 command which
 *  return all subpage 00h mode pages.
 */
static void
mode_sense_6_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	char cdb[12];
	unsigned char *data;
	int rc;
	unsigned char mode_data_len = 0;
	unsigned char medium_type = 0;
	unsigned char dev_specific_param = 0;
	unsigned char blk_descriptor_len = 0;

	memset(&bdev, 0, sizeof(struct spdk_bdev));
	spdk_init_task(&task);
	memset(cdb, 0, sizeof(cdb));

	cdb[0] = 0x1A;
	cdb[2] = 0x3F;
	cdb[4] = 0xFF;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	data = task.iovs[0].iov_base;
	mode_data_len = data[0];
	medium_type = data[1];
	dev_specific_param = data[2];
	blk_descriptor_len = data[3];

	CU_ASSERT(mode_data_len >= 11);
	CU_ASSERT_EQUAL(medium_type, 0);
	CU_ASSERT_EQUAL(dev_specific_param, 0);
	CU_ASSERT_EQUAL(blk_descriptor_len, 8);

	spdk_put_task(&task);
}

/*
 * This test specifically tests a mode sense 10 command which
 *  return all subpage 00h mode pages.
 */
static void
mode_sense_10_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	char cdb[12];
	unsigned char *data;
	int rc;
	unsigned short mode_data_len = 0;
	unsigned char medium_type = 0;
	unsigned char dev_specific_param = 0;
	unsigned short blk_descriptor_len = 0;

	memset(&bdev, 0, sizeof(struct spdk_bdev));
	spdk_init_task(&task);
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x5A;
	cdb[2] = 0x3F;
	cdb[8] = 0xFF;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	data = task.iovs[0].iov_base;
	mode_data_len = ((data[0] << 8) + data[1]);
	medium_type = data[2];
	dev_specific_param = data[3];
	blk_descriptor_len = ((data[6] << 8) + data[7]);

	CU_ASSERT(mode_data_len >= 14);
	CU_ASSERT_EQUAL(medium_type, 0);
	CU_ASSERT_EQUAL(dev_specific_param, 0);
	CU_ASSERT_EQUAL(blk_descriptor_len, 8);

	spdk_put_task(&task);
}

/*
 * This test specifically tests a scsi inquiry command from the
 *  Windows SCSI compliance test that failed to return the
 *  expected SCSI error sense code.
 */
static void
inquiry_evpd_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	char cdb[6];
	int rc;

	spdk_init_task(&task);

	cdb[0] = 0x12;
	cdb[1] = 0x00; // EVPD = 0
	cdb[2] = 0xff; // PageCode non-zero
	cdb[3] = 0x00;
	cdb[4] = 0xff;
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[2] & 0xf, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(task.sense_data[12], 0x24);
	CU_ASSERT_EQUAL(task.sense_data[13], 0x0);

	spdk_put_task(&task);
}

/*
 * This test is to verify specific return data for a standard scsi inquiry
 *  command: Version
 */
static void
inquiry_standard_test(void)
{
	struct spdk_bdev bdev = { .blocklen = 512 };
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	char cdb[6];
	char *data;
	struct spdk_scsi_cdb_inquiry_data *inq_data;
	int rc;

	spdk_init_task(&task);

	cdb[0] = 0x12;
	cdb[1] = 0x00; // EVPD = 0
	cdb[2] = 0x00; // PageCode zero - requesting standard inquiry
	cdb[3] = 0x00;
	cdb[4] = 0xff; // Indicate data size used by conformance test
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	data = task.iovs[0].iov_base;
	inq_data = (struct spdk_scsi_cdb_inquiry_data *)&data[0];

	CU_ASSERT_EQUAL(inq_data->version, SPDK_SPC_VERSION_SPC3);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_put_task(&task);
}

static void
_inquiry_overflow_test(uint8_t alloc_len)
{
	struct spdk_bdev bdev = { .blocklen = 512 };
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	uint8_t cdb[6];
	int rc;
	/* expects a 4K internal data buffer */
	char data[4096], data_compare[4096];

	spdk_init_task(&task);

	cdb[0] = 0x12;
	cdb[1] = 0x00; // EVPD = 0
	cdb[2] = 0x00; // PageCode zero - requesting standard inquiry
	cdb[3] = 0x00;
	cdb[4] = alloc_len; // Indicate data size used by conformance test
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	memset(data, 0, sizeof(data));
	memset(data_compare, 0, sizeof(data_compare));

	spdk_scsi_task_set_data(&task, data, sizeof(data));

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	CU_ASSERT_EQUAL(memcmp(data + alloc_len, data_compare + alloc_len, sizeof(data) - alloc_len), 0);
	CU_ASSERT(task.data_transferred <= alloc_len);

	spdk_put_task(&task);
}

static void
inquiry_overflow_test(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		_inquiry_overflow_test(i);
	}
}

/*
 * This test is to verify specific error translation from bdev to scsi.
 */
static void
task_complete_test(void)
{
	struct spdk_scsi_task task;
	struct spdk_bdev_io bdev_io = {};
	struct spdk_scsi_lun lun;

	spdk_init_task(&task);

	TAILQ_INIT(&lun.tasks);
	TAILQ_INSERT_TAIL(&lun.tasks, &task, scsi_link);
	task.lun = &lun;

	bdev_io.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	spdk_bdev_scsi_task_complete_cmd(&bdev_io, bdev_io.status, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_GOOD);

	bdev_io.status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
	bdev_io.error.scsi.sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
	bdev_io.error.scsi.sk = SPDK_SCSI_SENSE_HARDWARE_ERROR;
	bdev_io.error.scsi.asc = SPDK_SCSI_ASC_WARNING;
	bdev_io.error.scsi.ascq = SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED;
	spdk_bdev_scsi_task_complete_cmd(&bdev_io, bdev_io.status, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[2] & 0xf, SPDK_SCSI_SENSE_HARDWARE_ERROR);
	CU_ASSERT_EQUAL(task.sense_data[12], SPDK_SCSI_ASC_WARNING);
	CU_ASSERT_EQUAL(task.sense_data[13], SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED);

	bdev_io.status = SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_scsi_task_complete_cmd(&bdev_io, bdev_io.status, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[2] & 0xf, SPDK_SCSI_SENSE_ABORTED_COMMAND);
	CU_ASSERT_EQUAL(task.sense_data[12], SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	CU_ASSERT_EQUAL(task.sense_data[13], SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);

	spdk_put_task(&task);
}

static void
lba_range_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	uint8_t cdb[16];
	int rc;

	spdk_init_task(&task);
	task.cdb = cdb;

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x88; /* READ (16) */

	/* Test block device size of 4 blocks */
	g_test_bdev_num_blocks = 4;

	/* LBA = 0, length = 1 (in range) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);

	/* LBA = 4, length = 1 (LBA out of range) */
	to_be64(&cdb[2], 4); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);

	/* LBA = 0, length = 4 (in range, max valid size) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 4); /* transfer length */
	task.transfer_len = 4 * 512;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);

	/* LBA = 0, length = 5 (LBA in range, length beyond end of bdev) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 5); /* transfer length */
	task.transfer_len = 5 * 512;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);

	spdk_put_task(&task);
}

static void
xfer_len_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	uint8_t cdb[16];
	int rc;

	spdk_init_task(&task);
	task.cdb = cdb;

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x88; /* READ (16) */

	/* Test block device size of 512 MiB */
	g_test_bdev_num_blocks = 512 * 1024 * 1024;

	/* 1 block */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);

	/* max transfer length (as reported in block limits VPD page) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], SPDK_WORK_BLOCK_SIZE / 512); /* transfer length */
	task.transfer_len = SPDK_WORK_BLOCK_SIZE;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);

	/* max transfer length plus one block (invalid) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], SPDK_WORK_BLOCK_SIZE / 512 + 1); /* transfer length */
	task.transfer_len = SPDK_WORK_BLOCK_SIZE + 512;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT((task.sense_data[2] & 0xf) == SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB);

	/* zero transfer length (valid) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 0); /* transfer length */
	task.transfer_len = 0;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(task.data_transferred == 0);

	/* zero transfer length past end of disk (invalid) */
	to_be64(&cdb[2], g_test_bdev_num_blocks); /* LBA */
	to_be32(&cdb[10], 0); /* transfer length */
	task.transfer_len = 0;
	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);

	spdk_put_task(&task);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("translation_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "mode select 6 test", mode_select_6_test) == NULL
		|| CU_add_test(suite, "mode select 6 test2", mode_select_6_test2) == NULL
		|| CU_add_test(suite, "mode sense 6 test", mode_sense_6_test) == NULL
		|| CU_add_test(suite, "mode sense 10 test", mode_sense_10_test) == NULL
		|| CU_add_test(suite, "inquiry evpd test", inquiry_evpd_test) == NULL
		|| CU_add_test(suite, "inquiry standard test", inquiry_standard_test) == NULL
		|| CU_add_test(suite, "inquiry overflow test", inquiry_overflow_test) == NULL
		|| CU_add_test(suite, "task complete test", task_complete_test) == NULL
		|| CU_add_test(suite, "LBA range test", lba_range_test) == NULL
		|| CU_add_test(suite, "transfer length test", xfer_len_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
