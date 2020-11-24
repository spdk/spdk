/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_owner, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name,
		uint16_t tpoint_id, uint8_t owner_type,
		uint8_t object_type, uint8_t new_object,
		uint8_t arg1_type, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));
DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);


int g_status;
int g_count;
enum spdk_bdev_event_type g_event_type1;
enum spdk_bdev_event_type g_event_type2;
struct spdk_histogram_data *g_histogram;
void *g_unregister_arg;
int g_unregister_rc;

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
	void				*md_buf;
	TAILQ_ENTRY(ut_expected_io)	link;
};

struct bdev_ut_channel {
	TAILQ_HEAD(, spdk_bdev_io)	outstanding_io;
	uint32_t			outstanding_io_count;
	TAILQ_HEAD(, ut_expected_io)	expected_io;
};

static bool g_io_done;
static struct spdk_bdev_io *g_bdev_io;
static enum spdk_bdev_io_status g_io_status;
static enum spdk_bdev_io_status g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;
static uint32_t g_bdev_ut_io_device;
static struct bdev_ut_channel *g_bdev_ut_channel;
static void *g_compare_read_buf;
static uint32_t g_compare_read_buf_len;
static void *g_compare_write_buf;
static uint32_t g_compare_write_buf_len;
static bool g_abort_done;
static enum spdk_bdev_io_status g_abort_status;

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
	struct spdk_bdev_io *bio_to_abort;
	int i;

	g_bdev_io = bdev_io;

	if (g_compare_read_buf && bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		uint32_t len = bdev_io->u.bdev.iovs[0].iov_len;

		CU_ASSERT(bdev_io->u.bdev.iovcnt == 1);
		CU_ASSERT(g_compare_read_buf_len == len);
		memcpy(bdev_io->u.bdev.iovs[0].iov_base, g_compare_read_buf, len);
	}

	if (g_compare_write_buf && bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		uint32_t len = bdev_io->u.bdev.iovs[0].iov_len;

		CU_ASSERT(bdev_io->u.bdev.iovcnt == 1);
		CU_ASSERT(g_compare_write_buf_len == len);
		memcpy(g_compare_write_buf, bdev_io->u.bdev.iovs[0].iov_base, len);
	}

	if (g_compare_read_buf && bdev_io->type == SPDK_BDEV_IO_TYPE_COMPARE) {
		uint32_t len = bdev_io->u.bdev.iovs[0].iov_len;

		CU_ASSERT(bdev_io->u.bdev.iovcnt == 1);
		CU_ASSERT(g_compare_read_buf_len == len);
		if (memcmp(bdev_io->u.bdev.iovs[0].iov_base, g_compare_read_buf, len)) {
			g_io_exp_status = SPDK_BDEV_IO_STATUS_MISCOMPARE;
		}
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT) {
		if (g_io_exp_status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			TAILQ_FOREACH(bio_to_abort, &ch->outstanding_io, module_link) {
				if (bio_to_abort == bdev_io->u.abort.bio_to_abort) {
					TAILQ_REMOVE(&ch->outstanding_io, bio_to_abort, module_link);
					ch->outstanding_io_count--;
					spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_FAILED);
					break;
				}
			}
		}
	}

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

	if (expected_io->md_buf != NULL) {
		CU_ASSERT(expected_io->md_buf == bdev_io->u.bdev.md_buf);
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

static void
stub_submit_request_get_buf_cb(struct spdk_io_channel *_ch,
			       struct spdk_bdev_io *bdev_io, bool success)
{
	CU_ASSERT(success == true);

	stub_submit_request(_ch, bdev_io);
}

static void
stub_submit_request_get_buf(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	spdk_bdev_io_get_buf(bdev_io, stub_submit_request_get_buf_cb,
			     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}

static uint32_t
stub_complete_io(uint32_t num_to_complete)
{
	struct bdev_ut_channel *ch = g_bdev_ut_channel;
	struct spdk_bdev_io *bdev_io;
	static enum spdk_bdev_io_status io_status;
	uint32_t num_completed = 0;

	while (num_completed < num_to_complete) {
		if (TAILQ_EMPTY(&ch->outstanding_io)) {
			break;
		}
		bdev_io = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, bdev_io, module_link);
		ch->outstanding_io_count--;
		io_status = g_io_exp_status == SPDK_BDEV_IO_STATUS_SUCCESS ? SPDK_BDEV_IO_STATUS_SUCCESS :
			    g_io_exp_status;
		spdk_bdev_io_complete(bdev_io, io_status);
		num_completed++;
	}

	return num_completed;
}

static struct spdk_io_channel *
bdev_ut_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_bdev_ut_io_device);
}

static bool g_io_types_supported[SPDK_BDEV_NUM_IO_TYPES] = {
	[SPDK_BDEV_IO_TYPE_READ]		= true,
	[SPDK_BDEV_IO_TYPE_WRITE]		= true,
	[SPDK_BDEV_IO_TYPE_COMPARE]		= true,
	[SPDK_BDEV_IO_TYPE_UNMAP]		= true,
	[SPDK_BDEV_IO_TYPE_FLUSH]		= true,
	[SPDK_BDEV_IO_TYPE_RESET]		= true,
	[SPDK_BDEV_IO_TYPE_NVME_ADMIN]		= true,
	[SPDK_BDEV_IO_TYPE_NVME_IO]		= true,
	[SPDK_BDEV_IO_TYPE_NVME_IO_MD]		= true,
	[SPDK_BDEV_IO_TYPE_WRITE_ZEROES]	= true,
	[SPDK_BDEV_IO_TYPE_ZCOPY]		= true,
	[SPDK_BDEV_IO_TYPE_ABORT]		= true,
};

static void
ut_enable_io_type(enum spdk_bdev_io_type io_type, bool enable)
{
	g_io_types_supported[io_type] = enable;
}

static bool
stub_io_type_supported(void *_bdev, enum spdk_bdev_io_type io_type)
{
	return g_io_types_supported[io_type];
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

struct spdk_bdev_module bdev_ut_if;

static int
bdev_ut_module_init(void)
{
	spdk_io_device_register(&g_bdev_ut_io_device, bdev_ut_create_ch, bdev_ut_destroy_ch,
				sizeof(struct bdev_ut_channel), NULL);
	spdk_bdev_module_init_done(&bdev_ut_if);
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
	.async_init = true,
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

SPDK_BDEV_MODULE_REGISTER(bdev_ut, &bdev_ut_if)
SPDK_BDEV_MODULE_REGISTER(vbdev_ut, &vbdev_ut_if)

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
allocate_vbdev(char *name)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = &vbdev_ut_if;

	rc = spdk_bdev_register(bdev);
	CU_ASSERT(rc == 0);

	return bdev;
}

static void
free_bdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev, NULL, NULL);
	poll_threads();
	memset(bdev, 0xFF, sizeof(*bdev));
	free(bdev);
}

static void
free_vbdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev, NULL, NULL);
	poll_threads();
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

	*(bool *)cb_arg = true;
}

static void
bdev_unregister_cb(void *cb_arg, int rc)
{
	g_unregister_arg = cb_arg;
	g_unregister_rc = rc;
}

static void
bdev_ut_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

static void
bdev_open_cb1(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct spdk_bdev_desc *desc = *(struct spdk_bdev_desc **)event_ctx;

	g_event_type1 = type;
	if (SPDK_BDEV_EVENT_REMOVE == type) {
		spdk_bdev_close(desc);
	}
}

static void
bdev_open_cb2(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct spdk_bdev_desc *desc = *(struct spdk_bdev_desc **)event_ctx;

	g_event_type2 = type;
	if (SPDK_BDEV_EVENT_REMOVE == type) {
		spdk_bdev_close(desc);
	}
}

