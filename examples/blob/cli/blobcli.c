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
#include "spdk/uuid.h"

/*
 * The following is not a public header file, but the CLI does expose
 * some internals of blobstore for dev/debug puposes so we
 * include it here.
 */
#include "../lib/blob/blobstore.h"
static void cli_start(void *arg1);

static const char *program_name = "blobcli";
/* default name for .json file, any name can be used however with -j switch */
static const char *program_conf = "blobcli.json";

/*
 * CMD mode runs one command at a time which can be annoying as the init takes
 * a few seconds, so the shell mode, invoked with -S, does the init once and gives
 * the user an interactive shell instead. With script mode init is also done just
 * once.
 */
enum cli_mode_type {
	CLI_MODE_CMD,
	CLI_MODE_SHELL,
	CLI_MODE_SCRIPT
};

enum cli_action_type {
	CLI_NONE,
	CLI_IMPORT_BLOB,
	CLI_DUMP_BLOB,
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
	CLI_DUMP_BS,
	CLI_SHELL_EXIT,
	CLI_HELP,
};

#define BUFSIZE 255
#define MAX_ARGS 16
#define ALIGN_4K 4096
#define STARTING_IO_UNIT 0
#define NUM_IO_UNITS 1

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
	uint64_t io_unit_size;
	uint64_t io_unit_count;
	uint64_t blob_io_units;
	uint64_t bytes_so_far;
	FILE *fp;
	enum cli_action_type action;
	char key[BUFSIZE + 1];
	char value[BUFSIZE + 1];
	char file[BUFSIZE + 1];
	uint64_t filesize;
	int fill_value;
	char bdev_name[BUFSIZE];
	int rc;
	int num_clusters;
	enum cli_mode_type cli_mode;
	const char *config_file;
	int argc;
	char *argv[MAX_ARGS];
	bool app_started;
	char script_file[BUFSIZE + 1];
};

/* we store a bunch of stuff in a global struct for use by scripting mode */
#define MAX_SCRIPT_LINES 64
#define MAX_SCRIPT_BLOBS 16
struct cli_script_t {
	spdk_blob_id blobid[MAX_SCRIPT_BLOBS];
	int blobid_idx;
	int max_index;
	int cmdline_idx;
	bool ignore_errors;
	char *cmdline[MAX_SCRIPT_LINES];
};
struct cli_script_t g_script;

/*
 * Common printing of commands for CLI and shell modes.
 */
static void
print_cmds(void)
{
	printf("\nCommands include:\n");
	printf("\t-b bdev - name of the block device to use (example: Nvme0n1)\n");
	printf("\t-d <blobid> filename - dump contents of a blob to a file\n");
	printf("\t-D - dump metadata contents of an existing blobstore\n");
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
	printf("\t-T <filename> - automated script mode\n");
	printf("\n");
}

/*
 * Prints usage and relevant error message.
 */
static void
usage(struct cli_context_t *cli_context, char *msg)
{
	if (msg) {
		printf("%s", msg);
	}

	if (!cli_context || cli_context->cli_mode == CLI_MODE_CMD) {
		printf("Version %s\n", SPDK_VERSION_STRING);
		printf("Usage: %s [-j SPDK josn_config_file] Command\n", program_name);
		printf("\n%s is a command line tool for interacting with blobstore\n",
		       program_name);
		printf("on the underlying device specified in the conf file passed\n");
		printf("in as a command line option.\n");
	}
	if (!cli_context || cli_context->cli_mode != CLI_MODE_SCRIPT) {
		print_cmds();
	}
}

/*
 * Free up memory that we allocated.
 */
static void
cli_cleanup(struct cli_context_t *cli_context)
{
	if (cli_context->buff) {
		spdk_free(cli_context->buff);
	}
	if (cli_context->cli_mode == CLI_MODE_SCRIPT) {
		int i;

		for (i = 0; i <= g_script.max_index; i++) {
			free(g_script.cmdline[i]);
		}
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
		/* when action is CLI_NONE, we know we need to remain in the shell */
		cli_context->bs = NULL;
		cli_context->action = CLI_NONE;
		cli_start(cli_context);
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
			cli_context->channel = NULL;
		}
		spdk_bs_unload(cli_context->bs, unload_complete, cli_context);
	} else if (cli_context->cli_mode != CLI_MODE_SCRIPT) {
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
sync_cb(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in sync callback",
			  bserrno);
		return;
	}

	spdk_blob_close(cli_context->blob, close_cb, cli_context);
}

