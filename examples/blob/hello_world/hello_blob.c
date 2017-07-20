/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blob.h"
#include "spdk/log.h"

/*
 * we'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks
 */
struct hello_context_t {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	uint8_t *read_buff;
	uint8_t *write_buff;
	uint64_t page_size;
};

/*
 * free up memory that we allocated
 */
static void
hello_cleanup(struct hello_context_t *hello_context)
{
	if (hello_context->channel) {
		spdk_bs_free_io_channel(hello_context->channel);
	}
	if (hello_context->read_buff) {
		spdk_dma_free(hello_context->read_buff);
	}
	if (hello_context->write_buff) {
		spdk_dma_free(hello_context->write_buff);
	}
	if (hello_context) {
		free(hello_context);
	}
}

/*
 * callback routine for the blobstore unload
 */
static void
unload_complete(void *cb_arg, int bserrno)
{
	struct hello_context_t *hello_context = cb_arg;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d unloading the bobstore\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	hello_cleanup(hello_context);
	spdk_app_stop(0);
}

/*
 * callback routine for the deletion of a blob
 */
static void
delete_complete(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in delete completion\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	/* we're all done, we can unload the blobstore */
	spdk_bs_unload(hello_context->bs, unload_complete, hello_context);
}

/*
 * function for deleting a blob
 */
static void
delete_blob(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in delete completion\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	spdk_bs_md_delete_blob(hello_context->bs, hello_context->blobid,
			       delete_complete, hello_context);
}

/*
 * callback function for reading a blob
 */
static void
read_complete(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context = arg1;
	int match_res = -1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in read completion\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	/* now lets make sure things match */
	match_res = memcmp(hello_context->write_buff, hello_context->read_buff,
			   hello_context->page_size);
	if (match_res) {
		SPDK_ERRLOG("Error in read completion, buffers don't match\n");
		spdk_app_stop(-1);
		return;
	} else {
		SPDK_NOTICELOG("read SUCCESS and data matches!\n");
	}

	/* now lets close it and delete the blob in the callback */
	spdk_bs_md_close_blob(&hello_context->blob, delete_blob,
			      hello_context);
}

/*
 * function for reading a blob
 */
static void
read_blob(struct hello_context_t *hello_context)
{
	SPDK_NOTICELOG("entry\n");

	hello_context->read_buff = spdk_dma_malloc(hello_context->page_size,
				   1024, NULL);
	if (hello_context->read_buff == NULL) {
		SPDK_ERRLOG("Error trying to alocate read buffer\n");
		spdk_app_stop(-ENOMEM);
		return;
	}

	/* issue the read and compare the results in the callback */
	spdk_bs_io_read_blob(hello_context->blob, hello_context->channel,
			     hello_context->read_buff, 0, 1, read_complete,
			     hello_context);
}

/*
 * callback function for writing a blob
 */
static void
write_complete(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in write completion\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	/* now lets read back what we wrote and make sure it matches */
	read_blob(hello_context);
}

/*
 * function for writing to a blob
 */
static void
blob_write(struct hello_context_t *hello_context)
{
	SPDK_NOTICELOG("entry\n");

	/*
	 * buffers for data transfer need to be allocated via SPDK. We will
	 * tranfer 1 page of 4K aligned data at offset 0 in the blob
	 */
	hello_context->write_buff = spdk_dma_malloc(hello_context->page_size,
				    1024, NULL);
	if (hello_context->write_buff == NULL) {
		SPDK_ERRLOG("Error trying to alocate a channel\n");
		spdk_app_stop(-ENOMEM);
		return;
	}
	memset(hello_context->write_buff, 0x5a, hello_context->page_size);

	/* now we have to allocate a channel */
	hello_context->channel = spdk_bs_alloc_io_channel(hello_context->bs);
	if (hello_context->channel == NULL) {
		SPDK_ERRLOG("Error trying to alocate a channel\n");
		spdk_app_stop(-ENOMEM);
		return;
	}

	/* lets perform the write, 1 page at offset 0 */
	spdk_bs_io_write_blob(hello_context->blob, hello_context->channel,
			      hello_context->write_buff,
			      0, 1, write_complete, hello_context);
}

/*
 * callback function for opening a blob
 */
static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct hello_context_t *hello_context = cb_arg;
	uint64_t free = 0;
	uint64_t total = 0;
	int rc = 0;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in open completion\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	hello_context->blob = blob;
	free = spdk_bs_free_cluster_count(hello_context->bs);
	SPDK_NOTICELOG("blobstore has FREE clusters of %" PRIu64 "\n",
		       free);

	/*
	 * before we can use our new blob, we have to resize it
	 * as the initial size is 0. For this example we'll use the
	 * full size of the blobstore but it would be expected that
	 * there'd usually be many blobs of various sizes. The resize
	 * unit is a cluster.
	 */
	rc = spdk_bs_md_resize_blob(hello_context->blob, free);
	if (rc) {
		SPDK_ERRLOG("Error %d trying to rezie blob\n", rc);
		spdk_app_stop(rc);
		return;
	}

	total = spdk_blob_get_num_clusters(hello_context->blob);
	SPDK_NOTICELOG("resized blob now has USED clusters of %" PRIu64 "\n",
		       total);

	/* blob has been created & sized, lets write to it */
	blob_write(hello_context);
}

