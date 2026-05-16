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

/* Superblock - stored at the very beginning of the journal (LBA 0) */
struct wal_superblock
{
    uint32_t magic;     /* "WAL\0" */
    uint32_t version;   /* Format version */
    uint64_t write_pos; /* Current write offset in the journal */
    uint64_t head_pos;  /* Position of the oldest active record */
    uint64_t next_seq;  /* LSN for the next new record */
    uint64_t
            checkpoint_seq; /* LSN of the last record successfully flushed to main */
    uint64_t journal_size; /* Total journal size in blocks */
    uint32_t hdr_crc;      /* CRC of the superblock itself */
} __attribute__((packed));

/* Record header - precedes each data block in the journal */
struct wal_record_header
{
    uint32_t magic;      /* "RECH" */
    uint32_t type;       /* Record type (Data, Metadata, Checkpoint) */
    uint64_t seq;        /* LSN of this record */
    uint64_t lba;        /* Target starting LBA on the main bdev */
    uint32_t num_blocks; /* Number of data blocks */
    uint32_t rec_len;    /* Total size (header + data) */
    uint32_t flags;      /* Flags: DIRTY, CLEAN */
    uint32_t data_crc;   /* Payload CRC */
    uint32_t hdr_crc;    /* Header CRC */
} __attribute__((packed));

/* Structure representing the WAL virtual block device and its state */
struct wal_vbdev
{
    struct spdk_bdev bdev;
    struct spdk_bdev_desc *main_desc;
    struct spdk_bdev_desc *journal_desc;
    struct spdk_bdev *main_bdev;
    struct spdk_bdev *journal_bdev;
    TAILQ_ENTRY(wal_vbdev) link;

    bool rec_in_progress;
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
    } rec;

    struct wal_superblock sb;
};

/* IO channel context for accessing main and journal devices */
struct wal_io_channel
{
    struct spdk_io_channel *main_ch;
    struct spdk_io_channel *journal_ch;
};

/* Stages of a WAL IO operation */
enum wal_stage
{
    WAL_STAGE_NONE = 0,
    WAL_STAGE_J_WRITE,
    WAL_STAGE_J_FLUSH,
    WAL_STAGE_M_WRITE,
    WAL_STAGE_M_FLUSH,
    WAL_STAGE_SB_UPDATE, /* Superblock update on disk */
};

/* Context for superblock update (allocated on flush) */
struct wal_sb_update_ctx
{
    struct wal_vbdev *vb;
    struct spdk_io_channel *ch;
    void *buf;
    struct spdk_bdev_io *orig_io;
};

/* Context for a single WAL IO operation */
struct wal_bdev_io
{
    struct wal_io_channel *ch;
    enum wal_stage stage;
    struct iovec *iovs;
    int iovcnt;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    uint64_t journal_pos;         /* Record position in the journal */
    struct wal_record_header hdr; /* Record header on disk */
    struct iovec log_iovs[SPDK_BDEV_IO_NUM_CHILD_IOV
                          + 1]; /* iovec for journal write (hdr + data) */
};

/* Initialize the WAL module */
static int vbdev_wal_init(void)
{
    return 0;
}

/* Cleanup the WAL module */
static void vbdev_wal_fini(void) {}

/* Examine configuration for the SPDK module */
static void vbdev_wal_examine(struct spdk_bdev *bdev);

/* Returns the size of the private context on bdev_io */
static int vbdev_wal_get_ctx_size(void)
{
    return sizeof(struct wal_bdev_io);
}

/* One step of the data recovery process */
static void wal_recover_step(struct wal_vbdev *vb);
static int wal_recover_poll(void *cb_arg);

/* Callback for journal read completion during recovery */
static void wal_recover_read_j_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg);

/* Callback for main device read completion during recovery */
static void wal_recover_read_m_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg);

/* Callback for main device write completion during recovery */
static void wal_recover_write_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg);

/* Finishes the recovery process (success/failure) */
static void wal_recover_finish(struct wal_vbdev *vb, bool ok);

/* SPDK bdev module descriptor for WAL */
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

/* Register the WAL module in SPDK */
SPDK_BDEV_MODULE_REGISTER(wal, &wal_bdev_if)

/* Global list of registered WAL devices */
TAILQ_HEAD(wal_vbdev_list, wal_vbdev) g_wal = TAILQ_HEAD_INITIALIZER(g_wal);

