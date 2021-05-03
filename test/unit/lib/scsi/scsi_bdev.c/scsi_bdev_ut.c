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

#include "scsi/task.c"
#include "scsi/scsi_bdev.c"
#include "common/lib/test_env.c"

#include "spdk_cunit.h"

#include "spdk_internal/mock.h"

SPDK_LOG_REGISTER_COMPONENT(scsi)

struct spdk_scsi_globals g_spdk_scsi;

static uint64_t g_test_bdev_num_blocks;

TAILQ_HEAD(, spdk_bdev_io) g_bdev_io_queue;
int g_scsi_cb_called = 0;

TAILQ_HEAD(, spdk_bdev_io_wait_entry) g_io_wait_queue;
bool g_bdev_io_pool_full = false;

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	abort();
	return false;
}

DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));

DEFINE_STUB(spdk_bdev_get_name, const char *,
	    (const struct spdk_bdev *bdev), "test");

DEFINE_STUB(spdk_bdev_get_block_size, uint32_t,
	    (const struct spdk_bdev *bdev), 512);

DEFINE_STUB(spdk_bdev_get_md_size, uint32_t,
	    (const struct spdk_bdev *bdev), 8);

DEFINE_STUB(spdk_bdev_is_md_interleaved, bool,
	    (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_get_data_block_size, uint32_t,
	    (const struct spdk_bdev *bdev), 512);

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return g_test_bdev_num_blocks;
}

DEFINE_STUB(spdk_bdev_get_product_name, const char *,
	    (const struct spdk_bdev *bdev), "test product");

DEFINE_STUB(spdk_bdev_has_write_cache, bool,
	    (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_get_dif_type, enum spdk_dif_type,
	    (const struct spdk_bdev *bdev), SPDK_DIF_DISABLE);

DEFINE_STUB(spdk_bdev_is_dif_head_of_md, bool,
	    (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_is_dif_check_enabled, bool,
	    (const struct spdk_bdev *bdev, enum spdk_dif_check_type check_type), false);

DEFINE_STUB(scsi_pr_out, int, (struct spdk_scsi_task *task,
			       uint8_t *cdb, uint8_t *data, uint16_t data_len), 0);

DEFINE_STUB(scsi_pr_in, int, (struct spdk_scsi_task *task, uint8_t *cdb,
			      uint8_t *data, uint16_t data_len), 0);

DEFINE_STUB(scsi2_reserve, int, (struct spdk_scsi_task *task, uint8_t *cdb), 0);
DEFINE_STUB(scsi2_release, int, (struct spdk_scsi_task *task), 0);

void
scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	g_scsi_cb_called++;
}

DEFINE_STUB_V(scsi_lun_complete_reset_task,
	      (struct spdk_scsi_lun *lun, struct spdk_scsi_task *task));

DEFINE_STUB(spdk_scsi_lun_id_int_to_fmt, uint64_t, (int lun_id), 0);

static void
ut_put_task(struct spdk_scsi_task *task)
{
	if (task->alloc_len) {
		free(task->iov.iov_base);
	}

	task->iov.iov_base = NULL;
	task->iov.iov_len = 0;
	task->alloc_len = 0;
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));
}

static void
ut_init_task(struct spdk_scsi_task *task)
{
	memset(task, 0xFF, sizeof(*task));
	task->iov.iov_base = NULL;
	task->iovs = &task->iov;
	task->iovcnt = 1;
	task->alloc_len = 0;
	task->dxfer_dir = SPDK_SCSI_DIR_NONE;
}

