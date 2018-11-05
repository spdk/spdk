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

/*
 * This is a simple example of a virtual block device module that passes IO
 * down to a bdev (or bdevs) that its configured to attach to.
 */

#include "spdk/stdinc.h"

#include "vbdev_compress.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"


static int vbdev_compress_init(void);
static void vbdev_compress_get_spdk_running_config(FILE *fp);
static int vbdev_compress_get_ctx_size(void);
static void vbdev_compress_examine(struct spdk_bdev *bdev);
static void vbdev_compress_finish(void);
static int vbdev_compress_config_json(struct spdk_json_write_ctx *w);

static struct spdk_bdev_module compress_if = {
	.name = "compress",
	.module_init = vbdev_compress_init,
	.config_text = vbdev_compress_get_spdk_running_config,
	.get_ctx_size = vbdev_compress_get_ctx_size,
	.examine_config = vbdev_compress_examine,
	.module_fini = vbdev_compress_finish,
	.config_json = vbdev_compress_config_json
};

SPDK_BDEV_MODULE_REGISTER(&compress_if)

/* List of comp_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;
	char			*bdev_name;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
struct vbdev_compress {
	struct spdk_bdev		*base_bdev;	/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;	/* its descriptor we get from open */
	struct spdk_bdev		comp_bdev;	/* the compression virtual bdev */
	TAILQ_ENTRY(vbdev_compress)	link;
};
static TAILQ_HEAD(, vbdev_compress) g_comp_nodes = TAILQ_HEAD_INITIALIZER(g_comp_nodes);

/* The comp vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 */
struct comp_io_channel {
	struct spdk_io_channel		*base_ch;		/* IO channel of base device */
	struct spdk_poller		*poller;		/* completion poller */
	TAILQ_HEAD(, spdk_bdev_io)	pending_comp_ios;	/* outstanding operations to a comp library */
	struct spdk_io_channel_iter	*iter;			/* used with for_each_channel in reset */

};

/* Per I/O context for the compression vbdev. */
struct compress_bdev_io {

	struct comp_io_channel		*comp_ch;		/* used in completion handling */
	struct spdk_bdev_io_wait_entry	bdev_io_wait;		/* for bdev_io_wait */
};

static void
vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

/* This function is called after all channels have been quiesced following
 * a bdev reset.
 */
