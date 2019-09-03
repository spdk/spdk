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

#include "vbdev_delay.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"


static int vbdev_delay_init(void);
static int vbdev_delay_get_ctx_size(void);
static void vbdev_delay_examine(struct spdk_bdev *bdev);
static void vbdev_delay_finish(void);
static int vbdev_delay_config_json(struct spdk_json_write_ctx *w);

enum delay_io_type {
	DELAY_AVG_READ,
	DELAY_P99_READ,
	DELAY_AVG_WRITE,
	DELAY_P99_WRITE,
	DELAY_NONE
};

static struct spdk_bdev_module delay_if = {
	.name = "delay",
	.module_init = vbdev_delay_init,
	.config_text = NULL,
	.get_ctx_size = vbdev_delay_get_ctx_size,
	.examine_config = vbdev_delay_examine,
	.module_fini = vbdev_delay_finish,
	.config_json = vbdev_delay_config_json
};

SPDK_BDEV_MODULE_REGISTER(delay, &delay_if)

/* Associative list to be used in examine */
struct bdev_association {
	char			*vbdev_name;
	char			*bdev_name;
	uint64_t		avg_read_latency;
	uint64_t		p99_read_latency;
	uint64_t		avg_write_latency;
	uint64_t		p99_write_latency;
	TAILQ_ENTRY(bdev_association)	link;
};
static TAILQ_HEAD(, bdev_association) g_bdev_associations = TAILQ_HEAD_INITIALIZER(
			g_bdev_associations);

/* List of virtual bdevs and associated info for each. */
struct vbdev_delay {
	struct spdk_bdev		*base_bdev; /* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	struct spdk_bdev		delay_bdev;    /* the delay virtual bdev */
	uint64_t			average_read_latency_ticks; /* the average read delay */
	uint64_t			p99_read_latency_ticks; /* the p99 read delay */
	uint64_t			average_write_latency_ticks; /* the average write delay */
	uint64_t			p99_write_latency_ticks; /* the p99 write delay */
	TAILQ_ENTRY(vbdev_delay)	link;
};
static TAILQ_HEAD(, vbdev_delay) g_delay_nodes = TAILQ_HEAD_INITIALIZER(g_delay_nodes);

struct delay_bdev_io {
	int status;

	uint64_t completion_tick;

	enum delay_io_type type;

	struct spdk_io_channel *ch;

	struct spdk_bdev_io_wait_entry bdev_io_wait;

	STAILQ_ENTRY(delay_bdev_io) link;
};

struct delay_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
	STAILQ_HEAD(, delay_bdev_io) avg_read_io;
	STAILQ_HEAD(, delay_bdev_io) p99_read_io;
	STAILQ_HEAD(, delay_bdev_io) avg_write_io;
	STAILQ_HEAD(, delay_bdev_io) p99_write_io;
	struct spdk_poller *io_poller;
	unsigned int rand_seed;
};

static void
vbdev_delay_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);


/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_delay *delay_node  = io_device;

	/* Done with this delay_node. */
	free(delay_node->delay_bdev.name);
	free(delay_node);
}

static int
vbdev_delay_destruct(void *ctx)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;

	/* It is important to follow this exact sequence of steps for destroying
	 * a vbdev...
	 */

	TAILQ_REMOVE(&g_delay_nodes, delay_node, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(delay_node->base_bdev);

	/* Close the underlying bdev. */
	spdk_bdev_close(delay_node->base_desc);

	/* Unregister the io_device. */
	spdk_io_device_unregister(delay_node, _device_unregister_cb);

	return 0;
}

static void
_process_io_stailq(void *arg, uint64_t ticks)
{
	STAILQ_HEAD(, delay_bdev_io) *head = arg;
	struct delay_bdev_io *io_ctx, *tmp;

	STAILQ_FOREACH_SAFE(io_ctx, head, link, tmp) {
		if (io_ctx->completion_tick <= ticks) {
			STAILQ_REMOVE(head, io_ctx, delay_bdev_io, link);
			spdk_bdev_io_complete(SPDK_CONTAINEROF(io_ctx, struct spdk_bdev_io, driver_ctx), io_ctx->status);
		} else {
			/* We can assume that I/O are strictly ordered. If one is not expired, we can assume that all after it aren't either. */
			break;
		}
	}
}

