/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB(spdk_memory_domain_get_dma_device_id, const char *, (struct spdk_memory_domain *domain),
	    "test_domain");
DEFINE_STUB(spdk_memory_domain_get_dma_device_type, enum spdk_dma_device_type,
	    (struct spdk_memory_domain *domain), 0);

static bool g_memory_domain_pull_data_called;
static bool g_memory_domain_push_data_called;

DEFINE_RETURN_MOCK(spdk_memory_domain_pull_data, int);
int
spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	g_memory_domain_pull_data_called = true;
	HANDLE_RETURN_MOCK(spdk_memory_domain_pull_data);
	cpl_cb(cpl_cb_arg, 0);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_memory_domain_push_data, int);
int
spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	g_memory_domain_push_data_called = true;
	HANDLE_RETURN_MOCK(spdk_memory_domain_push_data);
	cpl_cb(cpl_cb_arg, 0);
	return 0;
}

int g_status;
int g_count;
enum spdk_bdev_event_type g_event_type1;
enum spdk_bdev_event_type g_event_type2;
enum spdk_bdev_event_type g_event_type3;
enum spdk_bdev_event_type g_event_type4;
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
	uint64_t			src_offset;
	uint64_t			length;
	int				iovcnt;
	struct iovec			iov[SPDK_BDEV_IO_NUM_CHILD_IOV];
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
static void *g_compare_md_buf;
static bool g_abort_done;
static enum spdk_bdev_io_status g_abort_status;
static void *g_zcopy_read_buf;
static uint32_t g_zcopy_read_buf_len;
static void *g_zcopy_write_buf;
static uint32_t g_zcopy_write_buf_len;
static struct spdk_bdev_io *g_zcopy_bdev_io;
static uint64_t g_seek_data_offset;
static uint64_t g_seek_hole_offset;
static uint64_t g_seek_offset;

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

static struct ut_expected_io *
ut_alloc_expected_copy_io(uint8_t type, uint64_t offset, uint64_t src_offset, uint64_t length)
{
	struct ut_expected_io *expected_io;

	expected_io = calloc(1, sizeof(*expected_io));
	SPDK_CU_ASSERT_FATAL(expected_io != NULL);

	expected_io->type = type;
	expected_io->offset = offset;
	expected_io->src_offset = src_offset;
	expected_io->length = length;

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
		if (bdev_io->bdev->md_len && bdev_io->u.bdev.md_buf && g_compare_md_buf) {
			memcpy(bdev_io->u.bdev.md_buf, g_compare_md_buf,
			       bdev_io->bdev->md_len * bdev_io->u.bdev.num_blocks);
		}
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
		if (bdev_io->u.bdev.md_buf &&
		    memcmp(bdev_io->u.bdev.md_buf, g_compare_md_buf,
			   bdev_io->bdev->md_len * bdev_io->u.bdev.num_blocks)) {
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

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_ZCOPY) {
		if (bdev_io->u.bdev.zcopy.start) {
			g_zcopy_bdev_io = bdev_io;
			if (bdev_io->u.bdev.zcopy.populate) {
				/* Start of a read */
				CU_ASSERT(g_zcopy_read_buf != NULL);
				CU_ASSERT(g_zcopy_read_buf_len > 0);
				bdev_io->u.bdev.iovs[0].iov_base = g_zcopy_read_buf;
				bdev_io->u.bdev.iovs[0].iov_len = g_zcopy_read_buf_len;
				bdev_io->u.bdev.iovcnt = 1;
			} else {
				/* Start of a write */
				CU_ASSERT(g_zcopy_write_buf != NULL);
				CU_ASSERT(g_zcopy_write_buf_len > 0);
				bdev_io->u.bdev.iovs[0].iov_base = g_zcopy_write_buf;
				bdev_io->u.bdev.iovs[0].iov_len = g_zcopy_write_buf_len;
				bdev_io->u.bdev.iovcnt = 1;
			}
		} else {
			if (bdev_io->u.bdev.zcopy.commit) {
				/* End of write */
				CU_ASSERT(bdev_io->u.bdev.iovs[0].iov_base == g_zcopy_write_buf);
				CU_ASSERT(bdev_io->u.bdev.iovs[0].iov_len == g_zcopy_write_buf_len);
				CU_ASSERT(bdev_io->u.bdev.iovcnt == 1);
				g_zcopy_write_buf = NULL;
				g_zcopy_write_buf_len = 0;
			} else {
				/* End of read */
				CU_ASSERT(bdev_io->u.bdev.iovs[0].iov_base == g_zcopy_read_buf);
				CU_ASSERT(bdev_io->u.bdev.iovs[0].iov_len == g_zcopy_read_buf_len);
				CU_ASSERT(bdev_io->u.bdev.iovcnt == 1);
				g_zcopy_read_buf = NULL;
				g_zcopy_read_buf_len = 0;
			}
		}
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_SEEK_DATA) {
		bdev_io->u.bdev.seek.offset = g_seek_data_offset;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_SEEK_HOLE) {
		bdev_io->u.bdev.seek.offset = g_seek_hole_offset;
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
	if (expected_io->type == SPDK_BDEV_IO_TYPE_COPY) {
		CU_ASSERT(expected_io->src_offset == bdev_io->u.bdev.copy.src_offset_blocks);
	}

	if (expected_io->iovcnt == 0) {
		free(expected_io);
		/* UNMAP, WRITE_ZEROES, FLUSH and COPY don't have iovs, so we can just return now. */
		return;
	}

	CU_ASSERT(expected_io->iovcnt == bdev_io->u.bdev.iovcnt);
	for (i = 0; i < expected_io->iovcnt; i++) {
		expected_iov = &expected_io->iov[i];
		if (bdev_io->internal.orig_iovcnt == 0) {
			iov = &bdev_io->u.bdev.iovs[i];
		} else {
			iov = bdev_io->internal.orig_iovs;
		}
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
	[SPDK_BDEV_IO_TYPE_SEEK_HOLE]		= true,
	[SPDK_BDEV_IO_TYPE_SEEK_DATA]		= true,
	[SPDK_BDEV_IO_TYPE_COPY]		= true,
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

static void vbdev_ut_examine_config(struct spdk_bdev *bdev);
static void vbdev_ut_examine_disk(struct spdk_bdev *bdev);

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
	.examine_config = vbdev_ut_examine_config,
	.examine_disk = vbdev_ut_examine_disk,
};

SPDK_BDEV_MODULE_REGISTER(bdev_ut, &bdev_ut_if)
SPDK_BDEV_MODULE_REGISTER(vbdev_ut, &vbdev_ut_if)

struct ut_examine_ctx {
	void (*examine_config)(struct spdk_bdev *bdev);
	void (*examine_disk)(struct spdk_bdev *bdev);
	uint32_t examine_config_count;
	uint32_t examine_disk_count;
};

static void
vbdev_ut_examine_config(struct spdk_bdev *bdev)
{
	struct ut_examine_ctx *ctx = bdev->ctxt;

	if (ctx != NULL) {
		ctx->examine_config_count++;
		if (ctx->examine_config != NULL) {
			ctx->examine_config(bdev);
		}
	}

	spdk_bdev_module_examine_done(&vbdev_ut_if);
}

static void
vbdev_ut_examine_disk(struct spdk_bdev *bdev)
{
	struct ut_examine_ctx *ctx = bdev->ctxt;

	if (ctx != NULL) {
		ctx->examine_disk_count++;
		if (ctx->examine_disk != NULL) {
			ctx->examine_disk(bdev);
		}
	}

	spdk_bdev_module_examine_done(&vbdev_ut_if);
}

static struct spdk_bdev *
allocate_bdev_ctx(char *name, void *ctx)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->ctxt = ctx;
	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = &bdev_ut_if;
	bdev->blockcnt = 1024;
	bdev->blocklen = 512;

	spdk_uuid_generate(&bdev->uuid);

	rc = spdk_bdev_register(bdev);
	poll_threads();
	CU_ASSERT(rc == 0);

	return bdev;
}

static struct spdk_bdev *
allocate_bdev(char *name)
{
	return allocate_bdev_ctx(name, NULL);
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
	poll_threads();
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
bdev_open_cb3(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	g_event_type3 = type;
}

static void
bdev_open_cb4(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	g_event_type4 = type;
}

static void
bdev_seek_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_seek_offset = spdk_bdev_io_get_seek_offset(bdev_io);
	spdk_bdev_free_io(bdev_io);
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
claim_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc, *open_desc;
	int rc;
	uint32_t count;

	/*
	 * A vbdev that uses a read-only bdev may need it to remain read-only.
	 * To do so, it opens the bdev read-only, then claims it without
	 * passing a spdk_bdev_desc.
	 */
	bdev = allocate_bdev("bdev0");
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc->write == false);

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);
	CU_ASSERT(bdev->internal.claim.v1.module == &bdev_ut_if);

	/* There should be only one open descriptor and it should still be ro */
	count = 0;
	TAILQ_FOREACH(open_desc, &bdev->internal.open_descs, link) {
		CU_ASSERT(open_desc == desc);
		CU_ASSERT(!open_desc->write);
		count++;
	}
	CU_ASSERT(count == 1);

	/* A read-only bdev is upgraded to read-write if desc is passed. */
	spdk_bdev_module_release_bdev(bdev);
	rc = spdk_bdev_module_claim_bdev(bdev, desc, &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);
	CU_ASSERT(bdev->internal.claim.v1.module == &bdev_ut_if);

	/* There should be only one open descriptor and it should be rw */
	count = 0;
	TAILQ_FOREACH(open_desc, &bdev->internal.open_descs, link) {
		CU_ASSERT(open_desc == desc);
		CU_ASSERT(open_desc->write);
		count++;
	}
	CU_ASSERT(count == 1);

	spdk_bdev_close(desc);
	free_bdev(bdev);
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
	int rc;

	memset(&bdev, 0, sizeof(bdev));
	bdev.name = "num_blocks";
	bdev.fn_table = &fn_table;
	bdev.module = &bdev_ut_if;
	spdk_bdev_register(&bdev);
	poll_threads();
	spdk_bdev_notify_blockcnt_change(&bdev, 50);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 70) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 30) == 0);

	rc = spdk_bdev_open_ext("num_blocks", false, bdev_open_cb1, &desc, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(&bdev == spdk_bdev_desc_get_bdev(desc));

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 80) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 20) != 0);

	g_event_type1 = 0xFF;
	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 90) == 0);

	poll_threads();
	CU_ASSERT_EQUAL(g_event_type1, SPDK_BDEV_EVENT_RESIZE);

	g_event_type1 = 0xFF;
	/* Growing block number and closing */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 100) == 0);

	spdk_bdev_close(desc);
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
	spdk_spin_init(&bdev.internal.spinlock);

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

	spdk_spin_destroy(&bdev.internal.spinlock);
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
	if ((bdev_io->type == SPDK_BDEV_IO_TYPE_ZCOPY) &&
	    (bdev_io->u.bdev.zcopy.start)) {
		g_zcopy_bdev_io = bdev_io;
	} else {
		spdk_bdev_free_io(bdev_io);
		g_zcopy_bdev_io = NULL;
	}
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

static void
ut_init_bdev(struct spdk_bdev_opts *opts)
{
	int rc;

	if (opts != NULL) {
		rc = spdk_bdev_set_opts(opts);
		CU_ASSERT(rc == 0);
	}
	rc = spdk_iobuf_initialize();
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);
	poll_threads();
}

static void
ut_fini_bdev(void)
{
	spdk_bdev_finish(bdev_fini_cb, NULL);
	spdk_iobuf_finish(bdev_fini_cb, NULL);
	poll_threads();
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
	struct spdk_bdev_opts bdev_opts = {};
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 4;
	bdev_opts.bdev_io_cache_size = 2;
	ut_init_bdev(&bdev_opts);

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

	/* COPY is not supported */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_COPY, false);
	rc = spdk_bdev_copy_blocks(desc, io_ch, 128, 0, 128, io_done, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_COPY, true);

	/* NVME_IO, NVME_IO_MD and NVME_ADMIN are not supported */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_NVME_IO, false);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_NVME_IO_MD, false);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_NVME_ADMIN, false);
	rc = spdk_bdev_nvme_io_passthru(desc, io_ch, NULL, NULL, 0, NULL, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	rc = spdk_bdev_nvme_io_passthru_md(desc, io_ch, NULL, NULL, 0, NULL, 0, NULL, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	rc = spdk_bdev_nvme_admin_passthru(desc, io_ch, NULL, NULL, 0, NULL, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_NVME_IO, true);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_NVME_IO_MD, true);
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_NVME_ADMIN, true);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_io_wait_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	struct bdev_ut_io_wait_entry io_wait_entry;
	struct bdev_ut_io_wait_entry io_wait_entry2;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 4;
	bdev_opts.bdev_io_cache_size = 2;
	ut_init_bdev(&bdev_opts);

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
	ut_fini_bdev();
}

