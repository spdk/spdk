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
 * This is not a public header file, but the CLI does expose
 * some internals of blobstore for dev/debug puposes so we
 * include it here.
 */
#include "../lib/blob/blobstore.h"

static const char *program_name = "blobcli";
static const char *program_conf = "blobcli.conf";
static const char *bdev_name = "Nvme0n1";
static const char *ver = "0.0";

enum cli_action_type {
	CLI_IMPORT,
	CLI_DUMP,
	CLI_FILL,
	CLI_DEL_XATTR,
	CLI_SET_XATTR,
	CLI_SET_SUPER,
	CLI_SHOW_BS,
	CLI_SHOW_BLOB,
	CLI_CREATE_BLOB,
	CLI_LIST_BDEVS,
	CLI_LIST_BLOBS,
	CLI_INIT_BS
};
#define BUFSIZE 255

/* todo, scrub this as there may be some extra junk in here picked up along the way... */
struct cli_context_t {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	spdk_blob_id superid;
	struct spdk_io_channel *channel;
	uint8_t *buff;
	uint64_t page_size;
	uint64_t page_count;
	uint64_t blob_pages;
	FILE *fp;
	enum cli_action_type action;
	char key[BUFSIZE + 1];
	char value[BUFSIZE + 1];
	char file[BUFSIZE + 1];
	uint64_t filesize;
	int fill_value;
	const char *bdev_name;
	int rc;
	int num_clusters;
	void (*nextFunc)(void *arg1, struct spdk_blob_store *bs, int bserrno);
};

/*
 * Prints usage and relevant error message.
 */
static void
usage(char *msg)
{
	if (msg) {
		printf("%s", msg);
	}
	printf("\nversion %s\n", ver);
	printf("Usage: %s [-c SPDK config_file] Command\n", program_name);
	printf("\n");
	printf("%s is a command line tool for interacting with blobstore\n",
	       program_name);
	printf("on the underlying device specified in the conf file passed\n");
	printf("in as a command line option.\n");
	printf("\nCommands include:\n");
	printf("\t-i <bs name> - initialize a blobstore\n");
	printf("\t-l bdevs | blobs - list either available bdevs or existing blobs\n");
	printf("\t-n <# clusters> - create new blob\n");
	printf("\t-p <blobid> - set the superblob to the ID provided\n");
	printf("\t-s <blobid> | bs - show blob info or blobstore info\n");
	printf("\t-x <blobid> name value - set xattr name/value pair\n");
	printf("\t-k <blobid> name - delete xattr name/value pair\n");
	printf("\t-f <blobid> value - fill a blob with a decimal value\n");
	printf("\t-d <blobid> filename - dump contents of a blob to a file\n");
	printf("\t-m <blobid> filename - import contents of a file to a blob\n");
	printf("\n");
}

/*
 * Free up memory that we allocated.
 */
static void
cli_cleanup(struct cli_context_t *cli_context)
{
	if (cli_context->buff) {
		spdk_dma_free(cli_context->buff);
	}
	if (cli_context->channel) {
		spdk_bs_free_io_channel(cli_context->channel);
	}
	free(cli_context);
}

/*
 * Callback routine for the blobstore unload.
 */
static void
unload_complete(void *cb_arg, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		SPDK_ERRLOG("Error %d unloading the bobstore\n", bserrno);
		cli_context->rc = bserrno;
	}

	spdk_app_stop(cli_context->rc);
}

/*
 * Unload the blobstore.
 */
static void
unload_bs(struct cli_context_t *cli_context, char *msg, int bserrno)
{
	if (bserrno) {
		SPDK_ERRLOG("%s (err %d)\n", msg, bserrno);
		cli_context->rc = bserrno;
	}
	if (cli_context->bs) {
		spdk_bs_unload(cli_context->bs, unload_complete, cli_context);
	} else {
		spdk_app_stop(bserrno);
	}
}

/*
 * Callback for closing a blob.
 */
static void
close_cb(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in close callback",
			  bserrno);
		return;
	}
	unload_bs(cli_context, "", 0);
}

/*
 * Callback function for sync'ing metadata.
 */
