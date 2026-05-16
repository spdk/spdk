/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 SPBU
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "common/lib/ut_multithread.c"
#include "spdk_internal/mock.h"

/* Include the implementation directly */
#include "module/bdev/wal/vbdev_wal.c"

/* SPDK Stubs */
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *vbdev), 0);
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb, void *event_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 1024);
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 64);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test_bdev");
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), (void *)0x1);
DEFINE_STUB_V(spdk_put_io_channel, (struct spdk_io_channel *ch));
DEFINE_STUB(spdk_io_device_register, int, (void *io_device, spdk_io_channel_create_cb create_cb, spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size, const char *name), 0);
DEFINE_STUB_V(spdk_io_device_unregister, (void *io_device, spdk_io_device_unregister_cb unregister_cb));
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));

/* Global state for async results */
int g_async_rc = 0;
bool g_async_done = false;

static void
test_cb(void *cb_arg, int rc)
{
    g_async_rc = rc;
    g_async_done = true;
}

/* Mock I/O Completion */
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

/* Mock spdk_bdev_write_blocks */
/* Global state for I/O tracking */
uint64_t g_last_write_lba = 0;
uint32_t g_last_write_blocks = 0;
struct wal_record_header g_last_hdr = {0};
uint8_t g_journal_data[1024 * 512]; /* Fake journal content (1024 blocks) */
uint8_t g_main_captured_data[1024 * 512]; /* Captured writes to main bdev */

/* Mock spdk_bdev_write_blocks */
int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    struct wal_vbdev *vb = TAILQ_FIRST(&g_wal);
    
    if (vb && desc == vb->main_desc) {
        g_last_write_lba = offset_blocks;
        g_last_write_blocks = (uint32_t)num_blocks;
        memcpy(g_main_captured_data, buf, num_blocks * 512);
    }
    
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

/* Mock spdk_bdev_writev_blocks */
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                        struct iovec *iov, int iovcnt,
                        uint64_t offset_blocks, uint64_t num_blocks,
                        spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    g_last_write_lba = offset_blocks;
    g_last_write_blocks = (uint32_t)num_blocks;
    
    /* Если это запись в журнал с заголовком */
    if (iovcnt > 0 && iov[0].iov_len == sizeof(struct wal_record_header)) {
        memcpy(&g_last_hdr, iov[0].iov_base, sizeof(struct wal_record_header));
    }

    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

/* Mock spdk_bdev_read_blocks */
int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    struct wal_vbdev *vb = TAILQ_FIRST(&g_wal);
    
    if (vb && desc == vb->journal_desc) {
        memcpy(buf, &g_journal_data[offset_blocks * 512], num_blocks * 512);
    }
    
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

/* Mock spdk_bdev_readv_blocks */
int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                       struct iovec *iov, int iovcnt,
                       uint64_t offset_blocks, uint64_t num_blocks,
                       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

/* Mock spdk_bdev_flush_blocks */
int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                       uint64_t offset_blocks, uint64_t num_blocks,
                       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
    ut_wal_io_cpl(cb, true, cb_arg);
    return 0;
}

static void
test_wal_recovery(void)
{
    struct wal_vbdev *vb;
    struct wal_record_header hdr = {0};
    char data[512] = "recovery payload";
    
    allocate_threads(1);
    set_thread(0);

    /* 1. Setup: Create WAL bdev */
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_FATAL(vb != NULL);

    /* 2. Prepare fake journal content at LBA 1 */
    hdr.magic = WAL_RECORD_MAGIC;
    hdr.seq = 10; /* Greater than checkpoint 0 */
    hdr.lba = 500; /* Replay to main LBA 500 */
    hdr.num_blocks = 1;
    hdr.data_crc = spdk_crc32c_update(data, 512, 0);
    hdr.hdr_crc = spdk_crc32c_update(&hdr, offsetof(struct wal_record_header, hdr_crc), 0);
    
    memcpy(&g_journal_data[1 * 512], &hdr, sizeof(hdr));
    memcpy(&g_journal_data[2 * 512], data, 512);

    /* 3. Start Recovery */
    g_async_done = false;
    wal_bdev_recover("wal0", test_cb, NULL);
    
    /* 4. Drive the poller manually. 
     * Step 1: Read Header
     * Step 2: Read Data
     * Step 3: Write to Main
     * Step 4: Next Step (finds invalid magic and stops)
     */
    for (int i = 0; i < 10; i++) {
        wal_recover_poll(vb);
        poll_threads();
        if (g_async_done) break;
    }

    /* 5. Verify Results */
    CU_ASSERT(g_async_done == true);
    CU_ASSERT(g_async_rc == 0);
    CU_ASSERT(g_last_write_lba == 500);
    CU_ASSERT(g_last_write_blocks == 1);
    CU_ASSERT(memcmp(g_main_captured_data, data, 512) == 0);

    free_threads();
}

