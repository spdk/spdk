/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright Hewlett Packard Enterprise Development LP
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "common/lib/ut_multithread.c"

#include "bdev/aio/bdev_aio.c"

/* UNMAP is serviced by either fallocate() or the BLKDISCARD ioctl, so both
 * syscalls are wrapped to observe which one a given configuration uses.
 */
static int g_ioctl_calls;
static int g_ioctl_fd;
static unsigned long g_ioctl_request;
static uint64_t g_ioctl_range[2];
static int g_ioctl_result;
static int g_ioctl_errno;

static int g_fallocate_calls;
static int g_fallocate_fd;
static int g_fallocate_mode;
static off_t g_fallocate_offset;
static off_t g_fallocate_len;
static int g_fallocate_result;
static int g_fallocate_errno;

int __wrap_ioctl(int fd, unsigned long request, ...);
int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_fallocate(int fd, int mode, off_t offset, off_t len);

int
__wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);

	/* Only BLKDISCARD is under test. Everything else has to reach the
	 * kernel, in particular the BLKGETSIZE64 and BLKSSZGET probes that
	 * spdk_fd_get_size() and spdk_fd_get_blocklen() perform: faking success
	 * for those would leave their output parameters uninitialised.
	 */
	if (request != BLKDISCARD) {
		return __real_ioctl(fd, request, arg);
	}

	g_ioctl_calls++;
	g_ioctl_fd = fd;
	g_ioctl_request = request;
	if (arg != NULL) {
		memcpy(g_ioctl_range, arg, sizeof(g_ioctl_range));
	}

	if (g_ioctl_result != 0) {
		errno = g_ioctl_errno;
		return -1;
	}

	return 0;
}

int
__wrap_fallocate(int fd, int mode, off_t offset, off_t len)
{
	g_fallocate_calls++;
	g_fallocate_fd = fd;
	g_fallocate_mode = mode;
	g_fallocate_offset = offset;
	g_fallocate_len = len;

	if (g_fallocate_result != 0) {
		errno = g_fallocate_errno;
		return -1;
	}

	return 0;
}

static int g_completion_calls;
static enum spdk_bdev_io_status g_completion_status;
static int g_completion_aio_status;

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_completion_calls++;
	g_completion_status = status;
}

void
spdk_bdev_io_complete_aio_status(struct spdk_bdev_io *bdev_io, int aio_result)
{
	g_completion_calls++;
	g_completion_status = SPDK_BDEV_IO_STATUS_AIO_ERROR;
	g_completion_aio_status = aio_result;
}

DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_unregister_by_name, int, (const char *bdev_name,
		struct spdk_bdev_module *module, spdk_bdev_unregister_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write,
				      spdk_bdev_event_cb_t event_cb, void *event_ctx,
				      struct spdk_bdev_desc **desc), 0);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_get_uuid, const struct spdk_uuid *, (const struct spdk_bdev *bdev), NULL);
DEFINE_STUB_V(spdk_bdev_io_get_buf, (struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
				     uint64_t len));
DEFINE_STUB(spdk_bdev_notify_blockcnt_change, int, (struct spdk_bdev *bdev, uint64_t size), 0);
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev,
		struct spdk_bdev_desc *desc, struct spdk_bdev_module *module), 0);

#define UT_BLOCKLEN	512
#define UT_FD		42

static struct spdk_bdev_io *
ut_alloc_bdev_io(struct file_disk *fdisk, uint64_t offset_blocks, uint64_t num_blocks)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = calloc(1, sizeof(*bdev_io) + sizeof(struct bdev_aio_task));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);

	bdev_io->bdev = &fdisk->disk;
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;

	return bdev_io;
}

static void
ut_init_fdisk(struct file_disk *fdisk, bool fallocate, bool blkdiscard)
{
	memset(fdisk, 0, sizeof(*fdisk));

	fdisk->fd = UT_FD;
	fdisk->fallocate = fallocate;
	fdisk->blkdiscard = blkdiscard;
	fdisk->disk.blocklen = UT_BLOCKLEN;
}

static void
ut_reset(void)
{
	g_ioctl_calls = 0;
	g_ioctl_fd = 0;
	g_ioctl_request = 0;
	memset(g_ioctl_range, 0, sizeof(g_ioctl_range));
	g_ioctl_result = 0;
	g_ioctl_errno = 0;

	g_fallocate_calls = 0;
	g_fallocate_fd = 0;
	g_fallocate_mode = 0;
	g_fallocate_offset = 0;
	g_fallocate_len = 0;
	g_fallocate_result = 0;
	g_fallocate_errno = 0;

	g_completion_calls = 0;
	g_completion_status = SPDK_BDEV_IO_STATUS_PENDING;
	g_completion_aio_status = 0;
}