static void
sync_complete(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in sync callback",
			  bserrno);
		return;
	}

	spdk_bs_md_close_blob(&cli_context->blob, close_cb,
			      cli_context);
}

/*
 * Callback function for opening a blob after creating.
 */
static void
open_now_resize(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;
	int rc = 0;
	uint64_t total = 0;

	if (bserrno) {
		unload_bs(cli_context, "Error in open completion",
			  bserrno);
		return;
	}
	cli_context->blob = blob;

	rc = spdk_bs_md_resize_blob(cli_context->blob,
				    cli_context->num_clusters);
	if (rc) {
		unload_bs(cli_context, "Error in blob resize",
			  bserrno);
		return;
	}

	total = spdk_blob_get_num_clusters(cli_context->blob);
	SPDK_NOTICELOG("blob now has USED clusters of %" PRIu64 "\n",
		       total);

	/*
	 * Always a good idea to sync after MD changes or the changes
	 * may be lost if things aren't closed cleanly.
	 */
	spdk_bs_md_sync_blob(cli_context->blob, sync_complete,
			     cli_context);
}

/*
 * Callback function for creating a blob.
 */
static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob create callback",
			  bserrno);
		return;
	}

	cli_context->blobid = blobid;
	SPDK_NOTICELOG("New blob id %" PRIu64 "\n", cli_context->blobid);

	/* We have to open the blob before we can do things like resize. */
	spdk_bs_md_open_blob(cli_context->bs, cli_context->blobid,
			     open_now_resize, cli_context);
}

/*
 * Callback for get_super where we'll continue on to show blobstore info.
 */
static void
show_bs(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	uint64_t val;
	struct spdk_bdev *bdev = NULL;

	if (bserrno && bserrno != -ENOENT) {
		unload_bs(cli_context, "Error in get_super callback",
			  bserrno);
		return;
	}
	cli_context->superid = blobid;

	bdev = spdk_bdev_get_by_name(cli_context->bdev_name);
	if (bdev == NULL) {
		unload_bs(cli_context, "Error w/bdev in get_super callback",
			  bserrno);
		return;
	}

	SPDK_NOTICELOG("Blobstore Public Info:\n");
	SPDK_NOTICELOG("\tUsing Bdev Product Name: %s\n",
		       spdk_bdev_get_product_name(bdev));
	SPDK_NOTICELOG("\tAPI Version: %d\n", SPDK_BS_VERSION);

	if (bserrno != -ENOENT) {
		SPDK_NOTICELOG("\tsuper blob ID: %" PRIu64 "\n", cli_context->superid);
	} else {
		SPDK_NOTICELOG("\tsuper blob ID: none assigned\n");
	}

	val = spdk_bs_get_page_size(cli_context->bs);
	SPDK_NOTICELOG("\tpage size: %" PRIu64 "\n", val);

	val = spdk_bs_get_cluster_size(cli_context->bs);
	SPDK_NOTICELOG("\tcluster size: %" PRIu64 "\n", val);

	val = spdk_bs_free_cluster_count(cli_context->bs);
	SPDK_NOTICELOG("\t# free clusters: %" PRIu64 "\n", val);

	/*
	 * Private info isn't accessible via the public API but
	 * may be useful for debug of blobstore based applications.
	 */
	SPDK_NOTICELOG("\n");
	SPDK_NOTICELOG("Blobstore Private Info:\n");
	SPDK_NOTICELOG("\tMetadata start (pages): %" PRIu64 "\n",
		       cli_context->bs->md_start);
	SPDK_NOTICELOG("\tMetadata length (pages): %d \n",
		       cli_context->bs->md_len);

	unload_bs(cli_context, "", 0);
}

/*
 * Load callback where we'll get the super blobid next.
 */
static void
get_super_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load blob callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	spdk_bs_get_super(cli_context->bs, show_bs, cli_context);
}

/*
 * Callback for load bs where we'll continue on to create a blob.
 */
static void
create_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	spdk_bs_md_create_blob(cli_context->bs, blob_create_complete,
			       cli_context);
}

/*
 * Show detailed info about a particular blob.
 */
