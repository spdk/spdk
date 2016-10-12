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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "scsi_bdev.c"

#include "CUnit/Basic.h"

SPDK_LOG_REGISTER_TRACE_FLAG("scsi", SPDK_TRACE_SCSI)

struct spdk_scsi_globals g_spdk_scsi;

void
spdk_scsi_lun_clear_all(struct spdk_scsi_lun *lun)
{
}

void
spdk_scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
}

void
spdk_scsi_task_set_check_condition(struct spdk_scsi_task *task, int sk, int asc, int ascq)
{
	spdk_scsi_task_build_sense_data(task, sk, asc, ascq);
	task->status = SPDK_SCSI_STATUS_CHECK_CONDITION;
}

void
spdk_scsi_task_alloc_data(struct spdk_scsi_task *task, uint32_t alloc_len,
			  uint8_t **data)
{
	if (alloc_len < 4096) {
		alloc_len = 4096;
	}

	task->alloc_len = alloc_len;
	*data = task->rbuf;
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

struct spdk_bdev_io *
spdk_bdev_read(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
}

struct spdk_bdev_io *
spdk_bdev_writev(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		 struct iovec *iov, int iovcnt,
		 uint64_t offset, uint64_t len,
		 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
}

struct spdk_bdev_io *
spdk_bdev_unmap(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_scsi_unmap_bdesc *unmap_d,
		uint16_t bdesc_count,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
}

int
spdk_bdev_reset(struct spdk_bdev *bdev, enum spdk_bdev_reset_type reset_type,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

struct spdk_bdev_io *
spdk_bdev_flush(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
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
	task.iov.iov_base = data;

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	CU_ASSERT_EQUAL(rc, 0);
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

	task.iov.iov_base = NULL;

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	CU_ASSERT_EQUAL(rc, 0);
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
	unsigned char data[4096];
	int rc = 0;
	unsigned char mode_data_len = 0;
	unsigned char medium_type = 0;
	unsigned char dev_specific_param = 0;
	unsigned char blk_descriptor_len = 0;

	memset(&bdev, 0 , sizeof(struct spdk_bdev));
	memset(cdb, 0, sizeof(cdb));

	cdb[0] = 0x1A;
	cdb[2] = 0x3F;
	cdb[4] = 0xFF;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	task.rbuf = data;

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	mode_data_len = data[0];
	medium_type = data[1];
	dev_specific_param = data[2];
	blk_descriptor_len = data[3];

	CU_ASSERT(mode_data_len >= 11);
	CU_ASSERT_EQUAL(medium_type, 0);
	CU_ASSERT_EQUAL(dev_specific_param, 0);
	CU_ASSERT_EQUAL(blk_descriptor_len, 8);
	CU_ASSERT_EQUAL(rc, 0);
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
	unsigned char data[4096];
	int rc;
	unsigned short mode_data_len = 0;
	unsigned char medium_type = 0;
	unsigned char dev_specific_param = 0;
	unsigned short blk_descriptor_len = 0;

	memset(&bdev, 0 , sizeof(struct spdk_bdev));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x5A;
	cdb[2] = 0x3F;
	cdb[8] = 0xFF;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.dev = &dev;
	task.lun = &lun;

	task.rbuf = data;

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	mode_data_len = ((data[0] << 8) + data[1]);
	medium_type = data[2];
	dev_specific_param = data[3];
	blk_descriptor_len = ((data[6] << 8) + data[7]);

	CU_ASSERT(mode_data_len >= 14);
	CU_ASSERT_EQUAL(medium_type, 0);
	CU_ASSERT_EQUAL(dev_specific_param, 0);
	CU_ASSERT_EQUAL(blk_descriptor_len, 8);
	CU_ASSERT_EQUAL(rc, 0);
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
	char data[4096];
	int rc;

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

	memset(data, 0, 4096);
	task.rbuf = data;

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[4], (SPDK_SCSI_SENSE_ILLEGAL_REQUEST & 0xf));
	CU_ASSERT_EQUAL(task.sense_data[14], 0x24);
	CU_ASSERT_EQUAL(task.sense_data[15], 0x0);
	CU_ASSERT_EQUAL(rc, 0);
}

/*
 * This test is to verify specific return data for a standard scsi inquiry
 *  command: Version
 */
static void
inquiry_standard_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	struct spdk_bdev_fn_table fn_table;
	char cdb[6];
	/* expects a 4K internal data buffer */
	char data[4096];
	struct spdk_scsi_cdb_inquiry_data *inq_data;
	int rc;

	bdev.fn_table = &fn_table;

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

	memset(data, 0, 4096);
	task.rbuf = data;

	rc = spdk_bdev_scsi_execute(&bdev, &task);

	inq_data = (struct spdk_scsi_cdb_inquiry_data *)&data[0];

	CU_ASSERT_EQUAL(inq_data->version, SPDK_SPC_VERSION_SPC3);
	CU_ASSERT_EQUAL(rc, 0);
}

static void
_inquiry_overflow_test(uint8_t alloc_len)
{
	struct spdk_bdev bdev;
	struct spdk_scsi_task task;
	struct spdk_scsi_lun lun;
	struct spdk_scsi_dev dev;
	struct spdk_bdev_fn_table fn_table;
	uint8_t cdb[6];
	/* expects a 4K internal data buffer */
	char data[256], data_compare[256];
	int rc;

	bdev.fn_table = &fn_table;

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
	task.rbuf = data;

	rc = spdk_bdev_scsi_execute(&bdev, &task);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(memcmp(data + alloc_len, data_compare + alloc_len, sizeof(data) - alloc_len), 0);
	CU_ASSERT(task.data_transferred <= alloc_len);
}

static void
inquiry_overflow_test(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		_inquiry_overflow_test(i);
	}
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
