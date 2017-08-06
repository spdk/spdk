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

static const char *program_name = "blobcli";
static const char *program_conf = "blobcli.conf";
static const char *bdev_name = "Nvme0n1";
static const char *ver = "0.0";

enum cli_action_type {
	CLI_SHOW_BS,
	CLI_SHOW_BLOB,
	CLI_CREATE_BLOB,
	CLI_LIST_BDEVS,
	CLI_LIST_BLOBS,
	CLI_INIT_BS
};
#define BUFSIZE 255

// TODO:  sort these, create union based on ACTION or common
struct cli_context_t {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	uint8_t *read_buff;
	uint8_t *write_buff;
	uint64_t page_size;
	enum cli_action_type action;
	char bs_name[BUFSIZE + 1];
	const char *bdev_name;
	int rc;
	int num_clusters;
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
	printf("Usage: %s [options]\n", program_name);
	printf("\n");
	printf("%s is a command line tool for interacting with blobstore\n",
	       program_name);
	printf("on the underlying device specified in the conf file passed\n");
	printf("in as a command line option.\n");
	printf("\nOptions include:\n");
	printf("\t-c <file.conf> - the SPDK config file\n");
	printf("\t-i <bs name> - initialize a blobstore\n");
	printf("\t-l bdevs | blobs - list either available bdevs or existing blobs\n");
	printf("\t-n <# clusters> - create new blob\n");
	printf("\t-s <blobid> | bs - show blob info or blobstore info\n");
	printf("\n");
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
	SPDK_NOTICELOG("blob now has USED clusters of %" PRIu64 "\n",
		       total);

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

	cli_context->blobid = blobid;  // START HERE< THIS LOOKS WRONG
	SPDK_NOTICELOG("New blob id %" PRIu64 "\n", cli_context->blobid);

	/* We have to open the blob before we can do things like resize. */
	spdk_bs_md_open_blob(cli_context->bs, cli_context->blobid,
			     open_now_resize, cli_context);
}

/*
 * callback for load bs where we'll continue on to show bs info.
 */
static void
show_bs(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;
	uint64_t val;

	if (bserrno) {
		unload_bs(cli_context, "Error in open callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	SPDK_NOTICELOG("Blobstore Public Info:\n");

	val = spdk_bs_get_page_size(cli_context->bs);
	SPDK_NOTICELOG("\tpage size: %" PRIu64 "\n", val);

	val = spdk_bs_get_cluster_size(cli_context->bs);
	SPDK_NOTICELOG("\tcluster size: %" PRIu64 "\n", val);

	val = spdk_bs_free_cluster_count(cli_context->bs);
	SPDK_NOTICELOG("\t# free clusters: %" PRIu64 "\n", val);

	SPDK_NOTICELOG("\n");
	SPDK_NOTICELOG("Blobstore Private Info:\n");

	unload_bs(cli_context, "", 0);
}

/*
 * callback for load bs where we'll continue on to create a blob.
 */
static void
create_blob(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in open callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;
	spdk_bs_md_create_blob(cli_context->bs, blob_create_complete,
			       cli_context);
}

/*
 * callback for getting the first blob
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

	SPDK_NOTICELOG("Found blob with ID# %" PRIu64 "\n",
		       spdk_blob_get_id(blob));

	spdk_bs_md_iter_next(cli_context->bs, &blob, blob_iter_cb,
			     cli_context);
}

/*
 * callback for load bs where we'll continue on to list all blobs.
 */
static void
list_blobs(void *arg1, struct spdk_blob_store *bs, int bserrno)
{
	struct cli_context_t *cli_context = arg1;

	if (bserrno) {
		unload_bs(cli_context, "Error in open callback",
			  bserrno);
		return;
	}
	cli_context->bs = bs;

	printf("\n");
	SPDK_NOTICELOG("List BLOBS:\n");

	spdk_bs_md_iter_first(cli_context->bs, blob_iter_cb, cli_context);
}

/*
 * multiple actions require us to open the bs first.
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
	switch (cli_context->action) {
	case CLI_LIST_BLOBS:
		spdk_bs_load(bs_dev, list_blobs, cli_context);
		break;
	case CLI_CREATE_BLOB:
		spdk_bs_load(bs_dev, create_blob, cli_context);
		break;
	case CLI_SHOW_BS:
		spdk_bs_load(bs_dev, show_bs, cli_context);
		break;
	default:
		/* should never get here */
		spdk_app_stop(-1);
		return;
	}
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
 * Free up memory that we allocated.
 */
static void
cli_cleanup(struct cli_context_t *cli_context)
{
	if (cli_context->read_buff) {
		spdk_dma_free(cli_context->read_buff);
	}
	if (cli_context->write_buff) {
		spdk_dma_free(cli_context->write_buff);
	}
	if (cli_context) {
		free(cli_context);
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
	SPDK_NOTICELOG("blobstore init'd: %s (%p)\n", cli_context->bs_name,
		       cli_context->bs);

	cli_context->page_size = spdk_bs_get_page_size(cli_context->bs);

	unload_bs(cli_context, "", 0);
}

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

static void
cli_start(void *arg1, void *arg2)
{
	struct cli_context_t *cli_context = arg1;

	switch (cli_context->action) {
	case CLI_SHOW_BS:
		load_bs(cli_context);
		break;
	case CLI_CREATE_BLOB:
		load_bs(cli_context);
		break;
	case CLI_INIT_BS:
		init_bs(cli_context);
		break;
	case CLI_LIST_BDEVS:
		list_bdevs();
		break;
	case CLI_LIST_BLOBS:
		load_bs(cli_context);
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
	int op_passed = 0;

	cli_context = calloc(1, sizeof(struct cli_context_t));
	if (cli_context == NULL) {
		SPDK_ERRLOG("ERROR: could not allocate context structure\n");
		exit(-1);
	}
	cli_context->bdev_name = bdev_name;

	while ((op = getopt(argc, argv, "s:n:c:l:i:")) != -1) {
		switch (op) {
		case 's':
			if (strcmp("bs", optarg) == 0) {
				cli_context->action = CLI_SHOW_BS;
			} else {
				cli_context->action = CLI_SHOW_BLOB;
				cli_context->blobid = atoi(optarg);
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
			snprintf(cli_context->bs_name, BUFSIZE, "%s", optarg);
			cli_context->action = CLI_INIT_BS;
			break;
		default:
			usage("ERROR: Invalid option\n");
			exit(1);
		}
		op_passed = 1;
	}

	if (op_passed == 0) {
		usage("ERROR: Invalid option\n");
		exit(1);
	}

	/* if they don't supply a conf name, use the default */
	if (!config_file) {
		config_file = program_conf;
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
