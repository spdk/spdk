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

#include "blob_nvme.h"

#include "spdk/env.h"
#include "spdk/stdinc.h"

struct nvme_blob_context_t {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	uint8_t *read_buff;
	uint8_t *write_buff;
	uint64_t page_size;
	int rc;
};

struct nvme_blob_msg {
	spdk_thread_fn cb_fn;
	void *cb_arg;
};

struct nvme_blob_thread {
	struct spdk_thread *thread;
	struct spdk_ring *ring;
};

struct ctrlr_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	struct ctrlr_entry *next;
	char name[1024];
};

static struct ctrlr_entry *g_controllers = NULL;
static struct spdk_nvme_ns *g_namespace = NULL;
static bool g_complete;

/*
 * nvme_blob_send_msg and nvme_blob_thread_init initialize the thread context for this blobstore
 *  and provide a method for communicating between threads.
 */

static void
nvme_blob_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	struct nvme_blob_thread *nvme_thread = thread_ctx;
	struct nvme_blob_msg *msg;
	msg = calloc(1, sizeof(struct nvme_blob_msg));
	assert(msg != NULL);

	msg->cb_fn = fn;
	msg->cb_arg = ctx;

	spdk_ring_enqueue(nvme_thread->ring, (void **)&msg, 1);
}

static struct nvme_blob_thread *
nvme_blob_thread_init(void)
{
	struct nvme_blob_thread *blob_thread = calloc(1, sizeof(struct nvme_blob_thread));
	blob_thread->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, SPDK_ENV_SOCKET_ID_ANY);
	blob_thread->thread = spdk_allocate_thread(nvme_blob_send_msg, blob_thread);
	return blob_thread;
}

static void
nvme_blob_thread_free(struct nvme_blob_thread *blob_thread)
{
	spdk_ring_free(blob_thread->ring);
	spdk_free_thread();
	free(blob_thread);
}

/* register_ns, probe_cb, and attach_cb are all used to reserve an NVMe Namespace. */

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	/*
	 * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
	 *  controller.  During initialization, the IDENTIFY data for the
	 *  controller is read using an NVMe admin command, and that data
	 *  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	 *  detailed information on the controller.  Refer to the NVMe
	 *  specification for more details on IDENTIFY for NVMe controllers.
	 */
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	g_namespace = ns;

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int num_ns;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	/*
	 * Each controller has one or more namespaces.  An NVMe namespace is basically
	 *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	 *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	 *  it will just be one namespace.
	 *
	 * Note that in NVMe, namespace IDs start at 1, not 0.
	 */
	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	printf("Using controller %s with %d namespaces.\n", entry->name, num_ns);
	if (num_ns >= 1) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
		register_ns(ctrlr, ns);
	}
}



/*
 * Free up memory that we allocated.
 */
static void
cleanup(struct nvme_blob_context_t *nvme_blob_context)
{
	spdk_dma_free(nvme_blob_context->read_buff);
	spdk_dma_free(nvme_blob_context->write_buff);
	free(nvme_blob_context);
}

/*
 * Callback routine for the blobstore unload.
 */
static void
unload_complete(void *cb_arg, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = cb_arg;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d unloading the bobstore\n", bserrno);
		nvme_blob_context->rc = bserrno;
	}

	g_complete = true;
}

/*
 * Unload the blobstore, cleaning up as needed.
 */
static void
unload_bs(struct nvme_blob_context_t *nvme_blob_context, char *msg, int bserrno)
{
	if (bserrno) {
		SPDK_ERRLOG("%s (err %d)\n", msg, bserrno);
		nvme_blob_context->rc = bserrno;
	}
	if (nvme_blob_context->bs) {
		if (nvme_blob_context->channel) {
			spdk_bs_free_io_channel(nvme_blob_context->channel);
		}
		spdk_bs_unload(nvme_blob_context->bs, unload_complete, nvme_blob_context);
	}
}

/*
 * Callback routine for the deletion of a blob.
 */
static void
delete_complete(void *arg1, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in delete completion",
			  bserrno);
		return;
	}

	/* We're all done, we can unload the blobstore. */
	unload_bs(nvme_blob_context, "", 0);
}

/*
 * Function for deleting a blob.
 */
static void
delete_blob(void *arg1, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in close completion",
			  bserrno);
		return;
	}

	spdk_bs_md_delete_blob(nvme_blob_context->bs, nvme_blob_context->blobid,
			       delete_complete, nvme_blob_context);
}