static int
_delay_finish_io(void *arg)
{
	struct delay_io_channel *delay_ch = arg;
	uint64_t ticks = spdk_get_ticks();

	_process_io_stailq(&delay_ch->avg_read_io, ticks);
	_process_io_stailq(&delay_ch->avg_write_io, ticks);
	_process_io_stailq(&delay_ch->p99_read_io, ticks);
	_process_io_stailq(&delay_ch->p99_write_io, ticks);

	return 0;
}

/* Completion callback for IO that were issued from this bdev. The original bdev_io
 * is passed in as an arg so we'll complete that one with the appropriate status
 * and then free the one that this module issued.
 */
static void
_delay_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_delay, delay_bdev);
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)orig_io->driver_ctx;
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(io_ctx->ch);

	io_ctx->status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_free_io(bdev_io);

	/* Put the I/O into the proper list for processing by the channel poller. */
	switch (io_ctx->type) {
	case DELAY_AVG_READ:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->average_read_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->avg_read_io, io_ctx, link);
		break;
	case DELAY_AVG_WRITE:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->average_write_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->avg_write_io, io_ctx, link);
		break;
	case DELAY_P99_READ:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->p99_read_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->p99_read_io, io_ctx, link);
		break;
	case DELAY_P99_WRITE:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->p99_write_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->p99_write_io, io_ctx, link);
		break;
	case DELAY_NONE:
	default:
		spdk_bdev_io_complete(orig_io, io_ctx->status);
		break;
	}
}

static void
vbdev_delay_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;

	vbdev_delay_submit_request(io_ctx->ch, bdev_io);
}

static void
vbdev_delay_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_delay_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_delay_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
delay_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_delay,
					 delay_bdev);
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = spdk_bdev_readv_blocks(delay_node->base_desc, delay_ch->base_ch, bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks, _delay_complete_io,
				    bdev_io);

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for delay.\n");
			vbdev_delay_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
vbdev_delay_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_delay, delay_bdev);
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;
	bool is_p99;

	is_p99 = rand_r(&delay_ch->rand_seed) % 100 == 0 ? true : false;

	io_ctx->ch = ch;
	io_ctx->type = DELAY_NONE;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		io_ctx->type = is_p99 ? DELAY_P99_READ : DELAY_AVG_READ;
		spdk_bdev_io_get_buf(bdev_io, delay_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_ctx->type = is_p99 ? DELAY_P99_WRITE : DELAY_AVG_WRITE;
		rc = spdk_bdev_writev_blocks(delay_node->base_desc, delay_ch->base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks, _delay_complete_io,
					     bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(delay_node->base_desc, delay_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _delay_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(delay_node->base_desc, delay_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _delay_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(delay_node->base_desc, delay_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _delay_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(delay_node->base_desc, delay_ch->base_ch,
				     _delay_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("delay: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for delay.\n");
			vbdev_delay_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static bool
vbdev_delay_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;

	return spdk_bdev_io_type_supported(delay_node->base_bdev, io_type);
}

static struct spdk_io_channel *
vbdev_delay_get_io_channel(void *ctx)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;
	struct spdk_io_channel *delay_ch = NULL;

	delay_ch = spdk_get_io_channel(delay_node);

	return delay_ch;
}

static void
_delay_write_conf_values(struct vbdev_delay *delay_node, struct spdk_json_write_ctx *w)
{
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&delay_node->delay_bdev));
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(delay_node->base_bdev));
	spdk_json_write_named_int64(w, "avg_read_latency",
				    delay_node->average_read_latency_ticks / spdk_get_ticks_hz() * SPDK_SEC_TO_USEC);
	spdk_json_write_named_int64(w, "p99_read_latency",
				    delay_node->p99_read_latency_ticks / spdk_get_ticks_hz() * SPDK_SEC_TO_USEC);
	spdk_json_write_named_int64(w, "avg_write_latency",
				    delay_node->average_write_latency_ticks / spdk_get_ticks_hz() * SPDK_SEC_TO_USEC);
	spdk_json_write_named_int64(w, "p99_write_latency",
				    delay_node->p99_write_latency_ticks / spdk_get_ticks_hz() * SPDK_SEC_TO_USEC);
}

static int
vbdev_delay_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;

	spdk_json_write_name(w, "delay");
	spdk_json_write_object_begin(w);
	_delay_write_conf_values(delay_node, w);
	spdk_json_write_object_end(w);

	return 0;
}