static void
resize_cb(void *cb_arg, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;
	uint64_t total = 0;

	if (bserrno) {
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
	spdk_blob_sync_md(cli_context->blob, sync_cb, cli_context);
}

/*
 * Callback function for opening a blob after creating.
 */
static void
open_now_resize_cb(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		unload_bs(cli_context, "Error in open completion",
			  bserrno);
		return;
	}
	cli_context->blob = blob;

	spdk_blob_resize(cli_context->blob, cli_context->num_clusters,
			 resize_cb, cli_context);
}

/*
 * Callback function for creating a blob.
 */
static void
blob_create_cb(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob create callback",
			  bserrno);
		return;
	}

	cli_context->blobid = blobid;
	printf("New blob id %" PRIu64 "\n", cli_context->blobid);

	/* if we're in script mode, we need info on all blobids for later */
	if (cli_context->cli_mode == CLI_MODE_SCRIPT) {
		g_script.blobid[g_script.blobid_idx++] = blobid;
	}

	/* We have to open the blob before we can do things like resize. */
	spdk_bs_open_blob(cli_context->bs, cli_context->blobid,
			  open_now_resize_cb, cli_context);
}

/*
 * Callback for get_super where we'll continue on to show blobstore info.
 */
static void
show_bs_cb(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	struct spdk_bs_type bstype;
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

	printf("\tpage size: %" PRIu64 "\n", cli_context->page_size);
	printf("\tio unit size: %" PRIu64 "\n", cli_context->io_unit_size);

	val = spdk_bs_get_cluster_size(cli_context->bs);
	printf("\tcluster size: %" PRIu64 "\n", val);

	val = spdk_bs_free_cluster_count(cli_context->bs);
	printf("\t# free clusters: %" PRIu64 "\n", val);

	bstype = spdk_bs_get_bstype(cli_context->bs);
	spdk_log_dump(stdout, "\tblobstore type:", &bstype, sizeof(bstype));

	/*
	 * Private info isn't accessible via the public API but
	 * may be useful for debug of blobstore based applications.
	 */
	printf("\nBlobstore Private Info:\n");
	printf("\tMetadata start (pages): %" PRIu64 "\n",
	       cli_context->bs->md_start);
	printf("\tMetadata length (pages): %d\n",
	       cli_context->bs->md_len);

	unload_bs(cli_context, "", 0);
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
	unsigned int i;

	printf("Blob Public Info:\n");

	printf("blob ID: %" PRIu64 "\n", cli_context->blobid);

	val = spdk_blob_get_num_clusters(cli_context->blob);
	printf("# of clusters: %" PRIu64 "\n", val);

	printf("# of bytes: %" PRIu64 "\n",
	       val * spdk_bs_get_cluster_size(cli_context->bs));

	val = spdk_blob_get_num_pages(cli_context->blob);
	printf("# of pages: %" PRIu64 "\n", val);

	spdk_blob_get_xattr_names(cli_context->blob, &names);

	printf("# of xattrs: %d\n", spdk_xattr_names_get_count(names));
	printf("xattrs:\n");
	for (i = 0; i < spdk_xattr_names_get_count(names); i++) {
		spdk_blob_get_xattr_value(cli_context->blob,
					  spdk_xattr_names_get_name(names, i),
					  &value, &value_len);
		if (value_len > BUFSIZE) {
			printf("FYI: adjusting size of xattr due to CLI limits.\n");
			value_len = BUFSIZE + 1;
		}
		printf("\n(%d) Name:%s\n", i,
		       spdk_xattr_names_get_name(names, i));
		printf("(%d) Value:\n", i);
		spdk_log_dump(stdout, "", value, value_len - 1);
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
	default:
		printf("state: UNKNOWN\n");
		break;
	}
	printf("open ref count: %d\n",
	       cli_context->blob->open_ref);

	spdk_xattr_names_free(names);
}

/*
 * Callback for getting the first blob, shared with simple blob listing as well.
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
		printf("\nList BLOBS:\n");
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

	spdk_bs_iter_next(cli_context->bs, blob, blob_iter_cb, cli_context);
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
 * Callback for set_xattr_open where we set or delete xattrs.
 */
static void
set_xattr_cb(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob open callback",
			  bserrno);
		return;
	}
	cli_context->blob = blob;

	if (cli_context->action == CLI_SET_XATTR) {
		spdk_blob_set_xattr(cli_context->blob, cli_context->key,
				    cli_context->value, strlen(cli_context->value) + 1);
		printf("Xattr has been set.\n");
	} else {
		spdk_blob_remove_xattr(cli_context->blob, cli_context->key);
		printf("Xattr has been removed.\n");
	}

	spdk_blob_sync_md(cli_context->blob, sync_cb, cli_context);
}

