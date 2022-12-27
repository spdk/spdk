/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/env.h"
#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"
#include "bdev/raid/bdev_raid_sb.c"

#define TEST_BUF_ALIGN	64
#define TEST_BLOCK_SIZE	512

DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), TEST_BLOCK_SIZE);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test_bdev");
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), TEST_BUF_ALIGN);

void *g_buf;
TAILQ_HEAD(, spdk_bdev_io) g_bdev_io_queue = TAILQ_HEAD_INITIALIZER(g_bdev_io_queue);
int g_read_counter;

static int
test_setup(void)
{
	g_buf = spdk_dma_zmalloc(RAID_BDEV_SB_MAX_LENGTH, TEST_BUF_ALIGN, NULL);
	if (!g_buf) {
		return -ENOMEM;
	}

	return 0;
}

static int
test_cleanup(void)
{
	spdk_dma_free(g_buf);

	return 0;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	free(bdev_io);
}

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	g_read_counter++;
	memcpy(buf, g_buf + offset, nbytes);
	cb(NULL, true, cb_arg);
	return 0;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct raid_bdev_superblock *sb = buf;
	struct spdk_bdev_io *bdev_io;

	CU_ASSERT(offset == 0);
	CU_ASSERT(nbytes / TEST_BLOCK_SIZE == spdk_divide_round_up(sb->length, TEST_BLOCK_SIZE));

	bdev_io = calloc(1, sizeof(*bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->internal.cb = cb;
	bdev_io->internal.caller_ctx = cb_arg;

	TAILQ_INSERT_TAIL(&g_bdev_io_queue, bdev_io, internal.link);

	return 0;
}

static void
process_io_completions(void)
{
	struct spdk_bdev_io *bdev_io;

	while ((bdev_io = TAILQ_FIRST(&g_bdev_io_queue))) {
		TAILQ_REMOVE(&g_bdev_io_queue, bdev_io, internal.link);

		bdev_io->internal.cb(bdev_io, true, bdev_io->internal.caller_ctx);
	}
}

static void
prepare_sb(struct raid_bdev_superblock *sb)
{
	/* prepare a simplest valid sb */
	memset(sb, 0, RAID_BDEV_SB_MAX_LENGTH);
	memcpy(sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature));
	sb->version.major = RAID_BDEV_SB_VERSION_MAJOR;
	sb->version.minor = RAID_BDEV_SB_VERSION_MINOR;
	sb->length = sizeof(*sb);
	sb->crc = spdk_crc32c_update(sb, sb->length, 0);
}

static void
write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	int *status_out = ctx;

	*status_out = status;
}

static void
test_raid_bdev_write_superblock(void)
{
	struct raid_base_bdev_info base_info[3] = {{0}};
	struct raid_bdev raid_bdev = {
		.sb = g_buf,
		.num_base_bdevs = SPDK_COUNTOF(base_info),
		.base_bdev_info = base_info,
	};
	int status;
	uint8_t i;

	for (i = 1; i < SPDK_COUNTOF(base_info); i++) {
		base_info[i].desc = (void *)0x1;
	}

	prepare_sb(raid_bdev.sb);

	status = INT_MAX;
	raid_bdev_write_superblock(&raid_bdev, write_sb_cb, &status);
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_io_queue) == false);
	process_io_completions();
	CU_ASSERT(status == 0);
}

static void
load_sb_cb(const struct raid_bdev_superblock *sb, int status, void *ctx)
{
	int *status_out = ctx;

	if (status == 0) {
		CU_ASSERT(memcmp(sb, g_buf, sb->length) == 0);
	}

	*status_out = status;
}

static void
test_raid_bdev_load_base_bdev_superblock(void)
{
	struct raid_bdev_superblock *sb = g_buf;
	int rc;
	int status;

	/* valid superblock */
	prepare_sb(sb);

	g_read_counter = 0;
	status = INT_MAX;
	rc = raid_bdev_load_base_bdev_superblock(NULL, NULL, load_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == 0);
	CU_ASSERT(g_read_counter == 1);

	/* invalid signature */
	prepare_sb(sb);
	sb->signature[3] = 'Z';
	raid_bdev_sb_update_crc(sb);

	g_read_counter = 0;
	status = INT_MAX;
	rc = raid_bdev_load_base_bdev_superblock(NULL, NULL, load_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == -EINVAL);
	CU_ASSERT(g_read_counter == 1);

	/* make the sb longer than 1 bdev block - expect 2 reads */
	prepare_sb(sb);
	sb->length = TEST_BLOCK_SIZE * 3;
	raid_bdev_sb_update_crc(sb);

	g_read_counter = 0;
	status = INT_MAX;
	rc = raid_bdev_load_base_bdev_superblock(NULL, NULL, load_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == 0);
	CU_ASSERT(g_read_counter == 2);

	/* corrupted sb contents, length > 1 bdev block - expect 2 reads */
	prepare_sb(sb);
	sb->length = TEST_BLOCK_SIZE * 3;
	raid_bdev_sb_update_crc(sb);
	sb->reserved[0] = 0xff;

	g_read_counter = 0;
	status = INT_MAX;
	rc = raid_bdev_load_base_bdev_superblock(NULL, NULL, load_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == -EINVAL);
	CU_ASSERT(g_read_counter == 2);

	/* invalid signature, length > 1 bdev block - expect 1 read */
	prepare_sb(sb);
	sb->signature[3] = 'Z';
	sb->length = TEST_BLOCK_SIZE * 3;
	raid_bdev_sb_update_crc(sb);

	g_read_counter = 0;
	status = INT_MAX;
	rc = raid_bdev_load_base_bdev_superblock(NULL, NULL, load_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == -EINVAL);
	CU_ASSERT(g_read_counter == 1);
}

static void
test_raid_bdev_parse_superblock(void)
{
	struct raid_bdev_superblock *sb = g_buf;
	struct raid_bdev_read_sb_ctx ctx = {
		.buf = g_buf,
		.buf_size = TEST_BLOCK_SIZE,
	};

	/* valid superblock */
	prepare_sb(sb);
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == 0);

	/* invalid signature */
	prepare_sb(sb);
	sb->signature[3] = 'Z';
	raid_bdev_sb_update_crc(sb);
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EINVAL);

	/* invalid crc */
	prepare_sb(sb);
	sb->crc = 0xdeadbeef;
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EINVAL);

	/* corrupted sb contents */
	prepare_sb(sb);
	sb->reserved[0] = 0xff;
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EINVAL);

	/* invalid major version */
	prepare_sb(sb);
	sb->version.major = 9999;
	raid_bdev_sb_update_crc(sb);
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EINVAL);

	/* sb longer than 1 bdev block */
	prepare_sb(sb);
	sb->length = TEST_BLOCK_SIZE * 3;
	raid_bdev_sb_update_crc(sb);
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EAGAIN);
	ctx.buf_size = sb->length;
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("raid_sb", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid_bdev_write_superblock);
	CU_ADD_TEST(suite, test_raid_bdev_load_base_bdev_superblock);
	CU_ADD_TEST(suite, test_raid_bdev_parse_superblock);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
