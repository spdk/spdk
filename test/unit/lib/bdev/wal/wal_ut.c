/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 SPBU
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "common/lib/ut_multithread.c"
#include "spdk_internal/mock.h"

#include "module/bdev/wal/vbdev_wal.c"

int g_async_rc = 0;
bool g_async_done = false;
enum spdk_bdev_io_status g_last_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
uint32_t g_io_complete_count = 0;
int g_write_blocks_rc = 0;
int g_writev_blocks_rc = 0;
int g_read_blocks_rc = 0;
int g_flush_blocks_rc = 0;
bool g_fail_journal_sb_write = false;

uint64_t g_last_write_lba = 0;
uint32_t g_last_write_blocks = 0;
uint32_t g_main_write_count = 0;
uint64_t g_last_journal_flush_lba = 0;
uint64_t g_last_journal_flush_blocks = 0;
uint32_t g_journal_flush_count = 0;
uint32_t g_full_journal_flush_count = 0;
uint32_t g_superblock_flush_count = 0;
struct wal_record_header g_last_hdr = {0};
uint8_t g_journal_data[1024 * 512];
uint8_t g_main_captured_data[1024 * 512];

int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
                   void *event_ctx, struct spdk_bdev_desc **_desc)
{
    if (strcmp(bdev_name, "main") == 0) {
        *_desc = (struct spdk_bdev_desc *)0x11111111;
    } else if (strcmp(bdev_name, "journal") == 0) {
        *_desc = (struct spdk_bdev_desc *)0x22222222;
    } else {
        *_desc = (struct spdk_bdev_desc *)0x12345678;
    }
    return 0;
}

static int g_dummy_io_device = 0;

static int
dummy_io_channel_create_cb(void *io_device, void *ctx_buf)
{
    return 0;
}

static void
dummy_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
    return (struct spdk_bdev *)0x87654321;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
    return spdk_get_io_channel(&g_dummy_io_device);
}

DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_bdev_reset, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 1024);
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 64);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test_bdev");
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type), true);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *vbdev), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
    g_last_io_status = status;
    g_io_complete_count++;
}

static void
test_cb(void *cb_arg, int rc)
{
    g_async_rc = rc;
    g_async_done = true;
}

struct ut_wal_io_cpl_args {
    spdk_bdev_io_completion_cb cb_fn;
    void *cb_arg;
    bool success;
};

static void
_ut_wal_io_cpl(void *arg)
{
    struct ut_wal_io_cpl_args *cpl_args = arg;
    struct spdk_bdev_io *bdev_io = calloc(1, sizeof(struct spdk_bdev_io));
    
    cpl_args->cb_fn(bdev_io, cpl_args->success, cpl_args->cb_arg);
    free(cpl_args);
}