static void
test_bdev_aio_io_type_supported(void)
{
	struct file_disk fdisk;

	ut_reset();

	ut_init_fdisk(&fdisk, false, false);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_UNMAP) == false);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == false);

	ut_init_fdisk(&fdisk, true, false);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_UNMAP) == true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == true);

	ut_init_fdisk(&fdisk, false, true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_UNMAP) == true);
	/* blkdiscard cannot zero, so WRITE ZEROES must stay unsupported. */
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == false);

	/* Combined, fallocate still provides WRITE ZEROES. */
	ut_init_fdisk(&fdisk, true, true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_UNMAP) == true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == true);

	/* Read/write/flush/reset are always supported, unrelated to unmap config. */
	ut_init_fdisk(&fdisk, false, false);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_READ) == true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_WRITE) == true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_FLUSH) == true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_RESET) == true);
	CU_ASSERT(bdev_aio_io_type_supported(&fdisk, SPDK_BDEV_IO_TYPE_COMPARE) == false);
}

static void
test_bdev_aio_unmap_dispatch(void)
{
	struct spdk_bdev_io *bdev_io;
	struct file_disk fdisk;

	/* Neither backend enabled: complete with -ENOTSUP, no syscall at all. */
	ut_reset();
	ut_init_fdisk(&fdisk, false, false);
	bdev_io = ut_alloc_bdev_io(&fdisk, 0, 1);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_fallocate_calls == 0);
	CU_ASSERT(g_ioctl_calls == 0);
	CU_ASSERT(g_completion_calls == 1);
	CU_ASSERT(g_completion_status == SPDK_BDEV_IO_STATUS_AIO_ERROR);
	CU_ASSERT(g_completion_aio_status == -ENOTSUP);
	free(bdev_io);

	/* fallocate only: punch hole, never BLKDISCARD. */
	ut_reset();
	ut_init_fdisk(&fdisk, true, false);
	bdev_io = ut_alloc_bdev_io(&fdisk, 0, 1);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_fallocate_calls == 1);
	CU_ASSERT(g_ioctl_calls == 0);
	CU_ASSERT(g_fallocate_mode == (FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE));
	CU_ASSERT(g_completion_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	free(bdev_io);

	/* blkdiscard only: BLKDISCARD, never fallocate. */
	ut_reset();
	ut_init_fdisk(&fdisk, false, true);
	bdev_io = ut_alloc_bdev_io(&fdisk, 0, 1);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_ioctl_calls == 1);
	CU_ASSERT(g_fallocate_calls == 0);
	CU_ASSERT(g_ioctl_request == BLKDISCARD);
	CU_ASSERT(g_ioctl_fd == UT_FD);
	CU_ASSERT(g_completion_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	free(bdev_io);

	/* Both: blkdiscard takes precedence for UNMAP. */
	ut_reset();
	ut_init_fdisk(&fdisk, true, true);
	bdev_io = ut_alloc_bdev_io(&fdisk, 0, 1);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_ioctl_calls == 1);
	CU_ASSERT(g_fallocate_calls == 0);
	CU_ASSERT(g_ioctl_request == BLKDISCARD);
	CU_ASSERT(g_completion_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	free(bdev_io);
}

static void
test_bdev_aio_blkdiscard_range(void)
{
	struct spdk_bdev_io *bdev_io;
	struct file_disk fdisk;

	ut_reset();
	ut_init_fdisk(&fdisk, false, true);
	bdev_io = ut_alloc_bdev_io(&fdisk, 8, 16);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_ioctl_calls == 1);
	CU_ASSERT(g_ioctl_range[0] == 8ULL * UT_BLOCKLEN);
	CU_ASSERT(g_ioctl_range[1] == 16ULL * UT_BLOCKLEN);
	free(bdev_io);

	/* A non-512 block size must scale both range members. */
	ut_reset();
	ut_init_fdisk(&fdisk, false, true);
	fdisk.disk.blocklen = 4096;
	bdev_io = ut_alloc_bdev_io(&fdisk, 3, 5);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_ioctl_calls == 1);
	CU_ASSERT(g_ioctl_range[0] == 3ULL * 4096);
	CU_ASSERT(g_ioctl_range[1] == 5ULL * 4096);
	free(bdev_io);

	/* Offsets beyond 4GiB must not be truncated to 32 bits. */
	ut_reset();
	ut_init_fdisk(&fdisk, false, true);
	bdev_io = ut_alloc_bdev_io(&fdisk, 0x100000000ULL, 8);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_ioctl_calls == 1);
	CU_ASSERT(g_ioctl_range[0] == 0x100000000ULL * UT_BLOCKLEN);
	CU_ASSERT(g_ioctl_range[1] == 8ULL * UT_BLOCKLEN);
	free(bdev_io);
}

