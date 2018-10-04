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

#include "spdk_cunit.h"

#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

DEFINE_STUB(spdk_conf_find_section, struct spdk_conf_section *, (struct spdk_conf *cp,
		const char *name), NULL);
DEFINE_STUB(spdk_conf_section_get_nmval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx1, int idx2), NULL);
DEFINE_STUB(spdk_conf_section_get_intval, int, (struct spdk_conf_section *sp, const char *key), -1);

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_owner, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name, const char *short_name,
		uint16_t tpoint_id, uint8_t owner_type,
		uint8_t object_type, uint8_t new_object,
		uint8_t arg1_is_ptr, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));

static void
_bdev_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			 int *sc, int *sk, int *asc, int *ascq)
{
}

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

static int
stub_destruct(void *ctx)
{
	return 0;
}

struct ut_expected_io {
	uint8_t				type;
	uint64_t			offset;
	uint64_t			length;
	int				iovcnt;
	struct iovec			iov[BDEV_IO_NUM_CHILD_IOV];
	TAILQ_ENTRY(ut_expected_io)	link;
};

struct bdev_ut_channel {
	TAILQ_HEAD(, spdk_bdev_io)	outstanding_io;
	uint32_t			outstanding_io_count;
	TAILQ_HEAD(, ut_expected_io)	expected_io;
};

static bool g_io_done;
static enum spdk_bdev_io_status g_io_status;
static uint32_t g_bdev_ut_io_device;
static struct bdev_ut_channel *g_bdev_ut_channel;

static struct ut_expected_io *
ut_alloc_expected_io(uint8_t type, uint64_t offset, uint64_t length, int iovcnt)
{
	struct ut_expected_io *expected_io;

	expected_io = calloc(1, sizeof(*expected_io));
	SPDK_CU_ASSERT_FATAL(expected_io != NULL);

	expected_io->type = type;
	expected_io->offset = offset;
	expected_io->length = length;
	expected_io->iovcnt = iovcnt;

	return expected_io;
}

static void
ut_expected_io_set_iov(struct ut_expected_io *expected_io, int pos, void *base, size_t len)
{
	expected_io->iov[pos].iov_base = base;
	expected_io->iov[pos].iov_len = len;
}

static void
stub_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_ut_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct ut_expected_io *expected_io;
	struct iovec *iov, *expected_iov;
	int i;

	TAILQ_INSERT_TAIL(&ch->outstanding_io, bdev_io, module_link);
	ch->outstanding_io_count++;

	expected_io = TAILQ_FIRST(&ch->expected_io);
	if (expected_io == NULL) {
		return;
	}
	TAILQ_REMOVE(&ch->expected_io, expected_io, link);

	if (expected_io->type != SPDK_BDEV_IO_TYPE_INVALID) {
		CU_ASSERT(bdev_io->type == expected_io->type);
	}

	if (expected_io->length == 0) {
		free(expected_io);
		return;
	}

	CU_ASSERT(expected_io->offset == bdev_io->u.bdev.offset_blocks);
	CU_ASSERT(expected_io->length = bdev_io->u.bdev.num_blocks);

	if (expected_io->iovcnt == 0) {
		free(expected_io);
		/* UNMAP, WRITE_ZEROES and FLUSH don't have iovs, so we can just return now. */
		return;
	}

	CU_ASSERT(expected_io->iovcnt == bdev_io->u.bdev.iovcnt);
	for (i = 0; i < expected_io->iovcnt; i++) {
		iov = &bdev_io->u.bdev.iovs[i];
		expected_iov = &expected_io->iov[i];
		CU_ASSERT(iov->iov_len == expected_iov->iov_len);
		CU_ASSERT(iov->iov_base == expected_iov->iov_base);
	}

	free(expected_io);
}

static uint32_t
stub_complete_io(uint32_t num_to_complete)
{
	struct bdev_ut_channel *ch = g_bdev_ut_channel;
	struct spdk_bdev_io *bdev_io;
	uint32_t num_completed = 0;

	while (num_completed < num_to_complete) {
		if (TAILQ_EMPTY(&ch->outstanding_io)) {
			break;
		}
		bdev_io = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, bdev_io, module_link);
		ch->outstanding_io_count--;
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		num_completed++;
	}

	return num_completed;
}

static struct spdk_io_channel *
bdev_ut_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_bdev_ut_io_device);
}

static bool
stub_io_type_supported(void *_bdev, enum spdk_bdev_io_type io_type)
{
	return true;
}

