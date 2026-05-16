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
#define WAL_RECORD_MAGIC     0x52454348 /* "RECH" */

/* Суперблок — хранится в самом начале журнала (LBA 0) */
struct wal_superblock
{
    uint32_t magic;          /* "WAL\0" */
    uint32_t version;        /* Версия формата */
    uint64_t write_pos;      /* Текущая позиция (оффсет) для записи в журнале */
    uint64_t next_seq;       /* Номер (LSN) для следующей новой записи */
    uint64_t checkpoint_seq; /* LSN последней записи, успешно перенесенной в main */
    uint32_t hdr_crc;        /* CRC самого суперблока */
} __attribute__((packed));

/* Заголовок записи — предваряет каждый блок данных в журнале */
struct wal_record_header
{
    uint32_t magic;          /* "RECH" */
    uint32_t type;           /* Тип записи (Data, Metadata, Checkpoint) */
    uint64_t seq;            /* Порядковый номер (LSN) этой записи */
    uint64_t lba;            /* Адрес начала диапазона на main bdev */
    uint32_t num_blocks;     /* Количество блоков данных */
    uint32_t rec_len;        /* Общий размер (header + data) */
    uint32_t flags;          /* Флаги: DIRTY (нужен replay), CLEAN (уже в main) */
    uint32_t data_crc;       /* CRC полезных данных */
    uint32_t hdr_crc;        /* CRC самого заголовка */
} __attribute__((packed));

/* Структура виртуального WAL-устройства и состояния восстановления */
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

/* Контекст io-канала для доступа к основному и журнальному устройствам */
struct wal_io_channel
{
    struct spdk_io_channel *main_ch;
    struct spdk_io_channel *journal_ch;
};

/* Перечисление этапов выполнения операции WAL */
enum wal_stage
{
    WAL_STAGE_NONE = 0,
    WAL_STAGE_J_WRITE,
    WAL_STAGE_J_FLUSH,
    WAL_STAGE_M_WRITE,
    WAL_STAGE_M_FLUSH,
};

/* Контекст одной операции ввода-вывода WAL */
struct wal_bdev_io
{
    struct wal_io_channel *ch;
    enum wal_stage stage;
    struct iovec *iovs;
    int iovcnt;
    uint64_t offset_blocks;
    uint64_t num_blocks;
};

/* Инициализация модуля WAL */
static int vbdev_wal_init(void)
{
    return 0;
}

/* Завершение работы модуля WAL */
static void vbdev_wal_fini(void) {}

/* Examine-конфигурация для SPDK-модуля */
static void vbdev_wal_examine(struct spdk_bdev *bdev);

/* Возвращает размер приватного контекста на bdev_io */
static int vbdev_wal_get_ctx_size(void)
{
    return sizeof(struct wal_bdev_io);
}

/* Один шаг процесса восстановления данных */
static void wal_recover_step(struct wal_vbdev *vb);
static int wal_recover_poll(void *cb_arg);

/* Callback завершения чтения из журнала при восстановлении */
static void wal_recover_read_j_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg);

/* Callback завершения чтения из основного устройства при восстановлении */
static void wal_recover_read_m_done(struct spdk_bdev_io *child_io,
                                    bool success,
                                    void *cb_arg);

/* Callback завершения записи в основное устройство при восстановлении */
static void wal_recover_write_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg);

/* Завершает процесс восстановления (успех/ошибка) */
static void wal_recover_finish(struct wal_vbdev *vb, bool ok);

/* Callback завершения flush при восстановлении */
static void wal_recover_flush_done(struct spdk_bdev_io *child_io,
                                   bool success,
                                   void *cb_arg);

/* Описание bdev-модуля WAL для SPDK */
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

/* Регистрация модуля WAL в SPDK */
SPDK_BDEV_MODULE_REGISTER(wal, &wal_bdev_if)

/* Глобальный список зарегистрированных WAL-устройств */
TAILQ_HEAD(wal_vbdev_list, wal_vbdev) g_wal = TAILQ_HEAD_INITIALIZER(g_wal);

