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
#include "spdk/version.h"
#include "spdk/string.h"

/*
 * This is not a public header file, but the CLI does expose
 * some internals of blobstore for dev/debug puposes so we
 * include it here.
 */
#include "../lib/blob/blobstore.h"
static void cli_start(void *arg1, void *arg2);
static bool cli_shell(void *arg1, void *arg2);

static const char *program_name = "blobcli";
static const char *program_conf = "blobcli.conf";
static const char *bdev_name = "Nvme0n1";

/*
 * CMD mode runs one command at a time which can be annoying as the init takes
 * a few seconds, so the shell mode, invoked with -S, does the init once and gives
 * the user an interactive shell instead.
 */
enum cli_mode_type {
	CLI_MODE_CMD,
	CLI_MODE_SHELL
};

enum cli_action_type {
	CLI_NONE,
	CLI_IMPORT,
	CLI_DUMP,
	CLI_FILL,
	CLI_REM_XATTR,
	CLI_SET_XATTR,
	CLI_SET_SUPER,
	CLI_SHOW_BS,
	CLI_SHOW_BLOB,
	CLI_CREATE_BLOB,
	CLI_LIST_BDEVS,
	CLI_LIST_BLOBS,
	CLI_INIT_BS,
	CLI_SHELL_EXIT,
	CLI_HELP
};
#define BUFSIZE 255

#define MAX_ARGS 6
/*
 * The CLI uses the SPDK app framework so is async and callback driven. A
 * pointer to this structure is passed to SPDK calls and returned in the
 * callbacks for easy access to all the info we may need.
 */
struct cli_context_t {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	struct spdk_bs_dev *bs_dev;
	spdk_blob_id blobid;
	spdk_blob_id superid;
	struct spdk_io_channel *channel;
	uint8_t *buff;
	uint64_t page_size;
	uint64_t page_count;
	uint64_t blob_pages;
	uint64_t bytes_so_far;
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
	void (*next_func)(void *arg1, struct spdk_blob_store *bs, int bserrno);
	enum cli_mode_type cli_mode;
	const char *config_file;
	int argc;
	char *argv[MAX_ARGS];
	bool app_started;
};

/*
 * Common printing of commands for CLI and shell modes.
 */
static void
print_cmds(char *msg)
{
	if (msg) {
		printf("%s", msg);
	}
	printf("\nCommands include:\n");
	printf("\t-d <blobid> filename - dump contents of a blob to a file\n");
	printf("\t-f <blobid> value - fill a blob with a decimal value\n");
	printf("\t-h - this help screen\n");
	printf("\t-i - initialize a blobstore\n");
	printf("\t-l bdevs | blobs - list either available bdevs or existing blobs\n");
	printf("\t-m <blobid> filename - import contents of a file to a blob\n");
	printf("\t-n <# clusters> - create new blob\n");
	printf("\t-p <blobid> - set the superblob to the ID provided\n");
	printf("\t-r <blobid> name - remove xattr name/value pair\n");
	printf("\t-s <blobid> | bs - show blob info or blobstore info\n");
	printf("\t-x <blobid> name value - set xattr name/value pair\n");
	printf("\t-X - exit when in interactive shell mode\n");
	printf("\t-S - enter interactive shell mode\n");
	printf("\n");
}

/*
 * Prints usage and relevant error message.
 */
static void
usage(char *msg)
{
	if (msg) {
		printf("%s", msg);
	}
	printf("Version %s\n", SPDK_VERSION_STRING);
	printf("Usage: %s [-c SPDK config_file] Command\n", program_name);
	printf("\n%s is a command line tool for interacting with blobstore\n",
	       program_name);
	printf("on the underlying device specified in the conf file passed\n");
	printf("in as a command line option.\n");
	print_cmds("");
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
		printf("Error %d unloading the bobstore\n", bserrno);
		cli_context->rc = bserrno;
	}

	/*
	 * Quit if we're in cmd mode or exiting shell mode, otherwise
	 * clear the action field and start the main function again.
	 */
	if (cli_context->cli_mode == CLI_MODE_CMD ||
	    cli_context->action == CLI_SHELL_EXIT) {
		spdk_app_stop(cli_context->rc);
	} else {
		/* when action is NONE, we know we need to remain in the shell */
		cli_context->action = CLI_NONE;
		cli_start(cli_context, NULL);
	}
}