static void
ut_wal_io_cpl(spdk_bdev_io_completion_cb cb_fn, bool success, void *cb_arg)
{
    struct ut_wal_io_cpl_args *cpl_args = calloc(1, sizeof(*cpl_args));
    cpl_args->cb_fn = cb_fn;
    cpl_args->success = success;
    cpl_args->cb_arg = cb_arg;
    spdk_thread_send_msg(spdk_get_thread(), _ut_wal_io_cpl, cpl_args);
}

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    struct wal_vbdev *vb = TAILQ_FIRST(&g_wal);

    if (g_write_blocks_rc != 0) {
        return g_write_blocks_rc;
    }
    if (vb && desc == vb->journal_desc && offset_blocks == 0 && g_fail_journal_sb_write) {
        return -EIO;
    }
    
    if (vb && desc == vb->main_desc) {
        g_last_write_lba = offset_blocks;
        g_last_write_blocks = (uint32_t)num_blocks;
        g_main_write_count++;
        memcpy(g_main_captured_data, buf, num_blocks * 512);
    } else if (vb && desc == vb->journal_desc) {
        memcpy(&g_journal_data[offset_blocks * 512], buf, num_blocks * 512);
    }
    
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                        struct iovec *iov, int iovcnt,
                        uint64_t offset_blocks, uint64_t num_blocks,
                        spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    if (g_writev_blocks_rc != 0) {
        return g_writev_blocks_rc;
    }

    if (desc == (struct spdk_bdev_desc *)0x22222222) {
        g_last_write_lba = offset_blocks;
        g_last_write_blocks = (uint32_t)num_blocks;
        uint8_t *dst = &g_journal_data[offset_blocks * 512];
        size_t remaining = num_blocks * 512;

        for (int i = 0; i < iovcnt && remaining > 0; i++) {
            size_t len = spdk_min(iov[i].iov_len, remaining);
            memcpy(dst, iov[i].iov_base, len);
            dst += len;
            remaining -= len;
        }
        
        if (iovcnt > 0 && iov[0].iov_len >= sizeof(struct wal_record_header)) {
            struct wal_record_header *potential_hdr = (struct wal_record_header *)iov[0].iov_base;
            if (potential_hdr && potential_hdr->magic == WAL_RECORD_MAGIC) {
                memcpy(&g_last_hdr, iov[0].iov_base, sizeof(struct wal_record_header));
            }
        }
    }

    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    struct wal_vbdev *vb = TAILQ_FIRST(&g_wal);

    if (g_read_blocks_rc != 0) {
        return g_read_blocks_rc;
    }
    
    if (vb && desc == vb->journal_desc) {
        memcpy(buf, &g_journal_data[offset_blocks * 512], num_blocks * 512);
    }
    
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                       struct iovec *iov, int iovcnt,
                       uint64_t offset_blocks, uint64_t num_blocks,
                       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                       uint64_t offset_blocks, uint64_t num_blocks,
                       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    struct wal_vbdev *vb = TAILQ_FIRST(&g_wal);

    if (g_flush_blocks_rc != 0) {
        return g_flush_blocks_rc;
    }
    if (vb && desc == vb->journal_desc) {
        g_last_journal_flush_lba = offset_blocks;
        g_last_journal_flush_blocks = num_blocks;
        g_journal_flush_count++;
        if (offset_blocks == 0 && num_blocks == 1024) {
            g_full_journal_flush_count++;
        }
        if (offset_blocks == 0 && num_blocks == 1) {
            g_superblock_flush_count++;
        }
    }

    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

static void
test_setup(void)
{
    g_last_write_lba = 0;
    g_last_write_blocks = 0;
    g_main_write_count = 0;
    g_last_journal_flush_lba = 0;
    g_last_journal_flush_blocks = 0;
    g_journal_flush_count = 0;
    g_full_journal_flush_count = 0;
    g_superblock_flush_count = 0;
    memset(&g_last_hdr, 0, sizeof(g_last_hdr));
    memset(g_journal_data, 0, sizeof(g_journal_data));
    memset(g_main_captured_data, 0, sizeof(g_main_captured_data));
    g_async_rc = 0;
    g_async_done = false;
    g_last_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
    g_io_complete_count = 0;
    g_write_blocks_rc = 0;
    g_writev_blocks_rc = 0;
    g_read_blocks_rc = 0;
    g_flush_blocks_rc = 0;
    g_fail_journal_sb_write = false;

    allocate_threads(1);
    set_thread(0);
    spdk_io_device_register(&g_dummy_io_device, dummy_io_channel_create_cb, dummy_io_channel_destroy_cb, 0, "dummy");
}

static void
test_cleanup(void)
{
    struct wal_vbdev *vb, *tmp;
    TAILQ_FOREACH_SAFE(vb, &g_wal, link, tmp) {
        TAILQ_REMOVE(&g_wal, vb, link);
        spdk_io_device_unregister(vb, NULL);
        free(vb->bdev.name);
        free(vb);
    }

    spdk_io_device_unregister(&g_dummy_io_device, NULL);
    free_threads();
}

static struct spdk_bdev_io *
ut_alloc_wal_io(struct wal_vbdev *vb, enum spdk_bdev_io_type type,
                struct iovec *iov, int iovcnt, uint64_t offset_blocks,
                uint64_t num_blocks)
{
    struct spdk_bdev_io *bdev_io;

    bdev_io = calloc(1, sizeof(*bdev_io) + sizeof(struct wal_bdev_io));
    SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
    bdev_io->bdev = &vb->bdev;
    bdev_io->type = type;
    bdev_io->u.bdev.iovs = iov;
    bdev_io->u.bdev.iovcnt = iovcnt;
    bdev_io->u.bdev.offset_blocks = offset_blocks;
    bdev_io->u.bdev.num_blocks = num_blocks;

    return bdev_io;
}