static void
_ch_quiesce_done(struct spdk_io_channel_iter *i, int status)
{
	struct comp_bdev_io *io_ctx = spdk_io_channel_iter_get_ctx(i);

	assert(TAILQ_EMPTY(&io_ctx->comp_ch->pending_comp_ios));
	assert(io_ctx->orig_io != NULL);

	spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/* This function is called per channel to quiesce IOs before completing a
 * bdev reset that we received.
 */
static void
_ch_quiesce(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct comp_io_channel *comp_ch = spdk_io_channel_get_ctx(ch);

	comp_ch->iter = i;
	/* When the poller runs, it will see the non-NULL iter and handle
	 * the quiesce.
	 */
}

/* Poller for the DPDK compression driver. */
static int
comp_dev_poller(void *args)
{
	struct comp_io_channel *comp_ch = args;


	return 0;
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_compress_destruct(void *ctx)
{
	struct vbdev_compress *comp_node = (struct vbdev_compress *)ctx;

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(comp_node->base_bdev);

	/* Close the underlying bdev. */
	spdk_bdev_close(comp_node->base_desc);

	/* Done with this comp_node. */
	TAILQ_REMOVE(&g_comp_nodes, comp_node, link);
	free(comp_node->comp_bdev.name);
	free(comp_node);
	return 0;
}

/* Completion callback for IO that were issued from this bdev.
 */
static void
_comp_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct compress_bdev_io *io_ctx = (struct compress_bdev_io *)orig_io->driver_ctx;

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

/* Resubmission function used by the bdev layer when a queued IO is ready to be
 * submitted.
 */
static void
vbdev_compress_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct compress_bdev_io *io_ctx = (struct compress_bdev_io *)bdev_io->driver_ctx;

	vbdev_compress_submit_request(io_ctx->ch, bdev_io);
}

/* Used to queue an IO in the event of resource issues. */
static void
vbdev_compress_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct compress_bdev_io *io_ctx = (struct compress_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_compress_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->ch, &io_ctx->bdev_io_wait);
	if (rc) {
		SPDK_ERRLOG("Queue io failed in vbdev_compress_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
pt_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_compress *comp_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct comp_io_channel *comp_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_bdev_readv_blocks(comp_node->base_desc, comp_ch->base_ch, bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks, _comp_complete_io,
				    bdev_io);
	if (rc) {
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Called when someone above submits IO to this vbdev. */
static void
vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_compress *comp_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct comp_io_channel *comp_ch = spdk_io_channel_get_ctx(ch);
	struct compress_bdev_io *io_ctx = (struct compress_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	/* TODO: review any ctx fields that may need memset */

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, pt_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_writev_blocks(comp_node->base_desc, comp_ch->base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks, _comp_complete_io,
					     bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(comp_node->base_desc, comp_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _comp_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(comp_node->base_desc, comp_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _comp_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(comp_node->base_desc, comp_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _comp_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(comp_node->base_desc, comp_ch->base_ch,
				     _comp_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for compress.\n");
			io_ctx->ch = ch;
			vbdev_compress_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* We'll just call the base bdev and let it answer however if we were more
 * restrictive for some reason (or less) we could get the response back
 * and modify according to our purposes.
 */
static bool
vbdev_compress_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_compress *comp_node = (struct vbdev_compress *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return spdk_bdev_io_type_supported(comp_node->base_bdev, io_type);
	default:
		return false;
	}
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel.
 */
static struct spdk_io_channel *
vbdev_compress_get_io_channel(void *ctx)
{
	struct vbdev_compress *comp_node = (struct vbdev_compress *)ctx;
	struct spdk_io_channel *comp_ch = NULL;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our comp_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	comp_ch = spdk_get_io_channel(comp_node);

	return comp_ch;
}

/* This is the output for get_bdevs() for this vbdev */
static int
vbdev_compress_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_compress *comp_node = (struct vbdev_compress *)ctx;

	spdk_json_write_name(w, "compress");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&comp_node->comp_bdev));
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(comp_node->base_bdev));
	spdk_json_write_object_end(w);

	return 0;
}

/* This is used to generate JSON that can configure this module to its current state. */
static int
vbdev_compress_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_compress *comp_node;

	TAILQ_FOREACH(comp_node, &g_comp_nodes, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "construct_compress_bdev");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(comp_node->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&comp_node->comp_bdev));
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
static int
comp_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct comp_io_channel *comp_ch = ctx_buf;
	struct vbdev_compress *comp_node = io_device;

	comp_ch->base_ch = spdk_bdev_get_io_channel(comp_node->base_desc);
	comp_ch->poller = spdk_poller_register(comp_dev_poller, comp_ch, 0);

	/* We use this queue to track outstanding IO in our lyaer. */
	TAILQ_INIT(&comp_ch->pending_comp_ios);
	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregsiter it here.
 */
static void
comp_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct comp_io_channel *comp_ch = ctx_buf;

	spdk_poller_unregister(&comp_ch->poller);
	spdk_put_io_channel(comp_ch->base_ch);
}

/* Create the compress association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_compress_insert_name(const char *bdev_name, const char *vbdev_name)
{
	struct bdev_names *name;

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("could not allocate bdev_names\n");
		return -ENOMEM;
	}

	name->bdev_name = strdup(bdev_name);
	if (!name->bdev_name) {
		SPDK_ERRLOG("could not allocate name->bdev_name\n");
		free(name);
		return -ENOMEM;
	}

	name->vbdev_name = strdup(vbdev_name);
	if (!name->vbdev_name) {
		SPDK_ERRLOG("could not allocate name->vbdev_name\n");
		free(name->bdev_name);
		free(name);
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;
}

/* On init, just parse config file and build list of comp vbdevs and bdev name pairs. */
static int
vbdev_compress_init(void)
{
	struct spdk_conf_section *sp = NULL;
	const char *conf_bdev_name = NULL;
	const char *conf_vbdev_name = NULL;
	struct bdev_names *name;
	int i, rc;

	sp = spdk_conf_find_section(NULL, "compress");
	if (sp == NULL) {
		return 0;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "COMP", i)) {
			break;
		}

		conf_bdev_name = spdk_conf_section_get_nmval(sp, "COMP", i, 0);
		if (!conf_bdev_name) {
			SPDK_ERRLOG("compress configuration missing bdev name\n");
			break;
		}

		conf_vbdev_name = spdk_conf_section_get_nmval(sp, "COMP", i, 1);
		if (!conf_vbdev_name) {
			SPDK_ERRLOG("compress configuration missing comp_bdev name\n");
			break;
		}

		rc = vbdev_compress_insert_name(conf_bdev_name, conf_vbdev_name);
		if (rc) {
			return rc;
		}
	}
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		SPDK_NOTICELOG("conf parse matched: %s\n", name->bdev_name);
	}
	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_compress_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_compress_get_ctx_size(void)
{
	return sizeof(struct compress_bdev_io);
}

/* Called when SPDK wants to save the current config of this vbdev module to
 * a file.
 */
static void
vbdev_compress_get_spdk_running_config(FILE *fp)
{
	struct bdev_names *names = NULL;

	fprintf(fp, "\n[compress]\n");
	TAILQ_FOREACH(names, &g_bdev_names, link) {
		fprintf(fp, "  PT %s %s\n", names->bdev_name, names->vbdev_name);
	}
	fprintf(fp, "\n");
}

/* Called when SPDK wants to output the bdev specific methods. */
static void
vbdev_compress_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_compress_fn_table = {
	.destruct		= vbdev_compress_destruct,
	.submit_request		= vbdev_compress_submit_request,
	.io_type_supported	= vbdev_compress_io_type_supported,
	.get_io_channel		= vbdev_compress_get_io_channel,
	.dump_info_json		= vbdev_compress_dump_info_json,
	.write_config_json	= vbdev_compress_write_config_json,
};

/* Called when the underlying base bdev goes away. */
static void
vbdev_compress_base_bdev_hotremove_cb(void *ctx)
{
	struct vbdev_compress *comp_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(comp_node, &g_comp_nodes, link, tmp) {
		if (bdev_find == comp_node->base_bdev) {
			spdk_bdev_unregister(&comp_node->comp_bdev, NULL, NULL);
		}
	}
}

/* Create and register the compress vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
static void
vbdev_compress_register(struct spdk_bdev *bdev)
{
	struct bdev_names *name;
	struct vbdev_compress *comp_node;
	int rc;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the comp_node & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev->name) != 0) {
			continue;
		}

		SPDK_NOTICELOG("Match on %s\n", bdev->name);
		comp_node = calloc(1, sizeof(struct vbdev_compress));
		if (!comp_node) {
			SPDK_ERRLOG("could not allocate comp_node\n");
			break;
		}

		/* The base bdev that we're attaching to. */
		comp_node->base_bdev = bdev;
		comp_node->comp_bdev.name = strdup(name->vbdev_name);
		if (!comp_node->comp_bdev.name) {
			SPDK_ERRLOG("could not allocate comp_bdev name\n");
			free(comp_node);
			break;
		}
		comp_node->comp_bdev.product_name = "compress";

		/* Copy some properties from the underlying base bdev. */
		comp_node->comp_bdev.write_cache = bdev->write_cache;
		comp_node->comp_bdev.need_aligned_buffer = bdev->need_aligned_buffer;
		comp_node->comp_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		comp_node->comp_bdev.blocklen = bdev->blocklen;
		comp_node->comp_bdev.blockcnt = bdev->blockcnt;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our comp_bdev node here.
		 */
		comp_node->comp_bdev.ctxt = comp_node;
		comp_node->comp_bdev.fn_table = &vbdev_compress_fn_table;
		comp_node->comp_bdev.module = &compress_if;
		TAILQ_INSERT_TAIL(&g_comp_nodes, comp_node, link);

		spdk_io_device_register(comp_node, comp_bdev_ch_create_cb, comp_bdev_ch_destroy_cb,
					sizeof(struct comp_io_channel),
					name->vbdev_name);
		SPDK_NOTICELOG("io_device created at: 0x%p\n", comp_node);

		rc = spdk_bdev_open(bdev, true, vbdev_compress_base_bdev_hotremove_cb,
				    bdev, &comp_node->base_desc);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
			TAILQ_REMOVE(&g_comp_nodes, comp_node, link);
			free(comp_node->comp_bdev.name);
			free(comp_node);
			break;
		}
		SPDK_NOTICELOG("bdev opened\n");

		rc = spdk_bdev_module_claim_bdev(bdev, comp_node->base_desc, comp_node->comp_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
			spdk_bdev_close(comp_node->base_desc);
			TAILQ_REMOVE(&g_comp_nodes, comp_node, link);
			free(comp_node->comp_bdev.name);
			free(comp_node);
			break;
		}
		SPDK_NOTICELOG("bdev claimed\n");

		rc = spdk_vbdev_register(&comp_node->comp_bdev, &bdev, 1);
		if (rc) {
			SPDK_ERRLOG("could not register comp_bdev\n");
			spdk_bdev_close(comp_node->base_desc);
			TAILQ_REMOVE(&g_comp_nodes, comp_node, link);
			free(comp_node->comp_bdev.name);
			free(comp_node);
			break;
		}
		SPDK_NOTICELOG("comp_bdev registered\n");
		SPDK_NOTICELOG("created comp_bdev for: %s\n", name->vbdev_name);
	}
}