/*
 * Callback function for reading a blob.
 */
static void
read_complete(void *arg1, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;
	int match_res = -1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in read completion",
			  bserrno);
		return;
	}

	/* Now let's make sure things match. */
	match_res = memcmp(nvme_blob_context->write_buff, nvme_blob_context->read_buff,
			   nvme_blob_context->page_size);
	if (match_res) {
		unload_bs(nvme_blob_context, "Error in data compare", -1);
		return;
	} else {
		SPDK_NOTICELOG("read SUCCESS and data matches!\n");
	}

	/* Now let's close it and delete the blob in the callback. */
	spdk_bs_md_close_blob(&nvme_blob_context->blob, delete_blob,
			      nvme_blob_context);
}

/*
 * Function for reading a blob.
 */
static void
read_blob(struct nvme_blob_context_t *nvme_blob_context)
{
	SPDK_NOTICELOG("entry\n");

	nvme_blob_context->read_buff = spdk_dma_malloc(nvme_blob_context->page_size,
				       0x1000, NULL);
	if (nvme_blob_context->read_buff == NULL) {
		unload_bs(nvme_blob_context, "Error in memory allocation",
			  -ENOMEM);
		return;
	}

	/* Issue the read and compare the results in the callback. */
	spdk_bs_io_read_blob(nvme_blob_context->blob, nvme_blob_context->channel,
			     nvme_blob_context->read_buff, 0, 1, read_complete,
			     nvme_blob_context);
}

/*
 * Callback function for writing a blob.
 */
static void
write_complete(void *arg1, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in write completion",
			  bserrno);
		return;
	}

	/* Now let's read back what we wrote and make sure it matches. */
	read_blob(nvme_blob_context);
}

/*
 * Function for writing to a blob.
 */
static void
blob_write(struct nvme_blob_context_t *nvme_blob_context)
{
	SPDK_NOTICELOG("entry\n");

	/*
	 * Buffers for data transfer need to be allocated via SPDK. We will
	 * tranfer 1 page of 4K aligned data at offset 0 in the blob.
	 */
	nvme_blob_context->write_buff = spdk_dma_malloc(nvme_blob_context->page_size,
					0x1000, NULL);
	if (nvme_blob_context->write_buff == NULL) {
		unload_bs(nvme_blob_context, "Error in allocating memory",
			  -ENOMEM);
		return;
	}
	memset(nvme_blob_context->write_buff, 0x5a, nvme_blob_context->page_size);

	/* Now we have to allocate a channel. */
	nvme_blob_context->channel = spdk_bs_alloc_io_channel(nvme_blob_context->bs);
	if (nvme_blob_context->channel == NULL) {
		unload_bs(nvme_blob_context, "Error in allocating channel",
			  -ENOMEM);
		return;
	}

	/* Let's perform the write, 1 page at offset 0. */
	spdk_bs_io_write_blob(nvme_blob_context->blob, nvme_blob_context->channel,
			      nvme_blob_context->write_buff,
			      0, 1, write_complete, nvme_blob_context);
}

/*
 * Callback function for sync'ing metadata.
 */
static void
sync_complete(void *arg1, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in sync callback",
			  bserrno);
		return;
	}

	/* Blob has been created & sized & MD sync'd, let's write to it. */
	blob_write(nvme_blob_context);
}

/*
 * Callback function for opening a blob.
 */
static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = cb_arg;
	uint64_t free = 0;
	uint64_t total = 0;
	int rc = 0;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in open completion",
			  bserrno);
		return;
	}


	nvme_blob_context->blob = blob;
	free = spdk_bs_free_cluster_count(nvme_blob_context->bs);
	SPDK_NOTICELOG("blobstore has FREE clusters of %" PRIu64 "\n",
		       free);

	/*
	 * Before we can use our new blob, we have to resize it
	 * as the initial size is 0. For this example we'll use the
	 * full size of the blobstore but it would be expected that
	 * there'd usually be many blobs of various sizes. The resize
	 * unit is a cluster.
	 */
	rc = spdk_bs_md_resize_blob(nvme_blob_context->blob, free);
	if (rc) {
		unload_bs(nvme_blob_context, "Error in blob resize",
			  bserrno);
		return;
	}

	total = spdk_blob_get_num_clusters(nvme_blob_context->blob);
	SPDK_NOTICELOG("resized blob now has USED clusters of %" PRIu64 "\n",
		       total);

	/*
	 * Metadata is stored in volatile memory for performance
	 * reasons and therefore needs to be synchronized with
	 * non-volatile storage to make it persistent. This can be
	 * done manually, as shown here, or if not it will be done
	 * automatically when the blob is closed. It is always a
	 * good idea to sync after making metadata changes unless
	 * it has an unacceptable impact on application performance.
	 */
	spdk_bs_md_sync_blob(nvme_blob_context->blob, sync_complete,
			     nvme_blob_context);
}