static void
show_blob(struct cli_context_t *cli_context)
{
	uint64_t val;
	struct spdk_xattr_names *names;
	const void *value;
	size_t value_len;
	char data[256];
	unsigned int i;

	SPDK_NOTICELOG("Blob Public Info:\n");

	SPDK_NOTICELOG("\tBlob ID: %" PRIu64 "\n", cli_context->blobid);

	val = spdk_blob_get_num_clusters(cli_context->blob);
	SPDK_NOTICELOG("\t# of clusters: %" PRIu64 "\n", val);

	SPDK_NOTICELOG("\t# of bytes: %" PRIu64 "\n",
		       val * spdk_bs_get_cluster_size(cli_context->bs));

	val = spdk_blob_get_num_pages(cli_context->blob);
	SPDK_NOTICELOG("\t# of pages: %" PRIu64 "\n", val);

	spdk_bs_md_get_xattr_names(cli_context->blob, &names);

	SPDK_NOTICELOG("\t# of xattrs: %d\n", spdk_xattr_names_get_count(names));
	SPDK_NOTICELOG("\txattrs:\n");
	for (i = 0; i < spdk_xattr_names_get_count(names); i++) {
		spdk_bs_md_get_xattr_value(cli_context->blob,
					   spdk_xattr_names_get_name(names, i),
					   &value, &value_len);
		memcpy(&data, value, value_len);
		data[(int)value_len] = '\0';
		SPDK_NOTICELOG("\t\t %s: %s\n",
			       spdk_xattr_names_get_name(names, i),
			       data);
	}

	/*
	 * Private info isn't accessible via the public API but
	 * may be useful for debug of blobstore based applications.
	 */
	SPDK_NOTICELOG("\n");
	SPDK_NOTICELOG("Blob Private Info:\n");
	switch (cli_context->blob->state) {
	case SPDK_BLOB_STATE_DIRTY:
		SPDK_NOTICELOG("\tstate: DIRTY\n");
		break;
	case SPDK_BLOB_STATE_CLEAN:
		SPDK_NOTICELOG("\tstate: CLEAN\n");
		break;
	case SPDK_BLOB_STATE_LOADING:
		SPDK_NOTICELOG("\tstate: LOADING\n");
		break;
	case SPDK_BLOB_STATE_SYNCING:
		SPDK_NOTICELOG("\tstate: SYNCING\n");
		break;
	default:
		SPDK_NOTICELOG("\tstate: UNKNOWN\n");
		break;
	}
	SPDK_NOTICELOG("\topen ref count: %d\n",
		       cli_context->blob->open_ref);
}

/*
 * Callback for getting the first blob.
 */
static void
blob_iter_cb(void *arg1, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		if (bserrno == -ENOENT) {
			/* this simply means there are no more blobs */
			unload_bs(cli_context, "", 0);
		} else {
			unload_bs(cli_context, "Error in blob iter callback",
				  bserrno);
		}
		return;
	}

	if (cli_context->action == CLI_LIST_BLOBS) {
		/* just listing blobs */
		SPDK_NOTICELOG("Found blob with ID# %" PRIu64 "\n",
			       spdk_blob_get_id(blob));
	} else if (spdk_blob_get_id(blob) == cli_context->blobid) {
		/*
		 * Found the blob we're looking for, but we need to finish
		 * iteratting even after showing the info so that internally
		 * the blobstore logic will close the blob. Or we could
		 * chose to close it now, either way.
		 */
		cli_context->blob = blob;
		show_blob(cli_context);
	}

	spdk_bs_md_iter_next(cli_context->bs, &blob, blob_iter_cb,
			     cli_context);
}

/*
 * Callback for load bs where we'll continue on to list all blobs.
 */
static void
list_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	if (cli_context->action == CLI_LIST_BLOBS) {
		printf("\n");
		SPDK_NOTICELOG("List BLOBS:\n");
	}

	spdk_bs_md_iter_first(cli_context->bs, blob_iter_cb, cli_context);
}

/*
 * Callback for setting the super blob ID.
 */