/* Create the compress disk from the given bdev and vbdev name. */
int
create_compress_disk(const char *bdev_name, const char *vbdev_name)
{
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	bdev = spdk_bdev_get_by_name(bdev_name);

	/* Insert the bdev into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	rc = vbdev_compress_insert_name(bdev_name, vbdev_name);
	if (rc) {
		return rc;
	}

	if (!bdev) {
		return 0;
	}

	vbdev_compress_register(bdev);

	return 0;
}

void
delete_compress_disk(struct spdk_bdev *bdev, spdk_delete_compress_complete cb_fn, void *cb_arg)
{
	struct bdev_names *name;

	if (!bdev || bdev->module != &compress_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
	 * vbdev does not get re-created if the same bdev is constructed at some other time,
	 * unless the underlying bdev was hot-removed.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->vbdev_name, bdev->name) == 0) {
			TAILQ_REMOVE(&g_bdev_names, name, link);
			free(name->bdev_name);
			free(name->vbdev_name);
			free(name);
			break;
		}
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

/* Because we specified this function in our pt bdev function table when we
 * registered our pt bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
static void
vbdev_compress_examine(struct spdk_bdev *bdev)
{
	vbdev_compress_register(bdev);

	spdk_bdev_module_examine_done(&compress_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_compress", SPDK_LOG_VBDEV_compress)