static void
bdev_io_spans_split_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_bdev_io bdev_io;
	struct iovec iov[SPDK_BDEV_IO_NUM_CHILD_IOV];

	memset(&bdev, 0, sizeof(bdev));
	bdev_io.u.bdev.iovs = iov;

	bdev_io.type = SPDK_BDEV_IO_TYPE_READ;
	bdev.optimal_io_boundary = 0;
	bdev.max_segment_size = 0;
	bdev.max_num_segments = 0;
	bdev_io.bdev = &bdev;

	/* bdev has no optimal_io_boundary and max_size set - so this should return false. */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == false);

	bdev.split_on_optimal_io_boundary = true;
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

	bdev_io.u.bdev.num_blocks = 32;
	bdev.max_segment_size = 512 * 32;
	bdev.max_num_segments = 1;
	bdev_io.u.bdev.iovcnt = 1;
	iov[0].iov_len = 512;

	/* Does not cross and exceed max_size or max_segs */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == false);

	bdev.split_on_optimal_io_boundary = false;
	bdev.max_segment_size = 512;
	bdev.max_num_segments = 1;
	bdev_io.u.bdev.iovcnt = 2;

	/* Exceed max_segs */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == true);

	bdev.max_num_segments = 2;
	iov[0].iov_len = 513;
	iov[1].iov_len = 512;

	/* Exceed max_sizes */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == true);

	bdev.max_segment_size = 0;
	bdev.write_unit_size = 32;
	bdev.split_on_write_unit = true;
	bdev_io.type = SPDK_BDEV_IO_TYPE_WRITE;

	/* This I/O is one write unit */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == false);

	bdev_io.u.bdev.num_blocks = 32 * 2;

	/* This I/O is more than one write unit */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == true);

	bdev_io.u.bdev.offset_blocks = 1;
	bdev_io.u.bdev.num_blocks = 32;

	/* This I/O is not aligned to write unit size */
	CU_ASSERT(bdev_io_should_split(&bdev_io) == true);
}

static void
bdev_io_boundary_split_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	struct iovec iov[SPDK_BDEV_IO_NUM_CHILD_IOV * 2];
	struct ut_expected_io *expected_io;
	void *md_buf = (void *)0xFF000000;
	uint64_t i;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	ut_init_bdev(&bdev_opts);

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
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV * 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}

	bdev->optimal_io_boundary = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, SPDK_BDEV_IO_NUM_CHILD_IOV,
					   SPDK_BDEV_IO_NUM_CHILD_IOV);
	expected_io->md_buf = md_buf;
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i, (void *)((i + 1) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV,
					   SPDK_BDEV_IO_NUM_CHILD_IOV, SPDK_BDEV_IO_NUM_CHILD_IOV);
	expected_io->md_buf = md_buf + SPDK_BDEV_IO_NUM_CHILD_IOV * 8;
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1 + SPDK_BDEV_IO_NUM_CHILD_IOV) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV * 2, md_buf,
					    0, SPDK_BDEV_IO_NUM_CHILD_IOV * 2, io_done, NULL);
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
	 * is SPDK_BDEV_IO_NUM_CHILD_IOV + 1, which exceeds the capacity of child iovs.
	 */
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	for (i = SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 256;
	}
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV].iov_base = (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV].iov_len = 512;

	/* Add an extra iovec to trigger split */
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV + 1].iov_base = (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV + 1].iov_len = 512;

	bdev->optimal_io_boundary = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0,
					   SPDK_BDEV_IO_NUM_CHILD_IOV - 1, SPDK_BDEV_IO_NUM_CHILD_IOV);
	expected_io->md_buf = md_buf;
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1) * 0x10000), 512);
	}
	for (i = SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1) * 0x10000), 256);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV - 1,
					   1, 1);
	expected_io->md_buf = md_buf + (SPDK_BDEV_IO_NUM_CHILD_IOV - 1) * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV,
					   1, 1);
	expected_io->md_buf = md_buf + SPDK_BDEV_IO_NUM_CHILD_IOV * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV + 2, md_buf,
					    0, SPDK_BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
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
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 1].iov_base = (void *)(SPDK_BDEV_IO_NUM_CHILD_IOV * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 1].iov_len = 256;

	iov[SPDK_BDEV_IO_NUM_CHILD_IOV].iov_base = (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV].iov_len = 256;

	iov[SPDK_BDEV_IO_NUM_CHILD_IOV + 1].iov_base = (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV + 1].iov_len = 512;

	bdev->optimal_io_boundary = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	g_io_status = 0;
	/* The first expected io should be start from offset 0 to SPDK_BDEV_IO_NUM_CHILD_IOV - 1 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0,
					   SPDK_BDEV_IO_NUM_CHILD_IOV - 1, SPDK_BDEV_IO_NUM_CHILD_IOV - 1);
	expected_io->md_buf = md_buf;
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		ut_expected_io_set_iov(expected_io, i,
				       (void *)((i + 1) * 0x10000), 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	/* The second expected io should be start from offset SPDK_BDEV_IO_NUM_CHILD_IOV - 1 to SPDK_BDEV_IO_NUM_CHILD_IOV */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV - 1,
					   1, 2);
	expected_io->md_buf = md_buf + (SPDK_BDEV_IO_NUM_CHILD_IOV - 1) * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)(SPDK_BDEV_IO_NUM_CHILD_IOV * 0x10000), 256);
	ut_expected_io_set_iov(expected_io, 1,
			       (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000), 256);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	/* The third expected io should be start from offset SPDK_BDEV_IO_NUM_CHILD_IOV to SPDK_BDEV_IO_NUM_CHILD_IOV + 1 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV,
					   1, 1);
	expected_io->md_buf = md_buf + SPDK_BDEV_IO_NUM_CHILD_IOV * 8;
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_with_md(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV * 2, md_buf,
					    0, SPDK_BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
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

	/* Test a COPY.  This should also not be split. */
	bdev->optimal_io_boundary = 15;
	g_io_done = false;
	expected_io = ut_alloc_expected_copy_io(SPDK_BDEV_IO_TYPE_COPY, 9, 45, 36);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_copy_blocks(desc, io_ch, 9, 45, 36, io_done, NULL);
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

	/* Test if a multi vector command terminated with failure before continuing
	 * splitting process when one of child I/O failed.
	 * The multi vector command is as same as the above that needs to be split by strip
	 * and then needs to be split further due to the capacity of child iovs.
	 */
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 1; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 1].iov_base = (void *)(SPDK_BDEV_IO_NUM_CHILD_IOV * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 1].iov_len = 256;

	iov[SPDK_BDEV_IO_NUM_CHILD_IOV].iov_base = (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 1) * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV].iov_len = 256;

	iov[SPDK_BDEV_IO_NUM_CHILD_IOV + 1].iov_base = (void *)((SPDK_BDEV_IO_NUM_CHILD_IOV + 2) * 0x10000);
	iov[SPDK_BDEV_IO_NUM_CHILD_IOV + 1].iov_len = 512;

	bdev->optimal_io_boundary = SPDK_BDEV_IO_NUM_CHILD_IOV;

	g_io_exp_status = SPDK_BDEV_IO_STATUS_FAILED;
	g_io_done = false;
	g_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV * 2, 0,
				    SPDK_BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
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
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV + 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 0x212;
	}

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, SPDK_BDEV_IO_NUM_CHILD_IOV,
					   SPDK_BDEV_IO_NUM_CHILD_IOV - 1);
	/* expect 0-29 to be 1:1 with the parent iov */
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		ut_expected_io_set_iov(expected_io, i, iov[i].iov_base, iov[i].iov_len);
	}

	/* expect index 30 to be shortened to 0x1e4 (0x212 - 0x1e) because of the alignment
	 * where 0x1e is the amount we overshot the 16K boundary
	 */
	ut_expected_io_set_iov(expected_io, SPDK_BDEV_IO_NUM_CHILD_IOV - 2,
			       (void *)(iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 2].iov_base), 0x1e4);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* 2nd child IO will have 2 remaining vectors, one to pick up from the one that was
	 * shortened that take it to the next boundary and then a final one to get us to
	 * 0x4200 bytes for the IO.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV,
					   SPDK_BDEV_IO_NUM_CHILD_IOV, 2);
	/* position 30 picked up the remaining bytes to the next boundary */
	ut_expected_io_set_iov(expected_io, 0,
			       (void *)(iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 2].iov_base + 0x1e4), 0x2e);

	/* position 31 picked the the rest of the transfer to get us to 0x4200 */
	ut_expected_io_set_iov(expected_io, 1,
			       (void *)(iov[SPDK_BDEV_IO_NUM_CHILD_IOV - 1].iov_base), 0x1d2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV + 1, 0,
				    SPDK_BDEV_IO_NUM_CHILD_IOV + 1, io_done, NULL);
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
	ut_fini_bdev();
}