void
spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
			     int *sc, int *sk, int *asc, int *ascq)
{
	switch (bdev_io->internal.status) {
	case SPDK_BDEV_IO_STATUS_SUCCESS:
		*sc = SPDK_SCSI_STATUS_GOOD;
		*sk = SPDK_SCSI_SENSE_NO_SENSE;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
		*sc = bdev_io->internal.error.scsi.sc;
		*sk = bdev_io->internal.error.scsi.sk;
		*asc = bdev_io->internal.error.scsi.asc;
		*ascq = bdev_io->internal.error.scsi.ascq;
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

static void
ut_bdev_io_flush(void)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_io_wait_entry *entry;

	while (!TAILQ_EMPTY(&g_bdev_io_queue) || !TAILQ_EMPTY(&g_io_wait_queue)) {
		while (!TAILQ_EMPTY(&g_bdev_io_queue)) {
			bdev_io = TAILQ_FIRST(&g_bdev_io_queue);
			TAILQ_REMOVE(&g_bdev_io_queue, bdev_io, internal.link);
			bdev_io->internal.cb(bdev_io, true, bdev_io->internal.caller_ctx);
			free(bdev_io);
		}

		while (!TAILQ_EMPTY(&g_io_wait_queue)) {
			entry = TAILQ_FIRST(&g_io_wait_queue);
			TAILQ_REMOVE(&g_io_wait_queue, entry, link);
			entry->cb_fn(entry->cb_arg);
		}
	}
}

static int
_spdk_bdev_io_op(spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;

	if (g_bdev_io_pool_full) {
		g_bdev_io_pool_full = false;
		return -ENOMEM;
	}

	bdev_io = calloc(1, sizeof(*bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	bdev_io->internal.cb = cb;
	bdev_io->internal.caller_ctx = cb_arg;

	TAILQ_INSERT_TAIL(&g_bdev_io_queue, bdev_io, internal.link);

	return 0;
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return _spdk_bdev_io_op(cb, cb_arg);
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return _spdk_bdev_io_op(cb, cb_arg);
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return _spdk_bdev_io_op(cb, cb_arg);
}

int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return _spdk_bdev_io_op(cb, cb_arg);
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return _spdk_bdev_io_op(cb, cb_arg);
}

int
spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			struct spdk_bdev_io_wait_entry *entry)
{
	TAILQ_INSERT_TAIL(&g_io_wait_queue, entry, link);
	return 0;
}

int
spdk_dif_ctx_init(struct spdk_dif_ctx *ctx, uint32_t block_size, uint32_t md_size,
		  bool md_interleave, bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		  uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		  uint32_t data_offset, uint16_t guard_seed)
{
	ctx->init_ref_tag = init_ref_tag;
	ctx->ref_tag_offset = data_offset / 512;
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

	ut_init_task(&task);

	cdb[0] = 0x15;
	cdb[1] = 0x11;
	cdb[2] = 0x00;
	cdb[3] = 0x00;
	cdb[4] = 0x18;
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	memset(data, 0, sizeof(data));
	data[4] = 0x08;
	data[5] = 0x02;
	spdk_scsi_task_set_data(&task, data, sizeof(data));

	rc = bdev_scsi_execute(&task);

	CU_ASSERT_EQUAL(rc, 0);

	ut_put_task(&task);
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

	ut_init_task(&task);

	cdb[0] = 0x15;
	cdb[1] = 0x00;
	cdb[2] = 0x00;
	cdb[3] = 0x00;
	cdb[4] = 0x00;
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	rc = bdev_scsi_execute(&task);

	CU_ASSERT_EQUAL(rc, 0);

	ut_put_task(&task);
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
	ut_init_task(&task);
	memset(cdb, 0, sizeof(cdb));

	cdb[0] = 0x1A;
	cdb[2] = 0x3F;
	cdb[4] = 0xFF;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	rc = bdev_scsi_execute(&task);
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

	ut_put_task(&task);
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
	ut_init_task(&task);
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x5A;
	cdb[2] = 0x3F;
	cdb[8] = 0xFF;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	rc = bdev_scsi_execute(&task);
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

	ut_put_task(&task);
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

	ut_init_task(&task);

	cdb[0] = 0x12;
	cdb[1] = 0x00;	/* EVPD = 0 */
	cdb[2] = 0xff;	/* PageCode non-zero */
	cdb[3] = 0x00;
	cdb[4] = 0xff;
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	rc = bdev_scsi_execute(&task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[2] & 0xf, SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT_EQUAL(task.sense_data[12], 0x24);
	CU_ASSERT_EQUAL(task.sense_data[13], 0x0);

	ut_put_task(&task);
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

	ut_init_task(&task);

	cdb[0] = 0x12;
	cdb[1] = 0x00;	/* EVPD = 0 */
	cdb[2] = 0x00;	/* PageCode zero - requesting standard inquiry */
	cdb[3] = 0x00;
	cdb[4] = 0xff;	/* Indicate data size used by conformance test */
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	rc = bdev_scsi_execute(&task);

	data = task.iovs[0].iov_base;
	inq_data = (struct spdk_scsi_cdb_inquiry_data *)&data[0];

	CU_ASSERT_EQUAL(inq_data->version, SPDK_SPC_VERSION_SPC3);
	CU_ASSERT_EQUAL(rc, 0);

	ut_put_task(&task);
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

	ut_init_task(&task);

	cdb[0] = 0x12;
	cdb[1] = 0x00;		/* EVPD = 0 */
	cdb[2] = 0x00;		/* PageCode zero - requesting standard inquiry */
	cdb[3] = 0x00;
	cdb[4] = alloc_len;	/* Indicate data size used by conformance test */
	cdb[5] = 0x00;
	task.cdb = cdb;

	snprintf(&dev.name[0], sizeof(dev.name), "spdk_iscsi_translation_test");
	lun.bdev = &bdev;
	lun.dev = &dev;
	task.lun = &lun;

	memset(data, 0, sizeof(data));
	memset(data_compare, 0, sizeof(data_compare));

	spdk_scsi_task_set_data(&task, data, sizeof(data));

	rc = bdev_scsi_execute(&task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	CU_ASSERT_EQUAL(memcmp(data + alloc_len, data_compare + alloc_len, sizeof(data) - alloc_len), 0);
	CU_ASSERT(task.data_transferred <= alloc_len);

	ut_put_task(&task);
}

static void
inquiry_overflow_test(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		_inquiry_overflow_test(i);
	}
}

static void
scsi_name_padding_test(void)
{
	char name[SPDK_SCSI_DEV_MAX_NAME + 1];
	char buf[SPDK_SCSI_DEV_MAX_NAME + 1];
	int written, i;

	/* case 1 */
	memset(name, '\0', sizeof(name));
	memset(name, 'x', 251);
	written = bdev_scsi_pad_scsi_name(buf, name);

	CU_ASSERT(written == 252);
	CU_ASSERT(buf[250] == 'x');
	CU_ASSERT(buf[251] == '\0');

	/* case 2:  */
	memset(name, '\0', sizeof(name));
	memset(name, 'x', 252);
	written = bdev_scsi_pad_scsi_name(buf, name);

	CU_ASSERT(written == 256);
	CU_ASSERT(buf[251] == 'x');
	for (i = 252; i < 256; i++) {
		CU_ASSERT(buf[i] == '\0');
	}

	/* case 3 */
	memset(name, '\0', sizeof(name));
	memset(name, 'x', 255);
	written = bdev_scsi_pad_scsi_name(buf, name);

	CU_ASSERT(written == 256);
	CU_ASSERT(buf[254] == 'x');
	CU_ASSERT(buf[255] == '\0');
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

	ut_init_task(&task);

	TAILQ_INIT(&lun.tasks);
	TAILQ_INSERT_TAIL(&lun.tasks, &task, scsi_link);
	task.lun = &lun;

	bdev_io.internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	bdev_scsi_task_complete_cmd(&bdev_io, bdev_io.internal.status, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	bdev_io.internal.status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
	bdev_io.internal.error.scsi.sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
	bdev_io.internal.error.scsi.sk = SPDK_SCSI_SENSE_HARDWARE_ERROR;
	bdev_io.internal.error.scsi.asc = SPDK_SCSI_ASC_WARNING;
	bdev_io.internal.error.scsi.ascq = SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED;
	bdev_scsi_task_complete_cmd(&bdev_io, bdev_io.internal.status, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[2] & 0xf, SPDK_SCSI_SENSE_HARDWARE_ERROR);
	CU_ASSERT_EQUAL(task.sense_data[12], SPDK_SCSI_ASC_WARNING);
	CU_ASSERT_EQUAL(task.sense_data[13], SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	bdev_io.internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	bdev_scsi_task_complete_cmd(&bdev_io, bdev_io.internal.status, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT_EQUAL(task.sense_data[2] & 0xf, SPDK_SCSI_SENSE_ABORTED_COMMAND);
	CU_ASSERT_EQUAL(task.sense_data[12], SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	CU_ASSERT_EQUAL(task.sense_data[13], SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	ut_put_task(&task);
}

static void
lba_range_test(void)
{
	struct spdk_bdev bdev = { .blocklen = 512 };
	struct spdk_scsi_lun lun;
	struct spdk_scsi_task task;
	uint8_t cdb[16];
	int rc;

	lun.bdev = &bdev;

	ut_init_task(&task);
	task.lun = &lun;
	task.lun->bdev_desc = NULL;
	task.lun->io_channel = NULL;
	task.cdb = cdb;

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x88; /* READ (16) */

	/* Test block device size of 4 blocks */
	g_test_bdev_num_blocks = 4;

	/* LBA = 0, length = 1 (in range) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_bdev_io_queue));
	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	/* LBA = 4, length = 1 (LBA out of range) */
	to_be64(&cdb[2], 4); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));

	/* LBA = 0, length = 4 (in range, max valid size) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 4); /* transfer length */
	task.transfer_len = 4 * 512;
	task.status = 0xFF;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_bdev_io_queue));
	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	/* LBA = 0, length = 5 (LBA in range, length beyond end of bdev) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 5); /* transfer length */
	task.transfer_len = 5 * 512;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));

	ut_put_task(&task);
}

static void
xfer_len_test(void)
{
	struct spdk_bdev bdev = { .blocklen = 512 };
	struct spdk_scsi_lun lun;
	struct spdk_scsi_task task;
	uint8_t cdb[16];
	int rc;

	lun.bdev = &bdev;

	ut_init_task(&task);
	task.lun = &lun;
	task.lun->bdev_desc = NULL;
	task.lun->io_channel = NULL;
	task.cdb = cdb;

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x88; /* READ (16) */

	/* Test block device size of 512 MiB */
	g_test_bdev_num_blocks = 512 * 1024 * 1024;

	/* 1 block */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_bdev_io_queue));
	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	/* max transfer length (as reported in block limits VPD page) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], SPDK_WORK_BLOCK_SIZE / 512); /* transfer length */
	task.transfer_len = SPDK_WORK_BLOCK_SIZE;
	task.status = 0xFF;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_bdev_io_queue));
	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;

	/* max transfer length plus one block (invalid) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], SPDK_WORK_BLOCK_SIZE / 512 + 1); /* transfer length */
	task.transfer_len = SPDK_WORK_BLOCK_SIZE + 512;
	task.offset = 0;
	task.length = 1 * 512;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT((task.sense_data[2] & 0xf) == SPDK_SCSI_SENSE_ILLEGAL_REQUEST);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));

	/* zero transfer length (valid) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 0); /* transfer length */
	task.transfer_len = 0;
	task.offset = 0;
	task.length = 0;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(task.data_transferred == 0);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));

	/* zero transfer length past end of disk (invalid) */
	to_be64(&cdb[2], g_test_bdev_num_blocks); /* LBA */
	to_be32(&cdb[10], 0); /* transfer length */
	task.transfer_len = 0;
	task.offset = 0;
	task.length = 0;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_COMPLETE);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.sense_data[12] == SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));

	ut_put_task(&task);
}