static struct spdk_bdev_fn_table fn_table = {
	.destruct = stub_destruct,
	.submit_request = stub_submit_request,
	.get_io_channel = bdev_ut_get_io_channel,
	.io_type_supported = stub_io_type_supported,
};

static int
bdev_ut_create_ch(void *io_device, void *ctx_buf)
{
	struct bdev_ut_channel *ch = ctx_buf;

	CU_ASSERT(g_bdev_ut_channel == NULL);
	g_bdev_ut_channel = ch;

	TAILQ_INIT(&ch->outstanding_io);
	ch->outstanding_io_count = 0;
	TAILQ_INIT(&ch->expected_io);
	return 0;
}

static void
bdev_ut_destroy_ch(void *io_device, void *ctx_buf)
{
	CU_ASSERT(g_bdev_ut_channel != NULL);
	g_bdev_ut_channel = NULL;
}

static int
bdev_ut_module_init(void)
{
	spdk_io_device_register(&g_bdev_ut_io_device, bdev_ut_create_ch, bdev_ut_destroy_ch,
				sizeof(struct bdev_ut_channel), NULL);
	return 0;
}

static void
bdev_ut_module_fini(void)
{
	spdk_io_device_unregister(&g_bdev_ut_io_device, NULL);
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = bdev_ut_module_init,
	.module_fini = bdev_ut_module_fini,
};

static void vbdev_ut_examine(struct spdk_bdev *bdev);

static int
vbdev_ut_module_init(void)
{
	return 0;
}

static void
vbdev_ut_module_fini(void)
{
}

struct spdk_bdev_module vbdev_ut_if = {
	.name = "vbdev_ut",
	.module_init = vbdev_ut_module_init,
	.module_fini = vbdev_ut_module_fini,
	.examine_config = vbdev_ut_examine,
};

SPDK_BDEV_MODULE_REGISTER(&bdev_ut_if)
SPDK_BDEV_MODULE_REGISTER(&vbdev_ut_if)

static void
vbdev_ut_examine(struct spdk_bdev *bdev)
{
	spdk_bdev_module_examine_done(&vbdev_ut_if);
}

static struct spdk_bdev *
allocate_bdev(char *name)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = &bdev_ut_if;
	bdev->blockcnt = 1024;
	bdev->blocklen = 512;

	rc = spdk_bdev_register(bdev);
	CU_ASSERT(rc == 0);

	return bdev;
}

static struct spdk_bdev *
allocate_vbdev(char *name, struct spdk_bdev *base1, struct spdk_bdev *base2)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *array[2];
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = &vbdev_ut_if;

	/* vbdev must have at least one base bdev */
	CU_ASSERT(base1 != NULL);

	array[0] = base1;
	array[1] = base2;

	rc = spdk_vbdev_register(bdev, array, base2 == NULL ? 1 : 2);
	CU_ASSERT(rc == 0);

	return bdev;
}

static void
free_bdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev, NULL, NULL);
	memset(bdev, 0xFF, sizeof(*bdev));
	free(bdev);
}

static void
free_vbdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev, NULL, NULL);
	memset(bdev, 0xFF, sizeof(*bdev));
	free(bdev);
}

static void
get_device_stat_cb(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat, void *cb_arg, int rc)
{
	const char *bdev_name;

	CU_ASSERT(bdev != NULL);
	CU_ASSERT(rc == 0);
	bdev_name = spdk_bdev_get_name(bdev);
	CU_ASSERT_STRING_EQUAL(bdev_name, "bdev0");

	free(stat);
	free_bdev(bdev);
}

static void
get_device_stat_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_io_stat *stat;

	bdev = allocate_bdev("bdev0");
	stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
	if (stat == NULL) {
		free_bdev(bdev);
		return;
	}
	spdk_bdev_get_device_stat(bdev, stat, get_device_stat_cb, NULL);
}