static void
bdev_io_max_size_and_segment_split_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	struct iovec iov[SPDK_BDEV_IO_NUM_CHILD_IOV * 2];
	struct ut_expected_io *expected_io;
	uint64_t i;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	bdev_opts.opts_size = sizeof(bdev_opts);
	ut_init_bdev(&bdev_opts);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext(bdev->name, true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	bdev->split_on_optimal_io_boundary = false;
	bdev->optimal_io_boundary = 0;

	/* Case 0 max_num_segments == 0.
	 * but segment size 2 * 512 > 512
	 */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = 0;
	g_io_done = false;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)(0xF000 + 512), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 1 max_segment_size == 0
	 * but iov num 2 > 1.
	 */
	bdev->max_segment_size = 0;
	bdev->max_num_segments = 1;
	g_io_done = false;

	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 512;
	iov[1].iov_base = (void *)0x20000;
	iov[1].iov_len = 8 * 512;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 1, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[0].iov_base, iov[0].iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 15, 8, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[1].iov_base, iov[1].iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, 2, 14, 9, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test that a non-vector command is split correctly.
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = 1;
	g_io_done = false;

	/* Child IO 0 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 1, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* Child IO 1 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 15, 1, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 1 * 512), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* spdk_bdev_read_blocks will submit the first child immediately. */
	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Now set up a more complex, multi-vector command that needs to be split,
	 * including splitting iovecs.
	 */
	bdev->max_segment_size = 2 * 512;
	bdev->max_num_segments = 1;
	g_io_done = false;

	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 2 * 512;
	iov[1].iov_base = (void *)0x20000;
	iov[1].iov_len = 4 * 512;
	iov[2].iov_base = (void *)0x30000;
	iov[2].iov_len = 6 * 512;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 14, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[0].iov_base, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* Split iov[1].size to 2 iov entries then split the segments */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[1].iov_base, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 18, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[1].iov_base + 512 * 2, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* Split iov[2].size to 3 iov entries then split the segments */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 20, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[2].iov_base, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 22, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[2].iov_base + 512 * 2, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 24, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[2].iov_base + 512 * 4, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks(desc, io_ch, iov, 3, 14, 12, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 6);
	stub_complete_io(6);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test multi vector command that needs to be split by strip and then needs to be
	 * split further due to the capacity of parent IO child iovs.
	 */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = 1;
	g_io_done = false;

	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512 * 2;
	}

	/* Each input iov.size is split into 2 iovs,
	 * half of the input iov can fill all child iov entries of a single IO.
	 */
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV / 2; i++) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 2 * i, 1, 1);
		ut_expected_io_set_iov(expected_io, 0, iov[i].iov_base, 512);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 2 * i + 1, 1, 1);
		ut_expected_io_set_iov(expected_io, 0, iov[i].iov_base + 512, 512);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	}

	/* The remaining iov is split in the second round */
	for (i = SPDK_BDEV_IO_NUM_CHILD_IOV / 2; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, i * 2, 1, 1);
		ut_expected_io_set_iov(expected_io, 0, iov[i].iov_base, 512);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, i * 2 + 1, 1, 1);
		ut_expected_io_set_iov(expected_io, 0, iov[i].iov_base + 512, 512);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	}

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV, 0,
				    SPDK_BDEV_IO_NUM_CHILD_IOV * 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == SPDK_BDEV_IO_NUM_CHILD_IOV);
	stub_complete_io(SPDK_BDEV_IO_NUM_CHILD_IOV);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == SPDK_BDEV_IO_NUM_CHILD_IOV);
	stub_complete_io(SPDK_BDEV_IO_NUM_CHILD_IOV);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* A wrong case, a child IO that is divided does
	 * not meet the principle of multiples of block size,
	 * and exits with error
	 */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = 1;
	g_io_done = false;

	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 512 + 256;
	iov[1].iov_base = (void *)0x20000;
	iov[1].iov_len = 256;

	/* iov[0] is split to 512 and 256.
	 * 256 is less than a block size, and it is found
	 * in the next round of split that it is the first child IO smaller than
	 * the block size, so the error exit
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, 1, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[0].iov_base, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, 2, 0, 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	/* First child IO is OK */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* error exit */
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test multi vector command that needs to be split by strip and then needs to be
	 * split further due to the capacity of child iovs.
	 *
	 * In this case, the last two iovs need to be split, but it will exceed the capacity
	 * of child iovs, so it needs to wait until the first batch completed.
	 */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;

	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}
	for (i = SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i < SPDK_BDEV_IO_NUM_CHILD_IOV; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512 * 2;
	}

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0,
					   SPDK_BDEV_IO_NUM_CHILD_IOV, SPDK_BDEV_IO_NUM_CHILD_IOV);
	/* 0 ~ (SPDK_BDEV_IO_NUM_CHILD_IOV - 2) Will not be split */
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		ut_expected_io_set_iov(expected_io, i, iov[i].iov_base, iov[i].iov_len);
	}
	/* (SPDK_BDEV_IO_NUM_CHILD_IOV - 2) is split */
	ut_expected_io_set_iov(expected_io, i, iov[i].iov_base, 512);
	ut_expected_io_set_iov(expected_io, i + 1, iov[i].iov_base + 512, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* Child iov entries exceed the max num of parent IO so split it in next round */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, iov[i + 1].iov_base, 512);
	ut_expected_io_set_iov(expected_io, 1, iov[i + 1].iov_base + 512, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV, 0,
				    SPDK_BDEV_IO_NUM_CHILD_IOV + 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	/* Next round */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* This case is similar to the previous one, but the io composed of
	 * the last few entries of child iov is not enough for a blocklen, so they
	 * cannot be put into this IO, but wait until the next time.
	 */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;

	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}

	for (i = SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i < SPDK_BDEV_IO_NUM_CHILD_IOV + 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 128;
	}

	/* First child iovcnt is't SPDK_BDEV_IO_NUM_CHILD_IOV but SPDK_BDEV_IO_NUM_CHILD_IOV - 2.
	 * Because the left 2 iov is not enough for a blocklen.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0,
					   SPDK_BDEV_IO_NUM_CHILD_IOV - 2, SPDK_BDEV_IO_NUM_CHILD_IOV - 2);
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		ut_expected_io_set_iov(expected_io, i, iov[i].iov_base, iov[i].iov_len);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The second child io waits until the end of the first child io before executing.
	 * Because the iovcnt of the two IOs exceeds the child iovcnt of the parent IO.
	 * SPDK_BDEV_IO_NUM_CHILD_IOV - 2 to SPDK_BDEV_IO_NUM_CHILD_IOV + 2
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, SPDK_BDEV_IO_NUM_CHILD_IOV - 2,
					   1, 4);
	ut_expected_io_set_iov(expected_io, 0, iov[i].iov_base, iov[i].iov_len);
	ut_expected_io_set_iov(expected_io, 1, iov[i + 1].iov_base, iov[i + 1].iov_len);
	ut_expected_io_set_iov(expected_io, 2, iov[i + 2].iov_base, iov[i + 2].iov_len);
	ut_expected_io_set_iov(expected_io, 3, iov[i + 3].iov_base, iov[i + 3].iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV + 2, 0,
				    SPDK_BDEV_IO_NUM_CHILD_IOV - 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* A very complicated case. Each sg entry exceeds max_segment_size and
	 * needs to be split. At the same time, child io must be a multiple of blocklen.
	 * At the same time, child iovcnt exceeds parent iovcnt.
	 */
	bdev->max_segment_size = 512 + 128;
	bdev->max_num_segments = 3;
	g_io_done = false;

	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512 + 256;
	}

	for (i = SPDK_BDEV_IO_NUM_CHILD_IOV - 2; i < SPDK_BDEV_IO_NUM_CHILD_IOV + 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512 + 128;
	}

	/* Child IOs use 9 entries per for() round and 3 * 9 = 27 child iov entries.
	 * Consume 4 parent IO iov entries per for() round and 6 block size.
	 * Generate 9 child IOs.
	 */
	for (i = 0; i < 3; i++) {
		uint32_t j = i * 4;
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, i * 6, 2, 3);
		ut_expected_io_set_iov(expected_io, 0, iov[j].iov_base, 640);
		ut_expected_io_set_iov(expected_io, 1, iov[j].iov_base + 640, 128);
		ut_expected_io_set_iov(expected_io, 2, iov[j + 1].iov_base, 256);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

		/* Child io must be a multiple of blocklen
		 * iov[j + 2] must be split. If the third entry is also added,
		 * the multiple of blocklen cannot be guaranteed. But it still
		 * occupies one iov entry of the parent child iov.
		 */
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, i * 6 + 2, 2, 2);
		ut_expected_io_set_iov(expected_io, 0, iov[j + 1].iov_base + 256, 512);
		ut_expected_io_set_iov(expected_io, 1, iov[j + 2].iov_base, 512);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, i * 6 + 4, 2, 3);
		ut_expected_io_set_iov(expected_io, 0, iov[j + 2].iov_base + 512, 256);
		ut_expected_io_set_iov(expected_io, 1, iov[j + 3].iov_base, 640);
		ut_expected_io_set_iov(expected_io, 2, iov[j + 3].iov_base + 640, 128);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	}

	/* Child iov position at 27, the 10th child IO
	 * iov entry index is 3 * 4 and offset is 3 * 6
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 18, 2, 3);
	ut_expected_io_set_iov(expected_io, 0, iov[12].iov_base, 640);
	ut_expected_io_set_iov(expected_io, 1, iov[12].iov_base + 640, 128);
	ut_expected_io_set_iov(expected_io, 2, iov[13].iov_base, 256);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* Child iov position at 30, the 11th child IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 20, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, iov[13].iov_base + 256, 512);
	ut_expected_io_set_iov(expected_io, 1, iov[14].iov_base, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 2nd split round and iovpos is 0, the 12th child IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 22, 2, 3);
	ut_expected_io_set_iov(expected_io, 0, iov[14].iov_base + 512, 256);
	ut_expected_io_set_iov(expected_io, 1, iov[15].iov_base, 640);
	ut_expected_io_set_iov(expected_io, 2, iov[15].iov_base + 640, 128);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* Consume 9 child IOs and 27 child iov entries.
	 * Consume 4 parent IO iov entries per for() round and 6 block size.
	 * Parent IO iov index start from 16 and block offset start from 24
	 */
	for (i = 0; i < 3; i++) {
		uint32_t j = i * 4 + 16;
		uint32_t offset = i * 6 + 24;
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, offset, 2, 3);
		ut_expected_io_set_iov(expected_io, 0, iov[j].iov_base, 640);
		ut_expected_io_set_iov(expected_io, 1, iov[j].iov_base + 640, 128);
		ut_expected_io_set_iov(expected_io, 2, iov[j + 1].iov_base, 256);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

		/* Child io must be a multiple of blocklen
		 * iov[j + 2] must be split. If the third entry is also added,
		 * the multiple of blocklen cannot be guaranteed. But it still
		 * occupies one iov entry of the parent child iov.
		 */
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, offset + 2, 2, 2);
		ut_expected_io_set_iov(expected_io, 0, iov[j + 1].iov_base + 256, 512);
		ut_expected_io_set_iov(expected_io, 1, iov[j + 2].iov_base, 512);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, offset + 4, 2, 3);
		ut_expected_io_set_iov(expected_io, 0, iov[j + 2].iov_base + 512, 256);
		ut_expected_io_set_iov(expected_io, 1, iov[j + 3].iov_base, 640);
		ut_expected_io_set_iov(expected_io, 2, iov[j + 3].iov_base + 640, 128);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	}

	/* The 22th child IO, child iov position at 30 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 42, 1, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[28].iov_base, 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The third round */
	/* Here is the 23nd child IO and child iovpos is 0 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 43, 2, 3);
	ut_expected_io_set_iov(expected_io, 0, iov[28].iov_base + 512, 256);
	ut_expected_io_set_iov(expected_io, 1, iov[29].iov_base, 640);
	ut_expected_io_set_iov(expected_io, 2, iov[29].iov_base + 640, 128);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 24th child IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 45, 3, 3);
	ut_expected_io_set_iov(expected_io, 0, iov[30].iov_base, 640);
	ut_expected_io_set_iov(expected_io, 1, iov[31].iov_base, 640);
	ut_expected_io_set_iov(expected_io, 2, iov[32].iov_base, 256);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 25th child IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 48, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, iov[32].iov_base + 256, 384);
	ut_expected_io_set_iov(expected_io, 1, iov[33].iov_base, 640);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV + 2, 0,
				    50, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	/* Parent IO supports up to 32 child iovs, so it is calculated that
	 * a maximum of 11 IOs can be split at a time, and the
	 * splitting will continue after the first batch is over.
	 */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 11);
	stub_complete_io(11);
	CU_ASSERT(g_io_done == false);

	/* The 2nd round */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 11);
	stub_complete_io(11);
	CU_ASSERT(g_io_done == false);

	/* The last round */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 3);
	stub_complete_io(3);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Test an WRITE_ZEROES.  This should also not be split. */
	bdev->max_segment_size = 512;
	bdev->max_num_segments = 1;
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
	g_io_done = false;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_UNMAP, 15, 4, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_unmap_blocks(desc, io_ch, 15, 4, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	/* Test a FLUSH.  This should also not be split. */
	g_io_done = false;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_FLUSH, 15, 4, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_flush_blocks(desc, io_ch, 15, 2, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	/* Test a COPY.  This should also not be split. */
	g_io_done = false;

	expected_io = ut_alloc_expected_copy_io(SPDK_BDEV_IO_TYPE_COPY, 9, 45, 36);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_copy_blocks(desc, io_ch, 9, 45, 36, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_io_mix_split_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	struct iovec iov[SPDK_BDEV_IO_NUM_CHILD_IOV * 2];
	struct ut_expected_io *expected_io;
	uint64_t i;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	ut_init_bdev(&bdev_opts);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext(bdev->name, true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* First case optimal_io_boundary == max_segment_size * max_num_segments */
	bdev->split_on_optimal_io_boundary = true;
	bdev->optimal_io_boundary = 16;

	bdev->max_segment_size = 512;
	bdev->max_num_segments = 16;
	g_io_done = false;

	/* IO crossing the IO boundary requires split
	 * Total 2 child IOs.
	 */

	/* The 1st child IO split the segment_size to multiple segment entry */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)(0xF000 + 512), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 2nd child IO split the segment_size to multiple segment entry */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 2, 2);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 2 * 512), 512);
	ut_expected_io_set_iov(expected_io, 1, (void *)(0xF000 + 3 * 512), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 14, 4, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Second case optimal_io_boundary > max_segment_size * max_num_segments */
	bdev->max_segment_size = 15 * 512;
	bdev->max_num_segments = 1;
	g_io_done = false;

	/* IO crossing the IO boundary requires split.
	 * The 1st child IO segment size exceeds the max_segment_size,
	 * So 1st child IO will be split to multiple segment entry.
	 * Then it split to 2 child IOs because of the max_num_segments.
	 * Total 3 child IOs.
	 */

	/* The first 2 IOs are in an IO boundary.
	 * Because the optimal_io_boundary > max_segment_size * max_num_segments
	 * So it split to the first 2 IOs.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, 15, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 512 * 15);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 15, 1, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 512 * 15), 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 3rd Child IO is because of the io boundary */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 512 * 16), 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 0, 18, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 3);
	stub_complete_io(3);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Third case optimal_io_boundary < max_segment_size * max_num_segments */
	bdev->max_segment_size = 17 * 512;
	bdev->max_num_segments = 1;
	g_io_done = false;

	/* IO crossing the IO boundary requires split.
	 * Child IO does not split.
	 * Total 2 child IOs.
	 */

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, 16, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 512 * 16);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 512 * 16), 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_read_blocks(desc, io_ch, (void *)0xF000, 0, 18, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Now set up a more complex, multi-vector command that needs to be split,
	 * including splitting iovecs.
	 * optimal_io_boundary < max_segment_size * max_num_segments
	 */
	bdev->max_segment_size = 3 * 512;
	bdev->max_num_segments = 6;
	g_io_done = false;

	iov[0].iov_base = (void *)0x10000;
	iov[0].iov_len = 4 * 512;
	iov[1].iov_base = (void *)0x20000;
	iov[1].iov_len = 4 * 512;
	iov[2].iov_base = (void *)0x30000;
	iov[2].iov_len = 10 * 512;

	/* IO crossing the IO boundary requires split.
	 * The 1st child IO segment size exceeds the max_segment_size and after
	 * splitting segment_size, the num_segments exceeds max_num_segments.
	 * So 1st child IO will be split to 2 child IOs.
	 * Total 3 child IOs.
	 */

	/* The first 2 IOs are in an IO boundary.
	 * After splitting segment size the segment num exceeds.
	 * So it splits to 2 child IOs.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 0, 14, 6);
	ut_expected_io_set_iov(expected_io, 0, iov[0].iov_base, 512 * 3);
	ut_expected_io_set_iov(expected_io, 1, iov[0].iov_base + 512 * 3, 512);
	ut_expected_io_set_iov(expected_io, 2, iov[1].iov_base, 512 * 3);
	ut_expected_io_set_iov(expected_io, 3, iov[1].iov_base + 512 * 3, 512);
	ut_expected_io_set_iov(expected_io, 4, iov[2].iov_base, 512 * 3);
	ut_expected_io_set_iov(expected_io, 5, iov[2].iov_base + 512 * 3, 512 * 3);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* The 2nd child IO has the left segment entry */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 14, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[2].iov_base + 512 * 6, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 2, 1);
	ut_expected_io_set_iov(expected_io, 0, iov[2].iov_base + 512 * 8, 512 * 2);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks(desc, io_ch, iov, 3, 0, 18, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 3);
	stub_complete_io(3);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* A very complicated case. Each sg entry exceeds max_segment_size
	 * and split on io boundary.
	 * optimal_io_boundary < max_segment_size * max_num_segments
	 */
	bdev->max_segment_size = 3 * 512;
	bdev->max_num_segments = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;

	for (i = 0; i < 20; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512 * 4;
	}

	/* IO crossing the IO boundary requires split.
	 * 80 block length can split 5 child IOs base on offset and IO boundary.
	 * Each iov entry needs to be split to 2 entries because of max_segment_size
	 * Total 5 child IOs.
	 */

	/* 4 iov entries are in an IO boundary and each iov entry splits to 2.
	 * So each child IO occupies 8 child iov entries.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 0, 16, 8);
	for (i = 0; i < 4; i++) {
		int iovcnt = i * 2;
		ut_expected_io_set_iov(expected_io, iovcnt, iov[i].iov_base, 512 * 3);
		ut_expected_io_set_iov(expected_io, iovcnt + 1, iov[i].iov_base + 512 * 3, 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* 2nd child IO and total 16 child iov entries of parent IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 16, 8);
	for (i = 4; i < 8; i++) {
		int iovcnt = (i - 4) * 2;
		ut_expected_io_set_iov(expected_io, iovcnt, iov[i].iov_base, 512 * 3);
		ut_expected_io_set_iov(expected_io, iovcnt + 1, iov[i].iov_base + 512 * 3, 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* 3rd child IO and total 24 child iov entries of parent IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 16, 8);
	for (i = 8; i < 12; i++) {
		int iovcnt = (i - 8) * 2;
		ut_expected_io_set_iov(expected_io, iovcnt, iov[i].iov_base, 512 * 3);
		ut_expected_io_set_iov(expected_io, iovcnt + 1, iov[i].iov_base + 512 * 3, 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* 4th child IO and total 32 child iov entries of parent IO */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 48, 16, 8);
	for (i = 12; i < 16; i++) {
		int iovcnt = (i - 12) * 2;
		ut_expected_io_set_iov(expected_io, iovcnt, iov[i].iov_base, 512 * 3);
		ut_expected_io_set_iov(expected_io, iovcnt + 1, iov[i].iov_base + 512 * 3, 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	/* 5th child IO and because of the child iov entry it should be split
	 * in next round.
	 */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 64, 16, 8);
	for (i = 16; i < 20; i++) {
		int iovcnt = (i - 16) * 2;
		ut_expected_io_set_iov(expected_io, iovcnt, iov[i].iov_base, 512 * 3);
		ut_expected_io_set_iov(expected_io, iovcnt + 1, iov[i].iov_base + 512 * 3, 512);
	}
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks(desc, io_ch, iov, 20, 0, 80, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	/* First split round */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);
	stub_complete_io(4);
	CU_ASSERT(g_io_done == false);

	/* Second split round */
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_io_split_with_io_wait(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *channel;
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	struct spdk_bdev_opts bdev_opts = {};
	struct iovec iov[3];
	struct ut_expected_io *expected_io;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 2;
	bdev_opts.bdev_io_cache_size = 1;
	ut_init_bdev(&bdev_opts);

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
	ut_fini_bdev();
}

static void
bdev_io_write_unit_split_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	struct iovec iov[SPDK_BDEV_IO_NUM_CHILD_IOV * 4];
	struct ut_expected_io *expected_io;
	uint64_t i;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	ut_init_bdev(&bdev_opts);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext(bdev->name, true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Write I/O 2x larger than write_unit_size should get split into 2 I/Os */
	bdev->write_unit_size = 32;
	bdev->split_on_write_unit = true;
	g_io_done = false;

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 0, 32, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 32 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 32, 1);
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 32 * 512), 32 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_write_blocks(desc, io_ch, (void *)0xF000, 0, 64, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Same as above but with optimal_io_boundary < write_unit_size - the I/O should be split
	 * based on write_unit_size, not optimal_io_boundary */
	bdev->split_on_optimal_io_boundary = true;
	bdev->optimal_io_boundary = 16;
	g_io_done = false;

	rc = spdk_bdev_write_blocks(desc, io_ch, (void *)0xF000, 0, 64, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Write I/O should fail if it is smaller than write_unit_size */
	g_io_done = false;

	rc = spdk_bdev_write_blocks(desc, io_ch, (void *)0xF000, 0, 31, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);

	/* Same for I/O not aligned to write_unit_size */
	g_io_done = false;

	rc = spdk_bdev_write_blocks(desc, io_ch, (void *)0xF000, 1, 32, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);

	/* Write should fail if it needs to be split but there are not enough iovs to submit
	 * an entire write unit */
	bdev->write_unit_size = SPDK_COUNTOF(iov) / 2;
	g_io_done = false;

	for (i = 0; i < SPDK_COUNTOF(iov); i++) {
		iov[i].iov_base = (void *)(0x1000 + 512 * i);
		iov[i].iov_len = 512;
	}

	rc = spdk_bdev_writev_blocks(desc, io_ch, iov, SPDK_COUNTOF(iov), 0, SPDK_COUNTOF(iov),
				     io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_io_alignment(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	int rc;
	void *buf = NULL;
	struct iovec iovs[2];
	int iovcnt;
	uint64_t alignment;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 20;
	bdev_opts.bdev_io_cache_size = 2;
	ut_init_bdev(&bdev_opts);

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
	ut_fini_bdev();

	free(buf);
}

static void
bdev_io_alignment_with_boundary(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {};
	int rc;
	void *buf = NULL;
	struct iovec iovs[2];
	int iovcnt;
	uint64_t alignment;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 20;
	bdev_opts.bdev_io_cache_size = 2;
	bdev_opts.opts_size = sizeof(bdev_opts);
	ut_init_bdev(&bdev_opts);

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
	ut_fini_bdev();

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
histogram_channel_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	spdk_histogram_data_fn cb_fn = cb_arg;

	g_status = status;

	if (status == 0) {
		spdk_histogram_data_iterate(histogram, cb_fn, NULL);
	}
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

	ut_init_bdev(NULL);

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

	g_count = 0;
	spdk_bdev_channel_get_histogram(ch, histogram_channel_data_cb, histogram_io_count);
	CU_ASSERT(g_status == 0);
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

	spdk_bdev_channel_get_histogram(ch, histogram_channel_data_cb, NULL);
	CU_ASSERT(g_status == -EFAULT);

	spdk_histogram_data_free(histogram);
	spdk_put_io_channel(ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
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
	uint8_t expected_io_type;
	int rc;

	if (emulated) {
		expected_io_type = SPDK_BDEV_IO_TYPE_READ;
	} else {
		expected_io_type = SPDK_BDEV_IO_TYPE_COMPARE;
	}

	memset(aa_buf, 0xaa, sizeof(aa_buf));
	memset(bb_buf, 0xbb, sizeof(bb_buf));

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = !emulated;

	ut_init_bdev(NULL);
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

	/* 1. successful compare */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
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

	/* 2. miscompare */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
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
	ut_fini_bdev();

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = true;

	g_compare_read_buf = NULL;
}

static void
_bdev_compare_with_md(bool emulated)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t offset, num_blocks;
	uint32_t num_completed;
	char buf[1024 + 16 /* 2 * blocklen + 2 * mdlen */];
	char buf_interleaved_miscompare[1024 + 16 /* 2 * blocklen + 2 * mdlen */];
	char buf_miscompare[1024 /* 2 * blocklen */];
	char md_buf[16];
	char md_buf_miscompare[16];
	struct iovec compare_iov;
	uint8_t expected_io_type;
	int rc;

	if (emulated) {
		expected_io_type = SPDK_BDEV_IO_TYPE_READ;
	} else {
		expected_io_type = SPDK_BDEV_IO_TYPE_COMPARE;
	}

	memset(buf, 0xaa, sizeof(buf));
	memset(buf_interleaved_miscompare, 0xaa, sizeof(buf_interleaved_miscompare));
	/* make last md different */
	memset(buf_interleaved_miscompare + 1024 + 8, 0xbb, 8);
	memset(buf_miscompare, 0xbb, sizeof(buf_miscompare));
	memset(md_buf, 0xaa, 16);
	memset(md_buf_miscompare, 0xbb, 16);

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = !emulated;

	ut_init_bdev(NULL);
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
	num_blocks = 2;

	/* interleaved md & data */
	bdev->md_interleave = true;
	bdev->md_len = 8;
	bdev->blocklen = 512 + 8;
	compare_iov.iov_base = buf;
	compare_iov.iov_len = sizeof(buf);

	/* 1. successful compare with md interleaved */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = buf;
	g_compare_read_buf_len = sizeof(buf);
	rc = spdk_bdev_comparev_blocks(desc, ioch, &compare_iov, 1, offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* 2. miscompare with md interleaved */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = buf_interleaved_miscompare;
	g_compare_read_buf_len = sizeof(buf_interleaved_miscompare);
	rc = spdk_bdev_comparev_blocks(desc, ioch, &compare_iov, 1, offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_MISCOMPARE);

	/* Separate data & md buffers */
	bdev->md_interleave = false;
	bdev->blocklen = 512;
	compare_iov.iov_base = buf;
	compare_iov.iov_len = 1024;

	/* 3. successful compare with md separated */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = buf;
	g_compare_read_buf_len = 1024;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_compare_md_buf = md_buf;
	rc = spdk_bdev_comparev_blocks_with_md(desc, ioch, &compare_iov, 1, md_buf,
					       offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* 4. miscompare with md separated where md buf is different */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = buf;
	g_compare_read_buf_len = 1024;
	g_compare_md_buf = md_buf_miscompare;
	rc = spdk_bdev_comparev_blocks_with_md(desc, ioch, &compare_iov, 1, md_buf,
					       offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_MISCOMPARE);

	/* 5. miscompare with md separated where buf is different */
	expected_io = ut_alloc_expected_io(expected_io_type, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	g_io_done = false;
	g_compare_read_buf = buf_miscompare;
	g_compare_read_buf_len = sizeof(buf_miscompare);
	g_compare_md_buf = md_buf;
	rc = spdk_bdev_comparev_blocks_with_md(desc, ioch, &compare_iov, 1, md_buf,
					       offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_MISCOMPARE);

	bdev->md_len = 0;
	g_compare_md_buf = NULL;

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	fn_table.submit_request = stub_submit_request;
	ut_fini_bdev();

	g_io_types_supported[SPDK_BDEV_IO_TYPE_COMPARE] = true;

	g_compare_read_buf = NULL;
}

static void
bdev_compare(void)
{
	_bdev_compare(false);
	_bdev_compare_with_md(false);
}

static void
bdev_compare_emulated(void)
{
	_bdev_compare(true);
	_bdev_compare_with_md(true);
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

	ut_init_bdev(NULL);
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

	/* Test miscompare */
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
	ut_fini_bdev();

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

	ut_init_bdev(NULL);
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
	ut_fini_bdev();
}

static void
bdev_zcopy_write(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t offset, num_blocks;
	uint32_t num_completed;
	char aa_buf[512];
	struct iovec iov;
	int rc;
	const bool populate = false;
	const bool commit = true;

	memset(aa_buf, 0xaa, sizeof(aa_buf));

	ut_init_bdev(NULL);
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	offset = 50;
	num_blocks = 1;
	iov.iov_base = NULL;
	iov.iov_len = 0;

	g_zcopy_read_buf = (void *) 0x1122334455667788UL;
	g_zcopy_read_buf_len = (uint32_t) -1;
	/* Do a zcopy start for a write (populate=false) */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_ZCOPY, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	g_io_done = false;
	g_zcopy_write_buf = aa_buf;
	g_zcopy_write_buf_len = sizeof(aa_buf);
	g_zcopy_bdev_io = NULL;
	rc = spdk_bdev_zcopy_start(desc, ioch, &iov, 1, offset, num_blocks, populate, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* Check that the iov has been set up */
	CU_ASSERT(iov.iov_base == g_zcopy_write_buf);
	CU_ASSERT(iov.iov_len == g_zcopy_write_buf_len);
	/* Check that the bdev_io has been saved */
	CU_ASSERT(g_zcopy_bdev_io != NULL);
	/* Now do the zcopy end for a write (commit=true) */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_ZCOPY, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_zcopy_end(g_zcopy_bdev_io, commit, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* Check the g_zcopy are reset by io_done */
	CU_ASSERT(g_zcopy_write_buf == NULL);
	CU_ASSERT(g_zcopy_write_buf_len == 0);
	/* Check that io_done has freed the g_zcopy_bdev_io */
	CU_ASSERT(g_zcopy_bdev_io == NULL);

	/* Check the zcopy read buffer has not been touched which
	 * ensures that the correct buffers were used.
	 */
	CU_ASSERT(g_zcopy_read_buf == (void *) 0x1122334455667788UL);
	CU_ASSERT(g_zcopy_read_buf_len == (uint32_t) -1);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_zcopy_read(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t offset, num_blocks;
	uint32_t num_completed;
	char aa_buf[512];
	struct iovec iov;
	int rc;
	const bool populate = true;
	const bool commit = false;

	memset(aa_buf, 0xaa, sizeof(aa_buf));

	ut_init_bdev(NULL);
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	offset = 50;
	num_blocks = 1;
	iov.iov_base = NULL;
	iov.iov_len = 0;

	g_zcopy_write_buf = (void *) 0x1122334455667788UL;
	g_zcopy_write_buf_len = (uint32_t) -1;

	/* Do a zcopy start for a read (populate=true) */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_ZCOPY, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	g_io_done = false;
	g_zcopy_read_buf = aa_buf;
	g_zcopy_read_buf_len = sizeof(aa_buf);
	g_zcopy_bdev_io = NULL;
	rc = spdk_bdev_zcopy_start(desc, ioch, &iov, 1, offset, num_blocks, populate, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* Check that the iov has been set up */
	CU_ASSERT(iov.iov_base == g_zcopy_read_buf);
	CU_ASSERT(iov.iov_len == g_zcopy_read_buf_len);
	/* Check that the bdev_io has been saved */
	CU_ASSERT(g_zcopy_bdev_io != NULL);

	/* Now do the zcopy end for a read (commit=false) */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_ZCOPY, offset, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_zcopy_end(g_zcopy_bdev_io, commit, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* Check the g_zcopy are reset by io_done */
	CU_ASSERT(g_zcopy_read_buf == NULL);
	CU_ASSERT(g_zcopy_read_buf_len == 0);
	/* Check that io_done has freed the g_zcopy_bdev_io */
	CU_ASSERT(g_zcopy_bdev_io == NULL);

	/* Check the zcopy write buffer has not been touched which
	 * ensures that the correct buffers were used.
	 */
	CU_ASSERT(g_zcopy_write_buf == (void *) 0x1122334455667788UL);
	CU_ASSERT(g_zcopy_write_buf_len == (uint32_t) -1);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
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
	/* Bdev unregister is handled asynchronously. Poll thread to complete. */
	poll_threads();

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

static void
bdev_open_ext_unregister(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc1 = NULL;
	struct spdk_bdev_desc *desc2 = NULL;
	struct spdk_bdev_desc *desc3 = NULL;
	struct spdk_bdev_desc *desc4 = NULL;
	int rc = 0;

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, NULL, NULL, &desc1);
	CU_ASSERT_EQUAL(rc, -EINVAL);

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb1, &desc1, &desc1);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb2, &desc2, &desc2);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb3, &desc3, &desc3);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_bdev_open_ext("bdev", true, bdev_open_cb4, &desc4, &desc4);
	CU_ASSERT_EQUAL(rc, 0);

	g_event_type1 = 0xFF;
	g_event_type2 = 0xFF;
	g_event_type3 = 0xFF;
	g_event_type4 = 0xFF;

	g_unregister_arg = NULL;
	g_unregister_rc = -1;

	/* Simulate hot-unplug by unregistering bdev */
	spdk_bdev_unregister(bdev, bdev_unregister_cb, (void *)0x12345678);

	/*
	 * Unregister is handled asynchronously and event callback
	 * (i.e., above bdev_open_cbN) will be called.
	 * For bdev_open_cb3 and bdev_open_cb4, it is intended to not
	 * close the desc3 and desc4 so that the bdev is not closed.
	 */
	poll_threads();

	/* Check if correct events have been triggered in event callback fn */
	CU_ASSERT_EQUAL(g_event_type1, SPDK_BDEV_EVENT_REMOVE);
	CU_ASSERT_EQUAL(g_event_type2, SPDK_BDEV_EVENT_REMOVE);
	CU_ASSERT_EQUAL(g_event_type3, SPDK_BDEV_EVENT_REMOVE);
	CU_ASSERT_EQUAL(g_event_type4, SPDK_BDEV_EVENT_REMOVE);

	/* Check that unregister callback is delayed */
	CU_ASSERT(g_unregister_arg == NULL);
	CU_ASSERT(g_unregister_rc == -1);

	/*
	 * Explicitly close desc3. As desc4 is still opened there, the
	 * unergister callback is still delayed to execute.
	 */
	spdk_bdev_close(desc3);
	CU_ASSERT(g_unregister_arg == NULL);
	CU_ASSERT(g_unregister_rc == -1);

	/*
	 * Explicitly close desc4 to trigger the ongoing bdev unregister
	 * operation after last desc is closed.
	 */
	spdk_bdev_close(desc4);

	/* Poll the thread for the async unregister operation */
	poll_threads();

	/* Check that unregister callback is executed */
	CU_ASSERT(g_unregister_arg == (void *)0x12345678);
	CU_ASSERT(g_unregister_rc == 0);

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

	ut_init_bdev(NULL);
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
	ut_fini_bdev();
}

static void
bdev_set_qd_sampling(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch = NULL;
	struct spdk_bdev_channel *bdev_ch = NULL;
	struct timeout_io_cb_arg cb_arg;

	ut_init_bdev(NULL);
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

	/* This is the part2.
	 * Test the bdev's qd poller register
	 */
	/* 1st Successfully set the qd sampling period */
	spdk_bdev_set_qd_sampling_period(bdev, 10);
	CU_ASSERT(bdev->internal.new_period == 10);
	CU_ASSERT(bdev->internal.period == 10);
	CU_ASSERT(bdev->internal.qd_desc != NULL);
	poll_threads();
	CU_ASSERT(bdev->internal.qd_poller != NULL);

	/* 2nd Change the qd sampling period */
	spdk_bdev_set_qd_sampling_period(bdev, 20);
	CU_ASSERT(bdev->internal.new_period == 20);
	CU_ASSERT(bdev->internal.period == 10);
	CU_ASSERT(bdev->internal.qd_desc != NULL);
	poll_threads();
	CU_ASSERT(bdev->internal.qd_poller != NULL);
	CU_ASSERT(bdev->internal.period == bdev->internal.new_period);

	/* 3rd Change the qd sampling period and verify qd_poll_in_progress */
	spdk_delay_us(20);
	poll_thread_times(0, 1);
	CU_ASSERT(bdev->internal.qd_poll_in_progress == true);
	spdk_bdev_set_qd_sampling_period(bdev, 30);
	CU_ASSERT(bdev->internal.new_period == 30);
	CU_ASSERT(bdev->internal.period == 20);
	poll_threads();
	CU_ASSERT(bdev->internal.qd_poll_in_progress == false);
	CU_ASSERT(bdev->internal.period == bdev->internal.new_period);

	/* 4th Disable the qd sampling period */
	spdk_bdev_set_qd_sampling_period(bdev, 0);
	CU_ASSERT(bdev->internal.new_period == 0);
	CU_ASSERT(bdev->internal.period == 30);
	poll_threads();
	CU_ASSERT(bdev->internal.qd_poller == NULL);
	CU_ASSERT(bdev->internal.period == bdev->internal.new_period);
	CU_ASSERT(bdev->internal.qd_desc == NULL);

	/* This is the part3.
	 * We will test the submitted IO and reset works
	 * properly with the qd sampling.
	 */
	memset(&cb_arg, 0, sizeof(cb_arg));
	spdk_bdev_set_qd_sampling_period(bdev, 1);
	poll_threads();

	CU_ASSERT(spdk_bdev_write(desc, io_ch, (void *)0x2000, 0, 4096, io_done, NULL) == 0);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch) == 1);

	/* Also include the reset IO */
	memset(&cb_arg, 0, sizeof(cb_arg));
	CU_ASSERT(spdk_bdev_reset(desc, io_ch, io_done, NULL) == 0);
	poll_threads();

	/* Close the desc */
	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);

	/* Complete the submitted IO and reset */
	stub_complete_io(2);
	poll_threads();

	free_bdev(bdev);
	ut_fini_bdev();
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

	ut_init_bdev(NULL);
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
	ut_fini_bdev();
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

	ut_init_bdev(NULL);
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
	ut_fini_bdev();
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

	ut_init_bdev(NULL);
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
	ut_fini_bdev();
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
	struct spdk_bdev_opts bdev_opts = {};
	struct iovec iov[SPDK_BDEV_IO_NUM_CHILD_IOV * 2];
	uint64_t io_ctx1 = 0, io_ctx2 = 0, i;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 7;
	bdev_opts.bdev_io_cache_size = 2;
	ut_init_bdev(&bdev_opts);

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
	for (i = 0; i < SPDK_BDEV_IO_NUM_CHILD_IOV * 2; i++) {
		iov[i].iov_base = (void *)((i + 1) * 0x10000);
		iov[i].iov_len = 512;
	}

	bdev->optimal_io_boundary = SPDK_BDEV_IO_NUM_CHILD_IOV;
	g_io_done = false;
	rc = spdk_bdev_readv_blocks(desc, io_ch, iov, SPDK_BDEV_IO_NUM_CHILD_IOV * 2, 0,
				    SPDK_BDEV_IO_NUM_CHILD_IOV * 2, io_done, &io_ctx1);
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
	ut_fini_bdev();
}

static void
bdev_unmap(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct spdk_bdev_channel *bdev_ch;
	struct ut_expected_io *expected_io;
	struct spdk_bdev_opts bdev_opts = {};
	uint32_t i, num_outstanding;
	uint64_t offset, num_blocks, max_unmap_blocks, num_children;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	ut_init_bdev(&bdev_opts);

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);
	bdev_ch = spdk_io_channel_get_ctx(ioch);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch->io_submitted));

	fn_table.submit_request = stub_submit_request;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* Case 1: First test the request won't be split */
	num_blocks = 32;

	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_UNMAP, 0, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_unmap_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 2: Test the split with 2 children requests */
	bdev->max_unmap = 8;
	bdev->max_unmap_segments = 2;
	max_unmap_blocks = bdev->max_unmap * bdev->max_unmap_segments;
	num_blocks = max_unmap_blocks * 2;
	offset = 0;

	g_io_done = false;
	for (i = 0; i < 2; i++) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_UNMAP, offset, max_unmap_blocks, 0);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += max_unmap_blocks;
	}

	rc = spdk_bdev_unmap_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 3: Test the split with 15 children requests, will finish 8 requests first */
	num_children = 15;
	num_blocks = max_unmap_blocks * num_children;
	g_io_done = false;
	offset = 0;
	for (i = 0; i < num_children; i++) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_UNMAP, offset, max_unmap_blocks, 0);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += max_unmap_blocks;
	}

	rc = spdk_bdev_unmap_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);

	while (num_children > 0) {
		num_outstanding = spdk_min(num_children, SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS);
		CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == num_outstanding);
		stub_complete_io(num_outstanding);
		num_children -= num_outstanding;
	}
	CU_ASSERT(g_io_done == true);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_write_zeroes_split_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct spdk_bdev_channel *bdev_ch;
	struct ut_expected_io *expected_io;
	struct spdk_bdev_opts bdev_opts = {};
	uint32_t i, num_outstanding;
	uint64_t offset, num_blocks, max_write_zeroes_blocks, num_children;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	ut_init_bdev(&bdev_opts);

	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);
	bdev_ch = spdk_io_channel_get_ctx(ioch);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch->io_submitted));

	fn_table.submit_request = stub_submit_request;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* Case 1: First test the request won't be split */
	num_blocks = 32;

	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, 0, num_blocks, 0);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 2: Test the split with 2 children requests */
	max_write_zeroes_blocks = 8;
	bdev->max_write_zeroes = max_write_zeroes_blocks;
	num_blocks = max_write_zeroes_blocks * 2;
	offset = 0;

	g_io_done = false;
	for (i = 0; i < 2; i++) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, offset, max_write_zeroes_blocks,
						   0);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += max_write_zeroes_blocks;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 3: Test the split with 15 children requests, will finish 8 requests first */
	num_children = 15;
	num_blocks = max_write_zeroes_blocks * num_children;
	g_io_done = false;
	offset = 0;
	for (i = 0; i < num_children; i++) {
		expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE_ZEROES, offset, max_write_zeroes_blocks,
						   0);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += max_write_zeroes_blocks;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ioch, 0, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);

	while (num_children > 0) {
		num_outstanding = spdk_min(num_children, SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS);
		CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == num_outstanding);
		stub_complete_io(num_outstanding);
		num_children -= num_outstanding;
	}
	CU_ASSERT(g_io_done == true);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_set_options_test(void)
{
	struct spdk_bdev_opts bdev_opts = {};
	int rc;

	/* Case1: Do not set opts_size */
	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == -1);

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 4;
	bdev_opts.bdev_io_cache_size = 2;
	bdev_opts.small_buf_pool_size = 4;

	/* Case 2: Do not set valid small_buf_pool_size and large_buf_pool_size */
	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == -1);

	/* Case 3: Do not set valid large_buf_pool_size */
	bdev_opts.small_buf_pool_size = BUF_SMALL_POOL_SIZE;
	bdev_opts.large_buf_pool_size = BUF_LARGE_POOL_SIZE - 1;
	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == -1);

	/* Case4: set valid large buf_pool_size */
	bdev_opts.large_buf_pool_size = BUF_LARGE_POOL_SIZE;
	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);

	/* Case5: Set different valid value for small and large buf pool */
	bdev_opts.large_buf_pool_size = BUF_SMALL_POOL_SIZE + 3;
	bdev_opts.large_buf_pool_size = BUF_LARGE_POOL_SIZE + 3;
	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
}

