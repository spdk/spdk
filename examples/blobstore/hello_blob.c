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

#include "hello_blob.h"

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
 * our cleanup routine will start by unloading the blobstore
 * and then invoke spdk_app_stop() to unblock in main()
 * via the callback unload_complete()
 */
void
hello_cleanup(void *arg1, void *arg2)
{
	struct spdk_blob_store *bs = arg1;

	SPDK_NOTICELOG("entry\n");
	spdk_bs_unload(bs, unload_complete, NULL);
}

static void
delete_complete(void *arg1, int bserrno)
{
	struct del_blob *kill_it = arg1;
	struct spdk_blob_store *bs = kill_it->bs;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in delete completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}
	free(kill_it);
	hello_cleanup(bs, NULL);
}

static void
delete_blob(void *arg1, int bserrno)
{
	struct del_blob *kill_it = arg1;

	SPDK_NOTICELOG("entry\n");
	spdk_bs_md_delete_blob(kill_it->bs, kill_it->blobid,
			       delete_complete, kill_it);
}

static void
read_complete(void *arg1, int bserrno)
{
	struct read_comp *cb_arg = arg1;
	struct del_blob *kill_it = NULL;
	struct spdk_blob *blob = cb_arg->blob;
	int8_t *buff = cb_arg->buff;
	int8_t match[BUFF_SZ];
	int match_res = -1;

	SPDK_NOTICELOG("entry\n");
	memset(&match, 0x5a, BUFF_SZ);
	if (bserrno) {
		SPDK_ERRLOG("Error %d in read completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/* now lets make sure things match */
	match_res = memcmp(&match, buff, BUFF_SZ);
	if (match_res) {
		SPDK_ERRLOG("Error in read completion, buffers don't match\n");
		spdk_app_stop(-1);
		return;
	} else {
		SPDK_NOTICELOG("read SUCCESS and data matches!\n");
	}

	/* done with our read buffers */
	spdk_dma_free(buff);
	free(cb_arg);

	/*
	 * now lets close it and delete the blob in the callback, after
	 * its closed we won't have the blobid handy anymore so we'll
	 * pass it along with bs* into the callback
	 */
	kill_it = malloc(sizeof(struct del_blob));
	kill_it->bs = blob->bs;
	kill_it->blobid = blob->id;
	spdk_bs_md_close_blob(&blob, delete_blob, kill_it);
}

static void
read_blob(struct spdk_blob *blob)
{
	struct spdk_io_channel *channel = NULL;
	uint8_t *payload;
	struct read_comp *cb_arg;

	SPDK_NOTICELOG("entry\n");

	payload = spdk_dma_malloc(BUFF_SZ, (1024 * 1024), NULL);
	cb_arg = malloc(sizeof(struct read_comp));

	if ((payload == NULL) || (cb_arg == NULL)) {
		SPDK_ERRLOG("Error trying to alocate read buffers\n");
		spdk_app_stop(-1);
		return;
	}

	/* first we have to allocate a channel */
	channel = spdk_bs_alloc_io_channel(blob->bs);
	if (channel == NULL) {
		SPDK_ERRLOG("Error trying to alocate a channel\n");
		spdk_app_stop(-1);
		return;
	}

	/*
	 * we need both the blob* as well as the payload * in the
	 * completion so we can compare and free the payload buffer
	 */
	cb_arg->buff = payload;
	cb_arg->blob = blob;
	spdk_bs_io_read_blob(blob, channel, payload, 0, 1, read_complete, cb_arg);
	spdk_bs_free_io_channel(channel); // TODO: here or in the callback?
}

static void
write_complete(void *arg1, int bserrno)
{
	struct spdk_blob *blob = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in write completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/* now lets read back what we wrote and make sure it matches */
	read_blob(blob);
}

/*
 * write some data pattern to our new blob
 */
static void
blob_write(struct spdk_blob *blob)
{
	struct spdk_io_channel *channel = NULL;
	uint8_t payload[BUFF_SZ];

	SPDK_NOTICELOG("entry\n");
	memset(&payload, 0x5a, BUFF_SZ);

	/* first we have to allocate a channel */
	channel = spdk_bs_alloc_io_channel(blob->bs);
	if (channel == NULL) {
		SPDK_ERRLOG("Error trying to alocate a channel\n");
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_io_write_blob(blob, channel, payload, 0, 1, write_complete, blob);
	spdk_bs_free_io_channel(channel); // TODO: safe to free this here or need to be in in callback?
}

static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	uint64_t free = 0;
	int rc = 0;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d in open completion\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	free = spdk_bs_free_cluster_count(blob->bs);
	SPDK_NOTICELOG("blob opened and has free clusters of %" PRIu64 "\n", free);

	/*
	 * before we can use our new blob, we have to resize it
	 * as the initial size is 0
	 */
	rc = spdk_bs_md_resize_blob(blob, free);
	if (rc) {
		SPDK_ERRLOG("Error %d trying to rezie blob\n", rc);
		spdk_app_stop(-1);
		return;
	}
	free = spdk_bs_free_cluster_count(blob->bs);
	SPDK_NOTICELOG("resized blob now has free clusters of %" PRIu64 "\n", free);

	/* blob has been created & sized, lets write to it */
	blob_write(blob);
}

/*
 * create our blob and open it, the continue in the open callback
 */
static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct spdk_blob_store *bs = arg1;

	SPDK_NOTICELOG("entry\n");
	SPDK_NOTICELOG("new blob id %" PRIu64 "\n", blobid);

	/* we have to open the blob before we can do things like resize */
	spdk_bs_md_open_blob(bs, blobid, open_complete, NULL);
}

/*
 * creates our blob, upon completion we'll write to it
 */
static void
create_blob(void *arg1, void *arg2)
{
	struct spdk_blob_store *bs = arg1;

	SPDK_NOTICELOG("entry\n");
	spdk_bs_md_create_blob(bs, blob_create_complete, bs);
}

static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
	struct spdk_event *event;

	SPDK_NOTICELOG("entry\n");
	SPDK_NOTICELOG("blobstore: %p\n", bs);
	if (bserrno) {
		SPDK_ERRLOG("Error %d init'ing the bobstore\n", bserrno);
		spdk_app_stop(-1);
		return;
	}

	/* the blostore has been initialized, lets create a blob */
	event = spdk_event_allocate(0, create_blob, bs, NULL);
	spdk_event_call(event);
}