static void
get_device_stat_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_io_stat *stat;
	bool done;

	bdev = allocate_bdev("bdev0");
	stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
	if (stat == NULL) {
		free_bdev(bdev);
		return;
	}

	done = false;
	spdk_bdev_get_device_stat(bdev, stat, get_device_stat_cb, &done);
	while (!done) { poll_threads(); }

	free_bdev(bdev);
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

	bdev[4] = allocate_vbdev("bdev4");
	rc = spdk_bdev_module_claim_bdev(bdev[4], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[5] = allocate_vbdev("bdev5");
	rc = spdk_bdev_module_claim_bdev(bdev[5], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[6] = allocate_vbdev("bdev6");

	bdev[7] = allocate_vbdev("bdev7");

	bdev[8] = allocate_vbdev("bdev8");

	/* Open bdev0 read-only.  This should succeed. */
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc[0]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[0] != NULL);
	CU_ASSERT(bdev[0] == spdk_bdev_desc_get_bdev(desc[0]));
	spdk_bdev_close(desc[0]);

	/*
	 * Open bdev1 read/write.  This should fail since bdev1 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open_ext("bdev1", true, bdev_ut_event_cb, NULL, &desc[1]);
	CU_ASSERT(rc == -EPERM);

	/*
	 * Open bdev4 read/write.  This should fail since bdev3 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open_ext("bdev4", true, bdev_ut_event_cb, NULL, &desc[4]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open_ext("bdev4", false, bdev_ut_event_cb, NULL, &desc[4]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[4] != NULL);
	CU_ASSERT(bdev[4] == spdk_bdev_desc_get_bdev(desc[4]));
	spdk_bdev_close(desc[4]);

	/*
	 * Open bdev8 read/write.  This should succeed since it is a leaf
	 * bdev.
	 */
	rc = spdk_bdev_open_ext("bdev8", true, bdev_ut_event_cb, NULL, &desc[8]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[8] != NULL);
	CU_ASSERT(bdev[8] == spdk_bdev_desc_get_bdev(desc[8]));
	spdk_bdev_close(desc[8]);

	/*
	 * Open bdev5 read/write.  This should fail since bdev4 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open_ext("bdev5", true, bdev_ut_event_cb, NULL, &desc[5]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open_ext("bdev5", false, bdev_ut_event_cb, NULL, &desc[5]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[5] != NULL);
	CU_ASSERT(bdev[5] == spdk_bdev_desc_get_bdev(desc[5]));
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
	CU_ASSERT(bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 1024, &num_blocks) == 0);
	CU_ASSERT(offset_blocks == 1);
	CU_ASSERT(num_blocks == 2);

	/* Offset not a block multiple */
	CU_ASSERT(bdev_bytes_to_blocks(&bdev, 3, &offset_blocks, 512, &num_blocks) != 0);

	/* Length not a block multiple */
	CU_ASSERT(bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 3, &num_blocks) != 0);

	/* In case blocklen not the power of two */
	bdev.blocklen = 100;
	CU_ASSERT(bdev_bytes_to_blocks(&bdev, 100, &offset_blocks, 200, &num_blocks) == 0);
	CU_ASSERT(offset_blocks == 1);
	CU_ASSERT(num_blocks == 2);

	/* Offset not a block multiple */
	CU_ASSERT(bdev_bytes_to_blocks(&bdev, 3, &offset_blocks, 100, &num_blocks) != 0);

	/* Length not a block multiple */
	CU_ASSERT(bdev_bytes_to_blocks(&bdev, 100, &offset_blocks, 3, &num_blocks) != 0);
}

static void
num_blocks_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_bdev_desc *desc_ext = NULL;
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

	/* In case bdev opened with ext API */
	rc = spdk_bdev_open_ext("num_blocks", false, bdev_open_cb1, &desc_ext, &desc_ext);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc_ext != NULL);
	CU_ASSERT(&bdev == spdk_bdev_desc_get_bdev(desc_ext));

	g_event_type1 = 0xFF;
	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 90) == 0);

	poll_threads();
	CU_ASSERT_EQUAL(g_event_type1, SPDK_BDEV_EVENT_RESIZE);

	g_event_type1 = 0xFF;
	/* Growing block number and closing */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 100) == 0);

	spdk_bdev_close(desc);
	spdk_bdev_close(desc_ext);
	spdk_bdev_unregister(&bdev, NULL, NULL);

	poll_threads();

	/* Callback is not called for closed device */
	CU_ASSERT_EQUAL(g_event_type1, 0xFF);
}

static void
io_valid_test(void)
{
	struct spdk_bdev bdev;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;
	spdk_bdev_notify_blockcnt_change(&bdev, 100);

	/* All parameters valid */
	CU_ASSERT(bdev_io_valid_blocks(&bdev, 1, 2) == true);

	/* Last valid block */
	CU_ASSERT(bdev_io_valid_blocks(&bdev, 99, 1) == true);

	/* Offset past end of bdev */
	CU_ASSERT(bdev_io_valid_blocks(&bdev, 100, 1) == false);

	/* Offset + length past end of bdev */
	CU_ASSERT(bdev_io_valid_blocks(&bdev, 99, 2) == false);

	/* Offset near end of uint64_t range (2^64 - 1) */
	CU_ASSERT(bdev_io_valid_blocks(&bdev, 18446744073709551615ULL, 1) == false);
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

	poll_threads();

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

	poll_threads();

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
bdev_io_types_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 4,
		.bdev_io_cache_size = 2,
	};
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);
	poll_threads();

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* WRITE and WRITE ZEROES are not supported */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, false);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_WRITE, false);
	rc = spdk_bdev_write_zeroes_blocks(desc, io_ch, 0, 128, io_done, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, true);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_WRITE, true);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
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
	poll_threads();

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
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
	poll_threads();
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
	CU_ASSERT(bdev_io_should_split(&bdev_io) == false);

	bdev.optimal_io_boundary = 32;
	bdev_io.type = SPDK_BDEV_IO_TYPE_RESET;

	/* RESETs are not based on LBAs - so this should return false. */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == false);

	bdev_io.type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io.u.bdev.offset_blocks = 0;
	bdev_io.u.bdev.num_blocks = 32;

	/* This I/O run right up to, but does not cross, the boundary - so this should return false. */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == false);

	bdev_io.u.bdev.num_blocks = 33;

	/* This I/O spans a boundary. */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == true);
}