static uint64_t
get_ns_time(void)
{
	int rc;
	struct timespec ts;

	rc = clock_gettime(CLOCK_MONOTONIC, &ts);
	CU_ASSERT(rc == 0);
	return ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

static int
rb_tree_get_height(struct spdk_bdev_name *bdev_name)
{
	int h1, h2;

	if (bdev_name == NULL) {
		return -1;
	} else {
		h1 = rb_tree_get_height(RB_LEFT(bdev_name, node));
		h2 = rb_tree_get_height(RB_RIGHT(bdev_name, node));

		return spdk_max(h1, h2) + 1;
	}
}

static void
bdev_multi_allocation(void)
{
	const int max_bdev_num = 1024 * 16;
	char name[max_bdev_num][16];
	char noexist_name[] = "invalid_bdev";
	struct spdk_bdev *bdev[max_bdev_num];
	int i, j;
	uint64_t last_time;
	int bdev_num;
	int height;

	for (j = 0; j < max_bdev_num; j++) {
		snprintf(name[j], sizeof(name[j]), "bdev%d", j);
	}

	for (i = 0; i < 16; i++) {
		last_time = get_ns_time();
		bdev_num = 1024 * (i + 1);
		for (j = 0; j < bdev_num; j++) {
			bdev[j] = allocate_bdev(name[j]);
			height = rb_tree_get_height(&bdev[j]->internal.bdev_name);
			CU_ASSERT(height <= (int)(spdk_u32log2(2 * j + 2)));
		}
		SPDK_NOTICELOG("alloc bdev num %d takes %" PRIu64 " ms\n", bdev_num,
			       (get_ns_time() - last_time) / 1000 / 1000);
		for (j = 0; j < bdev_num; j++) {
			CU_ASSERT(spdk_bdev_get_by_name(name[j]) != NULL);
		}
		CU_ASSERT(spdk_bdev_get_by_name(noexist_name) == NULL);

		for (j = 0; j < bdev_num; j++) {
			free_bdev(bdev[j]);
		}
		for (j = 0; j < bdev_num; j++) {
			CU_ASSERT(spdk_bdev_get_by_name(name[j]) == NULL);
		}
	}
}

static struct spdk_memory_domain *g_bdev_memory_domain = (struct spdk_memory_domain *) 0xf00df00d;

static int
test_bdev_get_supported_dma_device_types_op(void *ctx, struct spdk_memory_domain **domains,
		int array_size)
{
	if (array_size > 0 && domains) {
		domains[0] = g_bdev_memory_domain;
	}

	return 1;
}

static void
bdev_get_memory_domains(void)
{
	struct spdk_bdev_fn_table fn_table = {
		.get_memory_domains = test_bdev_get_supported_dma_device_types_op
	};
	struct spdk_bdev bdev = { .fn_table = &fn_table };
	struct spdk_memory_domain *domains[2] = {};
	int rc;

	/* bdev is NULL */
	rc = spdk_bdev_get_memory_domains(NULL, domains, 2);
	CU_ASSERT(rc == -EINVAL);

	/* domains is NULL */
	rc = spdk_bdev_get_memory_domains(&bdev, NULL, 2);
	CU_ASSERT(rc == 1);

	/* array size is 0 */
	rc = spdk_bdev_get_memory_domains(&bdev, domains, 0);
	CU_ASSERT(rc == 1);

	/* get_supported_dma_device_types op is set */
	rc = spdk_bdev_get_memory_domains(&bdev, domains, 2);
	CU_ASSERT(rc == 1);
	CU_ASSERT(domains[0] == g_bdev_memory_domain);

	/* get_supported_dma_device_types op is not set */
	fn_table.get_memory_domains = NULL;
	rc = spdk_bdev_get_memory_domains(&bdev, domains, 2);
	CU_ASSERT(rc == 0);
}

static void
_bdev_io_ext(struct spdk_bdev_ext_io_opts *ext_io_opts)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	char io_buf[512];
	struct iovec iov = { .iov_base = io_buf, .iov_len = 512 };
	struct ut_expected_io *expected_io;
	int rc;

	ut_init_bdev(NULL);

	bdev = allocate_bdev("bdev0");
	bdev->md_interleave = false;
	bdev->md_len = 8;

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* read */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 32, 14, 1);
	if (ext_io_opts) {
		expected_io->md_buf = ext_io_opts->metadata;
	}
	ut_expected_io_set_iov(expected_io, 0, iov.iov_base, iov.iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, ext_io_opts);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	/* write */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 14, 1);
	if (ext_io_opts) {
		expected_io->md_buf = ext_io_opts->metadata;
	}
	ut_expected_io_set_iov(expected_io, 0, iov.iov_base, iov.iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, ext_io_opts);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();

}