static void
open_write_test(void)
{
	struct spdk_bdev *bdev[9];
	struct spdk_bdev_desc *desc[9] = {};
	int rc;

	/*
	 * Create a tree of bdevs to test various open w/ write cases.
	 *
	 * bdev0 through bdev3 are physical block devices, such as NVMe
	 * namespaces or Ceph block devices.
	 *
	 * bdev4 is a virtual bdev with multiple base bdevs.  This models
	 * caching or RAID use cases.
	 *
	 * bdev5 through bdev7 are all virtual bdevs with the same base
	 * bdev (except bdev7). This models partitioning or logical volume
	 * use cases.
	 *
	 * bdev7 is a virtual bdev with multiple base bdevs. One of base bdevs
	 * (bdev2) is shared with other virtual bdevs: bdev5 and bdev6. This
	 * models caching, RAID, partitioning or logical volumes use cases.
	 *
	 * bdev8 is a virtual bdev with multiple base bdevs, but these
	 * base bdevs are themselves virtual bdevs.
	 *
	 *                bdev8
	 *                  |
	 *            +----------+
	 *            |          |
	 *          bdev4      bdev5   bdev6   bdev7
	 *            |          |       |       |
	 *        +---+---+      +---+   +   +---+---+
	 *        |       |           \  |  /         \
	 *      bdev0   bdev1          bdev2         bdev3
	 */

	bdev[0] = allocate_bdev("bdev0");
	rc = spdk_bdev_module_claim_bdev(bdev[0], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[1] = allocate_bdev("bdev1");
	rc = spdk_bdev_module_claim_bdev(bdev[1], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[2] = allocate_bdev("bdev2");
	rc = spdk_bdev_module_claim_bdev(bdev[2], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[3] = allocate_bdev("bdev3");
	rc = spdk_bdev_module_claim_bdev(bdev[3], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[4] = allocate_vbdev("bdev4", bdev[0], bdev[1]);
	rc = spdk_bdev_module_claim_bdev(bdev[4], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[5] = allocate_vbdev("bdev5", bdev[2], NULL);
	rc = spdk_bdev_module_claim_bdev(bdev[5], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[6] = allocate_vbdev("bdev6", bdev[2], NULL);

	bdev[7] = allocate_vbdev("bdev7", bdev[2], bdev[3]);

	bdev[8] = allocate_vbdev("bdev8", bdev[4], bdev[5]);

	/* Open bdev0 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[0], false, NULL, NULL, &desc[0]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[0] != NULL);
	spdk_bdev_close(desc[0]);

	/*
	 * Open bdev1 read/write.  This should fail since bdev1 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[1], true, NULL, NULL, &desc[1]);
	CU_ASSERT(rc == -EPERM);

	/*
	 * Open bdev4 read/write.  This should fail since bdev3 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[4], true, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[4], false, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[4] != NULL);
	spdk_bdev_close(desc[4]);

	/*
	 * Open bdev8 read/write.  This should succeed since it is a leaf
	 * bdev.
	 */
	rc = spdk_bdev_open(bdev[8], true, NULL, NULL, &desc[8]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[8] != NULL);
	spdk_bdev_close(desc[8]);

	/*
	 * Open bdev5 read/write.  This should fail since bdev4 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[5], true, NULL, NULL, &desc[5]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[5], false, NULL, NULL, &desc[5]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[5] != NULL);
	spdk_bdev_close(desc[5]);

	free_vbdev(bdev[8]);

	free_vbdev(bdev[5]);
	free_vbdev(bdev[6]);
	free_vbdev(bdev[7]);

	free_vbdev(bdev[4]);

	free_bdev(bdev[0]);
	free_bdev(bdev[1]);
	free_bdev(bdev[2]);
	free_bdev(bdev[3]);
}

static void
bytes_to_blocks_test(void)
{
	struct spdk_bdev bdev;
	uint64_t offset_blocks, num_blocks;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;

	/* All parameters valid */
	offset_blocks = 0;
	num_blocks = 0;
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 1024, &num_blocks) == 0);
	CU_ASSERT(offset_blocks == 1);
	CU_ASSERT(num_blocks == 2);

	/* Offset not a block multiple */
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 3, &offset_blocks, 512, &num_blocks) != 0);

	/* Length not a block multiple */
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 3, &num_blocks) != 0);
}

static void
num_blocks_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_bdev_desc *desc = NULL;
	int rc;

	memset(&bdev, 0, sizeof(bdev));
	bdev.name = "num_blocks";
	bdev.fn_table = &fn_table;
	bdev.module = &bdev_ut_if;
	spdk_bdev_register(&bdev);
	spdk_bdev_notify_blockcnt_change(&bdev, 50);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 70) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 30) == 0);

	/* In case bdev opened */
	rc = spdk_bdev_open(&bdev, false, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 80) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 20) != 0);

	spdk_bdev_close(desc);
	spdk_bdev_unregister(&bdev, NULL, NULL);
}

