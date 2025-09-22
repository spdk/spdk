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

DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test_bdev");
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), TEST_BUF_ALIGN);

void *g_buf;
TAILQ_HEAD(, spdk_bdev_io) g_bdev_io_queue = TAILQ_HEAD_INITIALIZER(g_bdev_io_queue);
int g_read_counter;
int g_write_counter;
struct spdk_bdev g_bdev;
struct spdk_bdev_io g_bdev_io = {
	.bdev = &g_bdev,
};

static int
_test_setup(uint32_t blocklen, uint32_t md_len)
{
	g_bdev.blocklen = blocklen;
	g_bdev.md_len = md_len;

	g_buf = spdk_dma_zmalloc(SPDK_ALIGN_CEIL(RAID_BDEV_SB_MAX_LENGTH,
				 spdk_bdev_get_data_block_size(&g_bdev)), TEST_BUF_ALIGN, NULL);
	if (!g_buf) {
		return -ENOMEM;
	}

	return 0;
}

static int
test_setup(void)
{
	return _test_setup(512, 0);
}

static int
test_setup_md(void)
{
	return _test_setup(512, 8);
}

static int
test_setup_md_interleaved(void)
{
	return _test_setup(512 + 8, 8);
}

static int
test_cleanup(void)
{
	spdk_dma_free(g_buf);

	return 0;
}

bool
spdk_bdev_is_md_interleaved(const struct spdk_bdev *bdev)
{
	return spdk_u32_is_pow2(bdev->blocklen) == false;
}

uint32_t
spdk_bdev_get_data_block_size(const struct spdk_bdev *bdev)
{
	return spdk_bdev_is_md_interleaved(bdev) ? bdev->blocklen - bdev->md_len : bdev->blocklen;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return &g_bdev;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io != &g_bdev_io) {
		free(bdev_io);
	}
}

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io = &g_bdev_io;
	uint64_t offset_blocks = offset / bdev->blocklen;
	uint32_t data_block_size = spdk_bdev_get_data_block_size(bdev);
	void *src = g_buf + offset_blocks * data_block_size;

	g_read_counter++;

	memset(buf, 0xab, nbytes);

	while (nbytes > 0) {
		memcpy(buf, src, data_block_size);
		src += data_block_size;
		buf += bdev->blocklen;
		nbytes -= bdev->blocklen;
	}

	cb(bdev_io, true, cb_arg);
	return 0;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct raid_bdev_superblock *sb = buf;
	struct spdk_bdev_io *bdev_io;
	void *dest = g_buf;
	uint32_t data_block_size = spdk_bdev_get_data_block_size(bdev);

	g_write_counter++;
	CU_ASSERT(offset == 0);
	CU_ASSERT(nbytes == spdk_divide_round_up(sb->length, data_block_size) * bdev->blocklen);

	while (nbytes > 0) {
		memcpy(dest, buf, data_block_size);
		dest += data_block_size;
		buf += bdev->blocklen;
		nbytes -= bdev->blocklen;
	}

	bdev_io = calloc(1, sizeof(*bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->internal.cb = cb;
	bdev_io->internal.caller_ctx = cb_arg;
	bdev_io->bdev = bdev;

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
	struct raid_base_bdev_info base_info[3] = {};
	struct raid_bdev raid_bdev = {
		.num_base_bdevs = SPDK_COUNTOF(base_info),
		.base_bdev_info = base_info,
		.bdev = g_bdev,
	};
	int status;
	uint8_t i;

	for (i = 0; i < SPDK_COUNTOF(base_info); i++) {
		base_info[i].raid_bdev = &raid_bdev;
		if (i > 0) {
			base_info[i].is_configured = true;
		}
	}

	status = raid_bdev_alloc_superblock(&raid_bdev, spdk_bdev_get_data_block_size(&raid_bdev.bdev));
	CU_ASSERT(status == 0);

	/* test initial sb write */
	raid_bdev_init_superblock(&raid_bdev);

	status = INT_MAX;
	g_write_counter = 0;
	raid_bdev_write_superblock(&raid_bdev, write_sb_cb, &status);
	CU_ASSERT(g_write_counter == raid_bdev.num_base_bdevs - 1);
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_io_queue) == false);
	process_io_completions();
	CU_ASSERT(status == 0);
	CU_ASSERT(memcmp(raid_bdev.sb, g_buf, raid_bdev.sb->length) == 0);

	/* test max size sb write */
	raid_bdev.sb->length = RAID_BDEV_SB_MAX_LENGTH;
	if (spdk_bdev_is_md_interleaved(&raid_bdev.bdev)) {
		SPDK_CU_ASSERT_FATAL(raid_bdev.sb_io_buf != raid_bdev.sb);
		spdk_dma_free(raid_bdev.sb_io_buf);
	}
	raid_bdev.sb_io_buf = NULL;

	status = INT_MAX;
	g_write_counter = 0;
	raid_bdev_write_superblock(&raid_bdev, write_sb_cb, &status);
	CU_ASSERT(g_write_counter == raid_bdev.num_base_bdevs - 1);
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_io_queue) == false);
	process_io_completions();
	CU_ASSERT(status == 0);
	CU_ASSERT(memcmp(raid_bdev.sb, g_buf, raid_bdev.sb->length) == 0);

	raid_bdev_free_superblock(&raid_bdev);
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
	const uint32_t data_block_size = spdk_bdev_get_data_block_size(&g_bdev);
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
	sb->length = data_block_size * 3;
	memset(sb->base_bdevs, 0xef, sb->length - offsetof(struct raid_bdev_superblock, base_bdevs));
	raid_bdev_sb_update_crc(sb);

	g_read_counter = 0;
	status = INT_MAX;
	rc = raid_bdev_load_base_bdev_superblock(NULL, NULL, load_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == 0);
	CU_ASSERT(g_read_counter == 2);

	/* corrupted sb contents, length > 1 bdev block - expect 2 reads */
	prepare_sb(sb);
	sb->length = data_block_size * 3;
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
	sb->length = data_block_size * 3;
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
		.buf_size = g_bdev.blocklen,
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
	sb->length = spdk_bdev_get_data_block_size(&g_bdev) * 3;
	raid_bdev_sb_update_crc(sb);
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EAGAIN);
	ctx.buf_size = g_bdev.blocklen * 3;
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == 0);

	/* invalid base bdev slot number */
	prepare_sb(sb);
	sb->base_bdevs[0].slot = sb->num_base_bdevs = sb->base_bdevs_size = 2;
	raid_bdev_sb_update_crc(sb);
	CU_ASSERT(raid_bdev_parse_superblock(&ctx) == -EINVAL);
}

int
main(int argc, char **argv)
{
	unsigned int num_failures;
	CU_TestInfo tests[] = {
		{ "test_raid_bdev_write_superblock", test_raid_bdev_write_superblock },
		{ "test_raid_bdev_load_base_bdev_superblock", test_raid_bdev_load_base_bdev_superblock },
		{ "test_raid_bdev_parse_superblock", test_raid_bdev_parse_superblock },
		CU_TEST_INFO_NULL,
	};
	CU_SuiteInfo suites[] = {
		{ "raid_sb", test_setup, test_cleanup, NULL, NULL, tests },
		{ "raid_sb_md", test_setup_md, test_cleanup, NULL, NULL, tests },
		{ "raid_sb_md_interleaved", test_setup_md_interleaved, test_cleanup, NULL, NULL, tests },
		CU_SUITE_INFO_NULL,
	};

	CU_initialize_registry();
	CU_register_suites(suites);
	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