static void
set_super_cb(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in set_super callback",
			  bserrno);
		return;
	}

	SPDK_NOTICELOG("Super Blob ID has been set.\n");
	unload_bs(cli_context, "", 0);
}

/*
 * Callback for load bs where we'll continue on to set the super blob.
 */
static void
set_super_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	spdk_bs_set_super(cli_context->bs, cli_context->superid,
			  set_super_cb, cli_context);
}

/*
 * Callback for set_xattr_open where we set or delete xattrs.
 */
static void
set_xattr(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob open callback",
			  bserrno);
		return;
	}
	cli_context->blob = blob;

	if (cli_context->action == CLI_SET_XATTR) {
		spdk_blob_md_set_xattr(cli_context->blob,
				       cli_context->key,
				       cli_context->value,
				       strlen(cli_context->value) + 1);
		SPDK_NOTICELOG("Xattr has been set.\n");
	} else {
		spdk_blob_md_remove_xattr(cli_context->blob,
					  cli_context->key);
		SPDK_NOTICELOG("Xattr has been deleted.\n");
	}

	spdk_bs_md_sync_blob(cli_context->blob, sync_complete,
			     cli_context);
}

/*
 * Callback for load bs where we'll continue on to set/del an xattr.
 */
static void
xattr_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	spdk_bs_md_open_blob(cli_context->bs, cli_context->blobid,
			     set_xattr, cli_context);
}

/*
 * Callback function for reading a blob for dumping to a file.
 */
static void
read_dump_complete(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	int rc;

	if (bserrno) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error in read completion",
			  bserrno);
		return;
	}

	rc = fwrite(cli_context->buff, cli_context->page_size,
		    1 , cli_context->fp);
	/*
	 * so this can't happen but if we don't look at and use the
	 * the return value, one of two checks triggers in CI and
	 * fails things. This will either be 0 if the file size is
	 * less than 1 page or 1 if => 1 page size.
	 */
	if (rc > 1) {
		unload_bs(cli_context, "Error in fwrite",
			  -ENOMEM);
		return;
	}
	printf(".");
	if (++cli_context->page_count < cli_context->blob_pages) {
		/* perform another read */
		spdk_bs_io_read_blob(cli_context->blob, cli_context->channel,
				     cli_context->buff, cli_context->page_count,
				     1, read_dump_complete, cli_context);
	} else {
		/* done reading */
		printf("\n");
		SPDK_NOTICELOG("File write complete.\n");
		fclose(cli_context->fp);
		spdk_bs_md_close_blob(&cli_context->blob, close_cb,
				      cli_context);
	}
}

static void
write_dump_complete(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	int rc;

	if (bserrno) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error in write completion",
			  bserrno);
		return;
	}


	if ((++cli_context->page_count * cli_context->page_size) <
	    cli_context->filesize) {
		/* perform another read */
		printf(".");
		rc = fread(cli_context->buff, cli_context->page_size,
			   1, cli_context->fp);
		/*
		 * so this can't happen but if we don't look at and use the
		 * the return value, one of two checks triggers in CI and
		 * fails things. This will either be 0 if the file size is
		 * less than 1 page or 1 if => 1 page size.
		 */
		if (rc > 1) {
			unload_bs(cli_context, "Error in fread",
				  -ENOMEM);
			return;
		}
	} else {
		/*
		 * Done reading the file, fill the rest of the blob with 0s,
		 * yeah we're memsetting the same page over and over here
		 */
		memset(cli_context->buff, 0, cli_context->page_size);
	}
	if (cli_context->page_count < cli_context->blob_pages) {
		spdk_bs_io_write_blob(cli_context->blob, cli_context->channel,
				      cli_context->buff, cli_context->page_count,
				      1, write_dump_complete, cli_context);
	} else {
		/* done writing */
		printf("\n");
		SPDK_NOTICELOG("Blob import complete.\n");
		fclose(cli_context->fp);
		spdk_bs_md_close_blob(&cli_context->blob, close_cb,
				      cli_context);
	}
}

/*
 * Callback for open blobs where we'll continue on dump a blob to a file.
 */