/* This is used to generate JSON that can configure this module to its current state. */
static int
vbdev_delay_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_delay *delay_node;

	TAILQ_FOREACH(delay_node, &g_delay_nodes, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_delay_create");
		spdk_json_write_named_object_begin(w, "params");
		_delay_write_conf_values(delay_node, w);
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
delay_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct delay_io_channel *delay_ch = ctx_buf;
	struct vbdev_delay *delay_node = io_device;

	STAILQ_INIT(&delay_ch->avg_read_io);
	STAILQ_INIT(&delay_ch->p99_read_io);
	STAILQ_INIT(&delay_ch->avg_write_io);
	STAILQ_INIT(&delay_ch->p99_write_io);

	delay_ch->io_poller = spdk_poller_register(_delay_finish_io, delay_ch, 0);
	delay_ch->base_ch = spdk_bdev_get_io_channel(delay_node->base_desc);
	delay_ch->rand_seed = time(NULL);

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregsiter it here.
 */
static void
delay_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct delay_io_channel *delay_ch = ctx_buf;

	spdk_poller_unregister(&delay_ch->io_poller);
	spdk_put_io_channel(delay_ch->base_ch);
}

/* Create the delay association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_delay_insert_association(const char *bdev_name, const char *vbdev_name,
			       uint64_t avg_read_latency, uint64_t p99_read_latency,
			       uint64_t avg_write_latency, uint64_t p99_write_latency)
{
	struct bdev_association *assoc;

	TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
		if (strcmp(vbdev_name, assoc->vbdev_name) == 0) {
			SPDK_ERRLOG("delay bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
	}

	assoc = calloc(1, sizeof(struct bdev_association));
	if (!assoc) {
		SPDK_ERRLOG("could not allocate bdev_association\n");
		return -ENOMEM;
	}

	assoc->bdev_name = strdup(bdev_name);
	if (!assoc->bdev_name) {
		SPDK_ERRLOG("could not allocate assoc->bdev_name\n");
		free(assoc);
		return -ENOMEM;
	}

	assoc->vbdev_name = strdup(vbdev_name);
	if (!assoc->vbdev_name) {
		SPDK_ERRLOG("could not allocate assoc->vbdev_name\n");
		free(assoc->bdev_name);
		free(assoc);
		return -ENOMEM;
	}

	assoc->avg_read_latency = avg_read_latency;
	assoc->p99_read_latency = p99_read_latency;
	assoc->avg_write_latency = avg_write_latency;
	assoc->p99_write_latency = p99_write_latency;

	TAILQ_INSERT_TAIL(&g_bdev_associations, assoc, link);

	return 0;
}

static int
vbdev_delay_init(void)
{
	/* Not allowing for .ini style configuration. */
	return 0;
}

static void
vbdev_delay_finish(void)
{
	struct bdev_association *assoc;

	while ((assoc = TAILQ_FIRST(&g_bdev_associations))) {
		TAILQ_REMOVE(&g_bdev_associations, assoc, link);
		free(assoc->bdev_name);
		free(assoc->vbdev_name);
		free(assoc);
	}
}

static int
vbdev_delay_get_ctx_size(void)
{
	return sizeof(struct delay_bdev_io);
}

static void
vbdev_delay_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_delay_fn_table = {
	.destruct		= vbdev_delay_destruct,
	.submit_request		= vbdev_delay_submit_request,
	.io_type_supported	= vbdev_delay_io_type_supported,
	.get_io_channel		= vbdev_delay_get_io_channel,
	.dump_info_json		= vbdev_delay_dump_info_json,
	.write_config_json	= vbdev_delay_write_config_json,
};

/* Called when the underlying base bdev goes away. */
static void
vbdev_delay_base_bdev_hotremove_cb(void *ctx)
{
	struct vbdev_delay *delay_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(delay_node, &g_delay_nodes, link, tmp) {
		if (bdev_find == delay_node->base_bdev) {
			spdk_bdev_unregister(&delay_node->delay_bdev, NULL, NULL);
		}
	}
}