/*
 * Callback function for creating a blob.
 */
static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error in blob create callback",
			  bserrno);
		return;
	}

	nvme_blob_context->blobid = blobid;
	SPDK_NOTICELOG("new blob id %" PRIu64 "\n", nvme_blob_context->blobid);

	/* We have to open the blob before we can do things like resize. */
	spdk_bs_md_open_blob(nvme_blob_context->bs, nvme_blob_context->blobid,
			     open_complete, nvme_blob_context);
}

/*
 * Function for creating a blob.
 */
static void
create_blob(struct nvme_blob_context_t *nvme_blob_context)
{
	SPDK_NOTICELOG("entry\n");
	spdk_bs_md_create_blob(nvme_blob_context->bs, blob_create_complete,
			       nvme_blob_context);
}

/*
 * Callback function for initializing the blobstore.
 */
static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
	struct nvme_blob_context_t *nvme_blob_context = cb_arg;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(nvme_blob_context, "Error init'ing the blobstore",
			  bserrno);
		return;
	}

	nvme_blob_context->bs = bs;
	SPDK_NOTICELOG("blobstore: %p\n", nvme_blob_context->bs);
	/*
	 * We will use the page size in allocating buffers, etc., later
	 * so we'll just save it in out context buffer here.
	 */
	nvme_blob_context->page_size = spdk_bs_get_page_size(nvme_blob_context->bs);

	/*
	 * The blostore has been initialized, let's create a blob.
	 * Note that we could allcoate an SPDK event and use
	 * spdk_event_call() to schedule it if we wanted to keep
	 * our events as limited as possible wrt the amount of
	 * work that they do.
	 */
	create_blob(nvme_blob_context);
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1)
{
	struct nvme_blob_context_t *nvme_blob_context = arg1;
	struct spdk_nvme_ns			*ns;
	struct spdk_bs_dev *bs_dev = NULL;
	struct nvme_blob_io_ctx *ctx;
	struct nvme_blob_thread *blob_thread;
	struct nvme_blob_msg *msg;
	int count;

	SPDK_NOTICELOG("entry\n");

	ns = g_namespace;
	blob_thread = nvme_blob_thread_init();

	bs_dev = spdk_bdev_nvme_create_bs_dev(ns);

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Could not create blob bdev!!\n");
		return;
	}

	spdk_bs_init(bs_dev, NULL, bs_init_complete, nvme_blob_context);

	g_complete = false;
	ctx = spdk_io_channel_get_ctx(bs_dev->create_channel(bs_dev));

	if (ctx->qpair == NULL) {
		SPDK_NOTICELOG("qpair is null\n");
	}

	while (!g_complete) {
		count = spdk_ring_dequeue(blob_thread->ring, (void **)&msg, 1);
		if (count > 0) {
			msg->cb_fn(msg->cb_arg);
			free(msg);
		}

		spdk_nvme_qpair_process_completions(ctx->qpair, 0);
	}

	nvme_blob_thread_free(blob_thread);
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts = {};
	int rc = 0;
	struct nvme_blob_context_t *nvme_blob_context = NULL;

	SPDK_NOTICELOG("entry\n");

	spdk_env_opts_init(&opts);
	opts.name = "nvme_blob";
	opts.shm_id = 0;
	spdk_env_init(&opts);

	printf("Initializing NVMe Controllers\n");

	/*
	 * Start the SPDK NVMe enumeration process.  probe_cb will be called
	 *  for each NVMe controller found, giving our application a choice on
	 *  whether to attach to each controller.  attach_cb will then be
	 *  called for each controller after the SPDK NVMe driver has completed
	 *  initializing the controller we chose to attach.
	 */
	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_controllers == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		return 1;
	}
	printf("Initialization complete.\n");
	nvme_blob_context = calloc(1, sizeof(struct nvme_blob_context_t));

	if (!nvme_blob_context) {
		fprintf(stderr, "unable to allocate memory for context\n");
		return 1;
	}

	hello_start(nvme_blob_context);
	cleanup(nvme_blob_context);

	return rc;
}