static void
dump_open_cb(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;
	int rc;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob open callback",
			  bserrno);
		return;
	}
	cli_context->blob = blob;
	cli_context->page_size = spdk_bs_get_page_size(cli_context->bs);
	cli_context->channel = spdk_bs_alloc_io_channel(cli_context->bs);
	if (cli_context->channel == NULL) {
		unload_bs(cli_context, "Error in allocating channel",
			  -ENOMEM);
		return;
	}

	/*
	 * We'll transfer just one page at a time to keep the buffer
	 * small. This could be bigger of course.
	 */
	cli_context->buff = spdk_dma_malloc(cli_context->page_size,
					    0x1000, NULL);
	if (cli_context->buff == NULL) {
		unload_bs(cli_context, "Error in allocating memory",
			  -ENOMEM);
		return;
	}

	cli_context->blob_pages = spdk_blob_get_num_pages(cli_context->blob);
	cli_context->page_count = 0;
	printf("\n");
	if (cli_context->action == CLI_DUMP) {
		cli_context->fp = fopen(cli_context->file , "w");
		/* get the filesize then rewind and perform 1 page read */
		fseek(cli_context->fp, 0L, SEEK_END);
		cli_context->filesize = ftell(cli_context->fp);
		rewind(cli_context->fp);

		spdk_bs_io_read_blob(cli_context->blob, cli_context->channel,
				     cli_context->buff, cli_context->page_count,
				     1, read_dump_complete, cli_context);
	} else {
		cli_context->fp = fopen(cli_context->file , "r");

		/* get the filesize then rewind and perform 1 page read */
		fseek(cli_context->fp, 0L, SEEK_END);
		cli_context->filesize = ftell(cli_context->fp);
		rewind(cli_context->fp);
		rc = fread(cli_context->buff, cli_context->page_size,
			   1, cli_context->fp);
		/*
		 * so this can't happen but if we don't look at and use the
		 * the return value, one of two checks triggers in CI and
		 * fails things. This will either be 0 if the file size is
		 * less than 1 page or 1 if => 1 page size.
		 */
		if (rc > 1) {
			unload_bs(cli_context, "Error in fread",
				  -ENOMEM);
			return;
		}
		spdk_bs_io_write_blob(cli_context->blob, cli_context->channel,
				      cli_context->buff, cli_context->page_count,
				      1, write_dump_complete, cli_context);
	}
}

/*
 * Callback for load bs where we'll continue on dump a blob to a file.
 */
static void
dump_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	spdk_bs_md_open_blob(cli_context->bs, cli_context->blobid,
			     dump_open_cb, cli_context);
}

/*
 * Callback function for writing a specific pattern to page 0.
 */
static void
write_complete(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in write completion",
			  bserrno);
		return;
	}
	printf(".");
	if (++cli_context->page_count < cli_context->blob_pages) {
		spdk_bs_io_write_blob(cli_context->blob, cli_context->channel,
				      cli_context->buff, cli_context->page_count,
				      1, write_complete, cli_context);
	} else {
		/* done writing */
		printf("\n");
		SPDK_NOTICELOG("Blob fill complete.\n");
		spdk_bs_md_close_blob(&cli_context->blob, close_cb,
				      cli_context);
	}

}

/*
 * function to fill a blob with a value.
 */
static void
fill_blob(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob open callback",
			  bserrno);
		return;
	}
	cli_context->blob = blob;
	cli_context->page_count = 0;
	cli_context->blob_pages = spdk_blob_get_num_pages(cli_context->blob);
	cli_context->buff = spdk_dma_malloc(cli_context->page_size,
					    0x1000, NULL);
	if (cli_context->buff == NULL) {
		unload_bs(cli_context, "Error in allocating memory",
			  -ENOMEM);
		return;
	}

	memset(cli_context->buff, cli_context->fill_value,
	       cli_context->page_size);
	printf("\n");
	spdk_bs_io_write_blob(cli_context->blob, cli_context->channel,
			      cli_context->buff,
			      0, 1, write_complete, cli_context);
}

/*
 * Callback for load bs where we'll continue on to fill a blob.
 */