/* Создание io-канала для WAL-устройства */
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

/* Уничтожение io-канала WAL-устройства */
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

/* Завершение исходного io-запроса с заданным статусом */
static void wal_complete(struct spdk_bdev_io *orig, bool success)
{
    spdk_bdev_io_complete(orig,
                          success ? SPDK_BDEV_IO_STATUS_SUCCESS
                                  : SPDK_BDEV_IO_STATUS_FAILED);
}

/* Callback завершения flush на основном устройстве */
static void wal_main_flush_done(struct spdk_bdev_io *child_io,
                                bool success,
                                void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    spdk_bdev_free_io(child_io);
    wal_complete(orig, success);
}

/* Callback завершения записи в основное устройство */
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

/* Callback завершения flush на журнале */
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

/* Callback завершения записи в журнал */
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

/* Общий Callback завершения для passthrough-операций */
static void wal_passthru_done(struct spdk_bdev_io *child_io,
                              bool success,
                              void *cb_arg)
{
    struct spdk_bdev_io *orig = cb_arg;
    spdk_bdev_free_io(child_io);
    wal_complete(orig, success);
}

/* Обработка входящего запроса на WAL-устройство */
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

        spdk_bdev_writev_blocks(vb->journal_desc,
                                wch->journal_ch,
                                wio->iovs,
                                wio->iovcnt,
                                wio->offset_blocks,
                                wio->num_blocks,
                                wal_journal_write_done,
                                bdev_io);
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

/* Проверка поддерживаемых типов операций bdev */
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

/* Получение io-канала для WAL bdev */
static struct spdk_io_channel *wal_get_io_channel(void *ctx)
{
    struct wal_vbdev *vbdev = ctx;
    return spdk_get_io_channel(vbdev);
}

/* Деструктор WAL-устройства (закрытие и освобождение ресурсов) */
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

/* Таблица функций bdev для WAL */
static const struct spdk_bdev_fn_table g_wal_fn_table = {
        .destruct = wal_destruct,
        .submit_request = wal_submit_request,
        .io_type_supported = wal_io_type_supported,
        .get_io_channel = wal_get_io_channel,
};

/* Обработка событий базовых bdev (логирование) */
static void wal_base_bdev_event_cb(enum spdk_bdev_event_type type,
                                   struct spdk_bdev *bdev,
                                   void *arg)
{
    SPDK_NOTICELOG("wal: base bdev event %d on %s\n",
                   type,
                   spdk_bdev_get_name(bdev));
}

/* Поиск экземпляра WAL по имени экспортируемого bdev */
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

/* Создание и регистрация нового WAL-диска на основе двух базовых bdev */
int wal_bdev_create_disk(char *main_bdev_name,
                         char *journal_bdev_name,
                         char *name,
                         uint32_t *block_sz,
                         uint64_t *size_mb)
{
    int rc = 0;
    struct wal_vbdev *vb = NULL;
    struct spdk_bdev_desc *main_desc = NULL, *journal_desc = NULL;
    struct spdk_bdev *main_bdev = NULL, *journal_bdev = NULL;

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

    /* Инициализация суперблока */
    vb->sb.magic = WAL_SUPERBLOCK_MAGIC;
    vb->sb.version = 1;
    vb->sb.write_pos = 1;      /* Первая запись пойдет после суперблока (LBA 1) */
    vb->sb.next_seq = 1;
    vb->sb.checkpoint_seq = 0;
    vb->sb.hdr_crc = spdk_crc32c_update(&vb->sb, offsetof(struct wal_superblock, hdr_crc), 0);

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

    rc = spdk_bdev_register(&vb->bdev);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: spdk_bdev_register failed: %d\n", rc);
        goto err_unregister_io_dev;
    }

    TAILQ_INSERT_TAIL(&g_wal, vb, link);

    if (block_sz)
        *block_sz = vb->bdev.blocklen;
    if (size_mb)
        *size_mb = (vb->bdev.blockcnt * vb->bdev.blocklen) / (1024 * 1024);

    SPDK_NOTICELOG("wal: created '%s' (main=%s, journal=%s)\n",
                   name,
                   main_bdev_name,
                   journal_bdev_name);
    return 0;

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

