#include "vbdev_wal.h"
#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/queue.h>
#include <spdk/thread.h>
#include <spdk/util.h>
#include <spdk/crc32.h>
#include <string.h>

SPDK_LOG_REGISTER_COMPONENT(wal_vbdev);

#define WAL_SUPERBLOCK_MAGIC 0x57414c00 /* "WAL\0" */
#define WAL_RECORD_MAGIC 0x52454348     /* "RECH" */
#define WAL_RECORD_TYPE_DATA 1
#define WAL_RECORD_TYPE_PAD 2

struct wal_superblock
{
    uint32_t magic;     /* WAL_SUPERBLOCK_MAGIC */
    uint32_t version;
    uint64_t write_pos;
    uint64_t head_pos;
    uint64_t next_seq;
    uint64_t
            checkpoint_seq;
    uint64_t journal_size;
    uint32_t hdr_crc;
} __attribute__((packed));

struct wal_record_header
{
    uint32_t magic;      /* WAL_RECORD_MAGIC */
    uint32_t type;
    uint64_t seq;
    uint64_t lba;
    uint32_t num_blocks;
    uint32_t rec_len;
    uint32_t flags;
    uint32_t data_crc;
    uint32_t hdr_crc;
} __attribute__((packed));

struct wal_vbdev
{
    struct spdk_bdev bdev;
    struct spdk_bdev_desc *main_desc;
    struct spdk_bdev_desc *journal_desc;
    struct spdk_bdev *main_bdev;
    struct spdk_bdev *journal_bdev;
    TAILQ_ENTRY(wal_vbdev) link;

    bool rec_in_progress;
    bool write_in_progress;
    uint64_t rec_off_blocks;
    uint32_t rec_chunk_blocks;

    struct
    {
        struct spdk_io_channel *jch;
        struct spdk_io_channel *mch;
        struct spdk_poller *poller;
        void *buf_j;
        void *buf_m;
        size_t buf_bytes;
        uint64_t copied_since_flush;
        uint32_t flush_period_blocks;
        uint32_t last_step;
        spdk_bdev_unregister_cb done_cb;
        void *done_arg;
        spdk_bdev_unregister_cb delete_cb;
        void *delete_arg;
        bool delete_pending;
        bool step_inflight;
        bool stop;
        uint64_t scanned_blocks;
    } rec;

    struct wal_superblock sb;
};

struct wal_io_channel
{
    struct spdk_io_channel *main_ch;
    struct spdk_io_channel *journal_ch;
};

enum wal_stage
{
    WAL_STAGE_NONE = 0,
    WAL_STAGE_J_PAD,
    WAL_STAGE_J_PAD_FLUSH,
    WAL_STAGE_J_WRITE,
    WAL_STAGE_J_FLUSH,
    WAL_STAGE_M_WRITE,
    WAL_STAGE_M_FLUSH,
    WAL_STAGE_SB_UPDATE,
};

struct wal_sb_update_ctx
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *ch;
    void *buf;
    struct spdk_bdev_io *orig_io;
    struct wal_superblock sb;
};

struct wal_bdev_io
{
    struct wal_io_channel *ch;
    enum wal_stage stage;
    struct iovec *iovs;
    int iovcnt;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    uint64_t journal_pos;
    struct wal_record_header hdr;
    struct wal_record_header pad_hdr;
    void *hdr_block;
    void *pad_block;
    bool owns_write_lock;
    struct iovec log_iovs[SPDK_BDEV_IO_NUM_CHILD_IOV
                          + 1];
};

static int vbdev_wal_init(void)
{
    return 0;
}

static void vbdev_wal_fini(void) {}

static void vbdev_wal_examine(struct spdk_bdev *bdev);

static int vbdev_wal_get_ctx_size(void)
{
    return sizeof(struct wal_bdev_io);
}

static void wal_superblock_update_crc(struct wal_superblock *sb)
{
    sb->hdr_crc = spdk_crc32c_update(sb,
                                     offsetof(struct wal_superblock, hdr_crc),
                                     0);
}

static void wal_superblock_init(struct wal_vbdev *vb)
{
    memset(&vb->sb, 0, sizeof(vb->sb));
    vb->sb.magic = WAL_SUPERBLOCK_MAGIC;
    vb->sb.version = 1;
    vb->sb.write_pos = 1;
    vb->sb.head_pos = 1;
    vb->sb.next_seq = 1;
    vb->sb.checkpoint_seq = 0;
    vb->sb.journal_size = spdk_bdev_get_num_blocks(vb->journal_bdev);
    wal_superblock_update_crc(&vb->sb);
}

static bool wal_superblock_valid(struct wal_vbdev *vb,
                                 const struct wal_superblock *sb)
{
    uint32_t crc;

    if (sb->magic != WAL_SUPERBLOCK_MAGIC || sb->version != 1)
    {
        return false;
    }
    if (sb->journal_size != spdk_bdev_get_num_blocks(vb->journal_bdev))
    {
        return false;
    }
    if (sb->journal_size < 4 || sb->write_pos == 0 || sb->head_pos == 0 ||
        sb->write_pos >= sb->journal_size || sb->head_pos >= sb->journal_size)
    {
        return false;
    }
    if (sb->next_seq == 0 || sb->checkpoint_seq >= sb->next_seq)
    {
        return false;
    }

    crc = spdk_crc32c_update(sb,
                             offsetof(struct wal_superblock, hdr_crc),
                             0);
    return sb->hdr_crc == crc;
}

static bool wal_superblock_empty(const struct wal_superblock *sb)
{
    const uint8_t *data = (const uint8_t *)sb;

    for (size_t i = 0; i < sizeof(*sb); i++)
    {
        if (data[i] != 0)
        {
            return false;
        }
    }

    return true;
}

static uint64_t wal_record_next_pos(struct wal_vbdev *vb, uint64_t pos,
                                    uint64_t blocks)
{
    pos += blocks;
    return pos >= vb->sb.journal_size ? 1 : pos;
}

static bool wal_journal_has_space(struct wal_vbdev *vb, uint64_t needed)
{
    uint64_t tail = vb->sb.write_pos;
    uint64_t head = vb->sb.head_pos;
    uint64_t usable = vb->sb.journal_size - 1;
    uint64_t next;

    if (needed == 0 || needed >= usable)
    {
        return false;
    }

    if (tail == head)
    {
        return true;
    }

    if (tail < head)
    {
        return tail + needed < head;
    }

    if (tail + needed < vb->sb.journal_size)
    {
        return true;
    }
    if (tail + needed == vb->sb.journal_size)
    {
        return head > 1;
    }

    /* After a wrap, the new write position must stay before head_pos. */
    next = needed;
    return next < head;
}