/*
 * Unload the blobstore.
 */
static void
unload_bs(struct cli_context_t *cli_context, char *msg, int bserrno)
{
	if (bserrno) {
		printf("%s (err %d)\n", msg, bserrno);
		cli_context->rc = bserrno;
	}

	if (cli_context->bs) {
		if (cli_context->channel) {
			spdk_bs_free_io_channel(cli_context->channel);
		}
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
	printf("blob now has USED clusters of %" PRIu64 "\n",
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
	printf("New blob id %" PRIu64 "\n", cli_context->blobid);

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

	printf("Blobstore Public Info:\n");
	printf("\tUsing bdev Product Name: %s\n",
	       spdk_bdev_get_product_name(bdev));
	printf("\tAPI Version: %d\n", SPDK_BS_VERSION);

	if (bserrno != -ENOENT) {
		printf("\tsuper blob ID: %" PRIu64 "\n", cli_context->superid);
	} else {
		printf("\tsuper blob ID: none assigned\n");
	}

	val = spdk_bs_get_page_size(cli_context->bs);
	printf("\tpage size: %" PRIu64 "\n", val);

	val = spdk_bs_get_cluster_size(cli_context->bs);
	printf("\tcluster size: %" PRIu64 "\n", val);

	val = spdk_bs_free_cluster_count(cli_context->bs);
	printf("\t# free clusters: %" PRIu64 "\n", val);

	/*
	 * Private info isn't accessible via the public API but
	 * may be useful for debug of blobstore based applications.
	 */
	printf("\nBlobstore Private Info:\n");
	printf("\tMetadata start (pages): %" PRIu64 "\n",
	       cli_context->bs->md_start);
	printf("\tMetadata length (pages): %d \n",
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

	printf("Blob Public Info:\n");

	printf("blob ID: %" PRIu64 "\n", cli_context->blobid);

	val = spdk_blob_get_num_clusters(cli_context->blob);
	printf("# of clusters: %" PRIu64 "\n", val);

	printf("# of bytes: %" PRIu64 "\n",
	       val * spdk_bs_get_cluster_size(cli_context->bs));

	val = spdk_blob_get_num_pages(cli_context->blob);
	printf("# of pages: %" PRIu64 "\n", val);

	spdk_bs_md_get_xattr_names(cli_context->blob, &names);

	printf("# of xattrs: %d\n", spdk_xattr_names_get_count(names));
	printf("xattrs:\n");
	for (i = 0; i < spdk_xattr_names_get_count(names); i++) {
		spdk_bs_md_get_xattr_value(cli_context->blob,
					   spdk_xattr_names_get_name(names, i),
					   &value, &value_len);
		if ((value_len + 1) > sizeof(data)) {
			printf("FYI: adjusting size of xattr due to CLI limits.\n");
			value_len = sizeof(data) - 1;
		}
		memcpy(&data, value, value_len);
		data[value_len] = '\0';
		printf("\n(%d) Name:%s\n", i,
		       spdk_xattr_names_get_name(names, i));
		printf("(%d) Value:\n", i);
		spdk_trace_dump(stdout, "", value, value_len);
	}

	/*
	 * Private info isn't accessible via the public API but
	 * may be useful for debug of blobstore based applications.
	 */
	printf("\nBlob Private Info:\n");
	switch (cli_context->blob->state) {
	case SPDK_BLOB_STATE_DIRTY:
		printf("state: DIRTY\n");
		break;
	case SPDK_BLOB_STATE_CLEAN:
		printf("state: CLEAN\n");
		break;
	case SPDK_BLOB_STATE_LOADING:
		printf("state: LOADING\n");
		break;
	case SPDK_BLOB_STATE_SYNCING:
		printf("state: SYNCING\n");
		break;
	default:
		printf("state: UNKNOWN\n");
		break;
	}
	printf("open ref count: %d\n",
	       cli_context->blob->open_ref);

	spdk_xattr_names_free(names);
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
		printf("Found blob with ID# %" PRIu64 "\n",
		       spdk_blob_get_id(blob));
	} else if (spdk_blob_get_id(blob) == cli_context->blobid) {
		/*
		 * Found the blob we're looking for, but we need to finish
		 * iterating even after showing the info so that internally
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
		printf("\nList BLOBS:\n");
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

	printf("Super Blob ID has been set.\n");
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
		printf("Xattr has been set.\n");
	} else {
		spdk_blob_md_remove_xattr(cli_context->blob,
					  cli_context->key);
		printf("Xattr has been removed.\n");
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
	uint64_t bytes_written;

	if (bserrno) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error in read completion",
			  bserrno);
		return;
	}

	bytes_written = fwrite(cli_context->buff, 1, cli_context->page_size,
			       cli_context->fp);
	if (bytes_written != cli_context->page_size) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error with fwrite",
			  bserrno);
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
		printf("\nFile write complete.\n");
		fclose(cli_context->fp);
		spdk_bs_md_close_blob(&cli_context->blob, close_cb,
				      cli_context);
	}
}

/*
 * Callback for write completion on the import of a file to a blob.
 */
static void
write_imp_complete(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	uint64_t bytes_read;

	if (bserrno) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error in write completion",
			  bserrno);
		return;
	}

	if (cli_context->bytes_so_far < cli_context->filesize) {
		/* perform another file read */
		bytes_read = fread(cli_context->buff, 1,
				   cli_context->page_size,
				   cli_context->fp);
		cli_context->bytes_so_far += bytes_read;

		/* if this read is < 1 page, fill with 0s */
		if (bytes_read < cli_context->page_size) {
			uint8_t *offset = cli_context->buff + bytes_read;
			memset(offset, 0,
			       cli_context->page_size - bytes_read);
		}
	} else {
		/*
		 * Done reading the file, fill the rest of the blob with 0s,
		 * yeah we're memsetting the same page over and over here
		 */
		memset(cli_context->buff, 0, cli_context->page_size);
	}
	if (++cli_context->page_count < cli_context->blob_pages) {
		printf(".");
		spdk_bs_io_write_blob(cli_context->blob, cli_context->channel,
				      cli_context->buff, cli_context->page_count,
				      1, write_imp_complete, cli_context);
	} else {
		/* done writing */
		printf("\nBlob import complete.\n");
		fclose(cli_context->fp);
		spdk_bs_md_close_blob(&cli_context->blob, close_cb,
				      cli_context);
	}
}

/*
 * Callback for open blobs where we'll continue on dump a blob to a file or
 * import a file to a blob. For dump, the resulting file will always be the
 * full size of the blob.  For import, the blob will fill with the file
 * contents first and then 0 out the rest of the blob.
 */
static void
dmpimp_open_cb(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

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
	printf("Working");
	cli_context->blob_pages = spdk_blob_get_num_pages(cli_context->blob);
	cli_context->page_count = 0;
	if (cli_context->action == CLI_DUMP) {
		cli_context->fp = fopen(cli_context->file, "w");

		/* read a page of data from the blob */
		spdk_bs_io_read_blob(cli_context->blob, cli_context->channel,
				     cli_context->buff, cli_context->page_count,
				     1, read_dump_complete, cli_context);
	} else {
		cli_context->fp = fopen(cli_context->file, "r");

		/* get the filesize then rewind read a page of data from file */
		fseek(cli_context->fp, 0L, SEEK_END);
		cli_context->filesize = ftell(cli_context->fp);
		rewind(cli_context->fp);
		cli_context->bytes_so_far = fread(cli_context->buff, 1,
						  cli_context->page_size,
						  cli_context->fp);

		/* if the file is < a page, fill the rest with 0s */
		if (cli_context->filesize < cli_context->page_size) {
			uint8_t *offset =
				cli_context->buff + cli_context->filesize;

			memset(offset, 0,
			       cli_context->page_size - cli_context->filesize);
		}

		spdk_bs_io_write_blob(cli_context->blob, cli_context->channel,
				      cli_context->buff, cli_context->page_count,
				      1, write_imp_complete, cli_context);
	}
}

/*
 * Callback for load bs where we'll continue on dump a blob to a file or
 * import a file to a blob.
 */
static void
dmpimp_load_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	spdk_bs_md_open_blob(cli_context->bs, cli_context->blobid,
			     dmpimp_open_cb, cli_context);
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
		printf("\nBlob fill complete.\n");
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
		printf("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}

	bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	if (bs_dev == NULL) {
		printf("Could not create blob bdev!!\n");
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_load(bs_dev, cli_context->next_func, cli_context);
}

/*
 * Lists all the blobs on this blobstore.
 */
static void
list_bdevs(struct cli_context_t *cli_context)
{
	struct spdk_bdev *bdev = NULL;

	printf("\nList bdevs:\n");

	bdev = spdk_bdev_first();
	if (bdev == NULL) {
		printf("Could not find a bdev\n");
	}
	while (bdev) {
		printf("\tbdev Name: %s\n", spdk_bdev_get_name(bdev));
		printf("\tbdev Product Name: %s\n",
		       spdk_bdev_get_product_name(bdev));
		bdev = spdk_bdev_next(bdev);
	}

	printf("\n");
	if (cli_context->cli_mode == CLI_MODE_CMD) {
		spdk_app_stop(0);
	} else {
		cli_context->action = CLI_NONE;
		cli_start(cli_context, NULL);
	}
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
	printf("blobstore init'd: (%p)\n", cli_context->bs);

	unload_bs(cli_context, "", 0);
}

/*
 * Initialize a new blobstore.
 */
static void
init_bs(struct cli_context_t *cli_context)
{
	struct spdk_bdev *bdev = NULL;

	bdev = spdk_bdev_get_by_name(cli_context->bdev_name);
	if (bdev == NULL) {
		printf("Could not find a bdev\n");
		spdk_app_stop(-1);
		return;
	}
	printf("Blobstore using bdev Product Name: %s\n",
	       spdk_bdev_get_product_name(bdev));

	cli_context->bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	if (cli_context->bs_dev == NULL) {
		printf("Could not create blob bdev!!\n");
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_init(cli_context->bs_dev, NULL, bs_init_complete,
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
	 * The initial cmd line options are parsed once before this function is
	 * called so if there is no action, we're in shell mode and will loop
	 * here until a a valid option is parsed and returned.
	 */
	if (cli_context->action == CLI_NONE) {
		while (cli_shell(cli_context, NULL) == false);
	}

	/*
	 * Decide what to do next based on cmd line parsing that
	 * happened earlier, in many cases we setup a function pointer
	 * to be used as a callback following a generic action like
	 * loading the blobstore.
	 */
	switch (cli_context->action) {
	case CLI_SET_SUPER:
		cli_context->next_func = &set_super_load_cb;
		load_bs(cli_context);
		break;
	case CLI_SHOW_BS:
		cli_context->next_func = &get_super_load_cb;
		load_bs(cli_context);
		break;
	case CLI_CREATE_BLOB:
		cli_context->next_func = &create_load_cb;
		load_bs(cli_context);
		break;
	case CLI_SET_XATTR:
	case CLI_REM_XATTR:
		cli_context->next_func = &xattr_load_cb;
		load_bs(cli_context);
		break;
	case CLI_SHOW_BLOB:
	case CLI_LIST_BLOBS:
		cli_context->next_func = &list_load_cb;
		load_bs(cli_context);
		break;
	case CLI_DUMP:
	case CLI_IMPORT:
		cli_context->next_func = &dmpimp_load_cb;
		load_bs(cli_context);
		break;
	case CLI_FILL:
		cli_context->next_func = &fill_load_cb;
		load_bs(cli_context);
		break;
	case CLI_INIT_BS:
		init_bs(cli_context);
		break;
	case CLI_LIST_BDEVS:
		list_bdevs(cli_context);
		break;
	case CLI_SHELL_EXIT:
		/*
		 * Because shell mode reuses cmd mode functions, the blobstore
		 * is loaded/unloaded with every action so we just need to
		 * stop the framework. For this app there's no need to optimize
		 * and keep the blobstore open while the app is in shell mode.
		 */
		spdk_app_stop(0);
		break;
	case CLI_HELP:
		print_cmds("");
		unload_complete(cli_context, 0);
		break;
	default:
		/* should never get here */
		spdk_app_stop(-1);
		break;
	}
}

/*
 * Common cmd/option parser for command and shell modes.
 */
static bool
cmd_parser(int argc, char **argv, struct cli_context_t *cli_context)
{
	int op;
	int cmd_chosen = 0;
	char resp;

	while ((op = getopt(argc, argv, "c:d:f:hil:m:n:p:r:s:SXx:")) != -1) {
		switch (op) {
		case 'c':
			if (cli_context->app_started == false) {
				cmd_chosen++;
				cli_context->config_file = optarg;
			} else {
				print_cmds("ERROR: -c option not valid during shell mode.\n");
			}
			break;
		case 'd':
			cmd_chosen++;
			cli_context->action = CLI_DUMP;
			cli_context->blobid = atoll(optarg);
			break;
		case 'f':
			cmd_chosen++;
			cli_context->action = CLI_FILL;
			cli_context->blobid = atoll(optarg);
			break;
		case 'h':
			cmd_chosen++;
			cli_context->action = CLI_HELP;
			break;
		case 'i':
			printf("You entire blobstore will be destroyed. Are you sure? (y/n) ");
			if (scanf("%c%*c", &resp)) {
				if (resp == 'y' || resp == 'Y') {
					cmd_chosen++;
					cli_context->action = CLI_INIT_BS;
				} else {
					if (cli_context->cli_mode == CLI_MODE_CMD) {
						exit(0);
					}
				}
			}
			break;
		case 'r':
			cmd_chosen++;
			cli_context->action = CLI_REM_XATTR;
			cli_context->blobid = atoll(optarg);
			break;
		case 'l':
			if (strcmp("bdevs", optarg) == 0) {
				cmd_chosen++;
				cli_context->action = CLI_LIST_BDEVS;
			} else if (strcmp("blobs", optarg) == 0) {
				cmd_chosen++;
				cli_context->action = CLI_LIST_BLOBS;
			} else {
				if (cli_context->cli_mode == CLI_MODE_CMD) {
					usage("ERROR: invalid option for list\n");
					exit(-1);
				} else {
					print_cmds("ERROR: invalid option for list\n");
				}
			}
			break;
		case 'm':
			cmd_chosen++;
			cli_context->action = CLI_IMPORT;
			cli_context->blobid = atoll(optarg);
			break;
		case 'n':
			cmd_chosen++;
			cli_context->num_clusters = atoi(optarg);
			if (cli_context->num_clusters > 0) {
				cli_context->action = CLI_CREATE_BLOB;
			} else {
				if (cli_context->cli_mode == CLI_MODE_CMD) {
					usage("ERROR: invalid option for new\n");
					exit(-1);
				} else {
					print_cmds("ERROR: invalid option for new\n");
				}
			}
			break;
		case 'p':
			cmd_chosen++;
			cli_context->action = CLI_SET_SUPER;
			cli_context->superid = atoll(optarg);
			break;
		case 'S':
			if (cli_context->cli_mode == CLI_MODE_CMD) {
				cli_context->action = CLI_NONE;
				cli_context->cli_mode = CLI_MODE_SHELL;
			}
			cli_context->action = CLI_NONE;
			break;
		case 's':
			cmd_chosen++;
			if (strcmp("bs", optarg) == 0) {
				cli_context->action = CLI_SHOW_BS;
			} else {
				cli_context->action = CLI_SHOW_BLOB;
				cli_context->blobid = atoll(optarg);
			}
			break;
		case 'X':
			cmd_chosen++;
			cli_context->action = CLI_SHELL_EXIT;
			break;
		case 'x':
			cmd_chosen++;
			cli_context->action = CLI_SET_XATTR;
			cli_context->blobid = atoll(optarg);
			break;
		default:
			if (cli_context->cli_mode == CLI_MODE_CMD) {
				usage("ERROR: invalid option\n");
				exit(-1);
			} else {
				print_cmds("ERROR: invalid option\n");
			}
		}
		/* config file is the only option that can be combined */
		if (op != 'c') {
			if (cmd_chosen > 1) {
				if (cli_context->cli_mode == CLI_MODE_CMD) {
					usage("Error: Please choose only one command\n");
					cli_cleanup(cli_context);
					exit(1);
				} else {
					print_cmds("Error: Please choose only one command\n");
				}
			}
		}
	}

	if (cli_context->cli_mode == CLI_MODE_CMD && cmd_chosen == 0) {
		usage("Error: Please choose a command.\n");
		exit(1);
	}

	/* a few options require some extra paramters */
	if (cli_context->action == CLI_SET_XATTR ||
	    cli_context->action == CLI_REM_XATTR) {
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

	/* in shell mode we'll call getopt multiple times so need to reset its index */
	optind = 0;
	return (cmd_chosen > 0);
}

/*
 * Provides for a shell interface as opposed to one shot command line.
 */
static bool
cli_shell(void *arg1, void *arg2)
{
	struct cli_context_t *cli_context = arg1;
	char *line = NULL;
	ssize_t buf_size = 0;
	ssize_t bytes_in = 0;
	ssize_t tok_len = 0;
	char *tok = NULL;
	bool cmd_chosen = false;
	int start_idx = cli_context->argc;
	int i;

	printf("blob> ");
	bytes_in = getline(&line, &buf_size, stdin);

	/* parse input and update cli_context so we can use common option parser */
	if (bytes_in > 0) {
		tok = strtok(line, " ");
	}
	while ((tok != NULL) && (cli_context->argc < MAX_ARGS)) {
		cli_context->argv[cli_context->argc] = strdup(tok);
		tok_len = strlen(tok);
		cli_context->argc++;
		tok = strtok(NULL, " ,.-");
	}

	/* replace newline on last arg with null */
	if (tok_len) {
		spdk_str_chomp(cli_context->argv[cli_context->argc - 1]);
	}

	/* call parse cmd line with user input as args */
	cmd_chosen = cmd_parser(cli_context->argc, &cli_context->argv[0], cli_context);

	/* free strdup mem & reset arg count for next shell interaction */
	for (i = start_idx; i < cli_context->argc; i++) {
		free(cli_context->argv[i]);
	}
	cli_context->argc = 1;

	free(line);

	return cmd_chosen;
}


int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	struct cli_context_t *cli_context = NULL;
	int rc = 0;

	if (argc < 2) {
		usage("ERROR: Invalid option\n");
		exit(1);
	}

	cli_context = calloc(1, sizeof(struct cli_context_t));
	if (cli_context == NULL) {
		printf("ERROR: could not allocate context structure\n");
		exit(-1);
	}
	cli_context->bdev_name = bdev_name;
	/* default to CMD mode until we've parsed the first parms */
	cli_context->cli_mode = CLI_MODE_CMD;

	cli_context->argv[0] = strdup(argv[0]);
	cli_context->argc = 1;

	/* parse command line */
	cmd_parser(argc, argv, cli_context);
	free(cli_context->argv[0]);

	/* after displaying help, just exit */
	if (cli_context->action == CLI_HELP) {
		usage("");
		cli_cleanup(cli_context);
		exit(-1);
	}

	/* if they don't supply a conf name, use the default */
	if (!cli_context->config_file) {
		cli_context->config_file = program_conf;
	}

	/* if the config file doesn't exist, tell them how to make one */
	if (access(cli_context->config_file, F_OK) == -1) {
		printf("Error: No config file found.\n");
		printf("To create a config file named 'blobcli.conf' for your NVMe device:\n");
		printf("   <path to spdk>/scripts/gen_nvme.sh > blobcli.conf\n");
		printf("and then re-run the cli tool.\n");
		exit(1);
	}

	/* Set default values in opts struct along with name and conf file. */
	spdk_app_opts_init(&opts);
	opts.name = "blobcli";
	opts.config_file = cli_context->config_file;

	cli_context->app_started = true;
	rc = spdk_app_start(&opts, cli_start, cli_context, NULL);
	if (rc) {
		printf("ERROR!\n");
	}

	/* Free up memory that we allocated */
	cli_cleanup(cli_context);

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