/*
 * Callback function for reading a blob for dumping to a file.
 */
static void
read_dump_cb(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	uint64_t bytes_written;

	if (bserrno) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error in read completion",
			  bserrno);
		return;
	}

	bytes_written = fwrite(cli_context->buff, NUM_IO_UNITS, cli_context->io_unit_size,
			       cli_context->fp);
	if (bytes_written != cli_context->io_unit_size) {
		fclose(cli_context->fp);
		unload_bs(cli_context, "Error with fwrite",
			  bserrno);
		return;
	}

	printf(".");
	if (++cli_context->io_unit_count < cli_context->blob_io_units) {
		/* perform another read */
		spdk_blob_io_read(cli_context->blob, cli_context->channel,
				  cli_context->buff, cli_context->io_unit_count,
				  NUM_IO_UNITS, read_dump_cb, cli_context);
	} else {
		/* done reading */
		printf("\nFile write complete (to %s).\n", cli_context->file);
		fclose(cli_context->fp);
		spdk_blob_close(cli_context->blob, close_cb, cli_context);
	}
}

/*
 * Callback for write completion on the import of a file to a blob.
 */
static void
write_imp_cb(void *arg1, int bserrno)
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
				   cli_context->io_unit_size,
				   cli_context->fp);
		cli_context->bytes_so_far += bytes_read;

		/* if this read is < 1 io_unit, fill with 0s */
		if (bytes_read < cli_context->io_unit_size) {
			uint8_t *offset = cli_context->buff + bytes_read;
			memset(offset, 0, cli_context->io_unit_size - bytes_read);
		}
	} else {
		/*
		 * Done reading the file, fill the rest of the blob with 0s,
		 * yeah we're memsetting the same io_unit over and over here
		 */
		memset(cli_context->buff, 0, cli_context->io_unit_size);
	}
	if (++cli_context->io_unit_count < cli_context->blob_io_units) {
		printf(".");
		spdk_blob_io_write(cli_context->blob, cli_context->channel,
				   cli_context->buff, cli_context->io_unit_count,
				   NUM_IO_UNITS, write_imp_cb, cli_context);
	} else {
		/* done writing */
		printf("\nBlob import complete (from %s).\n", cli_context->file);
		fclose(cli_context->fp);
		spdk_blob_close(cli_context->blob, close_cb, cli_context);
	}
}

/*
 * Callback for open blobs where we'll continue on dump a blob to a file or
 * import a file to a blob. For dump, the resulting file will always be the
 * full size of the blob.  For import, the blob will fill with the file
 * contents first and then 0 out the rest of the blob.
 */
static void
dump_imp_open_cb(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = cb_arg;

	if (bserrno) {
		unload_bs(cli_context, "Error in blob open callback",
			  bserrno);
		return;
	}
	cli_context->blob = blob;

	/*
	 * We'll transfer just one io_unit at a time to keep the buffer
	 * small. This could be bigger of course.
	 */
	cli_context->buff = spdk_malloc(cli_context->io_unit_size, ALIGN_4K, NULL,
					SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (cli_context->buff == NULL) {
		printf("Error in allocating memory\n");
		spdk_blob_close(cli_context->blob, close_cb, cli_context);
		return;
	}
	printf("Working");
	cli_context->blob_io_units = spdk_blob_get_num_io_units(cli_context->blob);
	cli_context->io_unit_count = 0;
	if (cli_context->action == CLI_DUMP_BLOB) {
		cli_context->fp = fopen(cli_context->file, "w");
		if (cli_context->fp == NULL) {
			printf("Error in opening file\n");
			spdk_blob_close(cli_context->blob, close_cb, cli_context);
			return;
		}

		/* read a io_unit of data from the blob */
		spdk_blob_io_read(cli_context->blob, cli_context->channel,
				  cli_context->buff, cli_context->io_unit_count,
				  NUM_IO_UNITS, read_dump_cb, cli_context);
	} else {
		cli_context->fp = fopen(cli_context->file, "r");
		if (cli_context->fp == NULL) {
			printf("Error in opening file: errno %d\n", errno);
			spdk_blob_close(cli_context->blob, close_cb, cli_context);
			return;
		}

		/* get the filesize then rewind read a io_unit of data from file */
		fseek(cli_context->fp, 0L, SEEK_END);
		cli_context->filesize = ftell(cli_context->fp);
		rewind(cli_context->fp);
		cli_context->bytes_so_far = fread(cli_context->buff, NUM_IO_UNITS,
						  cli_context->io_unit_size,
						  cli_context->fp);

		/* if the file is < a io_unit, fill the rest with 0s */
		if (cli_context->filesize < cli_context->io_unit_size) {
			uint8_t *offset =
				cli_context->buff + cli_context->filesize;

			memset(offset, 0,
			       cli_context->io_unit_size - cli_context->filesize);
		}

		spdk_blob_io_write(cli_context->blob, cli_context->channel,
				   cli_context->buff, cli_context->io_unit_count,
				   NUM_IO_UNITS, write_imp_cb, cli_context);
	}
}

/*
 * Callback function for writing a specific pattern to io_unit 0.
 */
static void
write_cb(void *arg1, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in write completion",
			  bserrno);
		return;
	}
	printf(".");
	if (++cli_context->io_unit_count < cli_context->blob_io_units) {
		spdk_blob_io_write(cli_context->blob, cli_context->channel,
				   cli_context->buff, cli_context->io_unit_count,
				   NUM_IO_UNITS, write_cb, cli_context);
	} else {
		/* done writing */
		printf("\nBlob fill complete (with 0x%x).\n", cli_context->fill_value);
		spdk_blob_close(cli_context->blob, close_cb, cli_context);
	}
}