/* Create and register the delay vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
static int
vbdev_delay_register(struct spdk_bdev *bdev)
{
	struct bdev_association *assoc;
	struct vbdev_delay *delay_node;
	uint64_t ticks_mhz = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the delay_node & bdev accordingly.
	 */
	TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
		if (strcmp(assoc->bdev_name, bdev->name) != 0) {
			continue;
		}

		delay_node = calloc(1, sizeof(struct vbdev_delay));
		if (!delay_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate delay_node\n");
			break;
		}

		/* The base bdev that we're attaching to. */
		delay_node->base_bdev = bdev;
		delay_node->delay_bdev.name = strdup(assoc->vbdev_name);
		if (!delay_node->delay_bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate delay_bdev name\n");
			free(delay_node);
			break;
		}
		delay_node->delay_bdev.product_name = "delay";

		delay_node->delay_bdev.write_cache = bdev->write_cache;
		delay_node->delay_bdev.required_alignment = bdev->required_alignment;
		delay_node->delay_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		delay_node->delay_bdev.blocklen = bdev->blocklen;
		delay_node->delay_bdev.blockcnt = bdev->blockcnt;

		delay_node->delay_bdev.ctxt = delay_node;
		delay_node->delay_bdev.fn_table = &vbdev_delay_fn_table;
		delay_node->delay_bdev.module = &delay_if;

		/* Store the number of ticks you need to add to get the I/O expiration time. */
		delay_node->average_read_latency_ticks = ticks_mhz * assoc->avg_read_latency;
		delay_node->p99_read_latency_ticks = ticks_mhz * assoc->p99_read_latency;
		delay_node->average_write_latency_ticks = ticks_mhz * assoc->avg_write_latency;
		delay_node->p99_write_latency_ticks = ticks_mhz * assoc->p99_write_latency;

		spdk_io_device_register(delay_node, delay_bdev_ch_create_cb, delay_bdev_ch_destroy_cb,
					sizeof(struct delay_io_channel),
					assoc->vbdev_name);

		rc = spdk_bdev_open(bdev, true, vbdev_delay_base_bdev_hotremove_cb,
				    bdev, &delay_node->base_desc);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_unregister;
		}

		rc = spdk_bdev_module_claim_bdev(bdev, delay_node->base_desc, delay_node->delay_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_close;
		}

		rc = spdk_bdev_register(&delay_node->delay_bdev);
		if (rc) {
			SPDK_ERRLOG("could not register delay_bdev\n");
			spdk_bdev_module_release_bdev(delay_node->base_bdev);
			goto error_close;
		}

		TAILQ_INSERT_TAIL(&g_delay_nodes, delay_node, link);
	}

	return rc;

error_close:
	spdk_bdev_close(delay_node->base_desc);
error_unregister:
	spdk_io_device_unregister(delay_node, NULL);
	free(delay_node->delay_bdev.name);
	free(delay_node);
	return rc;
}

int
create_delay_disk(const char *bdev_name, const char *vbdev_name, uint64_t avg_read_latency,
		  uint64_t p99_read_latency, uint64_t avg_write_latency, uint64_t p99_write_latency)
{
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	if (p99_read_latency < avg_read_latency || p99_write_latency < avg_write_latency) {
		SPDK_ERRLOG("Unable to create a delay bdev where p99 latency is less than average latency.\n");
		return -EINVAL;
	}

	rc = vbdev_delay_insert_association(bdev_name, vbdev_name, avg_read_latency, p99_read_latency,
					    avg_write_latency, p99_write_latency);
	if (rc) {
		return rc;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		return 0;
	}

	return vbdev_delay_register(bdev);
}

void
delete_delay_disk(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_association *assoc;

	if (!bdev || bdev->module != &delay_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
		if (strcmp(assoc->vbdev_name, bdev->name) == 0) {
			TAILQ_REMOVE(&g_bdev_associations, assoc, link);
			free(assoc->bdev_name);
			free(assoc->vbdev_name);
			free(assoc);
			break;
		}
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static void
vbdev_delay_examine(struct spdk_bdev *bdev)
{
	vbdev_delay_register(bdev);

	spdk_bdev_module_examine_done(&delay_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_delay", SPDK_LOG_VBDEV_DELAY)