static void
fill_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;
	cli_context->page_size = spdk_bs_get_page_size(cli_context->bs);
	cli_context->channel = spdk_bs_alloc_io_channel(cli_context->bs);
	if (cli_context->channel == NULL) {
		unload_bs(cli_context, "Error in allocating channel",
			  -ENOMEM);
		return;
	}

	spdk_bs_md_open_blob(cli_context->bs, cli_context->blobid,
			     fill_blob, cli_context);
}

/*
 * Multiple actions require us to open the bs first. A function pointer
 * setup earlier will direct the callback accordingly.
 */
static void
load_bs(struct cli_context_t *cli_context)
{
	struct spdk_bdev *bdev = NULL;
	struct spdk_bs_dev *bs_dev = NULL;

	bdev = spdk_bdev_get_by_name(cli_context->bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}

	bs_dev = spdk_bdev_create_bs_dev(bdev);
	if (bs_dev == NULL) {
		SPDK_ERRLOG("Could not create blob bdev!!\n");
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_load(bs_dev, cli_context->nextFunc, cli_context);
}

/*
 * Lists all the blobs ion this blobstore.
 */
static void
list_bdevs(void)
{
	struct spdk_bdev *bdev = NULL;

	printf("\n");
	SPDK_NOTICELOG("List BDEVs:\n");

	bdev = spdk_bdev_first();
	if (bdev == NULL) {
		SPDK_ERRLOG("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}

	while (bdev) {
		SPDK_NOTICELOG("\tBdev Name: %s\n", spdk_bdev_get_name(bdev));
		SPDK_NOTICELOG("\tBdev Product Name: %s\n",
			       spdk_bdev_get_product_name(bdev));
		bdev = spdk_bdev_next(bdev);
	}

	printf("\n");
	spdk_app_stop(0);
}

/*
 * Callback function for initializing a blob.
 */
static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		unload_bs(cli_context, "Error in bs init callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;
	SPDK_NOTICELOG("blobstore init'd: (%p)\n", cli_context->bs);

	unload_bs(cli_context, "", 0);
}

/*
 * Initialize a new blobstore.
 */
static void
init_bs(struct cli_context_t *cli_context)
{
	struct spdk_bdev *bdev = NULL;
	struct spdk_bs_dev *bs_dev = NULL;

	bdev = spdk_bdev_get_by_name(cli_context->bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}
	SPDK_NOTICELOG("Blobstore using Bdev Product Name: %s\n",
		       spdk_bdev_get_product_name(bdev));

	bs_dev = spdk_bdev_create_bs_dev(bdev);
	if (bs_dev == NULL) {
		SPDK_ERRLOG("Could not create blob bdev!!\n");
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_init(bs_dev, NULL, bs_init_complete,
		     cli_context);
}

/*
 * This is the function we pass into the SPDK framework that gets
 * called first.
 */
static void
cli_start(void *arg1, void *arg2)
{
	struct cli_context_t *cli_context = arg1;

	/*
	 * Decide what to do next based on cmd lin parsing that
	 * happened earlier, in many cases we setup a function pointer
	 * to be used as a callback following a generic action like
	 * loading the blobstore.
	 */
	switch (cli_context->action) {
	case CLI_SET_SUPER:
		cli_context->nextFunc = &set_super_load_cb;
		load_bs(cli_context);
		break;
	case CLI_SHOW_BS:
		cli_context->nextFunc = &get_super_load_cb;
		load_bs(cli_context);
		break;
	case CLI_CREATE_BLOB:
		cli_context->nextFunc = &create_load_cb;
		load_bs(cli_context);
		break;
	case CLI_SET_XATTR:
	case CLI_DEL_XATTR:
		cli_context->nextFunc = &xattr_load_cb;
		load_bs(cli_context);
		break;
	case CLI_SHOW_BLOB:
	case CLI_LIST_BLOBS:
		cli_context->nextFunc = &list_load_cb;
		load_bs(cli_context);
		break;
	case CLI_DUMP:
	case CLI_IMPORT:
		cli_context->nextFunc = &dump_load_cb;
		load_bs(cli_context);
		break;
	case CLI_FILL:
		cli_context->nextFunc = &fill_load_cb;
		load_bs(cli_context);
		break;
	case CLI_INIT_BS:
		init_bs(cli_context);
		break;
	case CLI_LIST_BDEVS:
		list_bdevs();
		break;
	default:
		/* should never get here */
		spdk_app_stop(-1);
		break;
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	struct cli_context_t *cli_context = NULL;
	const char *config_file = NULL;
	int rc = 0;
	int op;

	if (argc < 2) {
		usage("ERROR: Invalid option\n");
		exit(1);
	}

	cli_context = calloc(1, sizeof(struct cli_context_t));
	if (cli_context == NULL) {
		SPDK_ERRLOG("ERROR: could not allocate context structure\n");
		exit(-1);
	}
	cli_context->bdev_name = bdev_name;

	while ((op = getopt(argc, argv, "m:d:f:k:x:p:s:n:c:l:i")) != -1) {
		switch (op) {
		case 'm':
			cli_context->action = CLI_IMPORT;
			cli_context->blobid = atoll(optarg);
			break;
		case 'd':
			cli_context->action = CLI_DUMP;
			cli_context->blobid = atoll(optarg);
			break;
		case 'f':
			cli_context->action = CLI_FILL;
			cli_context->blobid = atoll(optarg);
			break;
		case 'p':
			cli_context->action = CLI_SET_SUPER;
			cli_context->superid = atoll(optarg);
			break;
		case 's':
			if (strcmp("bs", optarg) == 0) {
				cli_context->action = CLI_SHOW_BS;
			} else {
				cli_context->action = CLI_SHOW_BLOB;
				cli_context->blobid = atoll(optarg);
			}
			break;
		case 'n':
			cli_context->num_clusters = atoi(optarg);
			if (cli_context->num_clusters > 0) {
				cli_context->action = CLI_CREATE_BLOB;
			} else {
				usage("ERROR: invalid option for new\n");
				exit(-1);
			}
			break;
		case 'c':
			config_file = optarg;
			break;
		case 'l':
			if (strcmp("bdevs", optarg) == 0) {
				cli_context->action = CLI_LIST_BDEVS;
			} else if (strcmp("blobs", optarg) == 0) {
				cli_context->action = CLI_LIST_BLOBS;
			} else {
				usage("ERROR: invalid option for list\n");
				exit(-1);
			}
			break;
		case 'i':
			cli_context->action = CLI_INIT_BS;
			break;
		case 'x':
			cli_context->action = CLI_SET_XATTR;
			cli_context->blobid = atoll(optarg);
			break;
		case 'k':
			cli_context->action = CLI_DEL_XATTR;
			cli_context->blobid = atoll(optarg);
			break;
		default:
			usage("ERROR: Invalid option\n");
			exit(1);
		}
		/* config file is the only option that can be combined */
		if (op != 'c')
			break;
	}

	/* if they don't supply a conf name, use the default */
	if (!config_file) {
		config_file = program_conf;
	}

	/* a few options require some extra paramters */
	if (cli_context->action == CLI_SET_XATTR ||
	    cli_context->action == CLI_DEL_XATTR) {
		snprintf(cli_context->key, BUFSIZE, "%s", argv[3]);
		snprintf(cli_context->value, BUFSIZE, "%s", argv[4]);
	}

	if (cli_context->action == CLI_DUMP ||
	    cli_context->action == CLI_IMPORT) {
		snprintf(cli_context->file, BUFSIZE, "%s", argv[3]);
	}

	if (cli_context->action == CLI_FILL) {
		cli_context->fill_value = atoi(argv[3]);
	}

	/* Set default values in opts struct along with name and conf file. */
	spdk_app_opts_init(&opts);
	opts.name = "blobcli";
	opts.config_file = config_file;

	/*
	 * spdk_app_start() will block running cli_start() until
	 * spdk_app_stop() is called by someone (not simply when
	 * cli_start() returns)
	 */
	rc = spdk_app_start(&opts, cli_start, cli_context, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR!\n");
	}

	/* Free up memory that we allocated */
	cli_cleanup(cli_context);

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