/* Create an IO channel for the WAL device */
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

/* Destroy the IO channel of the WAL device */
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

/* Complete the original IO request with the specified status */
static void wal_complete(struct spdk_bdev_io *orig, bool success)
{
    spdk_bdev_io_complete(orig,
                          success ? SPDK_BDEV_IO_STATUS_SUCCESS
                                  : SPDK_BDEV_IO_STATUS_FAILED);
}

static void wal_sb_flush_done(struct spdk_bdev_io *child_io,
                              bool success,
                              void *cb_arg)
{
    struct wal_sb_update_ctx *ctx = cb_arg;
    struct spdk_bdev_io *orig = ctx->orig_io;

    spdk_bdev_free_io(child_io);
    spdk_put_io_channel(ctx->ch);
    spdk_dma_free(ctx->buf);

    wal_complete(orig, success);
    free(ctx);
}

static void wal_main_flush_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;

    spdk_bdev_free_io(child_io);

    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    /* Start superblock update on disk (Checkpoint) */
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

    /* Update CRC and checkpoint before writing */
    vb->sb.checkpoint_seq = vb->sb.next_seq - 1;
    /* In this simplified model, we move Head to the current Tail on Flush,
     * assuming all data up to Tail is now in main. */
    vb->sb.head_pos = vb->sb.write_pos;

    vb->sb.hdr_crc =
            spdk_crc32c_update(&vb->sb,
                               offsetof(struct wal_superblock, hdr_crc),
                               0);
    memcpy(ctx->buf, &vb->sb, sizeof(vb->sb));

    ctx->ch = spdk_bdev_get_io_channel(vb->journal_desc);
    spdk_bdev_write_blocks(
            vb->journal_desc, ctx->ch, ctx->buf, 0, 1, wal_sb_flush_done, ctx);
}

/* Callback for main device write completion */
static void wal_main_write_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_M_FLUSH;
    spdk_bdev_flush_blocks(vb->main_desc,
                           wio->ch->main_ch,
                           wio->offset_blocks,
                           wio->num_blocks,
                           wal_main_flush_done,
                           orig);
}

/* Callback for journal flush completion */
static void wal_journal_flush_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    if (orig->type == SPDK_BDEV_IO_TYPE_FLUSH)
    {
        wio->stage = WAL_STAGE_M_FLUSH;
        spdk_bdev_flush_blocks(vb->main_desc,
                               wio->ch->main_ch,
                               wio->offset_blocks,
                               wio->num_blocks,
                               wal_main_flush_done,
                               orig);
        return;
    }
    wio->stage = WAL_STAGE_M_WRITE;
    spdk_bdev_writev_blocks(vb->main_desc,
                            wio->ch->main_ch,
                            wio->iovs,
                            wio->iovcnt,
                            wio->offset_blocks,
                            wio->num_blocks,
                            wal_main_write_done,
                            orig);
}

/* Callback for journal write completion */
static void wal_journal_write_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    struct wal_bdev_io *wio = (struct wal_bdev_io *)orig->driver_ctx;
    struct wal_vbdev *vb = (struct wal_vbdev *)orig->bdev->ctxt;

    spdk_bdev_free_io(child_io);
    if (!success)
    {
        wal_complete(orig, false);
        return;
    }

    wio->stage = WAL_STAGE_J_FLUSH;
    spdk_bdev_flush_blocks(vb->journal_desc,
                           wio->ch->journal_ch,
                           wio->offset_blocks,
                           wio->num_blocks,
                           wal_journal_flush_done,
                           orig);
}

/* General completion callback for passthrough operations */
static void wal_passthru_done(struct spdk_bdev_io *child_io,
                              bool success,
                              void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    spdk_bdev_free_io(child_io);
    wal_complete(orig, success);
}

/* Handle incoming I/O request for the WAL device */
static void wal_submit_request(struct spdk_io_channel *ch,
                               struct spdk_bdev_io *bdev_io)
{
    struct wal_vbdev *vb = (struct wal_vbdev *)bdev_io->bdev->ctxt;

    struct wal_io_channel *wch = spdk_io_channel_get_ctx(ch);