static void
bdev_io_ext(void)
{
	struct spdk_bdev_ext_io_opts ext_io_opts = {
		.metadata = (void *)0xFF000000,
		.size = sizeof(ext_io_opts)
	};

	_bdev_io_ext(&ext_io_opts);
}

static void
bdev_io_ext_no_opts(void)
{
	_bdev_io_ext(NULL);
}

static void
bdev_io_ext_invalid_opts(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	char io_buf[512];
	struct iovec iov = { .iov_base = io_buf, .iov_len = 512 };
	struct spdk_bdev_ext_io_opts ext_io_opts = {
		.metadata = (void *)0xFF000000,
		.size = sizeof(ext_io_opts)
	};
	int rc;

	ut_init_bdev(NULL);

	bdev = allocate_bdev("bdev0");
	bdev->md_interleave = false;
	bdev->md_len = 8;

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Test invalid ext_opts size */
	ext_io_opts.size = 0;
	rc = spdk_bdev_readv_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == -EINVAL);
	rc = spdk_bdev_writev_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == -EINVAL);

	ext_io_opts.size = sizeof(ext_io_opts) * 2;
	rc = spdk_bdev_readv_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == -EINVAL);
	rc = spdk_bdev_writev_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == -EINVAL);

	ext_io_opts.size = offsetof(struct spdk_bdev_ext_io_opts, metadata) +
			   sizeof(ext_io_opts.metadata) - 1;
	rc = spdk_bdev_readv_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == -EINVAL);
	rc = spdk_bdev_writev_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == -EINVAL);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_io_ext_split(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	char io_buf[512];
	struct iovec iov = { .iov_base = io_buf, .iov_len = 512 };
	struct ut_expected_io *expected_io;
	struct spdk_bdev_ext_io_opts ext_io_opts = {
		.metadata = (void *)0xFF000000,
		.size = sizeof(ext_io_opts)
	};
	int rc;

	ut_init_bdev(NULL);

	bdev = allocate_bdev("bdev0");
	bdev->md_interleave = false;
	bdev->md_len = 8;

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Check that IO request with ext_opts and metadata is split correctly
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 */
	bdev->optimal_io_boundary = 16;
	bdev->split_on_optimal_io_boundary = true;
	bdev->md_interleave = false;
	bdev->md_len = 8;

	iov.iov_base = (void *)0xF000;
	iov.iov_len = 4096;
	memset(&ext_io_opts, 0, sizeof(ext_io_opts));
	ext_io_opts.metadata = (void *)0xFF000000;
	ext_io_opts.size = sizeof(ext_io_opts);
	g_io_done = false;

	/* read */
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 14, 2, 1);
	expected_io->md_buf = ext_io_opts.metadata;
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 2 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 16, 6, 1);
	expected_io->md_buf = ext_io_opts.metadata + 2 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 2 * 512), 6 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_ext(desc, io_ch, &iov, 1, 14, 8, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* write */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 14, 2, 1);
	expected_io->md_buf = ext_io_opts.metadata;
	ut_expected_io_set_iov(expected_io, 0, (void *)0xF000, 2 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 16, 6, 1);
	expected_io->md_buf = ext_io_opts.metadata + 2 * 8;
	ut_expected_io_set_iov(expected_io, 0, (void *)(0xF000 + 2 * 512), 6 * 512);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks_ext(desc, io_ch, &iov, 1, 14, 8, io_done, NULL, &ext_io_opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);

	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 2);
	stub_complete_io(2);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_io_ext_bounce_buffer(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	char io_buf[512];
	struct iovec iov = { .iov_base = io_buf, .iov_len = 512 };
	struct ut_expected_io *expected_io;
	struct spdk_bdev_ext_io_opts ext_io_opts = {
		.metadata = (void *)0xFF000000,
		.size = sizeof(ext_io_opts)
	};
	int rc;

	ut_init_bdev(NULL);

	bdev = allocate_bdev("bdev0");
	bdev->md_interleave = false;
	bdev->md_len = 8;

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Verify data pull/push
	 * bdev doesn't support memory domains, so buffers from bdev memory pool will be used */
	ext_io_opts.memory_domain = (struct spdk_memory_domain *)0xdeadbeef;

	/* read */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 32, 14, 1);
	ut_expected_io_set_iov(expected_io, 0, iov.iov_base, iov.iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_readv_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_memory_domain_push_data_called == true);
	CU_ASSERT(g_io_done == true);

	/* write */
	g_io_done = false;
	expected_io = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_WRITE, 32, 14, 1);
	ut_expected_io_set_iov(expected_io, 0, iov.iov_base, iov.iov_len);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);

	rc = spdk_bdev_writev_blocks_ext(desc, io_ch, &iov, 1, 32, 14, io_done, NULL, &ext_io_opts);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_memory_domain_pull_data_called == true);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_register_uuid_alias(void)
{
	struct spdk_bdev *bdev, *second;
	char uuid[SPDK_UUID_STRING_LEN];
	int rc;

	ut_init_bdev(NULL);
	bdev = allocate_bdev("bdev0");

	/* Make sure an UUID was generated  */
	CU_ASSERT_FALSE(spdk_mem_all_zero(&bdev->uuid, sizeof(bdev->uuid)));

	/* Check that an UUID alias was registered */
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &bdev->uuid);
	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(uuid), bdev);

	/* Unregister the bdev */
	spdk_bdev_unregister(bdev, NULL, NULL);
	poll_threads();
	CU_ASSERT_PTR_NULL(spdk_bdev_get_by_name(uuid));

	/* Check the same, but this time register the bdev with non-zero UUID */
	rc = spdk_bdev_register(bdev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(uuid), bdev);

	/* Unregister the bdev */
	spdk_bdev_unregister(bdev, NULL, NULL);
	poll_threads();
	CU_ASSERT_PTR_NULL(spdk_bdev_get_by_name(uuid));

	/* Regiser the bdev using UUID as the name */
	bdev->name = uuid;
	rc = spdk_bdev_register(bdev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(uuid), bdev);

	/* Unregister the bdev */
	spdk_bdev_unregister(bdev, NULL, NULL);
	poll_threads();
	CU_ASSERT_PTR_NULL(spdk_bdev_get_by_name(uuid));

	/* Check that it's not possible to register two bdevs with the same UUIDs */
	bdev->name = "bdev0";
	second = allocate_bdev("bdev1");
	spdk_uuid_copy(&bdev->uuid, &second->uuid);
	rc = spdk_bdev_register(bdev);
	CU_ASSERT_EQUAL(rc, -EEXIST);

	/* Regenerate the UUID and re-check */
	spdk_uuid_generate(&bdev->uuid);
	rc = spdk_bdev_register(bdev);
	CU_ASSERT_EQUAL(rc, 0);

	/* And check that both bdevs can be retrieved through their UUIDs */
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &bdev->uuid);
	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(uuid), bdev);
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &second->uuid);
	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(uuid), second);

	free_bdev(second);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_unregister_by_name(void)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = allocate_bdev("bdev");

	g_event_type1 = 0xFF;
	g_unregister_arg = NULL;
	g_unregister_rc = -1;

	rc = spdk_bdev_unregister_by_name("bdev1", &bdev_ut_if, bdev_unregister_cb, (void *)0x12345678);
	CU_ASSERT(rc == -ENODEV);

	rc = spdk_bdev_unregister_by_name("bdev", &vbdev_ut_if, bdev_unregister_cb, (void *)0x12345678);
	CU_ASSERT(rc == -ENODEV);

	rc = spdk_bdev_unregister_by_name("bdev", &bdev_ut_if, bdev_unregister_cb, (void *)0x12345678);
	CU_ASSERT(rc == 0);

	/* Check that unregister callback is delayed */
	CU_ASSERT(g_unregister_arg == NULL);
	CU_ASSERT(g_unregister_rc == -1);

	poll_threads();

	/* Event callback shall not be issued because device was closed */
	CU_ASSERT(g_event_type1 == 0xFF);
	/* Unregister callback is issued */
	CU_ASSERT(g_unregister_arg == (void *)0x12345678);
	CU_ASSERT(g_unregister_rc == 0);

	free_bdev(bdev);
}