static void
bdev_io_split_test(void)
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
	void *md_buf = (void *)0xFF000000;
	uint64_t i;
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
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
	bdev->md_interleave = false;
	bdev->md_len = 8;

	/* Now test that a single-vector command is split correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 *
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 2, 1);
	expected_io->md_buf = md_buf;
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 2 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 6, 1);
	expected_io->md_buf = md_buf + 2 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 2 * 512), 6 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* spdk_bdev_read_blocks will submit the first child immediately. */
	rc = spdk_bdev_read_blocks_with_md(desc, io_ch, (void *)0xF000, md_buf,
					   14, 8, io_done, NULL);
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
	expected_io->md_buf = md_buf;
	ut_expected_io_set_iov(expected_io, 0, (void *)0x10000, 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)0x20000, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 16, 1);
	expected_io->md_buf = md_buf + 2 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)(0x20000 + 512), 16 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 14, 2);
	expected_io->md_buf = md_buf + 18 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)(0x20000 + 17 * 512), 3 * 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)0x30000, 11 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks_with_md(desc, io_ch, iov, 3, md_buf,
					     14, 32, io_done, NULL);
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
	expected_io->md_buf = md_buf;
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i, (void *)((i + 1) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV,
					   BDEV_IO_NUM_CHILD_IOV, BDEV_IO_NUM_CHILD_IOV);
	expected_io->md_buf = md_buf + BDEV_IO_NUM_CHILD_IOV * 8;
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1 + BDEV_IO_NUM_CHILD_IOV) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV * 2, md_buf,
					    0, BDEV_IO_NUM_CHILD_IOV * 2, io_done, NULL);
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
	 * split further due to the capacity of child iovs. In this case, the length of
	 * the rest of iovec array with an I/O boundary is the multiple of block size.
	 */

	/* Fill iovec array for exactly one boundary. The iovec cnt for this boundary
	 * is BDEV_IO_NUM_CHILD_IOV + 1, which exceeds the capacity of child iovs.
	 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	for (i = BDEV_IO_NUM_CHILD_IOV - 2; i < BDEV_IO_NUM_CHILD_IOV; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 256;
	}
	iov[BDEV_IO_NUM_CHILD_IOV].iov_base = (void *)((BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV].iov_len = 512;

	/* Add an extra iovec to trigger split */
	iov[BDEV_IO_NUM_CHILD_IOV + 1].iov_base = (void *)((BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV + 1].iov_len = 512;

	bdev->optimal_io_boundary = BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0,
					   BDEV_IO_NUM_CHILD_IOV - 1, BDEV_IO_NUM_CHILD_IOV);
	expected_io->md_buf = md_buf;
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1) * 0x10000), 512);
	}
	for (i = BDEV_IO_NUM_CHILD_IOV - 2; i < BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1) * 0x10000), 256);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV - 1,
					   1, 1);
	expected_io->md_buf = md_buf + (BDEV_IO_NUM_CHILD_IOV - 1) * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)((BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV,
					   1, 1);
	expected_io->md_buf = md_buf + BDEV_IO_NUM_CHILD_IOV * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)((BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV + 2, md_buf,
					    0, BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test multi vector command that needs to be split by strip and then needs to be
	 * split further due to the capacity of child iovs, the child request offset should
	 * be rewind to last aligned offset and go success without error.
	 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_base = (void *)(BDEV_IO_NUM_CHILD_IOV * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_len = 256;

	iov[BDEV_IO_NUM_CHILD_IOV].iov_base = (void *)((BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV].iov_len = 256;

	iov[BDEV_IO_NUM_CHILD_IOV + 1].iov_base = (void *)((BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV + 1].iov_len = 512;

	bdev->optimal_io_boundary = BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	g_io_status = 0;
	/* The first expected io should be start from offset 0 to BDEV_IO_NUM_CHILD_IOV - 1 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0,
					   BDEV_IO_NUM_CHILD_IOV - 1, BDEV_IO_NUM_CHILD_IOV - 1);
	expected_io->md_buf = md_buf;
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	/* The second expected io should be start from offset BDEV_IO_NUM_CHILD_IOV - 1 to BDEV_IO_NUM_CHILD_IOV */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV - 1,
					   1, 2);
	expected_io->md_buf = md_buf + (BDEV_IO_NUM_CHILD_IOV - 1) * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)(BDEV_IO_NUM_CHILD_IOV * 0x10000), 256);
	ut_expected_io_set_iov(expected_io, 1,
			       (void *)((BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000), 256);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	/* The third expected io should be start from offset BDEV_IO_NUM_CHILD_IOV to BDEV_IO_NUM_CHILD_IOV + 1 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV,
					   1, 1);
	expected_io->md_buf = md_buf + BDEV_IO_NUM_CHILD_IOV * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)((BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV * 2, md_buf,
					    0, BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test multi vector command that needs to be split due to the IO boundary and
	 * the capacity of child iovs. Especially test the case when the command is
	 * split due to the capacity of child iovs, the tail address is not aligned with
	 * block size and is rewinded to the aligned address.
	 *
	 * The iovecs used in read request is complex but is based on the data
	 * collected in the real issue. We change the base addresses but keep the lengths
	 * not to loose the credibility of the test.
	 */
	bdev->optimal_io_boundary = 128;
	g_io_done = false;
	g_io_status = 0;

	for (i = 0; i < 31; i++) {
		iov[i].iov_base = (void *)(0xFEED0000000 + (i << 20));
		iov[i].iov_len = 1024;
	}
	iov[31].iov_base = (void *)0xFEED1F00000;
	iov[31].iov_len = 32768;
	iov[32].iov_base = (void *)0xFEED2000000;
	iov[32].iov_len = 160;
	iov[33].iov_base = (void *)0xFEED2100000;
	iov[33].iov_len = 4096;
	iov[34].iov_base = (void *)0xFEED2200000;
	iov[34].iov_len = 4096;
	iov[35].iov_base = (void *)0xFEED2300000;
	iov[35].iov_len = 4096;
	iov[36].iov_base = (void *)0xFEED2400000;
	iov[36].iov_len = 4096;
	iov[37].iov_base = (void *)0xFEED2500000;
	iov[37].iov_len = 4096;
	iov[38].iov_base = (void *)0xFEED2600000;
	iov[38].iov_len = 4096;
	iov[39].iov_base = (void *)0xFEED2700000;
	iov[39].iov_len = 4096;
	iov[40].iov_base = (void *)0xFEED2800000;
	iov[40].iov_len = 4096;
	iov[41].iov_base = (void *)0xFEED2900000;
	iov[41].iov_len = 4096;
	iov[42].iov_base = (void *)0xFEED2A00000;
	iov[42].iov_len = 4096;
	iov[43].iov_base = (void *)0xFEED2B00000;
	iov[43].iov_len = 12288;
	iov[44].iov_base = (void *)0xFEED2C00000;
	iov[44].iov_len = 8192;
	iov[45].iov_base = (void *)0xFEED2F00000;
	iov[45].iov_len = 4096;
	iov[46].iov_base = (void *)0xFEED3000000;
	iov[46].iov_len = 4096;
	iov[47].iov_base = (void *)0xFEED3100000;
	iov[47].iov_len = 4096;
	iov[48].iov_base = (void *)0xFEED3200000;
	iov[48].iov_len = 24576;
	iov[49].iov_base = (void *)0xFEED3300000;
	iov[49].iov_len = 16384;
	iov[50].iov_base = (void *)0xFEED3400000;
	iov[50].iov_len = 12288;
	iov[51].iov_base = (void *)0xFEED3500000;
	iov[51].iov_len = 4096;
	iov[52].iov_base = (void *)0xFEED3600000;
	iov[52].iov_len = 4096;
	iov[53].iov_base = (void *)0xFEED3700000;
	iov[53].iov_len = 4096;
	iov[54].iov_base = (void *)0xFEED3800000;
	iov[54].iov_len = 28672;
	iov[55].iov_base = (void *)0xFEED3900000;
	iov[55].iov_len = 20480;
	iov[56].iov_base = (void *)0xFEED3A00000;
	iov[56].iov_len = 4096;
	iov[57].iov_base = (void *)0xFEED3B00000;
	iov[57].iov_len = 12288;
	iov[58].iov_base = (void *)0xFEED3C00000;
	iov[58].iov_len = 4096;
	iov[59].iov_base = (void *)0xFEED3D00000;
	iov[59].iov_len = 4096;
	iov[60].iov_base = (void *)0xFEED3E00000;
	iov[60].iov_len = 352;

	/* The 1st child IO must be from iov[0] to iov[31] split by the capacity
	 * of child iovs,
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, 126, 32);
	expected_io->md_buf = md_buf;
	for (i = 0; i < 32; i++) {
		ut_expected_io_set_iov(expected_io, i, iov[i].iov_base, iov[i].iov_len);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 2nd child IO must be from iov[32] to the first 864 bytes of iov[33]
	 * split by the IO boundary requirement.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 126, 2, 2);
	expected_io->md_buf = md_buf + 126 * 8;
	ut_expected_io_set_iov(expected_io, 0, iov[32].iov_base, iov[32].iov_len);
	ut_expected_io_set_iov(expected_io, 1, iov[33].iov_base, 864);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 3rd child IO must be from the remaining 3232 bytes of iov[33] to
	 * the first 864 bytes of iov[46] split by the IO boundary requirement.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 128, 128, 14);
	expected_io->md_buf = md_buf + 128 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)((uintptr_t)iov[33].iov_base + 864),
			       iov[33].iov_len - 864);
	ut_expected_io_set_iov(expected_io, 1, iov[34].iov_base, iov[34].iov_len);
	ut_expected_io_set_iov(expected_io, 2, iov[35].iov_base, iov[35].iov_len);
	ut_expected_io_set_iov(expected_io, 3, iov[36].iov_base, iov[36].iov_len);
	ut_expected_io_set_iov(expected_io, 4, iov[37].iov_base, iov[37].iov_len);
	ut_expected_io_set_iov(expected_io, 5, iov[38].iov_base, iov[38].iov_len);
	ut_expected_io_set_iov(expected_io, 6, iov[39].iov_base, iov[39].iov_len);
	ut_expected_io_set_iov(expected_io, 7, iov[40].iov_base, iov[40].iov_len);
	ut_expected_io_set_iov(expected_io, 8, iov[41].iov_base, iov[41].iov_len);
	ut_expected_io_set_iov(expected_io, 9, iov[42].iov_base, iov[42].iov_len);
	ut_expected_io_set_iov(expected_io, 10, iov[43].iov_base, iov[43].iov_len);
	ut_expected_io_set_iov(expected_io, 11, iov[44].iov_base, iov[44].iov_len);
	ut_expected_io_set_iov(expected_io, 12, iov[45].iov_base, iov[45].iov_len);
	ut_expected_io_set_iov(expected_io, 13, iov[46].iov_base, 864);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 4th child IO must be from the remaining 3232 bytes of iov[46] to the
	 * first 864 bytes of iov[52] split by the IO boundary requirement.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 256, 128, 7);
	expected_io->md_buf = md_buf + 256 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)((uintptr_t)iov[46].iov_base + 864),
			       iov[46].iov_len - 864);
	ut_expected_io_set_iov(expected_io, 1, iov[47].iov_base, iov[47].iov_len);
	ut_expected_io_set_iov(expected_io, 2, iov[48].iov_base, iov[48].iov_len);
	ut_expected_io_set_iov(expected_io, 3, iov[49].iov_base, iov[49].iov_len);
	ut_expected_io_set_iov(expected_io, 4, iov[50].iov_base, iov[50].iov_len);
	ut_expected_io_set_iov(expected_io, 5, iov[51].iov_base, iov[51].iov_len);
	ut_expected_io_set_iov(expected_io, 6, iov[52].iov_base, 864);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 5th child IO must be from the remaining 3232 bytes of iov[52] to
	 * the first 4096 bytes of iov[57] split by the IO boundary requirement.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 384, 128, 6);
	expected_io->md_buf = md_buf + 384 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)((uintptr_t)iov[52].iov_base + 864),
			       iov[52].iov_len - 864);
	ut_expected_io_set_iov(expected_io, 1, iov[53].iov_base, iov[53].iov_len);
	ut_expected_io_set_iov(expected_io, 2, iov[54].iov_base, iov[54].iov_len);
	ut_expected_io_set_iov(expected_io, 3, iov[55].iov_base, iov[55].iov_len);
	ut_expected_io_set_iov(expected_io, 4, iov[56].iov_base, iov[56].iov_len);
	ut_expected_io_set_iov(expected_io, 5, iov[57].iov_base, 4960);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 6th child IO must be from the remaining 7328 bytes of iov[57]
	 * to the first 3936 bytes of iov[58] split by the capacity of child iovs.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 512, 30, 3);
	expected_io->md_buf = md_buf + 512 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)((uintptr_t)iov[57].iov_base + 4960),
			       iov[57].iov_len - 4960);
	ut_expected_io_set_iov(expected_io, 1, iov[58].iov_base, iov[58].iov_len);
	ut_expected_io_set_iov(expected_io, 2, iov[59].iov_base, 3936);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 7th child IO is from the remaining 160 bytes of iov[59] and iov[60]. */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 542, 1, 2);
	expected_io->md_buf = md_buf + 542 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)((uintptr_t)iov[59].iov_base + 3936),
			       iov[59].iov_len - 3936);
	ut_expected_io_set_iov(expected_io, 1, iov[60].iov_base, iov[60].iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, 61, md_buf,
					    0, 543, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 5);
	stub_complete_io(5);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

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

	/* Children requests return an error status */
	bdev->optimal_io_boundary = 16;
	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 512 * 64;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_FAILED;
	g_io_done = false;
	g_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, 1, 1, 64, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 5);
	stub_complete_io(4);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);

	/* Test if a multi vector command terminated with failure before continueing
	 * splitting process when one of child I/O failed.
	 * The multi vector command is as same as the above that needs to be split by strip
	 * and then needs to be split further due to the capacity of child iovs.
	 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_base = (void *)(BDEV_IO_NUM_CHILD_IOV * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_len = 256;

	iov[BDEV_IO_NUM_CHILD_IOV].iov_base = (void *)((BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV].iov_len = 256;

	iov[BDEV_IO_NUM_CHILD_IOV + 1].iov_base = (void *)((BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000);
	iov[BDEV_IO_NUM_CHILD_IOV + 1].iov_len = 512;

	bdev->optimal_io_boundary = BDEV_IO_NUM_CHILD_IOV;

	g_io_exp_status = SPDK_BDEV_IO_STATUS_FAILED;
	g_io_done = false;
	g_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV * 2, 0,
				    BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* for this test we will create the following conditions to hit the code path where
	 * we are trying to send and IO following a split that has no iovs because we had to
	 * trim them for alignment reasons.
	 *
	 * - 16K boundary, our IO will start at offset 0 with a length of 0x4200
	 * - Our IOVs are 0x212 in size so that we run into the 16K boundary at child IOV
	 *   position 30 and overshoot by 0x2e.
	 * - That means we'll send the IO and loop back to pick up the remaining bytes at
	 *   child IOV index 31. When we do, we find that we have to shorten index 31 by 0x2e
	 *   which eliniates that vector so we just send the first split IO with 30 vectors
	 *   and let the completion pick up the last 2 vectors.
	 */
	bdev->optimal_io_boundary = 32;
	bdev->split_on_optimal_io_boundary = true;
	g_io_done = false;

	/* Init all parent IOVs to 0x212 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV + 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 0x212;
	}

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, BDEV_IO_NUM_CHILD_IOV,
					   BDEV_IO_NUM_CHILD_IOV - 1);
	/* expect 0-29 to be 1:1 with the parent iov */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		ut_expected_io_set_iov(expected_io, i, iov[i].iov_base, iov[i].iov_len);
	}

	/* expect index 30 to be shortened to 0x1e4 (0x212 - 0x1e) because of the alignment
	 * where 0x1e is the amount we overshot the 16K boundary
	 */
	ut_expected_io_set_iov(expected_io, BDEV_IO_NUM_CHILD_IOV - 2,
			       (void *)(iov[BDEV_IO_NUM_CHILD_IOV - 2].iov_base), 0x1e4);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* 2nd child IO will have 2 remaining vectors, one to pick up from the one that was
	 * shortened that take it to the next boundary and then a final one to get us to
	 * 0x4200 bytes for the IO.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, BDEV_IO_NUM_CHILD_IOV,
					   BDEV_IO_NUM_CHILD_IOV, 2);
	/* position 30 picked up the remaining bytes to the next boundary */
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)(iov[BDEV_IO_NUM_CHILD_IOV - 2].iov_base + 0x1e4), 0x2e);

	/* position 31 picked the the rest of the trasnfer to get us to 0x4200 */
	ut_expected_io_set_iov(expected_io, 1,
			       (void *)(iov[BDEV_IO_NUM_CHILD_IOV - 1].iov_base), 0x1d2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV + 1, 0,
				    BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
bdev_io_split_with_io_wait(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
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

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
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
	poll_threads();
}

static void
bdev_io_alignment(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 20,
		.bdev_io_cache_size = 2,
	};
	int rc;
	void *buf = NULL;
	struct iovec iovs[2];
	int iovcnt;
	uint64_t alignment;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	fn_table.submit_request = stub_submit_request_get_buf;
	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Create aligned buffer */
	rc = posix_memalign(&buf, 4096, 8192);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Pass aligned single buffer with no alignment required */
	alignment = 1;
	bdev->required_alignment = spdk_u32log2(alignment);

	rc = spdk_bdev_write_blocks(desc, io_ch, buf, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	stub_complete_io(1);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));

	rc = spdk_bdev_read_blocks(desc, io_ch, buf, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	stub_complete_io(1);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));

	/* Pass unaligned single buffer with no alignment required */
	alignment = 1;
	bdev->required_alignment = spdk_u32log2(alignment);

	rc = spdk_bdev_write_blocks(desc, io_ch, buf + 4, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == buf + 4);
	stub_complete_io(1);

	rc = spdk_bdev_read_blocks(desc, io_ch, buf + 4, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == buf + 4);
	stub_complete_io(1);

	/* Pass unaligned single buffer with 512 alignment required */
	alignment = 512;
	bdev->required_alignment = spdk_u32log2(alignment);

	rc = spdk_bdev_write_blocks(desc, io_ch, buf + 4, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs == &g_bdev_io->internal.bounce_iov);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);

	rc = spdk_bdev_read_blocks(desc, io_ch, buf + 4, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs == &g_bdev_io->internal.bounce_iov);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);

	/* Pass unaligned single buffer with 4096 alignment required */
	alignment = 4096;
	bdev->required_alignment = spdk_u32log2(alignment);

	rc = spdk_bdev_write_blocks(desc, io_ch, buf + 8, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs == &g_bdev_io->internal.bounce_iov);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);

	rc = spdk_bdev_read_blocks(desc, io_ch, buf + 8, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs == &g_bdev_io->internal.bounce_iov);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);

	/* Pass aligned iovs with no alignment required */
	alignment = 1;
	bdev->required_alignment = spdk_u32log2(alignment);

	iovcnt = 1;
	iovs[0].iov_base = buf;
	iovs[0].iov_len = 512;

	rc = spdk_bdev_writev(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == iovs[0].iov_base);

	rc = spdk_bdev_readv(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == iovs[0].iov_base);

	/* Pass unaligned iovs with no alignment required */
	alignment = 1;
	bdev->required_alignment = spdk_u32log2(alignment);

	iovcnt = 2;
	iovs[0].iov_base = buf + 16;
	iovs[0].iov_len = 256;
	iovs[1].iov_base = buf + 16 + 256 + 32;
	iovs[1].iov_len = 256;

	rc = spdk_bdev_writev(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == iovs[0].iov_base);

	rc = spdk_bdev_readv(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == iovs[0].iov_base);

	/* Pass unaligned iov with 2048 alignment required */
	alignment = 2048;
	bdev->required_alignment = spdk_u32log2(alignment);

	iovcnt = 2;
	iovs[0].iov_base = buf + 16;
	iovs[0].iov_len = 256;
	iovs[1].iov_base = buf + 16 + 256 + 32;
	iovs[1].iov_len = 256;

	rc = spdk_bdev_writev(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == iovcnt);
	CU_ASSERT(g_bdev_io->u.bdev.iovs == &g_bdev_io->internal.bounce_iov);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);

	rc = spdk_bdev_readv(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == iovcnt);
	CU_ASSERT(g_bdev_io->u.bdev.iovs == &g_bdev_io->internal.bounce_iov);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);

	/* Pass iov without allocated buffer without alignment required */
	alignment = 1;
	bdev->required_alignment = spdk_u32log2(alignment);

	iovcnt = 1;
	iovs[0].iov_base = NULL;
	iovs[0].iov_len = 0;

	rc = spdk_bdev_readv(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);

	/* Pass iov without allocated buffer with 1024 alignment required */
	alignment = 1024;
	bdev->required_alignment = spdk_u32log2(alignment);

	iovcnt = 1;
	iovs[0].iov_base = NULL;
	iovs[0].iov_len = 0;

	rc = spdk_bdev_readv(desc, io_ch, iovs, iovcnt, 0, 512, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.orig_iovcnt == 0);
	CU_ASSERT(_are_iovs_aligned(g_bdev_io->u.bdev.iovs, g_bdev_io->u.bdev.iovcnt,
				    alignment));
	stub_complete_io(1);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	fn_table.submit_request = stub_submit_request;
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();

	free(buf);
}

