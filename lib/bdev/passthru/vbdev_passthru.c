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
 * This is a simple example of a virtual block device that uses ISA-L to
 * provide a per logical volume encryption capability.
 */

#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

SPDK_DECLARE_BDEV_MODULE(passthru);

/* list of pt_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char                    *vbdev_name;
	char                    *bdev_name;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
struct pt_nodes {
	struct spdk_bdev                *base_bdev; /* the thing we're attaching to */
	struct spdk_bdev_desc	        *base_desc; /* its descriptor we get from open */
	struct spdk_bdev                pt_bdev;    /* the PT virtual bdev */
	TAILQ_ENTRY(pt_nodes)	link;
};
static TAILQ_HEAD(, pt_nodes) g_pt_nodes = TAILQ_HEAD_INITIALIZER(g_pt_nodes);

/* The pt vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * If this vbdev needed to implement a poller or a queue for IO, this is where those things
 * would be defined. This passthru bdev doesn't actually need to allocate a channel, it could
 * simply pass back the channel of the bdev underneath it but for example purposes we will
 * present it's own to the upper layers.
 */
struct pt_io_channel {
	struct spdk_io_channel          *base_ch;   /* IO channel of base device */
};

/* Just for fun, this pt_bdev module doesn't need it but this is essentially a per IO
 * IO context we we get handed by the bdev layer... */
struct vbdev_ctx {
	uint8_t test;
};

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next. */
static int
vbdev_passthru_destruct(void *ctx)
{
	struct pt_nodes *pt_node = (struct pt_nodes *)ctx;

	SPDK_NOTICELOG("Entry\n");

	/* unclaim the underlying bdev */
	spdk_bdev_module_release_bdev(pt_node->base_bdev);

	/* close the underlying bdev */
	spdk_bdev_close(pt_node->base_desc);

	/* Done with this pt_node, we can't free the node here because
	 * destruct will be called when the module goes down. */
	return 0;
}

/* Completion callback for IO that were issued from this bdev.  The original bdev_io
 * is passed in an an arg so we'll complete that one with the appriorirate status
 * and then free the one that this module issued.
 */
static void
_pt_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	/* we setup this value in the submission routine, just showing here that it is
	 * passed back to us. */
	if (orig_io->driver_ctx[0] != 0x5a) {
		SPDK_ERRLOG("Error, orignial IO device_ctx is wrong! 0x%x\n",
			    orig_io->driver_ctx[0]);
	}

	/* Complete the oringial IO and then free the one that we crated here in the pt bdev
	 * as a result of issuing an IO via submit_reqeust. */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}


/* Called when someone above submits IO to this pt vbdev. We're simply passing it on here
 * via spdk IO calls which in turn allocate another bdev IO and call our cpl callback provided
 * below along with the original bdiv_io so that we can complete it once this once completes.
 */