static int
count_bdevs(void *ctx, struct spdk_bdev *bdev)
{
	int *count = ctx;

	(*count)++;

	return 0;
}

static void
for_each_bdev_test(void)
{
	struct spdk_bdev *bdev[8];
	int rc, count;

	bdev[0] = allocate_bdev("bdev0");
	bdev[0]->internal.status = SPDK_BDEV_STATUS_REMOVING;

	bdev[1] = allocate_bdev("bdev1");
	rc = spdk_bdev_module_claim_bdev(bdev[1], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[2] = allocate_bdev("bdev2");

	bdev[3] = allocate_bdev("bdev3");
	rc = spdk_bdev_module_claim_bdev(bdev[3], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[4] = allocate_bdev("bdev4");

	bdev[5] = allocate_bdev("bdev5");
	rc = spdk_bdev_module_claim_bdev(bdev[5], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[6] = allocate_bdev("bdev6");

	bdev[7] = allocate_bdev("bdev7");

	count = 0;
	rc = spdk_for_each_bdev(&count, count_bdevs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 7);

	count = 0;
	rc = spdk_for_each_bdev_leaf(&count, count_bdevs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 4);

	bdev[0]->internal.status = SPDK_BDEV_STATUS_READY;
	free_bdev(bdev[0]);
	free_bdev(bdev[1]);
	free_bdev(bdev[2]);
	free_bdev(bdev[3]);
	free_bdev(bdev[4]);
	free_bdev(bdev[5]);
	free_bdev(bdev[6]);
	free_bdev(bdev[7]);
}

static void
bdev_seek_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *io_ch;
	int rc;

	ut_init_bdev(NULL);
	poll_threads();

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	/* Seek data not supported */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_SEEK_DATA, false);
	rc = spdk_bdev_seek_data(desc, io_ch, 0, bdev_seek_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	poll_threads();
	CU_ASSERT(g_seek_offset == 0);

	/* Seek hole not supported */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_SEEK_HOLE, false);
	rc = spdk_bdev_seek_hole(desc, io_ch, 0, bdev_seek_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	poll_threads();
	CU_ASSERT(g_seek_offset == UINT64_MAX);

	/* Seek data supported */
	g_seek_data_offset = 12345;
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_SEEK_DATA, true);
	rc = spdk_bdev_seek_data(desc, io_ch, 0, bdev_seek_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_seek_offset == 12345);

	/* Seek hole supported */
	g_seek_hole_offset = 67890;
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_SEEK_HOLE, true);
	rc = spdk_bdev_seek_hole(desc, io_ch, 0, bdev_seek_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);
	CU_ASSERT(g_seek_offset == 67890);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_copy(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct ut_expected_io *expected_io;
	uint64_t src_offset, num_blocks;
	uint32_t num_completed;
	int rc;

	ut_init_bdev(NULL);
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	fn_table.submit_request = stub_submit_request;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* First test that if the bdev supports copy, the request won't be split */
	bdev->md_len = 0;
	bdev->blocklen = 4096;
	num_blocks = 512;
	src_offset = bdev->blockcnt - num_blocks;

	expected_io = ut_alloc_expected_copy_io(SPDK_BDEV_IO_TYPE_COPY, 0, src_offset, num_blocks);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_copy_blocks(desc, ioch, 0, src_offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	num_completed = stub_complete_io(1);
	CU_ASSERT_EQUAL(num_completed, 1);

	/* Check that if copy is not supported it'll fail */
	ut_enable_io_type(SPDK_BDEV_IO_TYPE_COPY, false);

	rc = spdk_bdev_copy_blocks(desc, ioch, 0, src_offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, -ENOTSUP);

	ut_enable_io_type(SPDK_BDEV_IO_TYPE_COPY, true);
	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
bdev_copy_split_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ioch;
	struct spdk_bdev_channel *bdev_ch;
	struct ut_expected_io *expected_io;
	struct spdk_bdev_opts bdev_opts = {};
	uint32_t i, num_outstanding;
	uint64_t offset, src_offset, num_blocks, max_copy_blocks, num_children;
	int rc;

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	bdev_opts.bdev_io_pool_size = 512;
	bdev_opts.bdev_io_cache_size = 64;
	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);

	ut_init_bdev(NULL);
	bdev = allocate_bdev("bdev");

	rc = spdk_bdev_open_ext("bdev", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(bdev == spdk_bdev_desc_get_bdev(desc));
	ioch = spdk_bdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);
	bdev_ch = spdk_io_channel_get_ctx(ioch);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch->io_submitted));

	fn_table.submit_request = stub_submit_request;
	g_io_exp_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* Case 1: First test the request won't be split */
	num_blocks = 32;
	src_offset = bdev->blockcnt - num_blocks;

	g_io_done = false;
	expected_io = ut_alloc_expected_copy_io(SPDK_BDEV_IO_TYPE_COPY, 0, src_offset, num_blocks);
	TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
	rc = spdk_bdev_copy_blocks(desc, ioch, 0, src_offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 1);
	stub_complete_io(1);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 2: Test the split with 2 children requests */
	max_copy_blocks = 8;
	bdev->max_copy = max_copy_blocks;
	num_children = 2;
	num_blocks = max_copy_blocks * num_children;
	offset = 0;
	src_offset = bdev->blockcnt - num_blocks;

	g_io_done = false;
	for (i = 0; i < num_children; i++) {
		expected_io = ut_alloc_expected_copy_io(SPDK_BDEV_IO_TYPE_COPY, offset,
							src_offset + offset, max_copy_blocks);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += max_copy_blocks;
	}

	rc = spdk_bdev_copy_blocks(desc, ioch, 0, src_offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == num_children);
	stub_complete_io(num_children);
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	/* Case 3: Test the split with 15 children requests, will finish 8 requests first */
	num_children = 15;
	num_blocks = max_copy_blocks * num_children;
	offset = 0;
	src_offset = bdev->blockcnt - num_blocks;

	g_io_done = false;
	for (i = 0; i < num_children; i++) {
		expected_io = ut_alloc_expected_copy_io(SPDK_BDEV_IO_TYPE_COPY, offset,
							src_offset + offset, max_copy_blocks);
		TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, expected_io, link);
		offset += max_copy_blocks;
	}

	rc = spdk_bdev_copy_blocks(desc, ioch, 0, src_offset, num_blocks, io_done, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_io_done == false);

	while (num_children > 0) {
		num_outstanding = spdk_min(num_children, SPDK_BDEV_MAX_CHILDREN_COPY_REQS);
		CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == num_outstanding);
		stub_complete_io(num_outstanding);
		num_children -= num_outstanding;
	}
	CU_ASSERT(g_io_done == true);

	spdk_put_io_channel(ioch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
	ut_fini_bdev();
}