static void
bdev_io_alignment_with_boundary(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 20,
		.bdev_io_cache_size = 2,
	};
	int rc;
	void *buf = NULL;
	struct iovec iovs[2];
	int iovcnt;
	uint64_t alignment;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	fn_table.submit_request = stub_submit_request_get_buf;
	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Create aligned buffer */
	rc = posix_memalign(&buf, 4096, 131072);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* 512 * 3 with 2 IO boundary, allocate small data buffer from bdev layer */
	alignment = 512;
	bdev->required_alignment = spdk_u32log2(alignment);
	bdev->optimal_io_boundary = 2;
	bdev->split_on_optimal_io_boundary = true;

	iovcnt = 1;
	iovs[0].iov_base = NULL;
	iovs[0].iov_len = 512 * 3;

	rc = spdk_bdev_readv_blocks(desc, io_ch, iovs, iovcnt, 1, 3, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);

	/* 8KiB with 16 IO boundary, allocate large data buffer from bdev layer */
	alignment = 512;
	bdev->required_alignment = spdk_u32log2(alignment);
	bdev->optimal_io_boundary = 16;
	bdev->split_on_optimal_io_boundary = true;

	iovcnt = 1;
	iovs[0].iov_base = NULL;
	iovs[0].iov_len = 512 * 16;

	rc = spdk_bdev_readv_blocks(desc, io_ch, iovs, iovcnt, 1, 16, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);

	/* 512 * 160 with 128 IO boundary, 63.5KiB + 16.5KiB for the two children requests */
	alignment = 512;
	bdev->required_alignment = spdk_u32log2(alignment);
	bdev->optimal_io_boundary = 128;
	bdev->split_on_optimal_io_boundary = true;

	iovcnt = 1;
	iovs[0].iov_base = buf + 16;
	iovs[0].iov_len = 512 * 160;
	rc = spdk_bdev_readv_blocks(desc, io_ch, iovs, iovcnt, 1, 160, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);

	/* 512 * 3 with 2 IO boundary */
	alignment = 512;
	bdev->required_alignment = spdk_u32log2(alignment);
	bdev->optimal_io_boundary = 2;
	bdev->split_on_optimal_io_boundary = true;

	iovcnt = 2;
	iovs[0].iov_base = buf + 16;
	iovs[0].iov_len = 512;
	iovs[1].iov_base = buf + 16 + 512 + 32;
	iovs[1].iov_len = 1024;

	rc = spdk_bdev_writev_blocks(desc, io_ch, iovs, iovcnt, 1, 3, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iovs, iovcnt, 1, 3, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);

	/* 512 * 64 with 32 IO boundary */
	bdev->optimal_io_boundary = 32;
	iovcnt = 2;
	iovs[0].iov_base = buf + 16;
	iovs[0].iov_len = 16384;
	iovs[1].iov_base = buf + 16 + 16384 + 32;
	iovs[1].iov_len = 16384;

	rc = spdk_bdev_writev_blocks(desc, io_ch, iovs, iovcnt, 1, 64, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 3);
	stub_complete_io(3);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iovs, iovcnt, 1, 64, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 3);
	stub_complete_io(3);

	/* 512 * 160 with 32 IO boundary */
	iovcnt = 1;
	iovs[0].iov_base = buf + 16;
	iovs[0].iov_len = 16384 + 65536;

	rc = spdk_bdev_writev_blocks(desc, io_ch, iovs, iovcnt, 1, 160, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 6);
	stub_complete_io(6);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	fn_table.submit_request = stub_submit_request;
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();

	free(buf);
}