static bool wal_iovs_len(struct iovec *iovs, int iovcnt, uint64_t *total)
{
    uint64_t len = 0;

    for (int i = 0; i < iovcnt; i++)
    {
        if (iovs[i].iov_len > UINT64_MAX - len)
        {
            return false;
        }
        len += iovs[i].iov_len;
    }

    *total = len;
    return true;
}

static uint32_t wal_crc32c_iov_update_len(struct iovec *iovs, int iovcnt,
                                          uint64_t len, uint32_t crc)
{
    for (int i = 0; i < iovcnt && len > 0; i++)
    {
        size_t chunk = spdk_min((uint64_t)iovs[i].iov_len, len);
        crc = spdk_crc32c_update(iovs[i].iov_base, chunk, crc);
        len -= chunk;
    }

    return crc;
}

static void wal_bdev_io_cleanup(struct spdk_bdev_io *orig)
{
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;

    if (wio->hdr_block)
    {
        spdk_dma_free(wio->hdr_block);
        wio->hdr_block = NULL;
    }
    if (wio->pad_block)
    {
        spdk_dma_free(wio->pad_block);
        wio->pad_block = NULL;
    }
    if (wio->owns_write_lock)
    {
        __atomic_clear(&vb->write_in_progress, __ATOMIC_RELEASE);
        wio->owns_write_lock = false;
    }
}

static void wal_recover_step(struct wal_vbdev *vb);
static int wal_recover_poll(void *cb_arg);

static void wal_journal_write_done(struct spdk_bdev_io *child_io, bool success, void *cb_arg);
static void wal_user_journal_flush_done(struct spdk_bdev_io *child_io, bool success,
                                        void *cb_arg);
static void wal_main_reset_done(struct spdk_bdev_io *child_io, bool success, void *cb_arg);

static void wal_recover_read_j_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg);

static void wal_recover_read_m_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg);

static void wal_recover_write_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg);
static void wal_recover_sb_write_done(struct spdk_bdev_io *child_io,
                                      bool success,
                                      void *cb_arg);
static void wal_recover_flush_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg);
static void wal_recover_sb_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg);
static void wal_recover_finish_sb_write_done(struct spdk_bdev_io *child_io,
                                             bool success,
                                             void *cb_arg);
static void wal_recover_finish_sb_flush_done(struct spdk_bdev_io *child_io,
                                             bool success,
                                             void *cb_arg);

static void wal_recover_finish(struct wal_vbdev *vb, bool ok);
static void wal_recover_complete(struct wal_vbdev *vb, bool ok);

static struct spdk_bdev_module wal_bdev_if = {
        .name = "wal",
        .module_init = vbdev_wal_init,
        .module_fini = vbdev_wal_fini,
        .examine_config = vbdev_wal_examine,
        .get_ctx_size = vbdev_wal_get_ctx_size,
};

static void vbdev_wal_examine(struct spdk_bdev *bdev)
{
    spdk_bdev_module_examine_done(&wal_bdev_if);
}

SPDK_BDEV_MODULE_REGISTER(wal, &wal_bdev_if)

TAILQ_HEAD(wal_vbdev_list, wal_vbdev) g_wal = TAILQ_HEAD_INITIALIZER(g_wal);

static int wal_io_channel_create_cb(void *io_device, void *ctx_buf)
{
    struct wal_vbdev *vbdev = io_device;
    struct wal_io_channel *ch = ctx_buf;
    ch->main_ch = spdk_bdev_get_io_channel(vbdev->main_desc);
    if (!ch->main_ch)
    {
        return -ENOMEM;
    }
    ch->journal_ch = spdk_bdev_get_io_channel(vbdev->journal_desc);
    if (!ch->journal_ch)
    {
        spdk_put_io_channel(ch->main_ch);
        ch->main_ch = NULL;
        return -ENOMEM;
    }
    return 0;
}

static void wal_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
    struct wal_io_channel *ch = ctx_buf;
    if (ch->journal_ch)
    {
        spdk_put_io_channel(ch->journal_ch);
        ch->journal_ch = NULL;
    }
    if (ch->main_ch)
    {
        spdk_put_io_channel(ch->main_ch);
        ch->main_ch = NULL;
    }
}

static void wal_complete(struct spdk_bdev_io *orig, bool success)
{
    wal_bdev_io_cleanup(orig);
    spdk_bdev_io_complete(orig,
                          success ? SPDK_BDEV_IO_STATUS_SUCCESS
                                  : SPDK_BDEV_IO_STATUS_FAILED);
}

static void wal_sb_update_complete(struct wal_sb_update_ctx *ctx, bool success)
{
    struct spdk_bdev_io *orig = ctx->orig_io;

    spdk_put_io_channel(ctx->ch);
    spdk_dma_free(ctx->buf);

    if (success)
    {
        ctx->vb->sb = ctx->sb;
    }
    wal_complete(orig, success);
    free(ctx);
}

static void wal_sb_update_flush_done(struct spdk_bdev_io *child_io,
                                     bool success,
                                     void *cb_arg)
{
    struct wal_sb_update_ctx *ctx = cb_arg;

    spdk_bdev_free_io(child_io);
    wal_sb_update_complete(ctx, success);
}

static void wal_sb_update_write_done(struct spdk_bdev_io *child_io,
                                     bool success,
                                     void *cb_arg)
{
    struct wal_sb_update_ctx *ctx = cb_arg;
    int rc;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_sb_update_complete(ctx, false);
        return;
    }

    rc = spdk_bdev_flush_blocks(ctx->vb->journal_desc,
                                ctx->ch,
                                0,
                                1,
                                wal_sb_update_flush_done,
                                ctx);
    if (rc != 0)
    {
        wal_sb_update_complete(ctx, false);
    }
}

static void wal_main_flush_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    int rc;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    struct wal_sb_update_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        wal_complete(orig, false);
        return;
    }

    ctx->vb = vb;
    ctx->orig_io = orig;
    ctx->buf = spdk_dma_zmalloc(vb->bdev.blocklen,
                                spdk_bdev_get_buf_align(vb->journal_bdev),
                                NULL);
    if (!ctx->buf)
    {
        free(ctx);
        wal_complete(orig, false);
        return;
    }

    ctx->sb = vb->sb;
    ctx->sb.checkpoint_seq = wio->hdr.seq;
    ctx->sb.head_pos = ctx->sb.write_pos;
    wal_superblock_update_crc(&ctx->sb);
    memcpy(ctx->buf, &ctx->sb, sizeof(ctx->sb));
    wio->stage = WAL_STAGE_SB_UPDATE;

    ctx->ch = spdk_bdev_get_io_channel(vb->journal_desc);
    if (!ctx->ch)
    {
        spdk_dma_free(ctx->buf);
        free(ctx);
        wal_complete(orig, false);
        return;
    }

    rc = spdk_bdev_write_blocks(
            vb->journal_desc, ctx->ch, ctx->buf, 0, 1, wal_sb_update_write_done, ctx);
    if (rc != 0)
    {
        spdk_put_io_channel(ctx->ch);
        spdk_dma_free(ctx->buf);
        free(ctx);
        wal_complete(orig, false);
    }
}