/*
 * Callback function to fill a blob with a value, callback from open.
 */
static void
fill_blob_cb(void *arg1, struct spdk_blob *blob, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in open callback",
			  bserrno);
		return;
	}

	cli_context->blob = blob;
	cli_context->io_unit_count = 0;
	cli_context->blob_io_units = spdk_blob_get_num_io_units(cli_context->blob);
	cli_context->buff = spdk_malloc(cli_context->io_unit_size, ALIGN_4K, NULL,
					SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (cli_context->buff == NULL) {
		unload_bs(cli_context, "Error in allocating memory",
			  -ENOMEM);
		return;
	}

	memset(cli_context->buff, cli_context->fill_value,
	       cli_context->io_unit_size);
	printf("Working");
	spdk_blob_io_write(cli_context->blob, cli_context->channel,
			   cli_context->buff,
			   STARTING_IO_UNIT, NUM_IO_UNITS, write_cb, cli_context);
}

/*
 * Multiple actions require us to open the bs first so here we use
 * a common callback to set a bunch of values and then move on to
 * the next step saved off via function pointer.
 */
static void
load_bs_cb(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in load callback",
			  bserrno);
		return;
	}

	cli_context->bs = bs;
	cli_context->page_size = spdk_bs_get_page_size(cli_context->bs);
	cli_context->io_unit_size = spdk_bs_get_io_unit_size(cli_context->bs);
	cli_context->channel = spdk_bs_alloc_io_channel(cli_context->bs);
	if (cli_context->channel == NULL) {
		unload_bs(cli_context, "Error in allocating channel",
			  -ENOMEM);
		return;
	}

	switch (cli_context->action) {
	case CLI_SET_SUPER:
		spdk_bs_set_super(cli_context->bs, cli_context->superid,
				  set_super_cb, cli_context);
		break;
	case CLI_SHOW_BS:
		spdk_bs_get_super(cli_context->bs, show_bs_cb, cli_context);
		break;
	case CLI_CREATE_BLOB:
		spdk_bs_create_blob(cli_context->bs, blob_create_cb, cli_context);
		break;
	case CLI_SET_XATTR:
	case CLI_REM_XATTR:
		spdk_bs_open_blob(cli_context->bs, cli_context->blobid,
				  set_xattr_cb, cli_context);
		break;
	case CLI_SHOW_BLOB:
	case CLI_LIST_BLOBS:
		spdk_bs_iter_first(cli_context->bs, blob_iter_cb, cli_context);

		break;
	case CLI_DUMP_BLOB:
	case CLI_IMPORT_BLOB:
		spdk_bs_open_blob(cli_context->bs, cli_context->blobid,
				  dump_imp_open_cb, cli_context);
		break;
	case CLI_FILL:
		spdk_bs_open_blob(cli_context->bs, cli_context->blobid,
				  fill_blob_cb, cli_context);
		break;

	default:
		/* should never get here */
		exit(-1);
		break;
	}
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	printf("Unsupported bdev event: type %d\n", type);
}

/*
 * Load the blobstore.
 */