static void
histogram_status_cb(void *cb_arg, int status)
{
	g_status = status;
}

static void
histogram_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	g_status = status;
	g_histogram = histogram;
}

static void
histogram_io_count(void *ctx, uint64_t start, uint64_t end, uint64_t count,
		   uint64_t total, uint64_t so_far)
{
	g_count += count;
}

static void
bdev_histograms(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ch;
	struct spdk_histogram_data *histogram;
	uint8_t buf[4096];
	int rc;

	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));

	ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(ch != NULL);

	/* Enable histogram */
	g_status = -1;
	spdk_bdev_histogram_enable(bdev, histogram_status_cb, NULL, true);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(bdev->internal.histogram_enabled == true);

	/* Allocate histogram */
	histogram = spdk_histogram_data_alloc();
	SPDK_CU_ASSERT_FATAL(histogram != NULL);

	/* Check if histogram is zeroed */
	spdk_bdev_histogram_get(bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == 0);
	SPDK_CU_ASSERT_FATAL(g_histogram != NULL);

	g_count = 0;
	spdk_histogram_data_iterate(g_histogram, histogram_io_count, NULL);

	CU_ASSERT(g_count == 0);

	rc = spdk_bdev_write_blocks(desc, ch, buf, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(10);
	stub_complete_io(1);
	poll_threads();

	rc = spdk_bdev_read_blocks(desc, ch, buf, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(10);
	stub_complete_io(1);
	poll_threads();

	/* Check if histogram gathered data from all I/O channels */
	g_histogram = NULL;
	spdk_bdev_histogram_get(bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(bdev->internal.histogram_enabled == true);
	SPDK_CU_ASSERT_FATAL(g_histogram != NULL);

	g_count = 0;
	spdk_histogram_data_iterate(g_histogram, histogram_io_count, NULL);
	CU_ASSERT(g_count == 2);

	/* Disable histogram */
	spdk_bdev_histogram_enable(bdev, histogram_status_cb, NULL, false);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(bdev->internal.histogram_enabled == false);

	/* Try to run histogram commands on disabled bdev */
	spdk_bdev_histogram_get(bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == -EFAULT);

	spdk_histogram_data_free(histogram);
	spdk_put_io_channel(ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
_bdev_compare(bool emulated)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t offset, num_blocks;
	uint32_t num_completed;
	char aa_buf[512];
	char bb_buf[512];
	struct iovec compare_iov;
	uint8_t io_type;
	int rc;

	if (emulated) {
		io_type = SPDK_BDEV_IO_TYPE_READ;
	} else {
		io_type = SPDK_BDEV_IO_TYPE_COMPARE;
	}

	memset(aa_buf, 0xaa, sizeof(aa_buf));
	memset(bb_buf, 0xbb, sizeof(bb_buf));

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = !emulated;

	spdk_bdev_initialize(bdev_init_cb, NULL);
	fn_table.submit_request = stub_submit_request_get_buf;
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	fn_table.submit_request = stub_submit_request_get_buf;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	offset = 50;
	num_blocks = 1;
	compare_iov.iov_base = aa_buf;
	compare_iov.iov_len = sizeof(aa_buf);

	expected_io = ut_alloc_expected_io(io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = aa_buf;
	g_compare_read_buf_len = sizeof(aa_buf);
	rc = spdk_bdev_comparev_blocks(desc, ioch, &compare_iov, 1, offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	expected_io = ut_alloc_expected_io(io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = bb_buf;
	g_compare_read_buf_len = sizeof(bb_buf);
	rc = spdk_bdev_comparev_blocks(desc, ioch, &compare_iov, 1, offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_MISCOMPARE);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	fn_table.submit_request = stub_submit_request;
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = true;

	g_compare_read_buf = NULL;
}

static void
bdev_compare(void)
{
	_bdev_compare(true);
	_bdev_compare(false);
}

static void
bdev_compare_and_write(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t offset, num_blocks;
	uint32_t num_completed;
	char aa_buf[512];
	char bb_buf[512];
	char cc_buf[512];
	char write_buf[512];
	struct iovec compare_iov;
	struct iovec write_iov;
	int rc;

	memset(aa_buf, 0xaa, sizeof(aa_buf));
	memset(bb_buf, 0xbb, sizeof(bb_buf));
	memset(cc_buf, 0xcc, sizeof(cc_buf));

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = false;

	spdk_bdev_initialize(bdev_init_cb, NULL);
	fn_table.submit_request = stub_submit_request_get_buf;
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	fn_table.submit_request = stub_submit_request_get_buf;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	offset = 50;
	num_blocks = 1;
	compare_iov.iov_base = aa_buf;
	compare_iov.iov_len = sizeof(aa_buf);
	write_iov.iov_base = bb_buf;
	write_iov.iov_len = sizeof(bb_buf);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = aa_buf;
	g_compare_read_buf_len = sizeof(aa_buf);
	memset(write_buf, 0, sizeof(write_buf));
	g_compare_write_buf = write_buf;
	g_compare_write_buf_len = sizeof(write_buf);
	rc = spdk_bdev_comparev_and_writev_blocks(desc, ioch, &compare_iov, 1, &write_iov, 1,
			offset, num_blocks, io_done, NULL);
	/* Trigger range locking */
	poll_threads();
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == false);
	num_completed = stub_complete_io(1);
	/* Trigger range unlocking */
	poll_threads();
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(memcmp(write_buf, bb_buf, sizeof(write_buf)) == 0);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = cc_buf;
	g_compare_read_buf_len = sizeof(cc_buf);
	memset(write_buf, 0, sizeof(write_buf));
	g_compare_write_buf = write_buf;
	g_compare_write_buf_len = sizeof(write_buf);
	rc = spdk_bdev_comparev_and_writev_blocks(desc, ioch, &compare_iov, 1, &write_iov, 1,
			offset, num_blocks, io_done, NULL);
	/* Trigger range locking */
	poll_threads();
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	/* Trigger range unlocking earlier because we expect error here */
	poll_threads();
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_MISCOMPARE);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 0);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	fn_table.submit_request = stub_submit_request;
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = true;

	g_compare_read_buf = NULL;
	g_compare_write_buf = NULL;
}

static void
bdev_write_zeroes(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t offset, num_io_blocks, num_blocks;
	uint32_t num_completed, num_requests;
	int rc;

	spdk_bdev_initialize(bdev_init_cb, NULL);
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	fn_table.submit_request = stub_submit_request;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* First test that if the bdev supports write_zeroes, the request won't be split */
	bdev->md_len = 0;
	bdev->blocklen = 4096;
	num_blocks = (ZERO_BUFFER_SIZE / bdev->blocklen) * 2;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, 0, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);

	/* Check that if write zeroes is not supported it'll be replaced by regular writes */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, false);
	num_io_blocks = ZERO_BUFFER_SIZE / bdev->blocklen;
	num_requests = 2;
	num_blocks = (ZERO_BUFFER_SIZE / bdev->blocklen) * num_requests;

	for (offset = 0; offset < num_requests; ++offset) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE,
						   offset * num_io_blocks, num_io_blocks, 0);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(num_requests);
	CU_ASSERT_EQUAL(num_completed, num_requests);

	/* Check that the splitting is correct if bdev has interleaved metadata */
	bdev->md_interleave = true;
	bdev->md_len = 64;
	bdev->blocklen = 4096 + 64;
	num_blocks = (ZERO_BUFFER_SIZE / bdev->blocklen) * 2;

	num_requests = offset = 0;
	while (offset < num_blocks) {
		num_io_blocks = spdk_min(ZERO_BUFFER_SIZE / bdev->blocklen, num_blocks - offset);
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE,
						   offset, num_io_blocks, 0);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += num_io_blocks;
		num_requests++;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(num_requests);
	CU_ASSERT_EQUAL(num_completed, num_requests);
	num_completed = stub_complete_io(num_requests);
	assert(num_completed == 0);

	/* Check the the same for separate metadata buffer */
	bdev->md_interleave = false;
	bdev->md_len = 64;
	bdev->blocklen = 4096;

	num_requests = offset = 0;
	while (offset < num_blocks) {
		num_io_blocks = spdk_min(ZERO_BUFFER_SIZE / (bdev->blocklen + bdev->md_len), num_blocks);
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE,
						   offset, num_io_blocks, 0);
		expected_io->md_buf = (char *)g_bdev_mgr.zero_buffer + num_io_blocks * bdev->blocklen;
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += num_io_blocks;
		num_requests++;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(num_requests);
	CU_ASSERT_EQUAL(num_completed, num_requests);

	ut_enable_io_type(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, true);
	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
bdev_open_while_hotremove(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc[2] = {};
	int rc;

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", false, bdev_ut_event_cb, NULL, &desc[0]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc[0] != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc[0]));

	spdk_bdev_unregister(bdev, NULL, NULL);

	rc = spdk_bdev_open_ext("bdev", false, bdev_ut_event_cb, NULL, &desc[1]);
	CU_ASSERT(rc == -ENODEV);
	SPDK_CU_ASSERT_FATAL(desc[1] == NULL);

	spdk_bdev_close(desc[0]);
	free_bdev(bdev);
}

static void
bdev_close_while_hotremove(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	int rc = 0;

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb1, &desc, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));

	/* Simulate hot-unplug by unregistering bdev */
	g_event_type1 = 0xFF;
	g_unregister_arg = NULL;
	g_unregister_rc = -1;
	spdk_bdev_unregister(bdev, bdev_unregister_cb, (void *)0x12345678);
	/* Close device while remove event is in flight */
	spdk_bdev_close(desc);

	/* Ensure that unregister callback is delayed */
	CU_ASSERT_EQUAL(g_unregister_arg, NULL);
	CU_ASSERT_EQUAL(g_unregister_rc, -1);

	poll_threads();

	/* Event callback shall not be issued because device was closed */
	CU_ASSERT_EQUAL(g_event_type1, 0xFF);
	/* Unregister callback is issued */
	CU_ASSERT_EQUAL(g_unregister_arg, (void *)0x12345678);
	CU_ASSERT_EQUAL(g_unregister_rc, 0);

	free_bdev(bdev);
}