static void
examine_claim_v1(struct spdk_bdev *bdev)
{
	int rc;

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &vbdev_ut_if);
	CU_ASSERT(rc == 0);
}

static void
examine_no_lock_held(struct spdk_bdev *bdev)
{
	CU_ASSERT(!spdk_spin_held(&g_bdev_mgr.spinlock));
	CU_ASSERT(!spdk_spin_held(&bdev->internal.spinlock));
}

struct examine_claim_v2_ctx {
	struct ut_examine_ctx examine_ctx;
	enum spdk_bdev_claim_type claim_type;
	struct spdk_bdev_desc *desc;
};

static void
examine_claim_v2(struct spdk_bdev *bdev)
{
	struct examine_claim_v2_ctx *ctx = bdev->ctxt;
	int rc;

	rc = spdk_bdev_open_ext(bdev->name, false, bdev_ut_event_cb, NULL, &ctx->desc);
	CU_ASSERT(rc == 0);

	rc = spdk_bdev_module_claim_bdev_desc(ctx->desc, ctx->claim_type, NULL, &vbdev_ut_if);
	CU_ASSERT(rc == 0);
}

static void
examine_locks(void)
{
	struct spdk_bdev *bdev;
	struct ut_examine_ctx ctx = { 0 };
	struct examine_claim_v2_ctx v2_ctx;

	/* Without any claims, one code path is taken */
	ctx.examine_config = examine_no_lock_held;
	ctx.examine_disk = examine_no_lock_held;
	bdev = allocate_bdev_ctx("bdev0", &ctx);
	CU_ASSERT(ctx.examine_config_count == 1);
	CU_ASSERT(ctx.examine_disk_count == 1);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	CU_ASSERT(bdev->internal.claim.v1.module == NULL);
	free_bdev(bdev);

	/* Exercise another path that is taken when examine_config() takes a v1 claim. */
	memset(&ctx, 0, sizeof(ctx));
	ctx.examine_config = examine_claim_v1;
	ctx.examine_disk = examine_no_lock_held;
	bdev = allocate_bdev_ctx("bdev0", &ctx);
	CU_ASSERT(ctx.examine_config_count == 1);
	CU_ASSERT(ctx.examine_disk_count == 1);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);
	CU_ASSERT(bdev->internal.claim.v1.module == &vbdev_ut_if);
	spdk_bdev_module_release_bdev(bdev);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	CU_ASSERT(bdev->internal.claim.v1.module == NULL);
	free_bdev(bdev);

	/* Exercise the final path that comes with v2 claims. */
	memset(&v2_ctx, 0, sizeof(v2_ctx));
	v2_ctx.examine_ctx.examine_config = examine_claim_v2;
	v2_ctx.examine_ctx.examine_disk = examine_no_lock_held;
	v2_ctx.claim_type = SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE;
	bdev = allocate_bdev_ctx("bdev0", &v2_ctx);
	CU_ASSERT(v2_ctx.examine_ctx.examine_config_count == 1);
	CU_ASSERT(v2_ctx.examine_ctx.examine_disk_count == 1);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE);
	spdk_bdev_close(v2_ctx.desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	free_bdev(bdev);
}

#define UT_ASSERT_CLAIM_V2_COUNT(bdev, expect) \
	do { \
		uint32_t len = 0; \
		struct spdk_bdev_module_claim *claim; \
		TAILQ_FOREACH(claim, &bdev->internal.claim.v2.claims, link) { \
			len++; \
		} \
		CU_ASSERT(len == expect); \
	} while (0)

static void
claim_v2_rwo(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_desc *desc2;
	struct spdk_bdev_claim_opts opts;
	int rc;

	bdev = allocate_bdev("bdev0");

	/* Claim without options */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE);
	CU_ASSERT(desc->claim != NULL);
	CU_ASSERT(desc->claim->module == &bdev_ut_if);
	CU_ASSERT(strcmp(desc->claim->name, "") == 0);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* Release the claim by closing the descriptor */
	spdk_bdev_close(desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.open_descs));
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);

	/* Claim with options */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	snprintf(opts.name, sizeof(opts.name), "%s", "claim with options");
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE);
	CU_ASSERT(desc->claim != NULL);
	CU_ASSERT(desc->claim->module == &bdev_ut_if);
	CU_ASSERT(strcmp(desc->claim->name, "claim with options") == 0);
	memset(&opts, 0, sizeof(opts));
	CU_ASSERT(strcmp(desc->claim->name, "claim with options") == 0);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* The claim blocks new writers. */
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(desc2 == NULL);

	/* New readers are allowed */
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc2 != NULL);
	CU_ASSERT(!desc2->write);

	/* No new v2 RWO claims are allowed */
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);

	/* No new v2 ROM claims are allowed */
	CU_ASSERT(!desc2->write);
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(!desc2->write);

	/* No new v2 RWM claims are allowed */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	opts.shared_claim_key = (uint64_t)&opts;
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(!desc2->write);

	/* No new v1 claims are allowed */
	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);

	/* None of the above changed the existing claim */
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* Closing the first descriptor now allows a new claim and it is promoted to rw. */
	spdk_bdev_close(desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);
	CU_ASSERT(!desc2->write);
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc2->claim != NULL);
	CU_ASSERT(desc2->write);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc2->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);
	spdk_bdev_close(desc2);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);

	/* Cannot claim with a key */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	opts.shared_claim_key = (uint64_t)&opts;
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);
	spdk_bdev_close(desc);

	/* Clean up */
	free_bdev(bdev);
}

static void
claim_v2_rom(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_desc *desc2;
	struct spdk_bdev_claim_opts opts;
	int rc;

	bdev = allocate_bdev("bdev0");

	/* Claim without options */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE);
	CU_ASSERT(desc->claim != NULL);
	CU_ASSERT(desc->claim->module == &bdev_ut_if);
	CU_ASSERT(strcmp(desc->claim->name, "") == 0);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* Release the claim by closing the descriptor */
	spdk_bdev_close(desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.open_descs));
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);

	/* Claim with options */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	snprintf(opts.name, sizeof(opts.name), "%s", "claim with options");
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE);
	SPDK_CU_ASSERT_FATAL(desc->claim != NULL);
	CU_ASSERT(desc->claim->module == &bdev_ut_if);
	CU_ASSERT(strcmp(desc->claim->name, "claim with options") == 0);
	memset(&opts, 0, sizeof(opts));
	CU_ASSERT(strcmp(desc->claim->name, "claim with options") == 0);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* The claim blocks new writers. */
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(desc2 == NULL);

	/* New readers are allowed */
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc2 != NULL);
	CU_ASSERT(!desc2->write);

	/* No new v2 RWO claims are allowed */
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);

	/* No new v2 RWM claims are allowed */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	opts.shared_claim_key = (uint64_t)&opts;
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(!desc2->write);

	/* No new v1 claims are allowed */
	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);

	/* None of the above messed up the existing claim */
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* New v2 ROM claims are allowed and the descriptor stays read-only. */
	CU_ASSERT(!desc2->write);
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!desc2->write);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	CU_ASSERT(TAILQ_NEXT(desc->claim, link) == desc2->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 2);

	/* Claim remains when closing the first descriptor */
	spdk_bdev_close(desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE);
	CU_ASSERT(!TAILQ_EMPTY(&bdev->internal.open_descs));
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc2->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* Claim removed when closing the other descriptor */
	spdk_bdev_close(desc2);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.open_descs));

	/* Cannot claim with a key */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	opts.shared_claim_key = (uint64_t)&opts;
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);
	spdk_bdev_close(desc);

	/* Cannot claim with a read-write descriptor */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);
	spdk_bdev_close(desc);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.open_descs));

	/* Clean up */
	free_bdev(bdev);
}

static void
claim_v2_rwm(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_desc *desc2;
	struct spdk_bdev_claim_opts opts;
	char good_key, bad_key;
	int rc;

	bdev = allocate_bdev("bdev0");

	/* Claim without options should fail */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 0);
	CU_ASSERT(desc->claim == NULL);

	/* Claim with options */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	snprintf(opts.name, sizeof(opts.name), "%s", "claim with options");
	opts.shared_claim_key = (uint64_t)&good_key;
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED);
	SPDK_CU_ASSERT_FATAL(desc->claim != NULL);
	CU_ASSERT(desc->claim->module == &bdev_ut_if);
	CU_ASSERT(strcmp(desc->claim->name, "claim with options") == 0);
	memset(&opts, 0, sizeof(opts));
	CU_ASSERT(strcmp(desc->claim->name, "claim with options") == 0);
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* The claim blocks new writers. */
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(desc2 == NULL);

	/* New readers are allowed */
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc2 != NULL);
	CU_ASSERT(!desc2->write);

	/* No new v2 RWO claims are allowed */
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);

	/* No new v2 ROM claims are allowed and the descriptor stays read-only. */
	CU_ASSERT(!desc2->write);
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE, NULL,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(!desc2->write);

	/* No new v1 claims are allowed */
	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);

	/* No new v2 RWM claims are allowed if the key does not match */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	opts.shared_claim_key = (uint64_t)&bad_key;
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);
	CU_ASSERT(!desc2->write);

	/* None of the above messed up the existing claim */
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* New v2 RWM claims are allowed and the descriptor is promoted if the key matches. */
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	opts.shared_claim_key = (uint64_t)&good_key;
	CU_ASSERT(!desc2->write);
	rc = spdk_bdev_module_claim_bdev_desc(desc2, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc2->write);
	CU_ASSERT(TAILQ_NEXT(desc->claim, link) == desc2->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 2);

	/* Claim remains when closing the first descriptor */
	spdk_bdev_close(desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED);
	CU_ASSERT(!TAILQ_EMPTY(&bdev->internal.open_descs));
	CU_ASSERT(TAILQ_FIRST(&bdev->internal.claim.v2.claims) == desc2->claim);
	UT_ASSERT_CLAIM_V2_COUNT(bdev, 1);

	/* Claim removed when closing the other descriptor */
	spdk_bdev_close(desc2);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.open_descs));

	/* Cannot claim without a key */
	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	spdk_bdev_claim_opts_init(&opts, sizeof(opts));
	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED, &opts,
					      &bdev_ut_if);
	CU_ASSERT(rc == -EINVAL);
	spdk_bdev_close(desc);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	CU_ASSERT(TAILQ_EMPTY(&bdev->internal.open_descs));

	/* Clean up */
	free_bdev(bdev);
}

static void
claim_v2_existing_writer(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_desc *desc2;
	struct spdk_bdev_claim_opts opts;
	enum spdk_bdev_claim_type type;
	enum spdk_bdev_claim_type types[] = {
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED,
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE
	};
	size_t i;
	int rc;

	bdev = allocate_bdev("bdev0");

	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	desc2 = NULL;
	rc = spdk_bdev_open_ext("bdev0", true, bdev_ut_event_cb, NULL, &desc2);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc2 != NULL);

	for (i = 0; i < SPDK_COUNTOF(types); i++) {
		type = types[i];
		spdk_bdev_claim_opts_init(&opts, sizeof(opts));
		if (type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED) {
			opts.shared_claim_key = (uint64_t)&opts;
		}
		rc = spdk_bdev_module_claim_bdev_desc(desc, type, &opts, &bdev_ut_if);
		if (type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE) {
			CU_ASSERT(rc == -EINVAL);
		} else {
			CU_ASSERT(rc == -EPERM);
		}
		CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
		rc = spdk_bdev_module_claim_bdev_desc(desc2, type, &opts, &bdev_ut_if);
		if (type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE) {
			CU_ASSERT(rc == -EINVAL);
		} else {
			CU_ASSERT(rc == -EPERM);
		}
		CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE);
	}

	spdk_bdev_close(desc);
	spdk_bdev_close(desc2);

	/* Clean up */
	free_bdev(bdev);
}

