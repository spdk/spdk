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

/* size of the data transfer buffer we'll use for write/read/compare test */
#define BUFF_SZ 4096

void hello_start(void *arg1, void *arg2);

/*
 * we'll use this struct to gather housekeeping info to pass between
 * our events and callbacks
 */
struct my_blob_info {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	uint8_t *read_buff;
	uint8_t *write_buff;
};

/*
 * callback routine for the blobstore unload
 */
static void
unload_complete(void *cb_arg, int bserrno)
{
	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d unloading the bobstore\n", bserrno);
		spdk_app_stop(-1);
		return;
	}
	spdk_app_stop(0);
}

/*
 * callback routine for the deletion of a blob
 */
static void
delete_complete(void *arg1, int bserrno)
{
	struct my_blob_info *info = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in delete completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/* we're all done, we can unload the blobstore */
	spdk_bs_unload(info->bs, unload_complete, NULL);
	free(info);
}

/*
 * function for deleting a blob
 */
static void
delete_blob(void *arg1, int bserrno)
{
	struct my_blob_info *info = arg1;

	SPDK_NOTICELOG("entry\n");
	spdk_bs_md_delete_blob(info->bs, info->blobid,
			       delete_complete, info);
}

/*
 * callback function for reading a blob
 */
static void
read_complete(void *arg1, int bserrno)
{
	struct my_blob_info *info = arg1;
	int match_res = -1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in read completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/* now lets make sure things match */
	match_res = memcmp(info->write_buff, info->read_buff, BUFF_SZ);
	if (match_res) {
		SPDK_ERRLOG("Error in read completion, buffers don't match\n");
		spdk_app_stop(-1);
		return;
	} else {
		SPDK_NOTICELOG("read SUCCESS and data matches!\n");
	}

	/* done with our read buffers and the channel */
	spdk_dma_free(info->write_buff);
	spdk_dma_free(info->read_buff);
	spdk_bs_free_io_channel(info->channel);

	/* now lets close it and delete the blob in the callback */
	spdk_bs_md_close_blob(&info->blob, delete_blob, info);
}

/*
 * function for reading a blob
 */
static void
read_blob(struct my_blob_info *info)
{
	SPDK_NOTICELOG("entry\n");

	info->read_buff = spdk_dma_malloc(BUFF_SZ, (1024 * 1024), NULL);
	if (info->read_buff == NULL) {
		SPDK_ERRLOG("Error trying to alocate read buffer\n");
		spdk_app_stop(-1);
		return;
	}

	/* issue the read and compare the results in the callback */
	spdk_bs_io_read_blob(info->blob, info->channel, info->read_buff,
			     0, 1, read_complete, info);
}

/*
 * callback function for writing a blob
 */
static void
write_complete(void *arg1, int bserrno)
{
	struct my_blob_info *info = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in write completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/* now lets read back what we wrote and make sure it matches */
	read_blob(info);
}

/*
 * function for writing to a blob
 */
static void
blob_write(struct my_blob_info *info)
{
	SPDK_NOTICELOG("entry\n");

	/* buffers for data transfer need to be allocated via SPDK */
	info->write_buff = spdk_dma_malloc(BUFF_SZ, (1024 * 1024), NULL);
	if (info->write_buff == NULL) {
		SPDK_ERRLOG("Error trying to alocate a channel\n");
		spdk_app_stop(-1);
		return;
	}
	memset(info->write_buff, 0x5a, BUFF_SZ);

	/* now we have to allocate a channel */
	info->channel = spdk_bs_alloc_io_channel(info->bs);
	if (info->channel == NULL) {
		SPDK_ERRLOG("Error trying to alocate a channel\n");
		spdk_app_stop(-1);
		return;
	}

	/* lets perform the write */
	spdk_bs_io_write_blob(info->blob, info->channel, info->write_buff,
			      0, 1, write_complete, info);
}

/*
 * callback function for opening a blob
 */