static void
bdev_open_ext(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc1 = NULL;
	struct spdk_bdev_desc *desc2 = NULL;
	int rc = 0;

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, NULL, NULL, &desc1);
	CU_ASSERT_EQUAL(rc, -EINVAL);

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb1, &desc1, &desc1);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb2, &desc2, &desc2);
	CU_ASSERT_EQUAL(rc, 0);

	g_event_type1 = 0xFF;
	g_event_type2 = 0xFF;

	/* Simulate hot-unplug by unregistering bdev */
	spdk_bdev_unregister(bdev, NULL, NULL);
	poll_threads();

	/* Check if correct events have been triggered in event callback fn */
	CU_ASSERT_EQUAL(g_event_type1, SPDK_BDEV_EVENT_REMOVE);
	CU_ASSERT_EQUAL(g_event_type2, SPDK_BDEV_EVENT_REMOVE);

	free_bdev(bdev);
	poll_threads();
}

struct timeout_io_cb_arg {
	struct iovec iov;
	uint8_t type;
};

static int
bdev_channel_count_submitted_io(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io;
	int n = 0;

	if (!ch) {
		return -1;
	}

	TAILQ_FOREACH(bdev_io, &ch->io_submitted, internal.ch_link) {
		n++;
	}

	return n;
}

static void
bdev_channel_io_timeout_cb(void *cb_arg, struct spdk_bdev_io *bdev_io)
{
	struct timeout_io_cb_arg *ctx = cb_arg;

	ctx->type = bdev_io->type;
	ctx->iov.iov_base = bdev_io->iov.iov_base;
	ctx->iov.iov_len = bdev_io->iov.iov_len;
}