static void
io_valid_test(void)
{
	struct spdk_bdev bdev;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;
	spdk_bdev_notify_blockcnt_change(&bdev, 100);

	/* All parameters valid */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 1, 2) == true);

	/* Last valid block */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 99, 1) == true);

	/* Offset past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 100, 1) == false);

	/* Offset + length past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 99, 2) == false);

	/* Offset near end of uint64_t range (2^64 - 1) */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 18446744073709551615ULL, 1) == false);
}

static void
alias_add_del_test(void)
{
	struct spdk_bdev *bdev[3];
	int rc;

	/* Creating and registering bdevs */
	bdev[0] = allocate_bdev("bdev0");
	SPDK_CU_ASSERT_FATAL(bdev[0] != 0);

	bdev[1] = allocate_bdev("bdev1");
	SPDK_CU_ASSERT_FATAL(bdev[1] != 0);

	bdev[2] = allocate_bdev("bdev2");
	SPDK_CU_ASSERT_FATAL(bdev[2] != 0);

	/*
	 * Trying adding an alias identical to name.
	 * Alias is identical to name, so it can not be added to aliases list
	 */
	rc = spdk_bdev_alias_add(bdev[0], bdev[0]->name);
	CU_ASSERT(rc == -EEXIST);

	/*
	 * Trying to add empty alias,
	 * this one should fail
	 */
	rc = spdk_bdev_alias_add(bdev[0], NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Trying adding same alias to two different registered bdevs */

	/* Alias is used first time, so this one should pass */
	rc = spdk_bdev_alias_add(bdev[0], "proper alias 0");
	CU_ASSERT(rc == 0);

	/* Alias was added to another bdev, so this one should fail */
	rc = spdk_bdev_alias_add(bdev[1], "proper alias 0");
	CU_ASSERT(rc == -EEXIST);

	/* Alias is used first time, so this one should pass */
	rc = spdk_bdev_alias_add(bdev[1], "proper alias 1");
	CU_ASSERT(rc == 0);

	/* Trying removing an alias from registered bdevs */

	/* Alias is not on a bdev aliases list, so this one should fail */
	rc = spdk_bdev_alias_del(bdev[0], "not existing");
	CU_ASSERT(rc == -ENOENT);

	/* Alias is present on a bdev aliases list, so this one should pass */
	rc = spdk_bdev_alias_del(bdev[0], "proper alias 0");
	CU_ASSERT(rc == 0);

	/* Alias is present on a bdev aliases list, so this one should pass */
	rc = spdk_bdev_alias_del(bdev[1], "proper alias 1");
	CU_ASSERT(rc == 0);

	/* Trying to remove name instead of alias, so this one should fail, name cannot be changed or removed */
	rc = spdk_bdev_alias_del(bdev[0], bdev[0]->name);
	CU_ASSERT(rc != 0);

	/* Trying to del all alias from empty alias list */
	spdk_bdev_alias_del_all(bdev[2]);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&bdev[2]->aliases));

	/* Trying to del all alias from non-empty alias list */
	rc = spdk_bdev_alias_add(bdev[2], "alias0");
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_alias_add(bdev[2], "alias1");
	CU_ASSERT(rc == 0);
	spdk_bdev_alias_del_all(bdev[2]);
	CU_ASSERT(TAILQ_EMPTY(&bdev[2]->aliases));

	/* Unregister and free bdevs */
	spdk_bdev_unregister(bdev[0], NULL, NULL);
	spdk_bdev_unregister(bdev[1], NULL, NULL);
	spdk_bdev_unregister(bdev[2], NULL, NULL);

	free(bdev[0]);
	free(bdev[1]);
	free(bdev[2]);
}

static void
io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_io_done = true;
	g_io_status = bdev_io->internal.status;
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_init_cb(void *arg, int rc)
{
	CU_ASSERT(rc == 0);
}

static void
bdev_fini_cb(void *arg)
{
}

struct bdev_ut_io_wait_entry {
	struct spdk_bdev_io_wait_entry	entry;
	struct spdk_io_channel		*io_ch;
	struct spdk_bdev_desc		*desc;
	bool				submitted;
};