static void
load_bs(struct cli_context_t *cli_context)
{
	struct spdk_bs_dev *bs_dev = NULL;
	int rc;

	rc = spdk_bdev_create_bs_dev_ext(cli_context->bdev_name, base_bdev_event_cb,
					 NULL, &bs_dev);
	if (rc != 0) {
		printf("Could not create blob bdev, %s!!\n", spdk_strerror(-rc));
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_load(bs_dev, NULL, load_bs_cb, cli_context);
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
		cli_start(cli_context);
	}
}

/*
 * Callback function for initializing a blob.
 */
static void
bs_init_cb(void *cb_arg, struct spdk_blob_store *bs,
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
	int rc;

	printf("Init blobstore using bdev Name: %s\n", cli_context->bdev_name);

	rc = spdk_bdev_create_bs_dev_ext(cli_context->bdev_name, base_bdev_event_cb, NULL,
					 &cli_context->bs_dev);
	if (rc != 0) {
		printf("Could not create blob bdev, %s!!\n", spdk_strerror(-rc));
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_init(cli_context->bs_dev, NULL, bs_init_cb,
		     cli_context);
}

static void
spdk_bsdump_done(void *arg, int bserrno)
{
	struct cli_context_t *cli_context = arg;

	if (cli_context->cli_mode == CLI_MODE_CMD) {
		spdk_app_stop(0);
	} else {
		cli_context->action = CLI_NONE;
		cli_start(cli_context);
	}
}

static void
bsdump_print_xattr(FILE *fp, const char *bstype, const char *name, const void *value,
		   size_t value_len)
{
	if (strncmp(bstype, "BLOBFS", SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		if (strcmp(name, "name") == 0) {
			fprintf(fp, "%.*s", (int)value_len, (char *)value);
		} else if (strcmp(name, "length") == 0 && value_len == sizeof(uint64_t)) {
			uint64_t length;

			memcpy(&length, value, sizeof(length));
			fprintf(fp, "%" PRIu64, length);
		} else {
			fprintf(fp, "?");
		}
	} else if (strncmp(bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		if (strcmp(name, "name") == 0) {
			fprintf(fp, "%s", (char *)value);
		} else if (strcmp(name, "uuid") == 0 && value_len == sizeof(struct spdk_uuid)) {
			char uuid[SPDK_UUID_STRING_LEN];

			spdk_uuid_fmt_lower(uuid, sizeof(uuid), (struct spdk_uuid *)value);
			fprintf(fp, "%s", uuid);
		} else {
			fprintf(fp, "?");
		}
	} else {
		fprintf(fp, "?");
	}
}

/*
 * Dump metadata of an existing blobstore in a human-readable format.
 */
static void
dump_bs(struct cli_context_t *cli_context)
{
	int rc;

	printf("Init blobstore using bdev Name: %s\n", cli_context->bdev_name);

	rc = spdk_bdev_create_bs_dev_ext(cli_context->bdev_name, base_bdev_event_cb, NULL,
					 &cli_context->bs_dev);
	if (rc != 0) {
		printf("Could not create blob bdev, %s!!\n", spdk_strerror(-rc));
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_dump(cli_context->bs_dev, stdout, bsdump_print_xattr, spdk_bsdump_done, cli_context);
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

	while ((op = getopt(argc, argv, "b:d:f:hij:l:m:n:p:r:s:DST:Xx:")) != -1) {
		switch (op) {
		case 'b':
			if (strcmp(cli_context->bdev_name, "") == 0) {
				snprintf(cli_context->bdev_name, BUFSIZE, "%s", optarg);
			} else {
				printf("Current setting for -b is: %s\n", cli_context->bdev_name);
				usage(cli_context, "ERROR: -b option can only be set once.\n");
			}
			break;
		case 'D':
			cmd_chosen++;
			cli_context->action = CLI_DUMP_BS;
			break;
		case 'd':
			if (argv[optind] != NULL) {
				cmd_chosen++;
				cli_context->action = CLI_DUMP_BLOB;
				cli_context->blobid = spdk_strtoll(optarg, 10);
				snprintf(cli_context->file, BUFSIZE, "%s", argv[optind]);
			} else {
				usage(cli_context, "ERROR: missing parameter.\n");
			}
			break;
		case 'f':
			if (argv[optind] != NULL) {
				cmd_chosen++;
				cli_context->action = CLI_FILL;
				cli_context->blobid = spdk_strtoll(optarg, 10);
				cli_context->fill_value = spdk_strtol(argv[optind], 10);
			} else {
				usage(cli_context, "ERROR: missing parameter.\n");
			}
			break;
		case 'h':
			cmd_chosen++;
			cli_context->action = CLI_HELP;
			break;
		case 'i':
			if (cli_context->cli_mode != CLI_MODE_SCRIPT) {
				printf("Your entire blobstore will be destroyed. Are you sure? (y/n) ");
				if (scanf("%c%*c", &resp)) {
					if (resp == 'y' || resp == 'Y') {
						cmd_chosen++;
						cli_context->action = CLI_INIT_BS;
					} else {
						if (cli_context->cli_mode == CLI_MODE_CMD) {
							spdk_app_stop(0);
							return false;
						}
					}
				}
			} else {
				cmd_chosen++;
				cli_context->action = CLI_INIT_BS;
			}
			break;
		case 'j':
			if (cli_context->app_started == false) {
				cli_context->config_file = optarg;
			} else {
				usage(cli_context, "ERROR: -j option not valid during shell mode.\n");
			}
			break;
		case 'r':
			if (argv[optind] != NULL) {
				cmd_chosen++;
				cli_context->action = CLI_REM_XATTR;
				cli_context->blobid = spdk_strtoll(optarg, 10);
				snprintf(cli_context->key, BUFSIZE, "%s", argv[optind]);
			} else {
				usage(cli_context, "ERROR: missing parameter.\n");
			}
			break;
		case 'l':
			if (strcmp("bdevs", optarg) == 0) {
				cmd_chosen++;
				cli_context->action = CLI_LIST_BDEVS;
			} else if (strcmp("blobs", optarg) == 0) {
				cmd_chosen++;
				cli_context->action = CLI_LIST_BLOBS;
			} else {
				usage(cli_context, "ERROR: invalid option for list\n");
			}
			break;
		case 'm':
			if (argv[optind] != NULL) {
				cmd_chosen++;
				cli_context->action = CLI_IMPORT_BLOB;
				cli_context->blobid = spdk_strtoll(optarg, 10);
				snprintf(cli_context->file, BUFSIZE, "%s", argv[optind]);
			} else {
				usage(cli_context, "ERROR: missing parameter.\n");
			}
			break;
		case 'n':
			cli_context->num_clusters = spdk_strtol(optarg, 10);
			if (cli_context->num_clusters > 0) {
				cmd_chosen++;
				cli_context->action = CLI_CREATE_BLOB;
			} else {
				usage(cli_context, "ERROR: invalid option for new\n");
			}
			break;
		case 'p':
			cmd_chosen++;
			cli_context->action = CLI_SET_SUPER;
			cli_context->superid = spdk_strtoll(optarg, 10);
			break;
		case 'S':
			if (cli_context->cli_mode == CLI_MODE_CMD) {
				cmd_chosen++;
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
				cli_context->blobid = spdk_strtoll(optarg, 10);
			}
			break;
		case 'T':
			if (cli_context->cli_mode == CLI_MODE_CMD) {
				cmd_chosen++;
				cli_context->cli_mode = CLI_MODE_SCRIPT;
				if (argv[optind] && (strcmp("ignore", argv[optind]) == 0)) {
					g_script.ignore_errors = true;
				} else {
					g_script.ignore_errors = false;
				}
				snprintf(cli_context->script_file, BUFSIZE, "%s", optarg);
			} else {
				cli_context->action = CLI_NONE;
			}
			break;
		case 'X':
			cmd_chosen++;
			cli_context->action = CLI_SHELL_EXIT;
			break;
		case 'x':
			if (argv[optind] != NULL || argv[optind + 1] != NULL) {
				cmd_chosen++;
				cli_context->action = CLI_SET_XATTR;
				cli_context->blobid = spdk_strtoll(optarg, 10);
				snprintf(cli_context->key, BUFSIZE, "%s", argv[optind]);
				snprintf(cli_context->value, BUFSIZE, "%s", argv[optind + 1]);
			} else {
				usage(cli_context, "ERROR: missing parameter.\n");
			}
			break;
		default:
			usage(cli_context, "ERROR: invalid option\n");
		}
		/* only one actual command can be done at a time */
		if (cmd_chosen > 1) {
			usage(cli_context, "Error: Please choose only one command\n");
		}
	}

	if (cli_context->cli_mode == CLI_MODE_CMD && cmd_chosen == 0) {
		usage(cli_context, "Error: Please choose a command.\n");
	}

	/*
	 * We don't check the local boolean because in some modes it will have been set
	 * on and earlier command.
	 */
	if (strcmp(cli_context->bdev_name, "") == 0) {
		usage(cli_context, "Error: -b option is required.\n");
		cmd_chosen = 0;
	}

	/* in shell mode we'll call getopt multiple times so need to reset its index */
	optind = 0;
	return (cmd_chosen == 1);
}

/*
 * In script mode, we parsed a script file at startup and saved off a bunch of cmd
 * lines that we now parse with each run of cli_start so we us the same cmd parser
 * as cmd and shell modes.
 */
static bool
line_parser(struct cli_context_t *cli_context)
{
	bool cmd_chosen;
	char *tok = NULL;
	int blob_num = 0;
	int start_idx = cli_context->argc;
	int i;

	printf("\nSCRIPT NOW PROCESSING: %s\n", g_script.cmdline[g_script.cmdline_idx]);
	tok = strtok(g_script.cmdline[g_script.cmdline_idx], " ");
	while (tok != NULL) {
		/*
		 * We support one replaceable token right now, a $Bn
		 * represents the blobid that was created in position n
		 * so fish this out now and use it here.
		 */
		cli_context->argv[cli_context->argc] = strdup(tok);
		if (tok[0] == '$' && tok[1] == 'B') {
			tok += 2;
			blob_num = spdk_strtol(tok, 10);
			if (blob_num >= 0 && blob_num < MAX_SCRIPT_BLOBS) {
				cli_context->argv[cli_context->argc] =
					realloc(cli_context->argv[cli_context->argc], BUFSIZE);
				if (cli_context->argv[cli_context->argc] == NULL) {
					printf("ERROR: unable to realloc memory\n");
					spdk_app_stop(-1);
				}
				if (g_script.blobid[blob_num] == 0) {
					printf("ERROR: There is no blob for $B%d\n",
					       blob_num);
				}
				snprintf(cli_context->argv[cli_context->argc], BUFSIZE,
					 "%" PRIu64, g_script.blobid[blob_num]);
			} else {
				printf("ERROR: Invalid token or exceeded max blobs of %d\n",
				       MAX_SCRIPT_BLOBS);
			}
		}
		cli_context->argc++;
		tok = strtok(NULL, " ");
	}

	/* call parse cmd line with user input as args */
	cmd_chosen = cmd_parser(cli_context->argc, &cli_context->argv[0], cli_context);

	/* free strdup memory and reset arg count for next shell interaction */
	for (i = start_idx; i < cli_context->argc; i++) {
		free(cli_context->argv[i]);
		cli_context->argv[i] = NULL;
	}
	cli_context->argc = 1;

	g_script.cmdline_idx++;
	assert(g_script.cmdline_idx < MAX_SCRIPT_LINES);

	if (cmd_chosen == false) {
		printf("ERROR: Invalid script line starting with: %s\n\n",
		       g_script.cmdline[g_script.cmdline_idx - 1]);
		if (g_script.ignore_errors == false) {
			printf("** Aborting **\n");
			cli_context->action = CLI_SHELL_EXIT;
			cmd_chosen = true;
			unload_bs(cli_context, "", 0);
		} else {
			printf("** Skipping **\n");
		}
	}

	return cmd_chosen;
}

/*
 * For script mode, we read a series of commands from a text file and store them
 * in a global struct. That, along with the cli_mode that tells us we're in
 * script mode is what feeds the rest of the app in the same way as is it were
 * getting commands from shell mode.
 */
static void
parse_script(struct cli_context_t *cli_context)
{
	FILE *fp = NULL;
	size_t bufsize = BUFSIZE;
	int64_t bytes_in = 0;
	int i = 0;

	/* initialize global script values */
	for (i = 0; i < MAX_SCRIPT_BLOBS; i++) {
		g_script.blobid[i] = 0;
	}
	g_script.blobid_idx = 0;
	g_script.cmdline_idx = 0;
	i = 0;

	fp = fopen(cli_context->script_file, "r");
	if (fp == NULL) {
		printf("ERROR: unable to open script: %s\n",
		       cli_context->script_file);
		cli_cleanup(cli_context);
		exit(-1);
	}

	do {
		bytes_in = getline(&g_script.cmdline[i], &bufsize, fp);
		if (bytes_in > 0) {
			/* replace newline with null */
			spdk_str_chomp(g_script.cmdline[i]);

			/* ignore comments */
			if (g_script.cmdline[i][0] != '#') {
				i++;
			}
		}
	} while (bytes_in != -1 && i < MAX_SCRIPT_LINES - 1);
	fclose(fp);

	/* add an exit cmd in case they didn't */
	g_script.cmdline[i] = realloc(g_script.cmdline[i], BUFSIZE);
	if (g_script.cmdline[i] == NULL)  {
		int j;

		for (j = 0; j < i; j++) {
			free(g_script.cmdline[j]);
			g_script.cmdline[j] = NULL;
		}
		unload_bs(cli_context, "ERROR: unable to alloc memory.\n", 0);
	}
	snprintf(g_script.cmdline[i], BUFSIZE, "%s", "-X");
	g_script.max_index = i;
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

	/* If getline() failed (EOF), exit the shell. */
	if (bytes_in < 0) {
		free(line);
		cli_context->action = CLI_SHELL_EXIT;
		return true;
	}

	/* parse input and update cli_context so we can use common option parser */
	if (bytes_in > 0) {
		tok = strtok(line, " ");
	}
	while ((tok != NULL) && (cli_context->argc < MAX_ARGS)) {
		cli_context->argv[cli_context->argc] = strdup(tok);
		tok_len = strlen(tok);
		cli_context->argc++;
		tok = strtok(NULL, " ");
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
		cli_context->argv[i] = NULL;
	}
	cli_context->argc = 1;

	free(line);

	return cmd_chosen;
}

/*
 * This is the function we pass into the SPDK framework that gets
 * called first.
 */
static void
cli_start(void *arg1)
{
	struct cli_context_t *cli_context = arg1;

	/*
	 * If we're in script mode, we already have a list of commands so
	 * just need to pull them out one at a time and process them.
	 */
	if (cli_context->cli_mode == CLI_MODE_SCRIPT) {
		while (line_parser(cli_context) == false);
	}

	/*
	 * The initial cmd line options are parsed once before this function is
	 * called so if there is no action, we're in shell mode and will loop
	 * here until a a valid option is parsed and returned.
	 */
	if (cli_context->action == CLI_NONE) {
		while (cli_shell(cli_context, NULL) == false);
	}

	/* Decide what to do next based on cmd line parsing. */
	switch (cli_context->action) {
	case CLI_SET_SUPER:
	case CLI_SHOW_BS:
	case CLI_CREATE_BLOB:
	case CLI_SET_XATTR:
	case CLI_REM_XATTR:
	case CLI_SHOW_BLOB:
	case CLI_LIST_BLOBS:
	case CLI_DUMP_BLOB:
	case CLI_IMPORT_BLOB:
	case CLI_FILL:
		load_bs(cli_context);
		break;
	case CLI_INIT_BS:
		init_bs(cli_context);
		break;
	case CLI_DUMP_BS:
		dump_bs(cli_context);
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
		usage(cli_context, "");
		unload_complete(cli_context, 0);
		break;
	default:
		/* should never get here */
		exit(-1);
		break;
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	struct cli_context_t *cli_context = NULL;
	bool cmd_chosen;
	int rc = 0;

	if (argc < 2) {
		usage(cli_context, "ERROR: Invalid option\n");
		exit(-1);
	}

	cli_context = calloc(1, sizeof(struct cli_context_t));
	if (cli_context == NULL) {
		printf("ERROR: could not allocate context structure\n");
		exit(-1);
	}

	/* default to CMD mode until we've parsed the first parms */
	cli_context->cli_mode = CLI_MODE_CMD;
	cli_context->argv[0] = strdup(argv[0]);
	cli_context->argc = 1;

	/* parse command line */
	cmd_chosen = cmd_parser(argc, argv, cli_context);
	free(cli_context->argv[0]);
	cli_context->argv[0] = NULL;
	if (cmd_chosen == false) {
		cli_cleanup(cli_context);
		exit(-1);
	}

	/* after displaying help, just exit */
	if (cli_context->action == CLI_HELP) {
		usage(cli_context, "");
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
		printf("To create a config file named 'blobcli.json' for your NVMe device:\n");
		printf("   <path to spdk>/scripts/gen_nvme.sh --json-with-subsystems > blobcli.json\n");
		printf("and then re-run the cli tool.\n");
		exit(-1);
	}

	/*
	 * For script mode we keep a bunch of stuff in a global since
	 * none if it is passed back and forth to SPDK.
	 */
	if (cli_context->cli_mode == CLI_MODE_SCRIPT) {
		/*
		 * Now we'll build up the global which will direct this run of the app
		 * as it will have a list (g_script) of all of the commands line by
		 * line as if they were typed in on the shell at cmd line.
		 */
		parse_script(cli_context);
	}

	/* Set default values in opts struct along with name and conf file. */
	spdk_app_opts_init(&opts);
	opts.name = "blobcli";
	opts.json_config_file = cli_context->config_file;

	cli_context->app_started = true;
	rc = spdk_app_start(&opts, cli_start, cli_context);
	if (rc) {
		printf("ERROR!\n");
	}

	/* Free up memory that we allocated */
	cli_cleanup(cli_context);

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