static void
test_bdev_aio_blkdiscard_error(void)
{
	struct spdk_bdev_io *bdev_io;
	struct file_disk fdisk;

	ut_reset();
	ut_init_fdisk(&fdisk, false, true);
	g_ioctl_result = -1;
	g_ioctl_errno = EOPNOTSUPP;
	bdev_io = ut_alloc_bdev_io(&fdisk, 0, 1);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_ioctl_calls == 1);
	CU_ASSERT(g_completion_status == SPDK_BDEV_IO_STATUS_AIO_ERROR);
	CU_ASSERT(g_completion_aio_status == -EOPNOTSUPP);
	free(bdev_io);

	/* A failing fallocate propagates its errno the same way. */
	ut_reset();
	ut_init_fdisk(&fdisk, true, false);
	g_fallocate_result = -1;
	g_fallocate_errno = EINVAL;
	bdev_io = ut_alloc_bdev_io(&fdisk, 0, 1);
	bdev_aio_unmap(bdev_io);
	CU_ASSERT(g_fallocate_calls == 1);
	CU_ASSERT(g_completion_status == SPDK_BDEV_IO_STATUS_AIO_ERROR);
	CU_ASSERT(g_completion_aio_status == -EINVAL);
	free(bdev_io);
}

static void
test_bdev_aio_write_zeroes(void)
{
	struct spdk_bdev_io *bdev_io;
	struct file_disk fdisk;

	ut_reset();
	ut_init_fdisk(&fdisk, true, false);
	bdev_io = ut_alloc_bdev_io(&fdisk, 2, 4);
	bdev_aio_write_zeros(bdev_io);
	CU_ASSERT(g_fallocate_calls == 1);
	CU_ASSERT(g_ioctl_calls == 0);
	CU_ASSERT(g_fallocate_mode == FALLOC_FL_ZERO_RANGE);
	CU_ASSERT(g_fallocate_offset == 2 * UT_BLOCKLEN);
	CU_ASSERT(g_fallocate_len == 4 * UT_BLOCKLEN);
	free(bdev_io);

	/* With fallocate disabled the request must be refused, not discarded. */
	ut_reset();
	ut_init_fdisk(&fdisk, false, true);
	bdev_io = ut_alloc_bdev_io(&fdisk, 2, 4);
	bdev_aio_write_zeros(bdev_io);
	CU_ASSERT(g_fallocate_calls == 0);
	CU_ASSERT(g_ioctl_calls == 0);
	CU_ASSERT(g_completion_aio_status == -ENOTSUP);
	free(bdev_io);
}

static void
test_create_aio_bdev_params(void)
{
	struct file_disk *fdisk;
	char path[] = "/tmp/aio_ut_XXXXXX";
	int fd, rc;

	ut_reset();

	/* A read-only bdev cannot discard. */
	rc = create_aio_bdev("ut_aio", "/nonexistent/ut_aio_file", 0, true, false, true, NULL, false);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(TAILQ_EMPTY(&g_aio_disk_head));

	fd = mkstemp(path);
	SPDK_CU_ASSERT_FATAL(fd >= 0);
	SPDK_CU_ASSERT_FATAL(ftruncate(fd, 1024 * UT_BLOCKLEN) == 0);
	close(fd);

	/* BLKDISCARD is only valid on block devices, so a regular file is rejected. */
	rc = create_aio_bdev("ut_aio", path, UT_BLOCKLEN, false, false, true, NULL, false);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(TAILQ_EMPTY(&g_aio_disk_head));

	/* Neither flag set leaves both backends off. */
	rc = create_aio_bdev("ut_aio2", path, UT_BLOCKLEN, false, false, false, NULL, false);
	CU_ASSERT(rc == 0);
	if (rc == 0) {
		fdisk = TAILQ_FIRST(&g_aio_disk_head);
		SPDK_CU_ASSERT_FATAL(fdisk != NULL);
		CU_ASSERT(fdisk->blkdiscard == false);
		CU_ASSERT(bdev_aio_io_type_supported(fdisk, SPDK_BDEV_IO_TYPE_UNMAP) == false);

		TAILQ_REMOVE(&g_aio_disk_head, fdisk, link);
		spdk_io_device_unregister(fdisk, NULL);
		bdev_aio_close(fdisk);
		aio_free_disk(fdisk);
	}

	unlink(path);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("bdev_aio", NULL, NULL);

	CU_ADD_TEST(suite, test_bdev_aio_io_type_supported);
	CU_ADD_TEST(suite, test_bdev_aio_unmap_dispatch);
	CU_ADD_TEST(suite, test_bdev_aio_blkdiscard_range);
	CU_ADD_TEST(suite, test_bdev_aio_blkdiscard_error);
	CU_ADD_TEST(suite, test_bdev_aio_write_zeroes);
	CU_ADD_TEST(suite, test_create_aio_bdev_params);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	free_threads();
	CU_cleanup_registry();

	return num_failures;
}