void
hello_start(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev = NULL;
	struct spdk_bs_dev *bs_dev = NULL;

	SPDK_NOTICELOG("entry\n");
	/*
	 * grab the first configured bdev. for this example it is
	 * our malloc (RAM) disk configured via hello_blob.conf that
	 * was passed in when we started the SPDK app framework.
	 */
	bdev = spdk_bdev_first_leaf();
	if (bdev == NULL) {
		SPDK_ERRLOG("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}

	/*
	 * helper functions are available in the blob lib in
	 * blod_bdev.c that simply make it easuer to layer
	 * blobstore on top of a bdev however blosbstore can
	 * be more tightly integrated into any lower layer, such
	 * as NVMe for example
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
	 * defaults
	 */
	spdk_bs_init(bs_dev, NULL, bs_init_complete, NULL);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	const char *config_file = "./hello_blob.conf";

	SPDK_NOTICELOG("entry\n");

	/* set default values in opts structure */
	spdk_app_opts_init(&opts);

	/*
	 * setup a few obvious specifics, for most SPDK cmd line
	 * apps, the config file will be passed in as an arg but
	 * to make this example super simple we just hardcode it
	 */
	opts.name = "hello_blob";
	opts.config_file = config_file;

	/*
	 * spdk_app_start() will block running hello_start() until
	 * spdk_app_stop() is called by someone
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