static void
io_wait_cb(void *arg)
{
	struct bdev_ut_io_wait_entry *entry = arg;
	int rc;

	rc = spdk_bdev_read_blocks(entry->desc, entry->io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	entry->submitted = true;
}

static void
bdev_io_wait_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 4,
		.bdev_io_cache_size = 2,
	};
	struct bdev_ut_io_wait_entry io_wait_entry;
	struct bdev_ut_io_wait_entry io_wait_entry2;
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == -ENOMEM);

	io_wait_entry.entry.bdev = bdev;
	io_wait_entry.entry.cb_fn = io_wait_cb;
	io_wait_entry.entry.cb_arg = &io_wait_entry;
	io_wait_entry.io_ch = io_ch;
	io_wait_entry.desc = desc;
	io_wait_entry.submitted = false;
	/* Cannot use the same io_wait_entry for two different calls. */
	memcpy(&io_wait_entry2, &io_wait_entry, sizeof(io_wait_entry));
	io_wait_entry2.entry.cb_arg = &io_wait_entry2;

	/* Queue two I/O waits. */
	rc = spdk_bdev_queue_io_wait(bdev, io_ch, &io_wait_entry.entry);
	CU_ASSERT(rc == 0);
	CU_ASSERT(io_wait_entry.submitted == false);
	rc = spdk_bdev_queue_io_wait(bdev, io_ch, &io_wait_entry2.entry);
	CU_ASSERT(rc == 0);
	CU_ASSERT(io_wait_entry2.submitted == false);

	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);
	CU_ASSERT(io_wait_entry.submitted == true);
	CU_ASSERT(io_wait_entry2.submitted == false);

	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);
	CU_ASSERT(io_wait_entry2.submitted == true);

	stub_complete_io(4);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
}

static void
bdev_io_spans_boundary_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_bdev_io bdev_io;

	memset(&bdev, 0, sizeof(bdev));

	bdev.optimal_io_boundary = 0;
	bdev_io.bdev = &bdev;

	/* bdev has no optimal_io_boundary set - so this should return false. */
	CU_ASSERT(_spdk_bdev_io_should_split(&bdev_io) == false);

	bdev.optimal_io_boundary = 32;
	bdev_io.type = SPDK_BDEV_IO_TYPE_RESET;

	/* RESETs are not based on LBAs - so this should return false. */
	CU_ASSERT(_spdk_bdev_io_should_split(&bdev_io) == false);

	bdev_io.type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io.u.bdev.offset_blocks = 0;
	bdev_io.u.bdev.num_blocks = 32;

	/* This I/O run right up to, but does not cross, the boundary - so this should return false. */
	CU_ASSERT(_spdk_bdev_io_should_split(&bdev_io) == false);

	bdev_io.u.bdev.num_blocks = 33;

	/* This I/O spans a boundary. */
	CU_ASSERT(_spdk_bdev_io_should_split(&bdev_io) == true);
}