static struct wal_record_header
ut_make_record(uint64_t seq, uint64_t lba, uint32_t num_blocks, const void *data)
{
    struct wal_record_header hdr = {0};

    hdr.magic = WAL_RECORD_MAGIC;
    hdr.type = WAL_RECORD_TYPE_DATA;
    hdr.seq = seq;
    hdr.lba = lba;
    hdr.num_blocks = num_blocks;
    hdr.rec_len = sizeof(struct wal_record_header) + num_blocks * 512;
    hdr.data_crc = spdk_crc32c_update(data, num_blocks * 512, 0);
    hdr.hdr_crc = spdk_crc32c_update(&hdr,
                                     offsetof(struct wal_record_header, hdr_crc),
                                     0);

    return hdr;
}

static struct wal_record_header
ut_make_pad(uint64_t seq)
{
    struct wal_record_header hdr = {0};

    hdr.magic = WAL_RECORD_MAGIC;
    hdr.type = WAL_RECORD_TYPE_PAD;
    hdr.seq = seq;
    hdr.rec_len = sizeof(struct wal_record_header);
    hdr.hdr_crc = spdk_crc32c_update(&hdr,
                                     offsetof(struct wal_record_header, hdr_crc),
                                     0);

    return hdr;
}

static void
test_wal_init(void)
{
    int rc;
    uint32_t block_sz = 0;
    uint64_t size_mb = 0;
    
    g_async_done = false;
    test_setup();

    rc = wal_bdev_create_disk("main", "journal", "wal0", &block_sz, &size_mb, test_cb, NULL);
    CU_ASSERT(rc == 0);

    poll_threads();
    
    CU_ASSERT(g_async_done == true);
    CU_ASSERT(g_async_rc == 0);
    CU_ASSERT(block_sz == 512);

    test_cleanup();
}

static void
test_wal_init_rejects_corrupt_superblock(void)
{
    int rc;
    struct wal_superblock *sb = (struct wal_superblock *)g_journal_data;

    test_setup();
    sb->magic = WAL_SUPERBLOCK_MAGIC;
    sb->version = 99;

    rc = wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    CU_ASSERT(rc == 0);
    poll_threads();

    CU_ASSERT(g_async_done == true);
    CU_ASSERT_EQUAL(g_async_rc, -EIO);
    CU_ASSERT(TAILQ_EMPTY(&g_wal));

    test_cleanup();
}

static void
test_wal_write(void)
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;
    void *data = calloc(1, 512);

    test_setup();

    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);

    qch = wal_get_io_channel(vb);
    
    bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct wal_bdev_io));
    bdev_io->bdev = &vb->bdev;
    bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
    bdev_io->u.bdev.iovs = &iov;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.offset_blocks = 100;
    bdev_io->u.bdev.num_blocks = 1;
    iov.iov_base = data;
    iov.iov_len = 512;

    wal_submit_request(qch, bdev_io);
    for (int i = 0; i < 8; i++) {
        poll_threads();
    }

    CU_ASSERT_EQUAL(g_last_write_lba, 1);
    CU_ASSERT_EQUAL(g_last_write_blocks, 2);
    CU_ASSERT(g_last_hdr.magic == WAL_RECORD_MAGIC);
    CU_ASSERT(g_last_hdr.lba == 100);
    CU_ASSERT(g_last_hdr.seq == 1);
    CU_ASSERT(g_last_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
    
    CU_ASSERT(vb->sb.write_pos == 3);
    CU_ASSERT(vb->sb.checkpoint_seq == 1);
    CU_ASSERT(vb->sb.head_pos == 3);

    spdk_put_io_channel(qch);
    free(bdev_io);
    free(data);
    test_cleanup();
}

static void
test_wal_wrap_around(void)
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;
    void *data = calloc(1, 10 * 512);

    test_setup();

    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);
    
    vb->sb.write_pos = 1023;
    vb->sb.head_pos = 1023;
    
    qch = wal_get_io_channel(vb);
    bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct wal_bdev_io));
    bdev_io->bdev = &vb->bdev;
    bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
    bdev_io->u.bdev.iovs = &iov;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.offset_blocks = 500;
    bdev_io->u.bdev.num_blocks = 10;
    iov.iov_base = data;
    iov.iov_len = 10 * 512;

    wal_submit_request(qch, bdev_io);
    for (int i = 0; i < 8; i++) {
        poll_threads();
    }

    CU_ASSERT_EQUAL(g_last_write_lba, 1);
    CU_ASSERT_EQUAL(vb->sb.write_pos, 1 + 10 + 1);
    CU_ASSERT(g_full_journal_flush_count >= 1);
    CU_ASSERT(g_superblock_flush_count >= 1);

    spdk_put_io_channel(qch);
    free(bdev_io);
    free(data);
    test_cleanup();
}