/*
 * callback function for creating a blob
 */
static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct hello_context_t *hello_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d blob create callback\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}

	hello_context->blobid = blobid;
	SPDK_NOTICELOG("new blob id %" PRIu64 "\n", hello_context->blobid);

	/* we have to open the blob before we can do things like resize */
	spdk_bs_md_open_blob(hello_context->bs, hello_context->blobid,
			     open_complete, hello_context);
}

/*
 * function for creating a blob
 */
static void
create_blob(struct hello_context_t *hello_context)
{
	SPDK_NOTICELOG("entry\n");
	spdk_bs_md_create_blob(hello_context->bs, blob_create_complete,
			       hello_context);
}

/*
 * callback function for initializing a blob
 */
static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
	struct hello_context_t *hello_context = cb_arg;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d init'ing the bobstore\n", bserrno);
		spdk_app_stop(bserrno);
		return;
	}
	hello_context->bs = bs;
	SPDK_NOTICELOG("blobstore: %p\n", hello_context->bs);
	/*
	 * we will use the page size in allocating buffers, etc., later
	 * so we'll just save it in out context buffer here
	 */
	hello_context->page_size = spdk_bs_get_page_size(hello_context->bs);

	/*
	 * the blostore has been initialized, lets create a blob.
	 * Note that we could allcoate an SPDK event and use
	 * spdk_event_call() to schedule it if we wanted to keep
	 * our events as limited as possible wrt the amount of
	 * work that they do.
	 */
	create_blob(hello_context);
}

/*
 * our initial event that kicks off everything from main
 */
static void
hello_start(void *arg1, void *arg2)
{
	struct hello_context_t *hello_context = arg1;
	struct spdk_bdev *bdev = NULL;
	struct spdk_bs_dev *bs_dev = NULL;

	SPDK_NOTICELOG("entry\n");
	/*
	 * get the bdev. for this example it is our malloc (RAM)
	 * disk configured via hello_blob.conf that was passed
	 * in when we started the SPDK app framework so we can
	 * get it via it's name.
	 */
	bdev = spdk_bdev_get_by_name("Malloc0");
	if (bdev == NULL) {
		SPDK_ERRLOG("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}

	/*
	 * spdk_bs_init() requires us to fill out the structure
	 * spdk_bs_dev with a set of callbacks. These callbacks
	 * implement read, write, and other operations on the
	 * underlying disks. As a convenience, a utility function
	 * is provided that creates an spdk_bs_dev that implements
	 * all of the callbacks by forwarding the I/O to the
	 * SPDK bdev layer. Other helper functions are also
	 * available in the blob lib in blob_bdev.c that simply
	 * make it easier to layer blobstore on top of a bdev.
	 * However blobstore can be more tightly integrated into
	 * any lower layer, such as NVMe for example.
	 */
	bs_dev = spdk_bdev_create_bs_dev(bdev);
	if (bs_dev == NULL) {
		SPDK_ERRLOG("Could not create blob bdev!!\n");
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_init(bs_dev, NULL, bs_init_complete, hello_context);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context_t *hello_context = NULL;

	SPDK_NOTICELOG("entry\n");

	/* set default values in opts structure */
	spdk_app_opts_init(&opts);

	/*
	 * setup a few specifics beforew we init, for most SPDK cmd line
	 * apps, the config file will be passed in as an arg but to make
	 * this example super simple we just hardcode it. We also need to
	 * specify a name for the app.
	 */
	opts.name = "hello_blob";
	opts.config_file = "hello_blob.conf";


	/*
	 * now we'll allocate and intialize the blobstore itself. we
	 * can pass in an spdk_bs_opts if we want something other than
	 * the defaults (cluster size, etc), but here we'll just take the
	 * defaults.  We'll also pass in a struct that we'll use for
	 * callbacks so we've got efficient bookeeping of what we're
	 * creating. This is an async operation and bs_init_complete()
	 * will be called when its complete.
	 */
	hello_context = malloc(sizeof(struct hello_context_t));
	if (hello_context != NULL) {
		/*
		 * spdk_app_start() will block running hello_start() until
		 * spdk_app_stop() is called by someone (not simply when
		 * hello_start() returns)
		 */
		rc = spdk_app_start(&opts, hello_start, hello_context, NULL);
		if (rc) {
			SPDK_ERRLOG("Something went wrong!\n");
			hello_cleanup(hello_context);
		} else {
			SPDK_NOTICELOG("SUCCCESS!\n");
		}
	} else {
		SPDK_ERRLOG("Could not alloc hello_context struct!!\n");
		rc = -ENOMEM;
	}

	/* gracfully close out all of the SPDK subsystems */
	spdk_app_fini();
	return rc;
}
