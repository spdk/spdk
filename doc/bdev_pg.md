# Bdev Programmer's Guide {#bdev_pg}

# Table of Contents {#bdev_pg_toc}

 - @ref bdev_pg_audience
 - @ref bdev_pg_introduction
 - @ref bdev_pg_usage
 - TODO Bdev modules
 - TODO Writing new bdev modules
 - TODO Implementing SPDK bdev layer in applications

# Target Audience {#bdev_pg_audience}

This programmer's guide is intended for developers authoring applications that utilize the
SPDK bdev. It is intended to supplement the source code to provide an overall understanding
of how to integrate bdev library into an application as well as provide some high level
insight into how bdev works behind the scenes. It is not intended to serve as a design
document or an API reference but in some cases source code snippets and high level sequences
will be discussed. For the latest source code reference refer to the
[repo](https://github.com/spdk).

# Introduction {#bdev_pg_introduction}

From a programmer's perspective, bdev is a set of APIs that provide access to different I/O
devices through a single, generic block device layer. Unlike raw device operations, the bdev
layer is fairly user-friendly.

Some of the bdev-specific features are:
 - Handling the full I/O queue scenario. Bdevs keep an internal list of I/O that couldn't be
submitted immediately and will try to re-submit them once other I/O completes.
 - Hotremove support. Bdevs can be safely deleted at any time, regardless of their I/O traffic.
 - POSIX-like interface for issuing I/O, namely open/read/readv/write/writev. See the
@ref bdev_pg_io section for details.
 - I/O statistics. Bdevs can keep track of issued I/O and measure latency with integrated
Intel VTune Amplifier support.
 - Built-in support for device reset. All incoming I/O is immediately rejected when a reset
is in progress. Some @ref bdev_pg_modules (e.g. Linux AIO) implement software-only resets.

# Using Bdevs {#bdev_pg_usage}

Bdev public API is highly flexible and can be considered very low-level. As a consequence,
it requires a lot of resource management and puts much responsibility on its user. Some SPDK
applications rely on a higher level SCSI device layer that wraps (possibly multiple) bdevs.
struct spdk_scsi_lun and struct spdk_scsi_task might be a good example of how to use bdev
API in a real-case scenario.

The following subsections show the very simplest Bdev usage.

## Opening a Bdev {#bdev_pg_open}

Each function issuing I/O to a bdev requires passing a descriptor and a thread-local bdev
io_channel. While descriptors are farily straightforward, io_channels are described in
@ref concurrency guide.

```{.c}
/*
 * normally, those variables should be kept in some struct allocated on the heap;
 * we keep them here just for simplicity
 */
struct spdk_bdev *bdev;
struct spdk_bdev_desc desc;
struct spdk_io_channel *io_ch;
bool with_write_access = true;
int rc;

bdev = spdk_bdev_get_by_name(bdev_name);
if (bdev == NULL) { /* no bdev with such name */ }
rc = spdk_bdev_open(bdev, with_write_access, NULL, NULL, &desc);
if (rc != 0) {
    /*
     * bdev might be claimed (currently used by other bdev module);
     * try looking at RPC get_bdevs command output to see virtual bdevs
     * built on top this one; they should be openable; this one won't
     * untill all child bdevs are deleted
     */
}
io_ch = spdk_bdev_get_io_channel(&desc);
if (io_ch == NULL) {
    /*
     * device failed to reserve an additional io channel;
     * refer to module-specific settings to increase the number of usable
     * queues if the module allows that, or... do I/O from other (existing)
     * threads
     */
}

```

## Issuing I/O to a Bdev {#bdev_pg_io}

SPDK bdev supports following types of I/O:
 - read/readv - spdk_bdev_read(), spdk_bdev_readv()
 - write/writev - spdk_bdev_write(), spdk_bdev_writev()
 - unmap (also called trim or deallocate) - spdk_bdev_unmap()
 - flush - spdk_bdev_flush()
 - reset - spdk_bdev_reset()
 - write_zeroes - spdk_bdev_write_zeroes()
 - raw NVMe commands - spdk_bdev_nvme_admin_passthru(), spdk_bdev_nvme_io_passthru(),
spdk_bdev_nvme_io_passthru_md()

When issuing I/O, all provided offsets and I/O lengths must be blocksize-aligned.
Bdev also exposes *_blocks() function variants that use
[LBAs](https://en.wikipedia.org/wiki/Logical_block_addressing) directly. These variants
are used internally by the functions above and hence they are slightly more efficient.

 - read/readv - spdk_bdev_read_blocks(), spdk_bdev_readv_blocks()
 - write/writev - spdk_bdev_write_blocks(), spdk_bdev_writev_blocks()
 - flush - spdk_bdev_flush_blocks()
 - unmap - spdk_bdev_unmap_blocks()
 - write_zeroes - spdk_bdev_write_zeroes_blocks()

```{.c}
uint64_t lba;
uint32_t io_size;
void *buf;
int rc;

lba = 0; /* first block of the device */
io_size = spdk_bdev_get_block_size(bdev);
buf = spdk_dma_malloc(io_size, 0, NULL); /* unaligned 1-block-wide buffer */
if (buf == NULL) { /* not enough hugepage memory */ }
rc = spdk_bdev_read_blocks(&desc, io_ch,
                           buf, lba, 1,
                           complete_cb, buf);
/* we're passing buf as the last parameter - it will be passed to complete_cb */
if (rc != 0) {
    /*
     * we try to read from an invalid region; either exceeding device size
     * or non-aligned to bdev blocksize (the latter cannot be the case for *_blocks() calls)
     */
}
```

The above code flow continues in `complete_cb` after our I/O is completed.
We could do the following:

```{.c}

static void
complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    void *buf = cb_arg; /* parameter from spdk_bdev_read_blocks() call */

    if (!success) {
        int sct, sc; /* additional error information */
        spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
        /* spdk_bdev_io_get_scsi_status() can be used as well */
        fprintf(stderr, "I/O error. sct:%d sc:%d\n", sct, sc);
    } else {
        hexdump(buf, io_size);
    }

    spdk_dma_free(buf);
```

And if we don't need to do any more I/O:

```{.c}
static void
complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    [...]
    spdk_dma_free(buf);

    /* only if there are no other inflight I/O */
    spdk_put_io_channel(io_ch); /* these should be probably passed via cb_arg */
    spdk_bdev_close(desc);
```
