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

void *g_buf;
TAILQ_HEAD(, spdk_bdev_io) g_bdev_io_queue = TAILQ_HEAD_INITIALIZER(g_bdev_io_queue);

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

	prepare_sb(raid_bdev.sb);

	status = INT_MAX;
	raid_bdev_write_superblock(&raid_bdev, write_sb_cb, &status);
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_io_queue) == false);
	process_io_completions();
	CU_ASSERT(status == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("raid_sb", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid_bdev_write_superblock);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