/* Удаление и дерегистрация WAL-диска по имени */
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

/* Запуск процесса восстановления данных из журнала в основное устройство */
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

/* Завершение процесса восстановления, освобождение ресурсов и уведомление RPC */
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

/* Шаг восстановления: читает данные из журнала для сравнения/копирования */
static void wal_recover_step(struct wal_vbdev *vb)
{
    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    uint64_t total = vb->bdev.blockcnt;
    if (vb->rec_off_blocks >= total)
    {
        wal_recover_finish(vb, true);
        return;
    }

    uint64_t remain = total - vb->rec_off_blocks;
    uint32_t step = vb->rec_chunk_blocks;
    if ((uint64_t)step > remain)
        step = (uint32_t)remain;

    int rc = spdk_bdev_read_blocks(vb->journal_desc,
                                   vb->rec.jch,
                                   vb->rec.buf_j,
                                   vb->rec_off_blocks,
                                   step,
                                   wal_recover_read_j_done,
                                   vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: journal read submit failed: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

/* Callback: завершено чтение из журнала, переходим к чтению из основного устройства */
static void wal_recover_read_j_done(struct spdk_bdev_io *child_io,
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

    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    uint32_t step = (uint32_t)child_io->u.bdev.num_blocks;

    int rc = spdk_bdev_read_blocks(vb->main_desc,
                                   vb->rec.mch,
                                   vb->rec.buf_m,
                                   vb->rec_off_blocks,
                                   step,
                                   wal_recover_read_m_done,
                                   vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: main read submit failed: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

/* Callback: завершено чтение из основного устройства, сравнение и при необходимости запись */
static void wal_recover_read_m_done(struct spdk_bdev_io *child_io,
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

    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    uint32_t step = (uint32_t)child_io->u.bdev.num_blocks;

    if (memcmp(vb->rec.buf_j, vb->rec.buf_m, (size_t)step * vb->bdev.blocklen)
        == 0)
    {
        vb->rec_off_blocks += step;
        if (vb->rec_off_blocks >= vb->bdev.blockcnt)
        {
            wal_recover_finish(vb, true);
            return;
        }
        vb->rec.step_inflight = false;
        return;
    }

    int rc = spdk_bdev_write_blocks(vb->main_desc,
                                    vb->rec.mch,
                                    vb->rec.buf_j,
                                    vb->rec_off_blocks,
                                    step,
                                    wal_recover_write_done,
                                    vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: main write submit failed: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

/* Callback: завершена запись несоответствующих блоков в основное устройство */
static void wal_recover_write_done(struct spdk_bdev_io *child_io,
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

    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    uint32_t step = (uint32_t)child_io->u.bdev.num_blocks;

    vb->rec.last_step = step;
    int rc = spdk_bdev_flush_blocks(vb->main_desc,
                                    vb->rec.mch,
                                    vb->rec_off_blocks,
                                    step,
                                    wal_recover_flush_done,
                                    vb);
    if (rc != 0)
    {
        SPDK_ERRLOG("wal: main flush submit failed: %d\n", rc);
        wal_recover_finish(vb, false);
    }
}

/* Callback завершён flush записанных блоков, двигаем оффсет и следующий шаг */
static void wal_recover_flush_done(struct spdk_bdev_io *child_io,
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

    if (vb->rec.stop)
    {
        wal_recover_finish(vb, false);
        return;
    }

    vb->rec_off_blocks += vb->rec.last_step;
    if (vb->rec_off_blocks >= vb->bdev.blockcnt)
    {
        wal_recover_finish(vb, true);
        return;
    }

    vb->rec.step_inflight = false;
}
