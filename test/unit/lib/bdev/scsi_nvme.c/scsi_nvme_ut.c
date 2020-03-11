/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2016 FUJITSU LIMITED, All rights reserved.
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
 *     * Neither the name of the copyright holder nor the names of its
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

#include "spdk_cunit.h"

#include "bdev/scsi_nvme.c"

static int
null_init(void)
{
	return 0;
}

static int
null_clean(void)
{
	return 0;
}

static void
scsi_nvme_translate_test(void)
{
	struct spdk_bdev_io bdev_io;
	int sc, sk, asc, ascq;

	/* SPDK_NVME_SCT_GENERIC */
	bdev_io.internal.error.nvme.sct = SPDK_NVME_SCT_GENERIC;
	bdev_io.internal.error.nvme.sc = SPDK_NVME_SC_ABORTED_POWER_LOSS;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_TASK_ABORTED);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_ABORTED_COMMAND);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_WARNING);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED);

	bdev_io.internal.error.nvme.sc = SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);

	/* SPDK_NVME_SCT_COMMAND_SPECIFIC */
	bdev_io.internal.error.nvme.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	bdev_io.internal.error.nvme.sc = SPDK_NVME_SC_INVALID_FORMAT;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_FORMAT_COMMAND_FAILED);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_FORMAT_COMMAND_FAILED);

	bdev_io.internal.error.nvme.sc = SPDK_NVME_SC_OVERLAPPING_RANGE;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);

	/* SPDK_NVME_SCT_MEDIA_ERROR */
	bdev_io.internal.error.nvme.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	bdev_io.internal.error.nvme.sc = SPDK_NVME_SC_GUARD_CHECK_ERROR;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_MEDIUM_ERROR);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_LOGICAL_BLOCK_GUARD_CHECK_FAILED);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_LOGICAL_BLOCK_GUARD_CHECK_FAILED);

	bdev_io.internal.error.nvme.sc = SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);

	/* SPDK_NVME_SCT_VENDOR_SPECIFIC */
	bdev_io.internal.error.nvme.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	bdev_io.internal.error.nvme.sc = 0xff;
	spdk_scsi_nvme_translate(&bdev_io, &sc, &sk, &asc, &ascq);
	CU_ASSERT_EQUAL(sc, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(sk, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(asc, SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	CU_ASSERT_EQUAL(ascq, SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("scsi_nvme_suite", null_init, null_clean);

	CU_ADD_TEST(suite, scsi_nvme_translate_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