static void
bdev_io_split(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 512,
		.bdev_io_cache_size = 64,
	};
	struct iovec iov[BDEV_IO_NUM_CHILD_IOV * 2];
	struct ut_expected_io *expected_io;
	uint64_t i;
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	bdev->optimal_io_boundary = 16;
	bdev->split_on_optimal_io_boundary = false;

	g_io_done = false;

	/* First test that the I/O does not get split if split_on_optimal_io_boundary == false. */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 8, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 8 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 8, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	bdev->split_on_optimal_io_boundary = true;

	/* Now test that a single-vector command is split correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 *
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 2 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 6, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 2 * 512), 6 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* spdk_bdev_read_blocks will submit the first child immediately. */
	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 8, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Now set up a more complex, multi-vector command that needs to be split,
	 *  including splitting iovecs.
	 */
	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 512;
	iov[1].iov_base = (void *)0x20000;
	iov[1].iov_len = 20 * 512;
	iov[2].iov_base = (void *)0x30000;
	iov[2].iov_len = 11 * 512;

	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 14, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)0x10000, 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)0x20000, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 16, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0x20000 + 512), 16 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 14, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0x20000 + 17 * 512), 3 * 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)0x30000, 11 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks(desc, io_ch, iov, 3, 14, 32, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 3);
	stub_complete_io(3);
	CU_ASSERT(g_io_done == true);

	/* Test multi vector command that needs to be split by strip and then needs to be
	 * split further due to the capacity of child iovs.
	 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV * 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}

	bdev->optimal_io_boundary = BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, BDEV_IO_NUM_CHILD_IOV,
					   BDEV_IO_NUM_CHILD_IOV);
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i, (void *)((i + 1) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV,
					   BDEV_IO_NUM_CHILD_IOV, BDEV_IO_NUM_CHILD_IOV);
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1 + BDEV_IO_NUM_CHILD_IOV) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV * 2, 0,
				    BDEV_IO_NUM_CHILD_IOV * 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test multi vector command that needs to be split by strip and then needs to be
	 * split further due to the capacity of child iovs, but fails to split. The cause
	 * of failure of split is that the length of an iovec is not multiple of block size.
	 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_base = (void *)(BDEV_IO_NUM_CHILD_IOV * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_len = 256;

	bdev->optimal_io_boundary = BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	g_io_status = 0;

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV * 2, 0,
				    BDEV_IO_NUM_CHILD_IOV * 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);

	/* Test a WRITE_ZEROES that would span an I/O boundary.  WRITE_ZEROES should not be
	 * split, so test that.
	 */
	bdev->optimal_io_boundary = 15;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, 9, 36, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_write_zeroes_blocks(desc, io_ch, 9, 36, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	/* Test an UNMAP.  This should also not be split. */
	bdev->optimal_io_boundary = 16;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_UNMAP, 15, 2, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_unmap_blocks(desc, io_ch, 15, 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	/* Test a FLUSH.  This should also not be split. */
	bdev->optimal_io_boundary = 16;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_FLUSH, 15, 2, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_flush_blocks(desc, io_ch, 15, 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	CU_ASSERT(TAILQ_EMPTY(&g_bdev_ut_channel->expected_io));

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
}

static void
bdev_io_split_with_io_wait(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *channel;
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 2,
		.bdev_io_cache_size = 1,
	};
	struct iovec iov[3];
	struct ut_expected_io *expected_io;
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);
	channel = spdk_io_channel_get_ctx(io_ch);
	mgmt_ch = channel->shared_resource->mgmt_ch;

	bdev->optimal_io_boundary = 16;
	bdev->split_on_optimal_io_boundary = true;

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);

	/* Now test that a single-vector command is split correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 *
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 2 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 6, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 2 * 512), 6 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The following children will be submitted sequentially due to the capacity of
	 * spdk_bdev_io.
	 */

	/* The first child I/O will be queued to wait until an spdk_bdev_io becomes available */
	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 8, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&mgmt_ch->io_wait_queue));
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);

	/* Completing the first read I/O will submit the first child */
	stub_complete_io(1);
	CU_ASSERT(TAILQ_EMPTY(&mgmt_ch->io_wait_queue));
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);

	/* Completing the first child will submit the second child */
	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);

	/* Complete the second child I/O.  This should result in our callback getting
	 * invoked since the parent I/O is now complete.
	 */
	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Now set up a more complex, multi-vector command that needs to be split,
	 *  including splitting iovecs.
	 */
	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 512;
	iov[1].iov_base = (void *)0x20000;
	iov[1].iov_len = 20 * 512;
	iov[2].iov_base = (void *)0x30000;
	iov[2].iov_len = 11 * 512;

	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 14, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)0x10000, 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)0x20000, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 16, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0x20000 + 512), 16 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 14, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0x20000 + 17 * 512), 3 * 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)0x30000, 11 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks(desc, io_ch, iov, 3, 14, 32, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	/* The following children will be submitted sequentially due to the capacity of
	 * spdk_bdev_io.
	 */

	/* Completing the first child will submit the second child */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	/* Completing the second child will submit the third child */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	/* Completing the third child will result in our callback getting invoked
	 * since the parent I/O is now complete.
	 */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	CU_ASSERT(TAILQ_EMPTY(&g_bdev_ut_channel->expected_io));

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "bytes_to_blocks_test", bytes_to_blocks_test) == NULL ||
		CU_add_test(suite, "num_blocks_test", num_blocks_test) == NULL ||
		CU_add_test(suite, "io_valid", io_valid_test) == NULL ||
		CU_add_test(suite, "open_write", open_write_test) == NULL ||
		CU_add_test(suite, "alias_add_del", alias_add_del_test) == NULL ||
		CU_add_test(suite, "get_device_stat", get_device_stat_test) == NULL ||
		CU_add_test(suite, "bdev_io_wait", bdev_io_wait_test) == NULL ||
		CU_add_test(suite, "bdev_io_spans_boundary", bdev_io_spans_boundary_test) == NULL ||
		CU_add_test(suite, "bdev_io_split", bdev_io_split) == NULL ||
		CU_add_test(suite, "bdev_io_split_with_io_wait", bdev_io_split_with_io_wait) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	spdk_allocate_thread(_bdev_send_msg, NULL, NULL, NULL, "thread0");
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();
	return num_failures;
}