static void
test_wal_recovery(void)
{
    struct wal_vbdev *vb;
    struct wal_record_header hdr = {0};
    char data[512] = "recovery payload";
    
    test_setup();

    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);

    hdr = ut_make_record(10, 500, 1, data);
    
    memcpy(&g_journal_data[1 * 512], &hdr, sizeof(hdr));
    memcpy(&g_journal_data[2 * 512], data, 512);

    g_async_done = false;
    wal_bdev_recover("wal0", test_cb, NULL);
    
    for (int i = 0; i < 10; i++) {
        wal_recover_poll(vb);
        poll_threads();
        if (g_async_done) break;
    }

    CU_ASSERT(g_async_done == true);
    CU_ASSERT(g_async_rc == 0);
    CU_ASSERT(g_last_write_lba == 500);
    CU_ASSERT(g_last_write_blocks == 1);
    CU_ASSERT(memcmp(g_main_captured_data, data, 512) == 0);
    struct wal_superblock *sb = (struct wal_superblock *)g_journal_data;
    CU_ASSERT(sb->checkpoint_seq == 10);
    CU_ASSERT(sb->head_pos == 3);
    CU_ASSERT(wal_superblock_valid(vb, sb));

    test_cleanup();
}

static void
test_wal_write_rejects_bad_iov_and_bounds(void)
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;
    void *data = calloc(1, 512);

    test_setup();
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);
    qch = wal_get_io_channel(vb);

    iov.iov_base = data;
    iov.iov_len = 511;
    bdev_io = ut_alloc_wal_io(vb, SPDK_BDEV_IO_TYPE_WRITE, &iov, 1, 10, 1);
    wal_submit_request(qch, bdev_io);
    CU_ASSERT_EQUAL(g_last_io_status, SPDK_BDEV_IO_STATUS_FAILED);
    CU_ASSERT_EQUAL(g_io_complete_count, 1);
    CU_ASSERT_EQUAL(g_last_hdr.magic, 0);
    free(bdev_io);

    g_io_complete_count = 0;
    iov.iov_len = 512;
    bdev_io = ut_alloc_wal_io(vb, SPDK_BDEV_IO_TYPE_WRITE, &iov, 1, 1024, 1);
    wal_submit_request(qch, bdev_io);
    CU_ASSERT_EQUAL(g_last_io_status, SPDK_BDEV_IO_STATUS_FAILED);
    CU_ASSERT_EQUAL(g_io_complete_count, 1);

    free(bdev_io);
    spdk_put_io_channel(qch);
    free(data);
    test_cleanup();
}

static void
test_wal_write_immediate_error_releases_lock(void)
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;
    void *data = calloc(1, 512);

    test_setup();
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);
    qch = wal_get_io_channel(vb);

    iov.iov_base = data;
    iov.iov_len = 512;
    g_writev_blocks_rc = -ENOMEM;
    bdev_io = ut_alloc_wal_io(vb, SPDK_BDEV_IO_TYPE_WRITE, &iov, 1, 10, 1);
    wal_submit_request(qch, bdev_io);
    CU_ASSERT_EQUAL(g_last_io_status, SPDK_BDEV_IO_STATUS_FAILED);
    CU_ASSERT_EQUAL(g_io_complete_count, 1);
    CU_ASSERT(vb->write_in_progress == false);

    free(bdev_io);
    spdk_put_io_channel(qch);
    free(data);
    test_cleanup();
}

static void
test_wal_checkpoint_not_advanced_on_sb_failure(void)
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;
    void *data = calloc(1, 512);

    test_setup();
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);
    qch = wal_get_io_channel(vb);

    iov.iov_base = data;
    iov.iov_len = 512;
    g_fail_journal_sb_write = true;
    bdev_io = ut_alloc_wal_io(vb, SPDK_BDEV_IO_TYPE_WRITE, &iov, 1, 10, 1);
    wal_submit_request(qch, bdev_io);
    poll_threads();

    CU_ASSERT_EQUAL(g_last_io_status, SPDK_BDEV_IO_STATUS_FAILED);
    CU_ASSERT(vb->write_in_progress == false);
    CU_ASSERT_EQUAL(vb->sb.write_pos, 3);
    CU_ASSERT_EQUAL(vb->sb.head_pos, 1);
    CU_ASSERT_EQUAL(vb->sb.checkpoint_seq, 0);

    free(bdev_io);
    spdk_put_io_channel(qch);
    free(data);
    test_cleanup();
}