static void
_xfer_test(bool bdev_io_pool_full)
{
	struct spdk_bdev bdev = { .blocklen = 512 };
	struct spdk_scsi_lun lun;
	struct spdk_scsi_task task;
	uint8_t cdb[16];
	char data[4096];
	int rc;

	lun.bdev = &bdev;

	/* Test block device size of 512 MiB */
	g_test_bdev_num_blocks = 512 * 1024 * 1024;

	/* Read 1 block */
	ut_init_task(&task);
	task.lun = &lun;
	task.lun->bdev_desc = NULL;
	task.lun->io_channel = NULL;
	task.cdb = cdb;
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x88; /* READ (16) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	task.offset = 0;
	task.length = 1 * 512;
	g_bdev_io_pool_full = bdev_io_pool_full;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);

	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;
	ut_put_task(&task);

	/* Write 1 block */
	ut_init_task(&task);
	task.lun = &lun;
	task.cdb = cdb;
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x8a; /* WRITE (16) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* transfer length */
	task.transfer_len = 1 * 512;
	task.offset = 0;
	task.length = 1 * 512;
	g_bdev_io_pool_full = bdev_io_pool_full;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);

	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;
	ut_put_task(&task);

	/* Unmap 5 blocks using 2 descriptors */
	ut_init_task(&task);
	task.lun = &lun;
	task.cdb = cdb;
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x42; /* UNMAP */
	to_be16(&data[7], 2); /* 2 parameters in list */
	memset(data, 0, sizeof(data));
	to_be16(&data[2], 32); /* 2 descriptors */
	to_be64(&data[8], 1); /* LBA 1 */
	to_be32(&data[16], 2); /* 2 blocks */
	to_be64(&data[24], 10); /* LBA 10 */
	to_be32(&data[32], 3); /* 3 blocks */
	spdk_scsi_task_set_data(&task, data, sizeof(data));
	task.status = SPDK_SCSI_STATUS_GOOD;
	g_bdev_io_pool_full = bdev_io_pool_full;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);

	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;
	ut_put_task(&task);

	/* Flush 1 block */
	ut_init_task(&task);
	task.lun = &lun;
	task.cdb = cdb;
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x91; /* SYNCHRONIZE CACHE (16) */
	to_be64(&cdb[2], 0); /* LBA */
	to_be32(&cdb[10], 1); /* 1 blocks */
	g_bdev_io_pool_full = bdev_io_pool_full;
	rc = bdev_scsi_execute(&task);
	CU_ASSERT(rc == SPDK_SCSI_TASK_PENDING);
	CU_ASSERT(task.status == 0xFF);

	ut_bdev_io_flush();
	CU_ASSERT(task.status == SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT(g_scsi_cb_called == 1);
	g_scsi_cb_called = 0;
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_io_queue));

	ut_put_task(&task);
}