    struct wal_bdev_io *wio = (struct wal_bdev_io *)bdev_io->driver_ctx;
    wio->ch = wch;
    wio->offset_blocks = bdev_io->u.bdev.offset_blocks;
    wio->num_blocks = bdev_io->u.bdev.num_blocks;

    switch (bdev_io->type)
    {
    case SPDK_BDEV_IO_TYPE_READ:
    {
        struct iovec *iovs = bdev_io->u.bdev.iovs;
        int iovcnt = bdev_io->u.bdev.iovcnt;
        uint64_t end = wio->offset_blocks + wio->num_blocks;
        bool use_journal = vb->rec_in_progress && end > vb->rec_off_blocks;
        struct spdk_bdev_desc *target_desc =
                use_journal ? vb->journal_desc : vb->main_desc;
        struct spdk_io_channel *target_ch =
                use_journal ? wch->journal_ch : wch->main_ch;

        spdk_bdev_readv_blocks(target_desc,
                               target_ch,
                               iovs,
                               iovcnt,
                               wio->offset_blocks,
                               wio->num_blocks,
                               wal_passthru_done,
                               bdev_io);
        break;
    }

    case SPDK_BDEV_IO_TYPE_WRITE:
    {
        if (vb->rec_in_progress)
        {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        wio->iovs = bdev_io->u.bdev.iovs;
        wio->iovcnt = bdev_io->u.bdev.iovcnt;
        wio->stage = WAL_STAGE_J_WRITE;

        /* Prepare record header */
        wio->hdr.magic = WAL_RECORD_MAGIC;
        wio->hdr.type = 1; /* Data */
        wio->hdr.seq = vb->sb.next_seq++;
        wio->hdr.lba = wio->offset_blocks;
        wio->hdr.num_blocks = (uint32_t)wio->num_blocks;
        wio->hdr.rec_len = sizeof(struct wal_record_header)
                           + (wio->hdr.num_blocks * vb->bdev.blocklen);
        wio->hdr.flags = 1; /* DIRTY */

        /* Calculate data CRC */
        wio->hdr.data_crc = spdk_crc32c_iov_update(wio->iovs, wio->iovcnt, 0);
        /* Calculate header CRC */
        wio->hdr.hdr_crc =
                spdk_crc32c_update(&wio->hdr,
                                   offsetof(struct wal_record_header, hdr_crc),
                                   0);

        /* Construct iovec array for journal write: [Header][Data...] */
        wio->log_iovs[0].iov_base = &wio->hdr;
        wio->log_iovs[0].iov_len = sizeof(struct wal_record_header);
        for (int i = 0; i < wio->iovcnt; i++)
        {
            wio->log_iovs[i + 1] = wio->iovs[i];
        }

        /* Check journal boundaries and handle wrap-around */
        uint64_t needed = wio->num_blocks + 1;
        if (vb->sb.write_pos + needed > vb->sb.journal_size)
        {
            vb->sb.write_pos =
                    1; /* Return to start (LBA 0 is occupied by the superblock) */
        }

        /* Overflow check (Tail reaches Head) */
        if (vb->sb.write_pos < vb->sb.head_pos
            && (vb->sb.write_pos + needed) >= vb->sb.head_pos)
        {
            SPDK_ERRLOG("wal: journal overflow (Tail reached Head)\n");
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        wio->journal_pos = vb->sb.write_pos;

        /* Write to journal at the current write_pos */
        /* IMPORTANT: We assume header + data is aligned or bdev allows such writes. 
         * In reality, the header might require padding to the block size. */
        spdk_bdev_writev_blocks(
                vb->journal_desc,
                wch->journal_ch,
                wio->log_iovs,
                wio->iovcnt + 1,
                vb->sb.write_pos,
                wio->num_blocks + 1, /* +1 for the header block (simplified) */
                wal_journal_write_done,
                bdev_io);

        /* Update write position in memory */
        vb->sb.write_pos += (wio->num_blocks + 1);
        break;
    }

    case SPDK_BDEV_IO_TYPE_FLUSH:
    {
        wio->stage = WAL_STAGE_J_FLUSH;
        spdk_bdev_flush_blocks(vb->journal_desc,
                               wch->journal_ch,
                               wio->offset_blocks,
                               wio->num_blocks,
                               wal_journal_flush_done,
                               bdev_io);
        break;
    }

    case SPDK_BDEV_IO_TYPE_RESET:
    {
        spdk_bdev_reset(vb->main_desc,
                        wch->main_ch,
                        wal_passthru_done,
                        bdev_io);
        break;
    }

    default:
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}

/* Check supported bdev I/O types */
static bool wal_io_type_supported(void *ctx, enum spdk_bdev_io_type t)
{
    switch (t)
    {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_FLUSH:
    case SPDK_BDEV_IO_TYPE_RESET:
        return true;
    default:
        return false;
    }
}

/* Get IO channel for the WAL bdev */
static struct spdk_io_channel *wal_get_io_channel(void *ctx)
{
    struct wal_vbdev *vbdev = ctx;
    return spdk_get_io_channel(vbdev);
}

/* WAL device destructor (close and free resources) */
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

/* Bdev function table for WAL */
static const struct spdk_bdev_fn_table g_wal_fn_table = {
        .destruct = wal_destruct,
        .submit_request = wal_submit_request,
        .io_type_supported = wal_io_type_supported,
        .get_io_channel = wal_get_io_channel,
};

/* Handle events from base bdevs (logging) */
static void wal_base_bdev_event_cb(enum spdk_bdev_event_type type,
                                   struct spdk_bdev *bdev,
                                   void *arg)
{
    SPDK_NOTICELOG("wal: base bdev event %d on %s\n",
                   type,
                   spdk_bdev_get_name(bdev));
}

/* Find a WAL instance by its exported bdev name */
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

/* Initialization context for asynchronous superblock write */
struct wal_init_ctx
{
    struct wal_vbdev *vb;
    void *buf;
    wal_bdev_create_cb cb_fn;
    void *cb_arg;
    struct spdk_io_channel *ch;
};

/* Callback for superblock write completion */
static void wal_sb_write_done(struct spdk_bdev_io *bdev_io,
                              bool success,
                              void *cb_arg)
{
    struct wal_init_ctx *ctx = cb_arg;
    struct wal_vbdev *vb = ctx->vb;
    int rc = success ? 0 : -EIO;

    spdk_bdev_free_io(bdev_io);
    spdk_put_io_channel(ctx->ch);
    spdk_dma_free(ctx->buf);

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
    }

    ctx->cb_fn(ctx->cb_arg, rc);
    free(ctx);
}

/* Create and register a new WAL disk based on two base bdevs */
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

    /* superblock init */
    vb->sb.magic = WAL_SUPERBLOCK_MAGIC;
    vb->sb.version = 1;
    vb->sb.write_pos = 1;
    vb->sb.head_pos = 1;
    vb->sb.next_seq = 1;
    vb->sb.checkpoint_seq = 0;
    vb->sb.journal_size = spdk_bdev_get_num_blocks(journal_bdev);
    vb->sb.hdr_crc =
            spdk_crc32c_update(&vb->sb,
                               offsetof(struct wal_superblock, hdr_crc),
                               0);

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

    size_t align = max(spdk_bdev_get_buf_align(main_bdev),
                       spdk_bdev_get_buf_align(journal_bdev));
    vb->bdev.required_alignment =
            (align > 1) ? spdk_u32log2((uint32_t)align) : 0;

    spdk_io_device_register(vb,
                            wal_io_channel_create_cb,
                            wal_io_channel_destroy_cb,
                            sizeof(struct wal_io_channel),
                            vb->bdev.name);

    /* Asynchronous superblock write */
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

    memcpy(ctx->buf, &vb->sb, sizeof(vb->sb));
    ctx->ch = spdk_bdev_get_io_channel(vb->journal_desc);
    if (!ctx->ch)
    {
        rc = -ENOMEM;
        goto err_buf;
    }

    rc = spdk_bdev_write_blocks(
            vb->journal_desc, ctx->ch, ctx->buf, 0, 1, wal_sb_write_done, ctx);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: failed to write superblock: %d\n", rc);
        goto err_ch;
    }

