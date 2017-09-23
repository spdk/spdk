#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blob.h"
#include "spdk/log.h"
#include "hashtable.h"

#include "spdk/hashtable.h"

struct blobkv_context_t {
    struct spdk_blob_store *bs;
    struct spdk_blob *blob;
    spdk_blob_id blobid;
    struct spdk_io_channel *channel;
    uint8_t *read_buff;
    uint8_t *write_buff;
    uint64_t page_size;
    int rc;
};

static void
kv_blob_read(void *k, void *v)
{
    if (exists(k)) {
        hashget(k);
        read_blob(k->blob)
    }
}

static void
kv_blob_write(void *k, void *v)
{
    if(checksize(v)){
        kv_blob_write_part(k, v)
    }
    blob_delete(v)
}

static void
kv_blob_write_part ( void *k,  struct blobkv_context_t *v, uint64_t size )
{
    
}

static void
kv_blob_delete ( void *k, void *v )
{
    if(checksize(v)){
        kv_blob_write_part(k, v)
    }
    blob_write(v)
}

static void
kv_blob_delete_part ( void *k,  struct blobkv_context_t *v, uint64_t size )
{
    
}

static void
spdk_bs_destroy ( void *k, void *v )
{
    
}


static void
blobkv_cleanup(struct blobkv_context_t *blobkv_context)
{
    spdk_dma_free(blobkv_context->read_buff);
    spdk_dma_free(blobkv_context->write_buff);
    free(blobkv_context);
}

static void
read_blob(struct blobkv_context_t *blobkv_context)
{
    SPDK_NOTICELOG("entry\n");
    
    blobkv_context->read_buff = spdk_dma_malloc(blobkv_context->page_size,
                                                0x1000, NULL);
    
    spdk_bs_io_read_blob(blobkv_context->blob, blobkv_context->channel,
                         blobkv_context->read_buff, 0, 1, NULL,
                         blobkv_context);
}

static void
blob_write(struct blobkv_context_t *blobkv_context)
{
    SPDK_NOTICELOG("entry\n");
    
    blobkv_context->write_buff = spdk_dma_malloc(blobkv_context->page_size,
                                                 0x1000, NULL);
    memset(blobkv_context->write_buff, 0x5a, blobkv_context->page_size);
    
    blobkv_context->channel = spdk_bs_alloc_io_channel(blobkv_context->bs);
    
    spdk_bs_io_write_blob(blobkv_context->blob, blobkv_context->channel,
                          blobkv_context->write_buff,
                          0, 1, NULL, blobkv_context);
}

static void
delete_blob(void *arg1, int bserrno)
{
    struct blobkv_context_t *blobkv_context = arg1;
    
    SPDK_NOTICELOG("entry\n");
    
    if (bserrno) {
        unload_bs(blobkv_context, "Error in close completion",
                  bserrno);
        return;
    }
    
    spdk_bs_md_delete_blob(blobkv_context->bs, blobkv_context->blobid,
                           NULL, blobkv_context);
}


static void
unload_bs(struct blobkv_context_t *blobkv_context, char *msg, int bserrno)
{
    if (bserrno) {
        SPDK_ERRLOG("%s (err %d)\n", msg, bserrno);
        blobkv_context->rc = bserrno;
    }
    if (blobkv_context->bs) {
        if (blobkv_context->channel) {
            spdk_bs_free_io_channel(blobkv_context->channel);
        }
        spdk_bs_unload(blobkv_context->bs, NULL, blobkv_context);
    } else {
        spdk_app_stop(bserrno);
    }
}