static void
bdev_set_io_timeout(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch = NULL;
	struct spdk_bdev_channel *bdev_ch = NULL;
	struct timeout_io_cb_arg cb_arg;

	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev");

	CU_ASSERT(spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc) == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));

	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch->io_submitted));

	/* This is the part1.
	 * We will check the bdev_ch->io_submitted list
	 * TO make sure that it can link IOs and only the user submitted IOs
	 */
	CU_ASSERT(spdk_bdev_read(desc, io_ch, (void *)0x1000, 0, 4096, io_done, NULL) == 0);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 1);
	CU_ASSERT(spdk_bdev_write(desc, io_ch, (void *)0x2000, 0, 4096, io_done, NULL) == 0);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 2);
	stub_complete_io(1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 1);
	stub_complete_io(1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 0);

	/* Split IO */
	bdev->optimal_io_boundary = 16;
	bdev->split_on_optimal_io_boundary = true;

	/* Now test that a single-vector command is split correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 *
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	CU_ASSERT(spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 8, io_done, NULL) == 0);
	/* We count all submitted IOs including IO that are generated by splitting. */
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 3);
	stub_complete_io(1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 2);
	stub_complete_io(1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 0);

	/* Also include the reset IO */
	CU_ASSERT(spdk_bdev_reset(desc, io_ch, io_done, NULL) == 0);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 1);
	poll_threads();
	stub_complete_io(1);
	poll_threads();
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 0);

	/* This is part2
	 * Test the desc timeout poller register
	 */

	/* Successfully set the timeout */
	CU_ASSERT(spdk_bdev_set_timeout(desc, 30, bdev_channel_io_timeout_cb, &cb_arg) == 0);
	CU_ASSERT(desc->io_timeout_poller != NULL);
	CU_ASSERT(desc->timeout_in_sec == 30);
	CU_ASSERT(desc->cb_fn == bdev_channel_io_timeout_cb);
	CU_ASSERT(desc->cb_arg == &cb_arg);

	/* Change the timeout limit */
	CU_ASSERT(spdk_bdev_set_timeout(desc, 20, bdev_channel_io_timeout_cb, &cb_arg) == 0);
	CU_ASSERT(desc->io_timeout_poller != NULL);
	CU_ASSERT(desc->timeout_in_sec == 20);
	CU_ASSERT(desc->cb_fn == bdev_channel_io_timeout_cb);
	CU_ASSERT(desc->cb_arg == &cb_arg);

	/* Disable the timeout */
	CU_ASSERT(spdk_bdev_set_timeout(desc, 0, NULL, NULL) == 0);
	CU_ASSERT(desc->io_timeout_poller == NULL);

	/* This the part3
	 * We will test to catch timeout IO and check whether the IO is
	 * the submitted one.
	 */
	memset(&cb_arg, 0, sizeof(cb_arg));
	CU_ASSERT(spdk_bdev_set_timeout(desc, 30, bdev_channel_io_timeout_cb, &cb_arg) == 0);
	CU_ASSERT(spdk_bdev_write_blocks(desc, io_ch, (void *)0x1000, 0, 1, io_done, NULL) == 0);

	/* Don't reach the limit */
	spdk_delay_us(15 * spdk_get_ticks_hz());
	poll_threads();
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	/* 15 + 15 = 30 reach the limit */
	spdk_delay_us(15 * spdk_get_ticks_hz());
	poll_threads();
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x1000);
	CU_ASSERT(cb_arg.iov.iov_len == 1 * bdev->blocklen);
	stub_complete_io(1);

	/* Use the same split IO above and check the IO */
	memset(&cb_arg, 0, sizeof(cb_arg));
	CU_ASSERT(spdk_bdev_write_blocks(desc, io_ch, (void *)0xF000, 14, 8, io_done, NULL) == 0);

	/* The first child complete in time */
	spdk_delay_us(15 * spdk_get_ticks_hz());
	poll_threads();
	stub_complete_io(1);
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	/* The second child reach the limit */
	spdk_delay_us(15 * spdk_get_ticks_hz());
	poll_threads();
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0xF000);
	CU_ASSERT(cb_arg.iov.iov_len == 8 * bdev->blocklen);
	stub_complete_io(1);

	/* Also include the reset IO */
	memset(&cb_arg, 0, sizeof(cb_arg));
	CU_ASSERT(spdk_bdev_reset(desc, io_ch, io_done, NULL) == 0);
	spdk_delay_us(30 * spdk_get_ticks_hz());
	poll_threads();
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_RESET);
	stub_complete_io(1);
	poll_threads();

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
lba_range_overlap(void)
{
	struct lba_range r1, r2;

	r1.offset = 100;
	r1.length = 50;

	r2.offset = 0;
	r2.length = 1;
	CU_ASSERT(!bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 0;
	r2.length = 100;
	CU_ASSERT(!bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 0;
	r2.length = 110;
	CU_ASSERT(bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 100;
	r2.length = 10;
	CU_ASSERT(bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 110;
	r2.length = 20;
	CU_ASSERT(bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 140;
	r2.length = 150;
	CU_ASSERT(bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 130;
	r2.length = 200;
	CU_ASSERT(bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 150;
	r2.length = 100;
	CU_ASSERT(!bdev_lba_range_overlapped(&r1, &r2));

	r2.offset = 110;
	r2.length = 0;
	CU_ASSERT(!bdev_lba_range_overlapped(&r1, &r2));
}

static bool g_lock_lba_range_done;
static bool g_unlock_lba_range_done;

static void
lock_lba_range_done(void *ctx, int status)
{
	g_lock_lba_range_done = true;
}

static void
unlock_lba_range_done(void *ctx, int status)
{
	g_unlock_lba_range_done = true;
}

static void
lock_lba_range_check_ranges(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *channel;
	struct lba_range *range;
	int ctx1;
	int rc;

	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);
	channel = spdk_io_channel_get_ctx(io_ch);

	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 20, 10, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_lock_lba_range_done == true);
	range = TAILQ_FIRST(&channel->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 20);
	CU_ASSERT(range->length == 10);
	CU_ASSERT(range->owner_ch == channel);

	/* Unlocks must exactly match a lock. */
	g_unlock_lba_range_done = false;
	rc = bdev_unlock_lba_range(desc, io_ch, 20, 1, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_unlock_lba_range_done == false);

	rc = bdev_unlock_lba_range(desc, io_ch, 20, 10, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	spdk_delay_us(100);
	poll_threads();

	CU_ASSERT(g_unlock_lba_range_done == true);
	CU_ASSERT(TAILQ_EMPTY(&channel->locked_ranges));

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
lock_lba_range_with_io_outstanding(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *channel;
	struct lba_range *range;
	char buf[4096];
	int ctx1;
	int rc;

	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);
	channel = spdk_io_channel_get_ctx(io_ch);

	g_io_done = false;
	rc = spdk_bdev_read_blocks(desc, io_ch, buf, 20, 1, io_done, &ctx1);
	CU_ASSERT(rc == 0);

	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 20, 10, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	/* The lock should immediately become valid, since there are no outstanding
	 * write I/O.
	 */
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_lock_lba_range_done == true);
	range = TAILQ_FIRST(&channel->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 20);
	CU_ASSERT(range->length == 10);
	CU_ASSERT(range->owner_ch == channel);
	CU_ASSERT(range->locked_ctx == &ctx1);

	rc = bdev_unlock_lba_range(desc, io_ch, 20, 10, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	stub_complete_io(1);
	spdk_delay_us(100);
	poll_threads();

	CU_ASSERT(TAILQ_EMPTY(&channel->locked_ranges));

	/* Now try again, but with a write I/O. */
	g_io_done = false;
	rc = spdk_bdev_write_blocks(desc, io_ch, buf, 20, 1, io_done, &ctx1);
	CU_ASSERT(rc == 0);

	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 20, 10, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	/* The lock should not be fully valid yet, since a write I/O is outstanding.
	 * But note that the range should be on the channel's locked_list, to make sure no
	 * new write I/O are started.
	 */
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_lock_lba_range_done == false);
	range = TAILQ_FIRST(&channel->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 20);
	CU_ASSERT(range->length == 10);

	/* Complete the write I/O.  This should make the lock valid (checked by confirming
	 * our callback was invoked).
	 */
	stub_complete_io(1);
	spdk_delay_us(100);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_lock_lba_range_done == true);

	rc = bdev_unlock_lba_range(desc, io_ch, 20, 10, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(TAILQ_EMPTY(&channel->locked_ranges));

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
lock_lba_range_overlapped(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *channel;
	struct lba_range *range;
	int ctx1;
	int rc;

	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);
	channel = spdk_io_channel_get_ctx(io_ch);

	/* Lock range 20-29. */
	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 20, 10, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_lock_lba_range_done == true);
	range = TAILQ_FIRST(&channel->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 20);
	CU_ASSERT(range->length == 10);

	/* Try to lock range 25-39.  It should not lock immediately, since it overlaps with
	 * 20-29.
	 */
	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 25, 15, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_lock_lba_range_done == false);
	range = TAILQ_FIRST(&bdev->internal.pending_locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 25);
	CU_ASSERT(range->length == 15);

	/* Unlock 20-29.  This should result in range 25-39 now getting locked since it
	 * no longer overlaps with an active lock.
	 */
	g_unlock_lba_range_done = false;
	rc = bdev_unlock_lba_range(desc, io_ch, 20, 10, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_unlock_lba_range_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.pending_locked_ranges));
	range = TAILQ_FIRST(&channel->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 25);
	CU_ASSERT(range->length == 15);

	/* Lock 40-59.  This should immediately lock since it does not overlap with the
	 * currently active 25-39 lock.
	 */
	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 40, 20, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_lock_lba_range_done == true);
	range = TAILQ_FIRST(&bdev->internal.locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	range = TAILQ_NEXT(range, tailq);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 40);
	CU_ASSERT(range->length == 20);

	/* Try to lock 35-44.  Note that this overlaps with both 25-39 and 40-59. */
	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch, 35, 10, lock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_lock_lba_range_done == false);
	range = TAILQ_FIRST(&bdev->internal.pending_locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 35);
	CU_ASSERT(range->length == 10);

	/* Unlock 25-39.  Make sure that 35-44 is still in the pending list, since
	 * the 40-59 lock is still active.
	 */
	g_unlock_lba_range_done = false;
	rc = bdev_unlock_lba_range(desc, io_ch, 25, 15, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_unlock_lba_range_done == true);
	CU_ASSERT(g_lock_lba_range_done == false);
	range = TAILQ_FIRST(&bdev->internal.pending_locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 35);
	CU_ASSERT(range->length == 10);

	/* Unlock 40-59.  This should result in 35-44 now getting locked, since there are
	 * no longer any active overlapping locks.
	 */
	g_unlock_lba_range_done = false;
	rc = bdev_unlock_lba_range(desc, io_ch, 40, 20, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_unlock_lba_range_done == true);
	CU_ASSERT(g_lock_lba_range_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.pending_locked_ranges));
	range = TAILQ_FIRST(&bdev->internal.locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 35);
	CU_ASSERT(range->length == 10);

	/* Finally, unlock 35-44. */
	g_unlock_lba_range_done = false;
	rc = bdev_unlock_lba_range(desc, io_ch, 35, 10, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_unlock_lba_range_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.locked_ranges));

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
abort_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_abort_done = true;
	g_abort_status = bdev_io->internal.status;
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_io_abort(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *channel;
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 7,
		.bdev_io_cache_size = 2,
	};
	struct iovec iov[BDEV_IO_NUM_CHILD_IOV * 2];
	uint64_t io_ctx1 = 0, io_ctx2 = 0, i;
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);
	channel = spdk_io_channel_get_ctx(io_ch);
	mgmt_ch = channel->shared_resource->mgmt_ch;

	g_abort_done = false;

	ut_enable_io_type(SPDK_BDEV_IO_TYPE_ABORT, false);

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx1, abort_done, NULL);
	CU_ASSERT(rc == -ENOTSUP);

	ut_enable_io_type(SPDK_BDEV_IO_TYPE_ABORT, true);

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx2, abort_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_abort_done == true);
	CU_ASSERT(g_abort_status == SPDK_BDEV_IO_STATUS_FAILED);

	/* Test the case that the target I/O was successfully aborted. */
	g_io_done = false;

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, &io_ctx1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	g_abort_done = false;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx1, abort_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
	stub_complete_io(1);
	CU_ASSERT(g_abort_done == true);
	CU_ASSERT(g_abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Test the case that the target I/O was not aborted because it completed
	 * in the middle of execution of the abort.
	 */
	g_io_done = false;

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, &io_ctx1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	g_abort_done = false;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_FAILED;

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx1, abort_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_FAILED;
	stub_complete_io(1);
	CU_ASSERT(g_abort_done == true);
	CU_ASSERT(g_abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	bdev->optimal_io_boundary = 16;
	bdev->split_on_optimal_io_boundary = true;

	/* Test that a single-vector command which is split is aborted correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 */
	g_io_done = false;

	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 8, io_done, &io_ctx1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx1, abort_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
	stub_complete_io(2);
	CU_ASSERT(g_abort_done == true);
	CU_ASSERT(g_abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Test that a multi-vector command that needs to be split by strip and then
	 * needs to be split is aborted correctly. Abort is requested before the second
	 * child I/O was submitted. The parent I/O should complete with failure without
	 * submitting the second child I/O.
	 */
	for (i = 0; i < BDEV_IO_NUM_CHILD_IOV * 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}

	bdev->optimal_io_boundary = BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, BDEV_IO_NUM_CHILD_IOV * 2, 0,
				    BDEV_IO_NUM_CHILD_IOV * 2, io_done, &io_ctx1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx1, abort_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
	stub_complete_io(1);
	CU_ASSERT(g_abort_done == true);
	CU_ASSERT(g_abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	bdev->optimal_io_boundary = 16;
	g_io_done = false;

	/* Test that a ingle-vector command which is split is aborted correctly.
	 * Differently from the above, the child abort request will be submitted
	 * sequentially due to the capacity of spdk_bdev_io.
	 */
	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 50, io_done, &io_ctx1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);

	g_abort_done = false;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_abort(desc, io_ch, &io_ctx1, abort_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&mgmt_ch->io_wait_queue));
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);

	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
	stub_complete_io(3);
	CU_ASSERT(g_abort_done == true);
	CU_ASSERT(g_abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	spdk_bdev_finish(bdev_fini_cb, NULL);
	poll_threads();
}

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("bdev", null_init, null_clean);

	CU_ADD_TEST(suite, bytes_to_blocks_test);
	CU_ADD_TEST(suite, num_blocks_test);
	CU_ADD_TEST(suite, io_valid_test);
	CU_ADD_TEST(suite, open_write_test);
	CU_ADD_TEST(suite, alias_add_del_test);
	CU_ADD_TEST(suite, get_device_stat_test);
	CU_ADD_TEST(suite, bdev_io_types_test);
	CU_ADD_TEST(suite, bdev_io_wait_test);
	CU_ADD_TEST(suite, bdev_io_spans_boundary_test);
	CU_ADD_TEST(suite, bdev_io_split_test);
	CU_ADD_TEST(suite, bdev_io_split_with_io_wait);
	CU_ADD_TEST(suite, bdev_io_alignment_with_boundary);
	CU_ADD_TEST(suite, bdev_io_alignment);
	CU_ADD_TEST(suite, bdev_histograms);
	CU_ADD_TEST(suite, bdev_write_zeroes);
	CU_ADD_TEST(suite, bdev_compare_and_write);
	CU_ADD_TEST(suite, bdev_compare);
	CU_ADD_TEST(suite, bdev_open_while_hotremove);
	CU_ADD_TEST(suite, bdev_close_while_hotremove);
	CU_ADD_TEST(suite, bdev_open_ext);
	CU_ADD_TEST(suite, bdev_set_io_timeout);
	CU_ADD_TEST(suite, lba_range_overlap);
	CU_ADD_TEST(suite, lock_lba_range_check_ranges);
	CU_ADD_TEST(suite, lock_lba_range_with_io_outstanding);
	CU_ADD_TEST(suite, lock_lba_range_overlapped);
	CU_ADD_TEST(suite, bdev_io_abort);

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();
	free_cores();

	return num_failures;
}