static void
xfer_test(void)
{
	_xfer_test(false);
	_xfer_test(true);
}

static void
get_dif_ctx_test(void)
{
	struct spdk_bdev bdev = {};
	struct spdk_scsi_task task = {};
	struct spdk_dif_ctx dif_ctx = {};
	uint8_t cdb[16];
	bool ret;

	cdb[0] = SPDK_SBC_READ_6;
	cdb[1] = 0x12;
	cdb[2] = 0x34;
	cdb[3] = 0x50;
	task.cdb = cdb;
	task.offset = 0x6 * 512;

	ret = bdev_scsi_get_dif_ctx(&bdev, &task, &dif_ctx);
	CU_ASSERT(ret == true);
	CU_ASSERT(dif_ctx.init_ref_tag + dif_ctx.ref_tag_offset == 0x123456);

	cdb[0] = SPDK_SBC_WRITE_12;
	to_be32(&cdb[2], 0x12345670);
	task.offset = 0x8 * 512;

	ret = bdev_scsi_get_dif_ctx(&bdev, &task, &dif_ctx);
	CU_ASSERT(ret == true);
	CU_ASSERT(dif_ctx.init_ref_tag + dif_ctx.ref_tag_offset == 0x12345678);

	cdb[0] = SPDK_SBC_WRITE_16;
	to_be64(&cdb[2], 0x0000000012345670);
	task.offset = 0x8 * 512;

	ret = bdev_scsi_get_dif_ctx(&bdev, &task, &dif_ctx);
	CU_ASSERT(ret == true);
	CU_ASSERT(dif_ctx.init_ref_tag + dif_ctx.ref_tag_offset == 0x12345678);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	TAILQ_INIT(&g_bdev_io_queue);
	TAILQ_INIT(&g_io_wait_queue);

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("translation_suite", NULL, NULL);

	CU_ADD_TEST(suite, mode_select_6_test);
	CU_ADD_TEST(suite, mode_select_6_test2);
	CU_ADD_TEST(suite, mode_sense_6_test);
	CU_ADD_TEST(suite, mode_sense_10_test);
	CU_ADD_TEST(suite, inquiry_evpd_test);
	CU_ADD_TEST(suite, inquiry_standard_test);
	CU_ADD_TEST(suite, inquiry_overflow_test);
	CU_ADD_TEST(suite, task_complete_test);
	CU_ADD_TEST(suite, lba_range_test);
	CU_ADD_TEST(suite, xfer_len_test);
	CU_ADD_TEST(suite, xfer_test);
	CU_ADD_TEST(suite, scsi_name_padding_test);
	CU_ADD_TEST(suite, get_dif_ctx_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