static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct my_blob_info *info = cb_arg;
	uint64_t free = 0;
	uint64_t total = 0;
	int rc = 0;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in open completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	info->blob = blob;
	free = spdk_bs_free_cluster_count(info->bs);
	SPDK_NOTICELOG("blobstore and has FREE clusters of %" PRIu64 "\n",
		       free);

	/*
	 * before we can use our new blob, we have to resize it
	 * as the initial size is 0. For this example we'll use the
	 * full size of the blobstore but it would be expected that
	 * there'd usually be many blobs of various sizes. The resize
	 * unit is a cluster.
	 */
	rc = spdk_bs_md_resize_blob(info->blob, free);
	if (rc) {
		SPDK_ERRLOG("Error %d trying to rezie blob\n", rc);
		spdk_app_stop(-1);
		return;
	}

	total = spdk_blob_get_num_clusters(info->blob);
	SPDK_NOTICELOG("resized blob now has USED clusters of %" PRIu64 "\n",
		       total);

	/* blob has been created & sized, lets write to it */
	blob_write(info);
}

/*
 * callback function for creating a blob
 */
static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct my_blob_info *info = arg1;

	SPDK_NOTICELOG("entry\n");
	info->blobid = blobid;
	SPDK_NOTICELOG("new blob id %" PRIu64 "\n", info->blobid);

	/* we have to open the blob before we can do things like resize */
	spdk_bs_md_open_blob(info->bs, info->blobid, open_complete, info);
}

/*
 * function for creating a blob
 */
static void
create_blob(struct my_blob_info *info)
{
	SPDK_NOTICELOG("entry\n");
	spdk_bs_md_create_blob(info->bs, blob_create_complete, info);
}

/*
 * callback function for initializing a blob
 */
static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
	struct my_blob_info *info = cb_arg;

	SPDK_NOTICELOG("entry\n");
	info->bs = bs;
	SPDK_NOTICELOG("blobstore: %p\n", info->bs);
	if (bserrno) {
		SPDK_ERRLOG("Error %d init'ing the bobstore\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/*
	 * the blostore has been initialized, lets create a blob.
	 * Note that we could allcoate an SPDK event and use
	 * spdk_event_call() to schedule it if we wanted to keep
	 * our events as limited as possible wrt the amount of
	 * work that they do.
	 */
	create_blob(info);
}

/*
 * our initial event that kicks off everything from main
 */
void
hello_start(void *arg1, void *arg2)
{
	struct my_blob_info *info = NULL;
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
	 * available in the blob lib in blod_bdev.c that simply
	 * make it easuer to layer blobstore on top of a bdev
	 * however blosbstore can be more tightly integrated into
	 * any lower layer, such as NVMe for example.
	 *
	 * here we are using a helper function to provide easy
	 * access to the underlying bdev, its descriptor, and
	 * function pointers for read, write, etc.
	 */
	bs_dev = spdk_bdev_create_bs_dev(bdev);
	if (bs_dev == NULL) {
		SPDK_ERRLOG("Could not create blob bdev!!\n");
		spdk_app_stop(-1);
		return;
	}

	/*
	 * now we'll allocate and intialize the blobstore itself. we
	 * can pass in an spdk_bs_opts if we want something other than
	 * the defaults (cluster size, etc), here we'll just take the
	 * defaults.  We'll also pass in a struct that we'll use for
	 * callbacks so we've got efficient bookeeping of what we're
	 * creating. This is an async operation and bs_init_complete()
	 * will be called when its complete.
	 */
	info = malloc(sizeof(struct my_blob_info));
	if (info == NULL) {
		SPDK_ERRLOG("Could not alloc info struct!!\n");
		spdk_app_stop(-1);
		return;
	}
	spdk_bs_init(bs_dev, NULL, bs_init_complete, info);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	SPDK_NOTICELOG("entry\n");

	/* set default values in opts structure */
	spdk_app_opts_init(&opts);

	/*
	 * setup a few obvious specifics, for most SPDK cmd line
	 * apps, the config file will be passed in as an arg but
	 * to make this example super simple we just hardcode it
	 */
	opts.name = "hello_blob";
	opts.config_file = "hello_blob.conf";

	/*
	 * spdk_app_start() will block running hello_start() until
	 * spdk_app_stop() is called by someone (not simply when
	 * hello_start() returns)
	 */
	rc = spdk_app_start(&opts, hello_start, NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("Something went wrong!\n");
	} else {
		SPDK_NOTICELOG("SUCCCESS!\n");
	}

	/* gracfully close out all of the SPDK subsystems */
	spdk_app_fini();
	return rc;
}