static void
claim_v2_existing_v1(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_claim_opts opts;
	enum spdk_bdev_claim_type type;
	enum spdk_bdev_claim_type types[] = {
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED,
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE
	};
	size_t i;
	int rc;

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);

	desc = NULL;
	rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	for (i = 0; i < SPDK_COUNTOF(types); i++) {
		type = types[i];
		spdk_bdev_claim_opts_init(&opts, sizeof(opts));
		if (type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED) {
			opts.shared_claim_key = (uint64_t)&opts;
		}
		rc = spdk_bdev_module_claim_bdev_desc(desc, type, &opts, &bdev_ut_if);
		CU_ASSERT(rc == -EPERM);
		CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);
	}

	spdk_bdev_module_release_bdev(bdev);
	spdk_bdev_close(desc);

	/* Clean up */
	free_bdev(bdev);
}

static void
claim_v1_existing_v2(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_claim_opts opts;
	enum spdk_bdev_claim_type type;
	enum spdk_bdev_claim_type types[] = {
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED,
		SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE
	};
	size_t i;
	int rc;

	bdev = allocate_bdev("bdev0");

	for (i = 0; i < SPDK_COUNTOF(types); i++) {
		type = types[i];

		desc = NULL;
		rc = spdk_bdev_open_ext("bdev0", false, bdev_ut_event_cb, NULL, &desc);
		CU_ASSERT(rc == 0);
		SPDK_CU_ASSERT_FATAL(desc != NULL);

		/* Get a v2 claim */
		spdk_bdev_claim_opts_init(&opts, sizeof(opts));
		if (type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED) {
			opts.shared_claim_key = (uint64_t)&opts;
		}
		rc = spdk_bdev_module_claim_bdev_desc(desc, type, &opts, &bdev_ut_if);
		CU_ASSERT(rc == 0);

		/* Fail to get a v1 claim */
		rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
		CU_ASSERT(rc == -EPERM);

		spdk_bdev_close(desc);

		/* Now v1 succeeds */
		rc = spdk_bdev_module_claim_bdev(bdev, NULL, &bdev_ut_if);
		CU_ASSERT(rc == 0)
		spdk_bdev_module_release_bdev(bdev);
	}

	/* Clean up */
	free_bdev(bdev);
}

static void ut_examine_claimed_config0(struct spdk_bdev *bdev);
static void ut_examine_claimed_disk0(struct spdk_bdev *bdev);
static void ut_examine_claimed_config1(struct spdk_bdev *bdev);
static void ut_examine_claimed_disk1(struct spdk_bdev *bdev);

#define UT_MAX_EXAMINE_MODS 2
struct spdk_bdev_module examine_claimed_mods[UT_MAX_EXAMINE_MODS] = {
	{
		.name = "vbdev_ut_examine0",
		.module_init = vbdev_ut_module_init,
		.module_fini = vbdev_ut_module_fini,
		.examine_config = ut_examine_claimed_config0,
		.examine_disk = ut_examine_claimed_disk0,
	},
	{
		.name = "vbdev_ut_examine1",
		.module_init = vbdev_ut_module_init,
		.module_fini = vbdev_ut_module_fini,
		.examine_config = ut_examine_claimed_config1,
		.examine_disk = ut_examine_claimed_disk1,
	}
};

SPDK_BDEV_MODULE_REGISTER(bdev_ut_claimed0, &examine_claimed_mods[0])
SPDK_BDEV_MODULE_REGISTER(bdev_ut_claimed1, &examine_claimed_mods[1])

struct ut_examine_claimed_ctx {
	uint32_t examine_config_count;
	uint32_t examine_disk_count;

	/* Claim type to take, with these options */
	enum spdk_bdev_claim_type claim_type;
	struct spdk_bdev_claim_opts claim_opts;

	/* Expected return value from spdk_bdev_module_claim_bdev_desc() */
	int expect_claim_err;

	/* Descriptor used for a claim */
	struct spdk_bdev_desc *desc;
} examine_claimed_ctx[UT_MAX_EXAMINE_MODS];

bool ut_testing_examine_claimed;

static void
reset_examine_claimed_ctx(void)
{
	struct ut_examine_claimed_ctx *ctx;
	uint32_t i;

	for (i = 0; i < SPDK_COUNTOF(examine_claimed_ctx); i++) {
		ctx = &examine_claimed_ctx[i];
		if (ctx->desc != NULL) {
			spdk_bdev_close(ctx->desc);
		}
		memset(ctx, 0, sizeof(*ctx));
		spdk_bdev_claim_opts_init(&ctx->claim_opts, sizeof(ctx->claim_opts));
	}
}

static void
examine_claimed_config(struct spdk_bdev *bdev, uint32_t modnum)
{
	SPDK_CU_ASSERT_FATAL(modnum < UT_MAX_EXAMINE_MODS);
	struct spdk_bdev_module *module = &examine_claimed_mods[modnum];
	struct ut_examine_claimed_ctx *ctx = &examine_claimed_ctx[modnum];
	int rc;

	if (!ut_testing_examine_claimed) {
		spdk_bdev_module_examine_done(module);
		return;
	}

	ctx->examine_config_count++;

	if (ctx->claim_type != SPDK_BDEV_CLAIM_NONE) {
		rc = spdk_bdev_open_ext(bdev->name, false, bdev_ut_event_cb, &ctx->claim_opts,
					&ctx->desc);
		CU_ASSERT(rc == 0);

		rc = spdk_bdev_module_claim_bdev_desc(ctx->desc, ctx->claim_type, NULL, module);
		CU_ASSERT(rc == ctx->expect_claim_err);
	}
	spdk_bdev_module_examine_done(module);
}

static void
ut_examine_claimed_config0(struct spdk_bdev *bdev)
{
	examine_claimed_config(bdev, 0);
}

static void
ut_examine_claimed_config1(struct spdk_bdev *bdev)
{
	examine_claimed_config(bdev, 1);
}

static void
examine_claimed_disk(struct spdk_bdev *bdev, uint32_t modnum)
{
	SPDK_CU_ASSERT_FATAL(modnum < UT_MAX_EXAMINE_MODS);
	struct spdk_bdev_module *module = &examine_claimed_mods[modnum];
	struct ut_examine_claimed_ctx *ctx = &examine_claimed_ctx[modnum];

	if (!ut_testing_examine_claimed) {
		spdk_bdev_module_examine_done(module);
		return;
	}

	ctx->examine_disk_count++;

	spdk_bdev_module_examine_done(module);
}

static void
ut_examine_claimed_disk0(struct spdk_bdev *bdev)
{
	examine_claimed_disk(bdev, 0);
}

static void
ut_examine_claimed_disk1(struct spdk_bdev *bdev)
{
	examine_claimed_disk(bdev, 1);
}

static void
examine_claimed(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_module *mod = examine_claimed_mods;
	struct ut_examine_claimed_ctx *ctx = examine_claimed_ctx;

	ut_testing_examine_claimed = true;
	reset_examine_claimed_ctx();

	/*
	 * With one module claiming, both modules' examine_config should be called, but only the
	 * claiming module's examine_disk should be called.
	 */
	ctx[0].claim_type = SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE;
	bdev = allocate_bdev("bdev0");
	CU_ASSERT(ctx[0].examine_config_count == 1);
	CU_ASSERT(ctx[0].examine_disk_count == 1);
	SPDK_CU_ASSERT_FATAL(ctx[0].desc != NULL);
	CU_ASSERT(ctx[0].desc->claim->module == &mod[0]);
	CU_ASSERT(ctx[1].examine_config_count == 1);
	CU_ASSERT(ctx[1].examine_disk_count == 0);
	CU_ASSERT(ctx[1].desc == NULL);
	reset_examine_claimed_ctx();
	free_bdev(bdev);

	/*
	 * With two modules claiming, both modules' examine_config and examine_disk should be
	 * called.
	 */
	ctx[0].claim_type = SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE;
	ctx[1].claim_type = SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE;
	bdev = allocate_bdev("bdev0");
	CU_ASSERT(ctx[0].examine_config_count == 1);
	CU_ASSERT(ctx[0].examine_disk_count == 1);
	SPDK_CU_ASSERT_FATAL(ctx[0].desc != NULL);
	CU_ASSERT(ctx[0].desc->claim->module == &mod[0]);
	CU_ASSERT(ctx[1].examine_config_count == 1);
	CU_ASSERT(ctx[1].examine_disk_count == 1);
	SPDK_CU_ASSERT_FATAL(ctx[1].desc != NULL);
	CU_ASSERT(ctx[1].desc->claim->module == &mod[1]);
	reset_examine_claimed_ctx();
	free_bdev(bdev);

	/*
	 * If two vbdev modules try to claim with conflicting claim types, the module that was added
	 * last wins. The winner gets the claim and is the only one that has its examine_disk
	 * callback invoked.
	 */
	ctx[0].claim_type = SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE;
	ctx[0].expect_claim_err = -EPERM;
	ctx[1].claim_type = SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE;
	bdev = allocate_bdev("bdev0");
	CU_ASSERT(ctx[0].examine_config_count == 1);
	CU_ASSERT(ctx[0].examine_disk_count == 0);
	CU_ASSERT(ctx[1].examine_config_count == 1);
	CU_ASSERT(ctx[1].examine_disk_count == 1);
	SPDK_CU_ASSERT_FATAL(ctx[1].desc != NULL);
	CU_ASSERT(ctx[1].desc->claim->module == &mod[1]);
	CU_ASSERT(bdev->internal.claim_type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE);
	reset_examine_claimed_ctx();
	free_bdev(bdev);

	ut_testing_examine_claimed = false;
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
	CU_ADD_TEST(suite, claim_test);
	CU_ADD_TEST(suite, alias_add_del_test);
	CU_ADD_TEST(suite, get_device_stat_test);
	CU_ADD_TEST(suite, bdev_io_types_test);
	CU_ADD_TEST(suite, bdev_io_wait_test);
	CU_ADD_TEST(suite, bdev_io_spans_split_test);
	CU_ADD_TEST(suite, bdev_io_boundary_split_test);
	CU_ADD_TEST(suite, bdev_io_max_size_and_segment_split_test);
	CU_ADD_TEST(suite, bdev_io_mix_split_test);
	CU_ADD_TEST(suite, bdev_io_split_with_io_wait);
	CU_ADD_TEST(suite, bdev_io_write_unit_split_test);
	CU_ADD_TEST(suite, bdev_io_alignment_with_boundary);
	CU_ADD_TEST(suite, bdev_io_alignment);
	CU_ADD_TEST(suite, bdev_histograms);
	CU_ADD_TEST(suite, bdev_write_zeroes);
	CU_ADD_TEST(suite, bdev_compare_and_write);
	CU_ADD_TEST(suite, bdev_compare);
	CU_ADD_TEST(suite, bdev_compare_emulated);
	CU_ADD_TEST(suite, bdev_zcopy_write);
	CU_ADD_TEST(suite, bdev_zcopy_read);
	CU_ADD_TEST(suite, bdev_open_while_hotremove);
	CU_ADD_TEST(suite, bdev_close_while_hotremove);
	CU_ADD_TEST(suite, bdev_open_ext);
	CU_ADD_TEST(suite, bdev_open_ext_unregister);
	CU_ADD_TEST(suite, bdev_set_io_timeout);
	CU_ADD_TEST(suite, bdev_set_qd_sampling);
	CU_ADD_TEST(suite, lba_range_overlap);
	CU_ADD_TEST(suite, lock_lba_range_check_ranges);
	CU_ADD_TEST(suite, lock_lba_range_with_io_outstanding);
	CU_ADD_TEST(suite, lock_lba_range_overlapped);
	CU_ADD_TEST(suite, bdev_io_abort);
	CU_ADD_TEST(suite, bdev_unmap);
	CU_ADD_TEST(suite, bdev_write_zeroes_split_test);
	CU_ADD_TEST(suite, bdev_set_options_test);
	CU_ADD_TEST(suite, bdev_multi_allocation);
	CU_ADD_TEST(suite, bdev_get_memory_domains);
	CU_ADD_TEST(suite, bdev_io_ext);
	CU_ADD_TEST(suite, bdev_io_ext_no_opts);
	CU_ADD_TEST(suite, bdev_io_ext_invalid_opts);
	CU_ADD_TEST(suite, bdev_io_ext_split);
	CU_ADD_TEST(suite, bdev_io_ext_bounce_buffer);
	CU_ADD_TEST(suite, bdev_register_uuid_alias);
	CU_ADD_TEST(suite, bdev_unregister_by_name);
	CU_ADD_TEST(suite, for_each_bdev_test);
	CU_ADD_TEST(suite, bdev_seek_test);
	CU_ADD_TEST(suite, bdev_copy);
	CU_ADD_TEST(suite, bdev_copy_split_test);
	CU_ADD_TEST(suite, examine_locks);
	CU_ADD_TEST(suite, claim_v2_rwo);
	CU_ADD_TEST(suite, claim_v2_rom);
	CU_ADD_TEST(suite, claim_v2_rwm);
	CU_ADD_TEST(suite, claim_v2_existing_writer);
	CU_ADD_TEST(suite, claim_v2_existing_v1);
	CU_ADD_TEST(suite, claim_v1_existing_v2);
	CU_ADD_TEST(suite, examine_claimed);

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