static void
test_wal_recovery_rejects_bad_bounds(void)
{
    struct wal_vbdev *vb;
    struct wal_record_header hdr;
    char data[512] = "bad bounds";

    test_setup();
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);

    hdr = ut_make_record(10, 1024, 1, data);
    memcpy(&g_journal_data[1 * 512], &hdr, sizeof(hdr));
    memcpy(&g_journal_data[2 * 512], data, 512);

    g_async_done = false;
    wal_bdev_recover("wal0", test_cb, NULL);
    for (int i = 0; i < 10; i++) {
        wal_recover_poll(vb);
        poll_threads();
        if (g_async_done) break;
    }

    CU_ASSERT(g_async_done == true);
    CU_ASSERT_EQUAL(g_async_rc, -EIO);
    CU_ASSERT_EQUAL(g_main_write_count, 0);

    test_cleanup();
}

static void
test_wal_recovery_pad_wrap(void)
{
    struct wal_vbdev *vb;
    struct wal_record_header pad;
    struct wal_record_header hdr;
    char data[512] = "wrapped recovery";

    test_setup();
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_PTR_NOT_NULL_FATAL(vb);

    vb->sb.head_pos = 1023;
    vb->sb.write_pos = 3;
    vb->sb.checkpoint_seq = 9;
    vb->sb.next_seq = 12;
    wal_superblock_update_crc(&vb->sb);

    pad = ut_make_pad(10);
    hdr = ut_make_record(11, 600, 1, data);
    memcpy(&g_journal_data[1023 * 512], &pad, sizeof(pad));
    memcpy(&g_journal_data[1 * 512], &hdr, sizeof(hdr));
    memcpy(&g_journal_data[2 * 512], data, 512);

    g_async_done = false;
    wal_bdev_recover("wal0", test_cb, NULL);
    for (int i = 0; i < 20; i++) {
        wal_recover_poll(vb);
        poll_threads();
        if (g_async_done) break;
    }

    CU_ASSERT(g_async_done == true);
    CU_ASSERT_EQUAL(g_async_rc, 0);
    CU_ASSERT_EQUAL(g_last_write_lba, 600);
    CU_ASSERT(memcmp(g_main_captured_data, data, 512) == 0);
    CU_ASSERT_EQUAL(vb->sb.checkpoint_seq, 11);
    CU_ASSERT_EQUAL(vb->sb.head_pos, 3);

    test_cleanup();
}

int
main(int argc, char *argv[])
{
    CU_pSuite suite = NULL;
    unsigned int num_failures;

    if (CU_initialize_registry() != CUE_SUCCESS) {
        return CU_get_error();
    }

    suite = CU_add_suite("wal_suite", NULL, NULL);
    if (suite == NULL) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    if (CU_add_test(suite, "test_wal_init", test_wal_init) == NULL ||
        CU_add_test(suite, "test_wal_init_rejects_corrupt_superblock",
                    test_wal_init_rejects_corrupt_superblock) == NULL ||
        CU_add_test(suite, "test_wal_write", test_wal_write) == NULL ||
        CU_add_test(suite, "test_wal_wrap_around", test_wal_wrap_around) == NULL ||
        CU_add_test(suite, "test_wal_recovery", test_wal_recovery) == NULL ||
        CU_add_test(suite, "test_wal_write_rejects_bad_iov_and_bounds",
                    test_wal_write_rejects_bad_iov_and_bounds) == NULL ||
        CU_add_test(suite, "test_wal_write_immediate_error_releases_lock",
                    test_wal_write_immediate_error_releases_lock) == NULL ||
        CU_add_test(suite, "test_wal_checkpoint_not_advanced_on_sb_failure",
                    test_wal_checkpoint_not_advanced_on_sb_failure) == NULL ||
        CU_add_test(suite, "test_wal_recovery_rejects_bad_bounds",
                    test_wal_recovery_rejects_bad_bounds) == NULL ||
        CU_add_test(suite, "test_wal_recovery_pad_wrap",
                    test_wal_recovery_pad_wrap) == NULL) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    num_failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return num_failures > 0 ? 1 : 0;
}