static void wal_main_write_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_M_FLUSH;
    rc = spdk_bdev_flush_blocks(vb->main_desc,
                                wio->ch->main_ch,
                                wio->offset_blocks,
                                wio->num_blocks,
                                wal_main_flush_done,
                                orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_journal_flush_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_M_WRITE;
    rc = spdk_bdev_writev_blocks(vb->main_desc,
                                 wio->ch->main_ch,
                                 wio->iovs,
                                 wio->iovcnt,
                                 wio->offset_blocks,
                                 wio->num_blocks,
                                 wal_main_write_done,
                                 orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_user_main_flush_done(struct spdk_bdev_io *child_io,
                                     bool success,
                                     void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;

    spdk_bdev_free_io(child_io);
    wal_complete(orig, success);
}

static void wal_user_journal_flush_done(struct spdk_bdev_io *child_io,
                                        bool success,
                                        void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_M_FLUSH;
    rc = spdk_bdev_flush_blocks(vb->main_desc,
                                wio->ch->main_ch,
                                wio->offset_blocks,
                                wio->num_blocks,
                                wal_user_main_flush_done,
                                orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_journal_pad_flush_done(struct spdk_bdev_io *child_io,
                                       bool success,
                                       void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    struct wal_io_channel *wch = wio->ch;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_J_WRITE;
    vb->sb.write_pos = 1;
    wio->journal_pos = vb->sb.write_pos;

    wio->log_iovs[0].iov_base = wio->hdr_block;
    wio->log_iovs[0].iov_len = vb->bdev.blocklen;
    for (int i = 0; i < wio->iovcnt; i++)
    {
        wio->log_iovs[i + 1] = wio->iovs[i];
    }

    rc = spdk_bdev_writev_blocks(vb->journal_desc,
                                 wch->journal_ch,
                                 wio->log_iovs,
                                 wio->iovcnt + 1,
                                 wio->journal_pos,
                                 wio->num_blocks + 1,
                                 wal_journal_write_done,
                                 orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_journal_pad_done(struct spdk_bdev_io *child_io,
                                 bool success,
                                 void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_J_PAD_FLUSH;
    rc = spdk_bdev_flush_blocks(vb->journal_desc,
                                wio->ch->journal_ch,
                                0,
                                spdk_bdev_get_num_blocks(vb->journal_bdev),
                                wal_journal_pad_flush_done,
                                orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_journal_write_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    uint64_t record_blocks = wio->num_blocks + 1;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    vb->sb.write_pos = wal_record_next_pos(vb, wio->journal_pos, record_blocks);
    wio->stage = WAL_STAGE_J_FLUSH;
    rc = spdk_bdev_flush_blocks(vb->journal_desc,
                                wio->ch->journal_ch,
                                0,
                                spdk_bdev_get_num_blocks(vb->journal_bdev),
                                wal_journal_flush_done,
                                orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_passthru_done(struct spdk_bdev_io *child_io,
                              bool success,
                              void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    spdk_bdev_free_io(child_io);
    wal_complete(orig, success);
}

static void wal_journal_reset_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;

    spdk_bdev_free_io(child_io);
    wal_complete(orig, success);
}

static void wal_main_reset_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;
    int rc;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    rc = spdk_bdev_reset(vb->journal_desc,
                         wio->ch->journal_ch,
                         wal_journal_reset_done,
                         orig);
    if (rc != 0)
    {
        wal_complete(orig, false);
    }
}

static void wal_submit_request(struct spdk_io_channel *ch,
                               struct spdk_bdev_io *bdev_io)
{
    struct wal_vbdev *vb = (struct wal_vbdev *)bdev_io->bdev->ctxt;

    struct wal_io_channel *wch = spdk_io_channel_get_ctx(ch);

    struct wal_bdev_io *wio = (struct wal_bdev_io *)bdev_io->driver_ctx;
    memset(wio, 0, sizeof(*wio));
    wio->ch = wch;
    wio->offset_blocks = bdev_io->u.bdev.offset_blocks;
    wio->num_blocks = bdev_io->u.bdev.num_blocks;

    switch (bdev_io->type)
    {
    case SPDK_BDEV_IO_TYPE_READ:
    {
        struct iovec *iovs = bdev_io->u.bdev.iovs;
        int iovcnt = bdev_io->u.bdev.iovcnt;
        int rc;

        if (__atomic_load_n(&vb->rec_in_progress, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&vb->write_in_progress, __ATOMIC_ACQUIRE))
        {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        rc = spdk_bdev_readv_blocks(vb->main_desc,
                                    wch->main_ch,
                                    iovs,
                                    iovcnt,
                                    wio->offset_blocks,
                                    wio->num_blocks,
                                    wal_passthru_done,
                                    bdev_io);
        if (rc != 0)
        {
            wal_complete(bdev_io, false);
        }
        break;
    }

    case SPDK_BDEV_IO_TYPE_WRITE:
    {
        uint64_t data_bytes;
        uint64_t needed;
        uint64_t iov_bytes;
        size_t align;
        int rc;

        if (__atomic_load_n(&vb->rec_in_progress, __ATOMIC_ACQUIRE))
        {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        if (__atomic_test_and_set(&vb->write_in_progress, __ATOMIC_ACQUIRE))
        {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }
        wio->owns_write_lock = true;

        wio->iovs = bdev_io->u.bdev.iovs;
        wio->iovcnt = bdev_io->u.bdev.iovcnt;
        wio->stage = WAL_STAGE_J_WRITE;

        if (wio->iovcnt < 1 || wio->iovcnt > SPDK_BDEV_IO_NUM_CHILD_IOV ||
            wio->num_blocks == 0 || wio->num_blocks > vb->rec_chunk_blocks)
        {
            wal_complete(bdev_io, false);
            return;
        }

        if (wio->num_blocks > UINT64_MAX / vb->bdev.blocklen)
        {
            wal_complete(bdev_io, false);
            return;
        }

        data_bytes = wio->num_blocks * vb->bdev.blocklen;
        if (data_bytes > UINT32_MAX - sizeof(struct wal_record_header))
        {
            wal_complete(bdev_io, false);
            return;
        }

        if (wio->offset_blocks + wio->num_blocks < wio->offset_blocks ||
            wio->offset_blocks + wio->num_blocks > vb->bdev.blockcnt)
        {
            wal_complete(bdev_io, false);
            return;
        }

        if (!wal_iovs_len(wio->iovs, wio->iovcnt, &iov_bytes))
        {
            wal_complete(bdev_io, false);
            return;
        }
        if (iov_bytes != data_bytes)
        {
            wal_complete(bdev_io, false);
            return;
        }

        needed = wio->num_blocks + 1;
        if (vb->sb.write_pos + needed > vb->sb.journal_size)
        {
            needed++;
        }

        if (!wal_journal_has_space(vb, needed))
        {
            SPDK_ERRLOG("wal: journal overflow (Tail reached Head)\n");
            wal_complete(bdev_io, false);
            return;
        }

        align = spdk_bdev_get_buf_align(vb->journal_bdev);
        wio->hdr_block = spdk_dma_zmalloc(vb->bdev.blocklen, align, NULL);
        if (!wio->hdr_block)
        {
            wal_complete(bdev_io, false);
            return;
        }

        if (vb->sb.write_pos + wio->num_blocks + 1 > vb->sb.journal_size)
        {
            wio->stage = WAL_STAGE_J_PAD;

            wio->pad_block = spdk_dma_zmalloc(vb->bdev.blocklen, align, NULL);
            if (!wio->pad_block)
            {
                wal_complete(bdev_io, false);
                return;
            }

            wio->pad_hdr.magic = WAL_RECORD_MAGIC;
            wio->pad_hdr.type = WAL_RECORD_TYPE_PAD;
            wio->pad_hdr.seq = vb->sb.next_seq++;
            wio->pad_hdr.lba = 0;
            wio->pad_hdr.num_blocks = 0;
            wio->pad_hdr.rec_len = sizeof(struct wal_record_header);
            wio->pad_hdr.flags = 1;
            wio->pad_hdr.data_crc = 0;
            wio->pad_hdr.hdr_crc =
                    spdk_crc32c_update(&wio->pad_hdr,
                                       offsetof(struct wal_record_header, hdr_crc),
                                       0);
            memcpy(wio->pad_block, &wio->pad_hdr, sizeof(wio->pad_hdr));

            wio->hdr.magic = WAL_RECORD_MAGIC;
            wio->hdr.type = WAL_RECORD_TYPE_DATA;
            wio->hdr.seq = vb->sb.next_seq++;
            wio->hdr.lba = wio->offset_blocks;
            wio->hdr.num_blocks = (uint32_t)wio->num_blocks;
            wio->hdr.rec_len = sizeof(struct wal_record_header) + data_bytes;
            wio->hdr.flags = 1;
            wio->hdr.data_crc = wal_crc32c_iov_update_len(wio->iovs, wio->iovcnt, data_bytes, 0);
            wio->hdr.hdr_crc = spdk_crc32c_update(&wio->hdr, offsetof(struct wal_record_header, hdr_crc), 0);
            memcpy(wio->hdr_block, &wio->hdr, sizeof(wio->hdr));

            wio->log_iovs[0].iov_base = wio->pad_block;
            wio->log_iovs[0].iov_len = vb->bdev.blocklen;

            wio->journal_pos = vb->sb.write_pos;

            rc = spdk_bdev_writev_blocks(
                vb->journal_desc,
                wch->journal_ch,
                wio->log_iovs,
                1,
                wio->journal_pos,
                1,
                wal_journal_pad_done,
                bdev_io);
            if (rc != 0)
            {
                wal_complete(bdev_io, false);
            }
            return;
        }

        wio->hdr.magic = WAL_RECORD_MAGIC;
        wio->hdr.type = WAL_RECORD_TYPE_DATA;
        wio->hdr.seq = vb->sb.next_seq++;
        wio->hdr.lba = wio->offset_blocks;
        wio->hdr.num_blocks = (uint32_t)wio->num_blocks;
        wio->hdr.rec_len = sizeof(struct wal_record_header) + data_bytes;
        wio->hdr.flags = 1;
        wio->hdr.data_crc = wal_crc32c_iov_update_len(wio->iovs, wio->iovcnt, data_bytes, 0);
        wio->hdr.hdr_crc = spdk_crc32c_update(&wio->hdr, offsetof(struct wal_record_header, hdr_crc), 0);
        memcpy(wio->hdr_block, &wio->hdr, sizeof(wio->hdr));

        wio->journal_pos = vb->sb.write_pos;

        wio->log_iovs[0].iov_base = wio->hdr_block;
        wio->log_iovs[0].iov_len = vb->bdev.blocklen;
        for (int i = 0; i < wio->iovcnt; i++)
        {
            wio->log_iovs[i + 1] = wio->iovs[i];
        }

        rc = spdk_bdev_writev_blocks(
                vb->journal_desc,
                wch->journal_ch,
                wio->log_iovs,
                wio->iovcnt + 1,
                vb->sb.write_pos,
                wio->num_blocks + 1,
                wal_journal_write_done,
                bdev_io);
        if (rc != 0)
        {
            wal_complete(bdev_io, false);
        }
        break;
    }

    case SPDK_BDEV_IO_TYPE_FLUSH:
    {
        int rc;

        if (__atomic_load_n(&vb->rec_in_progress, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&vb->write_in_progress, __ATOMIC_ACQUIRE))
        {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        wio->stage = WAL_STAGE_J_FLUSH;
        rc = spdk_bdev_flush_blocks(vb->journal_desc,
                                    wch->journal_ch,
                                    0,
                                    spdk_bdev_get_num_blocks(vb->journal_bdev),
                                    wal_user_journal_flush_done,
                                    bdev_io);
        if (rc != 0)
        {
            wal_complete(bdev_io, false);
        }
        break;
    }

    case SPDK_BDEV_IO_TYPE_RESET:
    {
        int rc;

        if (__atomic_load_n(&vb->rec_in_progress, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&vb->write_in_progress, __ATOMIC_ACQUIRE))
        {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        rc = spdk_bdev_reset(vb->main_desc,
                             wch->main_ch,
                             wal_main_reset_done,
                             bdev_io);
        if (rc != 0)
        {
            wal_complete(bdev_io, false);
        }
        break;
    }

    default:
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}

static bool wal_io_type_supported(void *ctx, enum spdk_bdev_io_type t)
{
    struct wal_vbdev *vb = ctx;

    switch (t)
    {
    case SPDK_BDEV_IO_TYPE_READ:
        return spdk_bdev_io_type_supported(vb->main_bdev, t);
    case SPDK_BDEV_IO_TYPE_WRITE:
        return spdk_bdev_io_type_supported(vb->main_bdev, t) &&
               spdk_bdev_io_type_supported(vb->journal_bdev, t);
    case SPDK_BDEV_IO_TYPE_FLUSH:
        return spdk_bdev_io_type_supported(vb->main_bdev, t) &&
               spdk_bdev_io_type_supported(vb->journal_bdev, t);
    case SPDK_BDEV_IO_TYPE_RESET:
        return spdk_bdev_io_type_supported(vb->main_bdev, t) &&
               spdk_bdev_io_type_supported(vb->journal_bdev, t);
    default:
        return false;
    }
}

static struct spdk_io_channel *wal_get_io_channel(void *ctx)
{
    struct wal_vbdev *vbdev = ctx;
    return spdk_get_io_channel(vbdev);
}

static int wal_destruct(void *ctx)
{
    struct wal_vbdev *vb = ctx;

    spdk_io_device_unregister(vb, NULL);

    if (vb->main_desc)
    {
        spdk_bdev_close(vb->main_desc);
        vb->main_desc = NULL;
    }
    if (vb->journal_desc)
    {
        spdk_bdev_close(vb->journal_desc);
        vb->journal_desc = NULL;
    }
    if (vb->rec.poller)
    {
        spdk_poller_unregister(&vb->rec.poller);
        vb->rec.poller = NULL;
    }
    if (vb->rec.jch)
    {
        spdk_put_io_channel(vb->rec.jch);
        vb->rec.jch = NULL;
    }
    if (vb->rec.mch)
    {
        spdk_put_io_channel(vb->rec.mch);
        vb->rec.mch = NULL;
    }
    if (vb->rec.buf_j)
    {
        spdk_dma_free(vb->rec.buf_j);
        vb->rec.buf_j = NULL;
    }
    if (vb->rec.buf_m)
    {
        spdk_dma_free(vb->rec.buf_m);
        vb->rec.buf_m = NULL;
    }

    free(vb->bdev.name);
    free(vb);
    return 0;
}

static const struct spdk_bdev_fn_table g_wal_fn_table = {
        .destruct = wal_destruct,
        .submit_request = wal_submit_request,
        .io_type_supported = wal_io_type_supported,
        .get_io_channel = wal_get_io_channel,
};

static void wal_base_bdev_event_cb(enum spdk_bdev_event_type type,
                                   struct spdk_bdev *bdev,
                                   void *arg)
{
    SPDK_NOTICELOG("wal: base bdev event %d on %s\n",
                   type,
                   spdk_bdev_get_name(bdev));
}

static struct wal_vbdev *wal_find_by_name(const char *name)
{
    struct wal_vbdev *vb;
    TAILQ_FOREACH(vb, &g_wal, link)
    {
        if (strcmp(vb->bdev.name, name) == 0)
        {
            return vb;
        }
    }
    return NULL;
}

static void wal_unregister(struct wal_vbdev *vb,
                           spdk_bdev_unregister_cb cb_fn,
                           void *cb_arg)
{
    TAILQ_REMOVE(&g_wal, vb, link);
    spdk_bdev_unregister(&vb->bdev, cb_fn, cb_arg);
}

struct wal_init_ctx
{
    struct wal_vbdev *vb;
    void *buf;
    wal_bdev_create_cb cb_fn;
    void *cb_arg;
    struct spdk_io_channel *ch;
};

static void wal_init_free_vb(struct wal_vbdev *vb)
{
    spdk_io_device_unregister(vb, NULL);
    if (vb->journal_desc)
    {
        spdk_bdev_close(vb->journal_desc);
        vb->journal_desc = NULL;
    }
    if (vb->main_desc)
    {
        spdk_bdev_close(vb->main_desc);
        vb->main_desc = NULL;
    }
    free(vb->bdev.name);
    free(vb);
}

static void wal_init_complete(struct wal_init_ctx *ctx, int rc)
{
    struct wal_vbdev *vb = ctx->vb;

    if (ctx->ch)
    {
        spdk_put_io_channel(ctx->ch);
    }
    if (ctx->buf)
    {
        spdk_dma_free(ctx->buf);
    }

    if (rc == 0)
    {
        rc = spdk_bdev_register(&vb->bdev);
        if (rc != 0)
        {
            SPDK_ERRLOG("wal: spdk_bdev_register failed: %d\n", rc);
        }
    }

    if (rc != 0)
    {
        TAILQ_REMOVE(&g_wal, vb, link);
        wal_init_free_vb(vb);
    }

    if (ctx->cb_fn)
    {
        ctx->cb_fn(ctx->cb_arg, rc);
    }
    free(ctx);
}

static void wal_sb_write_done(struct spdk_bdev_io *bdev_io,
                              bool success,
                              void *cb_arg)
{
    struct wal_init_ctx *ctx = cb_arg;

    spdk_bdev_free_io(bdev_io);
    wal_init_complete(ctx, success ? 0 : -EIO);
}

static void wal_sb_read_done(struct spdk_bdev_io *bdev_io,
                             bool success,
                             void *cb_arg)
{
    struct wal_init_ctx *ctx = cb_arg;
    struct wal_vbdev *vb = ctx->vb;
    struct wal_superblock *disk_sb = (struct wal_superblock *)ctx->buf;
    int rc;

    spdk_bdev_free_io(bdev_io);

    if (!success)
    {
        wal_init_complete(ctx, -EIO);
        return;
    }

    if (success && wal_superblock_valid(vb, disk_sb))
    {
        memcpy(&vb->sb, disk_sb, sizeof(vb->sb));
        wal_init_complete(ctx, 0);
        return;
    }
    if (!wal_superblock_empty(disk_sb))
    {
        SPDK_ERRLOG("wal: invalid non-empty superblock on journal\n");
        wal_init_complete(ctx, -EIO);
        return;
    }

    wal_superblock_init(vb);
    memset(ctx->buf, 0, vb->bdev.blocklen);
    memcpy(ctx->buf, &vb->sb, sizeof(vb->sb));

    rc = spdk_bdev_write_blocks(vb->journal_desc,
                                ctx->ch,
                                ctx->buf,
                                0,
                                1,
                                wal_sb_write_done,
                                ctx);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: failed to write superblock: %d\n", rc);
        wal_init_complete(ctx, rc);
    }
}

int wal_bdev_create_disk(char *main_bdev_name,
                         char *journal_bdev_name,
                         char *name,
                         uint32_t *block_sz,
                         uint64_t *size_mb,
                         wal_bdev_create_cb cb_fn,
                         void *cb_arg)
{
    int rc = 0;
    struct wal_vbdev *vb = NULL;
    struct spdk_bdev_desc *main_desc = NULL, *journal_desc = NULL;
    struct spdk_bdev *main_bdev = NULL, *journal_bdev = NULL;
    struct wal_init_ctx *ctx = NULL;

    if (!main_bdev_name || !journal_bdev_name || !name)
    {
        return -EINVAL;
    }
    if (strcmp(main_bdev_name, journal_bdev_name) == 0)
    {
        SPDK_ERRLOG("WAL: main and journal must be different bdevs\n");
        return -EINVAL;
    }
    if (wal_find_by_name(name))
    {
        return -EEXIST;
    }

    rc = spdk_bdev_open_ext(main_bdev_name,
                            true,
                            wal_base_bdev_event_cb,
                            NULL,
                            &main_desc);
    if (rc != 0)
    {
        SPDK_ERRLOG("WAL: failed to open main bdev '%s': %d\n",
                    main_bdev_name,
                    rc);
        goto err;
    }
    main_bdev = spdk_bdev_desc_get_bdev(main_desc);

    rc = spdk_bdev_open_ext(journal_bdev_name,
                            true,
                            wal_base_bdev_event_cb,
                            NULL,
                            &journal_desc);
    if (rc != 0)
    {
        SPDK_ERRLOG("WAL: failed to open journal bdev '%s': %d\n",
                    journal_bdev_name,
                    rc);
        goto err;
    }
    journal_bdev = spdk_bdev_desc_get_bdev(journal_desc);

    if (spdk_bdev_get_block_size(main_bdev)
        != spdk_bdev_get_block_size(journal_bdev))
    {
        SPDK_ERRLOG("WAL: block sizes mismatch: %u vs %u\n",
                    spdk_bdev_get_block_size(main_bdev),
                    spdk_bdev_get_block_size(journal_bdev));
        rc = -EINVAL;
        goto err;
    }
    if (spdk_bdev_get_num_blocks(journal_bdev) < 4)
    {
        SPDK_ERRLOG("WAL: journal bdev is too small\n");
        rc = -EINVAL;
        goto err;
    }

    vb = calloc(1, sizeof(*vb));
    if (!vb)
    {
        rc = -ENOMEM;
        goto err;
    }

    vb->main_desc = main_desc;
    vb->journal_desc = journal_desc;
    vb->main_bdev = main_bdev;
    vb->journal_bdev = journal_bdev;

    wal_superblock_init(vb);

    vb->bdev.name = strdup(name);
    if (!vb->bdev.name)
    {
        rc = -ENOMEM;
        goto err;
    }
    vb->bdev.product_name = "WAL";
    vb->bdev.module = &wal_bdev_if;
    vb->bdev.fn_table = &g_wal_fn_table;
    vb->bdev.ctxt = vb;

    vb->bdev.blocklen = spdk_bdev_get_block_size(main_bdev);
    vb->bdev.blockcnt = spdk_bdev_get_num_blocks(main_bdev);
    vb->rec_chunk_blocks = 1024;

    size_t align = spdk_max(spdk_bdev_get_buf_align(main_bdev),
                       spdk_bdev_get_buf_align(journal_bdev));
    vb->bdev.required_alignment =
            (align > 1) ? spdk_u32log2((uint32_t)align) : 0;

    spdk_io_device_register(vb,
                            wal_io_channel_create_cb,
                            wal_io_channel_destroy_cb,
                            sizeof(struct wal_io_channel),
                            vb->bdev.name);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        rc = -ENOMEM;
        goto err_unregister_io_dev;
    }

    ctx->vb = vb;
    ctx->cb_fn = cb_fn;
    ctx->cb_arg = cb_arg;
    ctx->buf = spdk_dma_zmalloc(vb->bdev.blocklen,
                                spdk_bdev_get_buf_align(vb->journal_bdev),
                                NULL);
    if (!ctx->buf)
    {
        rc = -ENOMEM;
        goto err_ctx;
    }

    ctx->ch = spdk_bdev_get_io_channel(vb->journal_desc);
    if (!ctx->ch)
    {
        rc = -ENOMEM;
        goto err_buf;
    }

    TAILQ_INSERT_TAIL(&g_wal, vb, link);

    rc = spdk_bdev_read_blocks(
            vb->journal_desc, ctx->ch, ctx->buf, 0, 1, wal_sb_read_done, ctx);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: failed to read superblock: %d\n", rc);
        TAILQ_REMOVE(&g_wal, vb, link);
        goto err_ch;
    }

    if (block_sz)
        *block_sz = vb->bdev.blocklen;
    if (size_mb)
        *size_mb = (vb->bdev.blockcnt * vb->bdev.blocklen) / (1024 * 1024);

    SPDK_NOTICELOG("wal: init disk '%s' (async sb write started)\n", name);
    return 0;

err_ch:
    spdk_put_io_channel(ctx->ch);
err_buf:
    spdk_dma_free(ctx->buf);
err_ctx:
    free(ctx);
err_unregister_io_dev:
    spdk_io_device_unregister(vb, NULL);
err:
    if (vb)
    {
        free(vb->bdev.name);
        free(vb);
    }
    if (journal_desc)
    {
        spdk_bdev_close(journal_desc);
    }
    if (main_desc)
    {
        spdk_bdev_close(main_desc);
    }
    return rc;
}

int wal_bdev_delete_disk(char *name,
                         spdk_bdev_unregister_cb cb_fn,
                         void *cb_arg)
{
    struct wal_vbdev *vb = wal_find_by_name(name);
    if (!vb)
    {
        return -ENODEV;
    }
    if (__atomic_load_n(&vb->rec_in_progress, __ATOMIC_SEQ_CST))
    {
        if (vb->rec.delete_pending)
        {
            return -EBUSY;
        }
        vb->rec.stop = true;
        __atomic_store_n(&vb->rec.delete_pending, true, __ATOMIC_SEQ_CST);
        vb->rec.delete_cb = cb_fn;
        vb->rec.delete_arg = cb_arg;
        return 0;
    }
    if (__atomic_load_n(&vb->write_in_progress, __ATOMIC_SEQ_CST))
    {
        return -EBUSY;
    }

    wal_unregister(vb, cb_fn, cb_arg);
    return 0;
}

int wal_bdev_recover(const char *name,
                     spdk_bdev_unregister_cb cb_fn,
                     void *cb_arg)
{
    int rc = 0;
    struct wal_vbdev *vb = wal_find_by_name(name);
    if (!vb)
        return -ENODEV;
    if (__atomic_test_and_set(&vb->rec_in_progress, __ATOMIC_ACQUIRE))
        return -EBUSY;

    vb->rec_off_blocks = 0;
    vb->rec.done_cb = cb_fn;
    vb->rec.done_arg = cb_arg;
    vb->rec.stop = false;
    vb->rec.step_inflight = false;
    vb->rec.last_step = 0;
    vb->rec.scanned_blocks = 0;
    vb->rec.delete_cb = NULL;
    vb->rec.delete_arg = NULL;
    vb->rec.delete_pending = false;

    vb->rec.jch = spdk_bdev_get_io_channel(vb->journal_desc);
    vb->rec.mch = spdk_bdev_get_io_channel(vb->main_desc);
    if (!vb->rec.jch || !vb->rec.mch)
    {
        rc = -ENOMEM;
        goto err_ch;
    }

    size_t bl = vb->bdev.blocklen;
    size_t need = (size_t)vb->rec_chunk_blocks * bl;
    size_t align = spdk_max(spdk_bdev_get_buf_align(vb->main_bdev),
                       spdk_bdev_get_buf_align(vb->journal_bdev));
    if (!vb->rec.buf_j)
        vb->rec.buf_j = spdk_dma_zmalloc(need, align, NULL);
    if (!vb->rec.buf_m)
        vb->rec.buf_m = spdk_dma_zmalloc(need, align, NULL);
    if (!vb->rec.buf_j || !vb->rec.buf_m)
    {
        rc = -ENOMEM;
        goto err_buf;
    }
    vb->rec.buf_bytes = need;

    vb->rec.poller = spdk_poller_register(wal_recover_poll, vb, 0);
    if (!vb->rec.poller)
    {
        rc = -ENOMEM;
        goto err_buf;
    }
    return 0;

err_buf:
    if (vb->rec.buf_j)
    {
        spdk_dma_free(vb->rec.buf_j);
        vb->rec.buf_j = NULL;
    }
    if (vb->rec.buf_m)
    {
        spdk_dma_free(vb->rec.buf_m);
        vb->rec.buf_m = NULL;
    }
err_ch:
    if (vb->rec.jch)
    {
        spdk_put_io_channel(vb->rec.jch);
        vb->rec.jch = NULL;
    }
    if (vb->rec.mch)
    {
        spdk_put_io_channel(vb->rec.mch);
        vb->rec.mch = NULL;
    }
    __atomic_clear(&vb->rec_in_progress, __ATOMIC_RELEASE);
    vb->rec.stop = false;
    vb->rec.step_inflight = false;
    vb->rec.done_cb = NULL;
    vb->rec.done_arg = NULL;
    vb->rec.delete_pending = false;
    return rc;
}

static int wal_recover_poll(void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;

    if (!vb->rec_in_progress)
    {
        return SPDK_POLLER_IDLE;
    }

    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return SPDK_POLLER_IDLE;
    }

    if (vb->rec.step_inflight)
    {
        return SPDK_POLLER_BUSY;
    }

    vb->rec.step_inflight = true;
    wal_recover_step(vb);
    return vb->rec_in_progress ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void wal_recover_finish(struct wal_vbdev *vb, bool ok)
{
    int rc;

    if (vb->rec.poller)
    {
        spdk_poller_unregister(&vb->rec.poller);
        vb->rec.poller = NULL;
    }

    if (!ok)
    {
        wal_recover_complete(vb, false);
        return;
    }

    if (!vb->rec.jch || !vb->rec.buf_j)
    {
        wal_recover_complete(vb, false);
        return;
    }

    vb->sb.write_pos = vb->sb.head_pos;
    wal_superblock_update_crc(&vb->sb);
    memset(vb->rec.buf_j, 0, vb->bdev.blocklen);
    memcpy(vb->rec.buf_j, &vb->sb, sizeof(vb->sb));

    rc = spdk_bdev_write_blocks(vb->journal_desc,
                                vb->rec.jch,
                                vb->rec.buf_j,
                                0,
                                1,
                                wal_recover_finish_sb_write_done,
                                vb);
    if (rc != 0)
    {
        wal_recover_complete(vb, false);
    }
}

static void wal_recover_finish_sb_write_done(struct spdk_bdev_io *child_io,
                                             bool success,
                                             void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;
    int rc;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_complete(vb, false);
        return;
    }

    rc = spdk_bdev_flush_blocks(vb->journal_desc,
                                vb->rec.jch,
                                0,
                                1,
                                wal_recover_finish_sb_flush_done,
                                vb);
    if (rc != 0)
    {
        wal_recover_complete(vb, false);
    }
}

static void wal_recover_finish_sb_flush_done(struct spdk_bdev_io *child_io,
                                             bool success,
                                             void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;

    spdk_bdev_free_io(child_io);
    wal_recover_complete(vb, success);
}

static void wal_recover_complete(struct wal_vbdev *vb, bool ok)
{
    spdk_bdev_unregister_cb done_cb = vb->rec.done_cb;
    void *done_arg = vb->rec.done_arg;
    bool delete_pending = vb->rec.delete_pending;
    spdk_bdev_unregister_cb delete_cb = vb->rec.delete_cb;
    void *delete_arg = vb->rec.delete_arg;

    if (vb->rec.poller)
    {
        spdk_poller_unregister(&vb->rec.poller);
        vb->rec.poller = NULL;
    }

    if (vb->rec.jch)
    {
        spdk_put_io_channel(vb->rec.jch);
        vb->rec.jch = NULL;
    }
    if (vb->rec.mch)
    {
        spdk_put_io_channel(vb->rec.mch);
        vb->rec.mch = NULL;
    }

    vb->rec.step_inflight = false;
    vb->rec.stop = false;
    __atomic_clear(&vb->rec_in_progress, __ATOMIC_RELEASE);
    vb->rec.done_cb = NULL;
    vb->rec.done_arg = NULL;
    vb->rec.delete_pending = false;
    vb->rec.delete_cb = NULL;
    vb->rec.delete_arg = NULL;

    if (vb->rec.buf_j)
    {
        spdk_dma_free(vb->rec.buf_j);
        vb->rec.buf_j = NULL;
    }
    if (vb->rec.buf_m)
    {
        spdk_dma_free(vb->rec.buf_m);
        vb->rec.buf_m = NULL;
    }

    if (delete_pending)
    {
        wal_unregister(vb, delete_cb, delete_arg);
    }

    if (done_cb)
    {
        done_cb(done_arg, ok ? 0 : -EIO);
    }
}

static void wal_recover_step(struct wal_vbdev *vb)
{
    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    if (vb->rec_off_blocks == 0)
    {
        vb->rec_off_blocks = vb->sb.head_pos;
    }

    if (vb->rec_off_blocks >= vb->sb.journal_size)
    {
        vb->rec_off_blocks = 1;
    }

    if (vb->rec.scanned_blocks >= vb->sb.journal_size)
    {
        SPDK_NOTICELOG("wal: recovery scanned entire journal, stopping.\n");
        wal_recover_finish(vb, true);
        return;
    }

    int rc = spdk_bdev_read_blocks(vb->journal_desc,
                                   vb->rec.jch,
                                   vb->rec.buf_j,
                                   vb->rec_off_blocks,
                                   1,
                                   wal_recover_read_j_done,
                                   vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: journal header read failed: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

static void wal_recover_read_j_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;
    struct wal_record_header *hdr = (struct wal_record_header *)vb->rec.buf_j;
    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_finish(vb, false);
        return;
    }

    uint32_t crc =
            spdk_crc32c_update(hdr,
                               offsetof(struct wal_record_header, hdr_crc),
                               0);
    if (hdr->magic != WAL_RECORD_MAGIC || hdr->hdr_crc != crc)
    {
        SPDK_NOTICELOG("wal: recovery reached end of log at block %lu\n",
                       vb->rec_off_blocks);
        wal_recover_finish(vb, true);
        return;
    }

    if (hdr->seq <= vb->sb.checkpoint_seq)
    {
        SPDK_NOTICELOG("wal: recovery found old LSN %lu (last checkpoint was "
                       "%lu). Stopping.\n",
                       hdr->seq,
                       vb->sb.checkpoint_seq);
        wal_recover_finish(vb, true);
        return;
    }

    uint32_t data_blocks = hdr->num_blocks;

    if (hdr->type == WAL_RECORD_TYPE_PAD)
    {
        if (hdr->num_blocks != 0 ||
            hdr->rec_len != sizeof(struct wal_record_header) ||
            hdr->data_crc != 0)
        {
            SPDK_ERRLOG("wal: invalid pad record at block %lu\n",
                        vb->rec_off_blocks);
            wal_recover_finish(vb, false);
            return;
        }
        vb->rec.scanned_blocks += (vb->sb.journal_size - vb->rec_off_blocks);
        vb->rec_off_blocks = 1;
        vb->rec.step_inflight = false;
        return;
    }

    if (hdr->type != WAL_RECORD_TYPE_DATA)
    {
        SPDK_ERRLOG("wal: unknown record type %u at block %lu\n",
                    hdr->type,
                    vb->rec_off_blocks);
        wal_recover_finish(vb, false);
        return;
    }

    if (data_blocks == 0)
    {
        SPDK_ERRLOG("wal: zero-length data record at block %lu\n",
                    vb->rec_off_blocks);
        wal_recover_finish(vb, false);
        return;
    }

    if (data_blocks > vb->rec_chunk_blocks ||
        vb->rec_off_blocks + 1 + data_blocks > vb->sb.journal_size ||
        hdr->lba + data_blocks < hdr->lba ||
        hdr->lba + data_blocks > spdk_bdev_get_num_blocks(vb->main_bdev) ||
        hdr->rec_len != sizeof(struct wal_record_header) +
                        (uint64_t)data_blocks * vb->bdev.blocklen)
    {
        SPDK_ERRLOG("wal: invalid record bounds at block %lu\n",
                    vb->rec_off_blocks);
        wal_recover_finish(vb, false);
        return;
    }

    int rc = spdk_bdev_read_blocks(vb->journal_desc,
                                   vb->rec.jch,
                                   vb->rec.buf_m,
                                   vb->rec_off_blocks + 1,
                                   data_blocks,
                                   wal_recover_read_m_done,
                                   vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: journal data read failed: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

static void wal_recover_read_m_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;
    struct wal_record_header *hdr = (struct wal_record_header *)vb->rec.buf_j;
    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_finish(vb, false);
        return;
    }

    uint32_t data_crc = spdk_crc32c_update(vb->rec.buf_m,
                                           hdr->num_blocks * vb->bdev.blocklen,
                                           0);
    if (hdr->data_crc != data_crc)
    {
        SPDK_ERRLOG("wal: data CRC mismatch during recovery at block %lu\n",
                    vb->rec_off_blocks);
        wal_recover_finish(vb, false);
        return;
    }

    int rc = spdk_bdev_write_blocks(vb->main_desc,
                                    vb->rec.mch,
                                    vb->rec.buf_m,
                                    hdr->lba,
                                    hdr->num_blocks,
                                    wal_recover_write_done,
                                    vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: main write failed during recovery: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

static void wal_recover_write_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;
    struct wal_record_header *hdr = (struct wal_record_header *)vb->rec.buf_j;
    int rc;
    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_finish(vb, false);
        return;
    }

    rc = spdk_bdev_flush_blocks(vb->main_desc,
                                vb->rec.mch,
                                hdr->lba,
                                hdr->num_blocks,
                                wal_recover_flush_done,
                                vb);
    if (rc != 0)
    {
        wal_recover_finish(vb, false);
    }
}

static void wal_recover_flush_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;
    struct wal_record_header *hdr = (struct wal_record_header *)vb->rec.buf_j;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_finish(vb, false);
        return;
    }

    vb->rec_off_blocks += (1 + hdr->num_blocks);
    vb->rec.scanned_blocks += (1 + hdr->num_blocks);
    if (vb->rec_off_blocks >= vb->sb.journal_size)
    {
        vb->rec_off_blocks = 1;
    }
    vb->sb.checkpoint_seq = hdr->seq;
    if (vb->sb.next_seq <= hdr->seq)
    {
        vb->sb.next_seq = hdr->seq + 1;
    }
    vb->sb.head_pos = vb->rec_off_blocks;
    wal_superblock_update_crc(&vb->sb);
    memset(vb->rec.buf_j, 0, vb->bdev.blocklen);
    memcpy(vb->rec.buf_j, &vb->sb, sizeof(vb->sb));

    int rc = spdk_bdev_write_blocks(vb->journal_desc,
                                    vb->rec.jch,
                                    vb->rec.buf_j,
                                    0,
                                    1,
                                    wal_recover_sb_write_done,
                                    vb);
    if (rc != 0)
    {
        wal_recover_finish(vb, false);
    }
}

static void wal_recover_sb_write_done(struct spdk_bdev_io *child_io,
                                      bool success,
                                      void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;
    int rc;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_finish(vb, false);
        return;
    }

    rc = spdk_bdev_flush_blocks(vb->journal_desc,
                                vb->rec.jch,
                                0,
                                1,
                                wal_recover_sb_done,
                                vb);
    if (rc != 0)
    {
        wal_recover_finish(vb, false);
    }
}

static void wal_recover_sb_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct wal_vbdev *vb = cb_arg;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_recover_finish(vb, false);
        return;
    }

    vb->rec.step_inflight = false;
}