    TAILQ_INSERT_TAIL(&g_wal, vb, link);

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

/* Delete and unregister a WAL disk by name */
int wal_bdev_delete_disk(char *name,
                         spdk_bdev_unregister_cb cb_fn,
                         void *cb_arg)
{
    struct wal_vbdev *vb = wal_find_by_name(name);
    if (!vb)
    {
        return -ENODEV;
    }
    if (vb->rec_in_progress)
    {
        if (vb->rec.delete_pending)
        {
            return -EBUSY;
        }
        vb->rec.stop = true;
        vb->rec.delete_pending = true;
        vb->rec.delete_cb = cb_fn;
        vb->rec.delete_arg = cb_arg;
        return 0;
    }

    wal_unregister(vb, cb_fn, cb_arg);
    return 0;
}

/* Start the data recovery process from the journal to the main device */
int wal_bdev_recover(const char *name,
                     spdk_bdev_unregister_cb cb_fn,
                     void *cb_arg)
{
    int rc = 0;
    struct wal_vbdev *vb = wal_find_by_name(name);
    if (!vb)
        return -ENODEV;
    if (vb->rec_in_progress)
        return -EBUSY;

    vb->rec_in_progress = true;
    vb->rec_off_blocks = 0;
    vb->rec.done_cb = cb_fn;
    vb->rec.done_arg = cb_arg;
    vb->rec.stop = false;
    vb->rec.step_inflight = false;
    vb->rec.last_step = 0;
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
    size_t align = max(spdk_bdev_get_buf_align(vb->main_bdev),
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
    vb->rec_in_progress = false;
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

/* Finish the recovery process, free resources, and notify RPC */
static void wal_recover_finish(struct wal_vbdev *vb, bool ok)
{
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
    vb->rec_in_progress = false;

    if (vb->rec.done_cb)
    {
        vb->rec.done_cb(vb->rec.done_arg, ok ? 0 : -EIO);
        vb->rec.done_cb = NULL;
        vb->rec.done_arg = NULL;
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

    if (vb->rec.delete_pending)
    {
        wal_unregister(vb, vb->rec.delete_cb, vb->rec.delete_arg);
        vb->rec.delete_pending = false;
        vb->rec.delete_cb = NULL;
        vb->rec.delete_arg = NULL;
    }
}

/* Recovery step: reads a record header from the journal */
static void wal_recover_step(struct wal_vbdev *vb)
{
    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    /* Scan the journal starting from Head */
    if (vb->rec_off_blocks == 0)
    {
        vb->rec_off_blocks = vb->sb.head_pos;
    }

    /* Wrap-around to the beginning if we reach the end of the physical disk */
    if (vb->rec_off_blocks >= vb->sb.journal_size)
    {
        vb->rec_off_blocks = 1;
    }

    /* Read one block for the header */
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

/* Callback: journal header read completed */
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

    /* Check header validity */
    uint32_t crc =
            spdk_crc32c_update(hdr,
                               offsetof(struct wal_record_header, hdr_crc),
                               0);
    if (hdr->magic != WAL_RECORD_MAGIC || hdr->hdr_crc != crc)
    {
        /* Reached the end of valid records or encountered garbage */
        SPDK_NOTICELOG("wal: recovery reached end of log at block %lu\n",
                       vb->rec_off_blocks);
        wal_recover_finish(vb, true);
        return;
    }

    /* LSN check (protection against records from the previous journal lap) */
    if (hdr->seq <= vb->sb.checkpoint_seq)
    {
        SPDK_NOTICELOG("wal: recovery found old LSN %lu (last checkpoint was "
                       "%lu). Stopping.\n",
                       hdr->seq,
                       vb->sb.checkpoint_seq);
        wal_recover_finish(vb, true);
        return;
    }

    /* Header is valid, now read the data */
    uint32_t data_blocks = hdr->num_blocks;
    if (data_blocks == 0)
    {
        vb->rec_off_blocks += 1;
        vb->rec.step_inflight = false;
        return;
    }

    /* Use buf_m to read data from the journal */
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

/* Callback: journal data read completed, write to main at the address from the header */
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

    /* Check data CRC before writing (optional, but reliable) */
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

    /* Write data to the main disk at the LBA from the header */
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

/* Callback: main device write completed, advance the journal offset */
static void wal_recover_write_done(struct spdk_bdev_io *child_io,
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

    /* Advance the scan position in the journal: header (1 block) + data */
    vb->rec_off_blocks += (1 + hdr->num_blocks);
    if (vb->rec_off_blocks >= vb->sb.journal_size)
    {
        vb->rec_off_blocks = 1;
    }
    vb->rec.step_inflight = false;
}