static void
vbdev_passthru_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct pt_nodes *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct pt_nodes, pt_bdev);
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
	int rc = 1;

	SPDK_NOTICELOG("Entry IO type %d\n", bdev_io->type);

	/* Setup a per IO context value, we don't do anything with it in the vbdev other
	 * than confirm we get the same thing back in the caompletion callback just to
	 * demosntrate. */
	bdev_io->driver_ctx[0] = 0x5a;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rc = spdk_bdev_readv_blocks(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks, _pt_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_writev_blocks(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks, _pt_complete_io,
					     bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(pt_node->base_desc, pt_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(pt_node->base_desc, pt_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(pt_node->base_desc, pt_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(pt_node->base_desc, pt_ch->base_ch,
				     _pt_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("passthru: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* We'll just call the base bdev and let it answer however if we were more
 * restrictive for some reason (or less) we could get the repsonse back
 * and modify according to our purposes. */
static bool
vbdev_passthru_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct pt_nodes *pt_node = (struct pt_nodes *)ctx;

	SPDK_NOTICELOG("Entry\n");

	return spdk_bdev_io_type_supported(pt_node->base_bdev, io_type);
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the context we provided when
 * we created our PT vbdev in examine() which for this bdev is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our PT node */
static struct spdk_io_channel *
vbdev_passthru_get_io_channel(void *ctx)
{
	struct pt_nodes *pt_node = (struct pt_nodes *)ctx;
	struct spdk_io_channel *pt_ch = NULL;

	SPDK_NOTICELOG("Entry\n");

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK cahnnel structure plus the size of our pt_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	pt_ch = spdk_get_io_channel(pt_node);

	return pt_ch;
}

static int
vbdev_passthru_dump_config_json(void *ctx, struct spdk_json_write_ctx *write_ctx)
{
	struct pt_nodes *pt_node = (struct pt_nodes *)ctx;

	SPDK_NOTICELOG("Entry\n");

	if (pt_node) {
		// TODO: this is the output for get_bdevs() - include base bdev name, probably nothing else
		spdk_json_write_name(write_ctx, "passthru");
		spdk_json_write_object_begin(write_ctx);

		spdk_json_write_object_end(write_ctx);
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
pt_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct pt_io_channel *pt_ch = ctx_buf;
	struct pt_nodes *pt_node = io_device;

	SPDK_NOTICELOG("Entry\n");

	pt_ch->base_ch = spdk_bdev_get_io_channel(pt_node->base_desc);

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregsiter it here */
static void
pt_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct pt_io_channel *pt_ch = ctx_buf;

	SPDK_NOTICELOG("Entry\n");

	spdk_put_io_channel(pt_ch->base_ch);
}

/* On init, just parse config file and build list of pt vbdevs and bdev name pairs. */
static int
vbdev_passthru_init(void)
{
	struct spdk_conf_section *sp = NULL;
	const char *conf_bdev_name = NULL;
	const char *conf_vbdev_name = NULL;
	struct bdev_names *name;
	int i;

	SPDK_NOTICELOG("Entry\n");

	sp = spdk_conf_find_section(NULL, "passthru");
	if (sp != NULL) {
		for (i = 0; ; i++) {
			if (!spdk_conf_section_get_nval(sp, "PT", i)) {
				break;
			}

			conf_bdev_name = spdk_conf_section_get_nmval(sp, "PT", i, 0);
			if (!conf_bdev_name) {
				SPDK_ERRLOG("Passthru configuration missing bdev name\n");
				break;
			}

			conf_vbdev_name = spdk_conf_section_get_nmval(sp, "PT", i, 1);
			if (!conf_vbdev_name) {
				SPDK_ERRLOG("Passthru configuration missing pt_bdev name\n");
				break;
			}

			name = calloc(1, sizeof(struct bdev_names));
			if (!name) {
				SPDK_ERRLOG("could not allocate bdev_names\n");
				return 1;
			}
			name->bdev_name = strdup(conf_bdev_name);
			name->vbdev_name = strdup(conf_vbdev_name);
			TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

			TAILQ_FOREACH(name, &g_bdev_names, link) {
				SPDK_NOTICELOG("conf parse matched: %s\n", name->bdev_name);
			}
		}
	}
	return 0;
}

/* Called when the entire module is being torn down */
static void
vbdev_passthru_finish(void)
{
	struct bdev_names *name;
	struct pt_nodes *pt_node;

	SPDK_NOTICELOG("Entry\n");

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		SPDK_NOTICELOG("Free name\n");
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}

	while ((pt_node = TAILQ_FIRST(&g_pt_nodes))) {
		TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
		SPDK_NOTICELOG("Free node\n");
		free(pt_node->pt_bdev.name);
		free(pt_node->pt_bdev.product_name);
		free(pt_node);
	}
}

/* During init we'll be asked how much memroy we'd like passed to us
 * in bev_io structures as device context. Here's where we specify how
 * much context we want per IO */
static int
vbdev_passthru_get_ctx_size(void)
{
	SPDK_NOTICELOG("Entry\n");
	return sizeof(struct vbdev_ctx);
}

/* Called when SPDK wants to save the current config of this vbdev in
 * a file. */
static void
vbdev_passthru_get_spdk_running_config(FILE *fp)
{
	SPDK_NOTICELOG("Entry\n");
	fprintf(fp, "\n[Passthru]\n");
}

/* When we regsiter our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_passthru_fn_table = {
	.destruct		= vbdev_passthru_destruct,
	.submit_request		= vbdev_passthru_submit_request,
	.io_type_supported	= vbdev_passthru_io_type_supported,
	.get_io_channel		= vbdev_passthru_get_io_channel,
	.dump_config_json	= vbdev_passthru_dump_config_json,
};

/* Called when the underlying base bdev goes away. */
static void
vbdev_passthru_examine_hotremove_cb(void *ctx)
{
	struct pt_nodes *pt_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	SPDK_NOTICELOG("Entry\n");

	TAILQ_FOREACH_SAFE(pt_node, &g_pt_nodes, link, tmp) {
		if (bdev_find == pt_node->base_bdev) {
			spdk_bdev_unregister(&pt_node->pt_bdev, NULL, NULL);
		}
	}
}

/* Because we specified this function in our pt bdev function table when we
 * registered our pt bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it. */
static void
vbdev_passthru_examine(struct spdk_bdev *bdev)
{
	struct bdev_names *name;
	struct pt_nodes *pt_node;
	int rc;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the pt_node & bdev accordingly */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev->name) == 0) {

			SPDK_NOTICELOG("Match on %s\n", bdev->name);
			pt_node = calloc(1, sizeof(struct pt_nodes));

			/* the base bdev that we're attaching to */
			pt_node->base_bdev = bdev;
			pt_node->pt_bdev.name = strdup(name->vbdev_name);
			if (!pt_node->pt_bdev.name) {
				SPDK_ERRLOG("could not allocate pt_bdev name\n");
				return;
			}
			pt_node->pt_bdev.product_name = strdup(name->vbdev_name);

			pt_node->pt_bdev.write_cache = 0;
			pt_node->pt_bdev.blocklen = bdev->blocklen;
			pt_node->pt_bdev.blockcnt = bdev->blockcnt;

			/* This is the context that is passed to us when the bdev
			 * layer calls in so we'll save our pt_bdev node here */
			pt_node->pt_bdev.ctxt = pt_node;
			pt_node->pt_bdev.fn_table = &vbdev_passthru_fn_table;
			pt_node->pt_bdev.module = SPDK_GET_BDEV_MODULE(passthru);
			TAILQ_INSERT_TAIL(&g_pt_nodes, pt_node, link);

			spdk_io_device_register(pt_node, pt_bdev_ch_create_cb, pt_bdev_ch_destroy_cb,
						sizeof(struct pt_io_channel));

			SPDK_NOTICELOG("io_device created at: 0x%p\n", pt_node);

			rc = spdk_bdev_open(bdev, false, vbdev_passthru_examine_hotremove_cb,
					    bdev, &pt_node->base_desc);
			if (rc) {
				SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
				/* todo: follow all these error paths, just blindly returning for now */
				return;
			}

			SPDK_NOTICELOG("bdev opened\n");
			rc = spdk_bdev_module_claim_bdev(bdev, pt_node->base_desc, pt_node->pt_bdev.module);
			if (rc) {
				SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
				return;
			}

			SPDK_NOTICELOG("bdev claimed\n");

			rc = spdk_bdev_register(&pt_node->pt_bdev);
			if (rc) {
				SPDK_ERRLOG("could not register pt_bdev\n");
				free(pt_node->pt_bdev.name);
				return;
			}

			SPDK_NOTICELOG("pt_bdev registered\n");
			SPDK_NOTICELOG("created pt_bdev for: %s\n", name->vbdev_name);
		}
	}
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(passthru));
}

SPDK_BDEV_MODULE_REGISTER(passthru, vbdev_passthru_init, vbdev_passthru_finish,
			  vbdev_passthru_get_spdk_running_config,
			  vbdev_passthru_get_ctx_size, vbdev_passthru_examine)
SPDK_LOG_REGISTER_COMPONENT("vbdev_passthru", SPDK_LOG_VBDEV_passthru)