static void
test_wal_init(void)
{
    int rc;
    uint32_t block_sz = 0;
    uint64_t size_mb = 0;
    
    g_async_done = false;

    /* Initialize threads for async processing */
    allocate_threads(1);
    set_thread(0);

    rc = wal_bdev_create_disk("main", "journal", "wal0", &block_sz, &size_mb, test_cb, NULL);
    CU_ASSERT(rc == 0);

    /* Process messages until async write is done */
    poll_threads();
    
    CU_ASSERT(g_async_done == true);
    CU_ASSERT(g_async_rc == 0);
    CU_ASSERT(block_sz == 512);

    free_threads();
}

static void
test_wal_write(void)
{
    struct wal_vbdev *vb;
    struct wal_io_channel *wch;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;
    char data[512] = "test data";

    allocate_threads(1);
    set_thread(0);

    /* Setup: Create disk */
    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    CU_ASSERT_FATAL(vb != NULL);

    /* Create channel and IO */
    qch = wal_get_io_channel(vb);
    wch = spdk_io_channel_get_ctx(qch);
    
    bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct wal_bdev_io));
    bdev_io->bdev = &vb->bdev;
    bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
    bdev_io->u.bdev.iovs = &iov;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.offset_blocks = 100;
    bdev_io->u.bdev.num_blocks = 1;
    iov.iov_base = data;
    iov.iov_len = 512;

    /* Submit write */
    wal_submit_request(qch, bdev_io);
    poll_threads();

    /* Verify: Journal write LBA should be write_pos (1) */
    CU_ASSERT(g_last_write_lba == 1);
    CU_ASSERT(g_last_write_blocks == 2); /* Header(1) + Data(1) */
    CU_ASSERT(g_last_hdr.magic == WAL_RECORD_MAGIC);
    CU_ASSERT(g_last_hdr.lba == 100);
    CU_ASSERT(g_last_hdr.seq == 1);
    
    /* Verify: write_pos advanced */
    CU_ASSERT(vb->sb.write_pos == 3);

    spdk_put_io_channel(qch);
    free(bdev_io);
    free_threads();
}

static void
test_wal_wrap_around(void)
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *qch;
    struct spdk_bdev_io *bdev_io;
    struct iovec iov;

    allocate_threads(1);
    set_thread(0);

    wal_bdev_create_disk("main", "journal", "wal0", NULL, NULL, test_cb, NULL);
    poll_threads();
    vb = TAILQ_FIRST(&g_wal);
    
    /* Симулируем конец журнала: ставим write_pos почти в конец (размер 1024) */
    vb->sb.write_pos = 1023; 
    
    qch = wal_get_io_channel(vb);
    bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct wal_bdev_io));
    bdev_io->bdev = &vb->bdev;
    bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
    bdev_io->u.bdev.iovs = &iov;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.offset_blocks = 500;
    bdev_io->u.bdev.num_blocks = 10;
    iov.iov_base = NULL;
    iov.iov_len = 10 * 512;

    wal_submit_request(qch, bdev_io);
    poll_threads();

    /* Verify: should have wrapped to LBA 1 */
    CU_ASSERT(g_last_write_lba == 1);
    CU_ASSERT(vb->sb.write_pos == 1 + 10 + 1);

    spdk_put_io_channel(qch);
    free(bdev_io);
    free_threads();
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
        CU_add_test(suite, "test_wal_write", test_wal_write) == NULL ||
        CU_add_test(suite, "test_wal_wrap_around", test_wal_wrap_around) == NULL ||
        CU_add_test(suite, "test_wal_recovery", test_wal_recovery) == NULL) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    num_failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return num_failures > 0 ? 1 : 0;
}
