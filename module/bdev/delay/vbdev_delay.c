/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "vbdev_delay.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"


static int vbdev_delay_init(void);
static int vbdev_delay_get_ctx_size(void);
static void vbdev_delay_examine(struct spdk_bdev *bdev);
static void vbdev_delay_finish(void);
static int vbdev_delay_config_json(struct spdk_json_write_ctx *w);

static struct spdk_bdev_module delay_if = {
	.name = "delay",
	.module_init = vbdev_delay_init,
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
	struct spdk_thread		*thread;    /* thread where base device is opened */
};
static TAILQ_HEAD(, vbdev_delay) g_delay_nodes = TAILQ_HEAD_INITIALIZER(g_delay_nodes);

struct delay_bdev_io {
	int status;

	uint64_t completion_tick;

	enum delay_io_type type;

	struct spdk_io_channel *ch;

	struct spdk_bdev_io_wait_entry bdev_io_wait;

	struct spdk_bdev_io *zcopy_bdev_io;

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

static void vbdev_delay_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);


/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_delay *delay_node  = io_device;

	/* Done with this delay_node. */
	free(delay_node->delay_bdev.name);
	free(delay_node);
}

static void
_vbdev_delay_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
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

	/* Close the underlying bdev on its same opened thread. */
	if (delay_node->thread && delay_node->thread != spdk_get_thread()) {
		spdk_thread_send_msg(delay_node->thread, _vbdev_delay_destruct, delay_node->base_desc);
	} else {
		spdk_bdev_close(delay_node->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(delay_node, _device_unregister_cb);

	return 0;
}

static int
_process_io_stailq(void *arg, uint64_t ticks)
{
	STAILQ_HEAD(, delay_bdev_io) *head = arg;
	struct delay_bdev_io *io_ctx, *tmp;
	int completions = 0;

	STAILQ_FOREACH_SAFE(io_ctx, head, link, tmp) {
		if (io_ctx->completion_tick <= ticks) {
			STAILQ_REMOVE(head, io_ctx, delay_bdev_io, link);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io_ctx), io_ctx->status);
			completions++;
		} else {
			/* In the general case, I/O will become ready in an fifo order. When timeouts are dynamically
			 * changed, this is not necessarily the case. However, the normal behavior will be restored
			 * after the outstanding I/O at the time of the change have been completed.
			 * This essentially means that moving from a high to low latency creates a dam for the new I/O
			 * submitted after the latency change. This is considered desirable behavior for the use case where
			 * we are trying to trigger a pre-defined timeout on an initiator.
			 */
			break;
		}
	}

	return completions;
}

static int
_delay_finish_io(void *arg)
{
	struct delay_io_channel *delay_ch = arg;
	uint64_t ticks = spdk_get_ticks();
	int completions = 0;

	completions += _process_io_stailq(&delay_ch->avg_read_io, ticks);
	completions += _process_io_stailq(&delay_ch->avg_write_io, ticks);
	completions += _process_io_stailq(&delay_ch->p99_read_io, ticks);
	completions += _process_io_stailq(&delay_ch->p99_write_io, ticks);

	return completions == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
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

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_ZCOPY && bdev_io->u.bdev.zcopy.start && success) {
		io_ctx->zcopy_bdev_io = bdev_io;
	} else {
		assert(io_ctx->zcopy_bdev_io == NULL || io_ctx->zcopy_bdev_io == bdev_io);
		io_ctx->zcopy_bdev_io = NULL;
		spdk_bdev_free_io(bdev_io);
	}

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
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_delay_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, delay_ch->base_ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_delay_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
delay_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = bdev_io->u.bdev.md_buf;
}

static void
delay_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_delay,
					 delay_bdev);
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_bdev_ext_io_opts io_opts;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	delay_init_ext_io_opts(bdev_io, &io_opts);
	rc = spdk_bdev_readv_blocks_ext(delay_node->base_desc, delay_ch->base_ch, bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					bdev_io->u.bdev.num_blocks, _delay_complete_io,
					bdev_io, &io_opts);

	if (rc == -ENOMEM) {
		SPDK_ERRLOG("No memory, start to queue io for delay.\n");
		vbdev_delay_queue_io(bdev_io);
	} else if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_delay_reset_dev(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_io *bdev_io = spdk_io_channel_iter_get_ctx(i);
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	struct vbdev_delay *delay_node = spdk_io_channel_iter_get_io_device(i);
	int rc;

	rc = spdk_bdev_reset(delay_node->base_desc, delay_ch->base_ch,
			     _delay_complete_io, bdev_io);

	if (rc == -ENOMEM) {
		SPDK_ERRLOG("No memory, start to queue io for delay.\n");
		vbdev_delay_queue_io(bdev_io);
	} else if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
abort_zcopy_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	spdk_bdev_free_io(bdev_io);
}

static void
_abort_all_delayed_io(void *arg)
{
	STAILQ_HEAD(, delay_bdev_io) *head = arg;
	struct delay_bdev_io *io_ctx, *tmp;

	STAILQ_FOREACH_SAFE(io_ctx, head, link, tmp) {
		STAILQ_REMOVE(head, io_ctx, delay_bdev_io, link);
		if (io_ctx->zcopy_bdev_io != NULL) {
			spdk_bdev_zcopy_end(io_ctx->zcopy_bdev_io, false, abort_zcopy_io, NULL);
		}
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io_ctx), SPDK_BDEV_IO_STATUS_ABORTED);
	}
}

static void
vbdev_delay_reset_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);

	_abort_all_delayed_io(&delay_ch->avg_read_io);
	_abort_all_delayed_io(&delay_ch->avg_write_io);
	_abort_all_delayed_io(&delay_ch->p99_read_io);
	_abort_all_delayed_io(&delay_ch->p99_write_io);

	spdk_for_each_channel_continue(i, 0);
}

static bool
abort_delayed_io(void *_head, struct spdk_bdev_io *bio_to_abort)
{
	STAILQ_HEAD(, delay_bdev_io) *head = _head;
	struct delay_bdev_io *io_ctx_to_abort = (struct delay_bdev_io *)bio_to_abort->driver_ctx;
	struct delay_bdev_io *io_ctx;

	STAILQ_FOREACH(io_ctx, head, link) {
		if (io_ctx == io_ctx_to_abort) {
			STAILQ_REMOVE(head, io_ctx_to_abort, delay_bdev_io, link);
			if (io_ctx->zcopy_bdev_io != NULL) {
				spdk_bdev_zcopy_end(io_ctx->zcopy_bdev_io, false, abort_zcopy_io, NULL);
			}
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			return true;
		}
	}

	return false;
}

static int
vbdev_delay_abort(struct vbdev_delay *delay_node, struct delay_io_channel *delay_ch,
		  struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_io *bio_to_abort = bdev_io->u.abort.bio_to_abort;

	if (abort_delayed_io(&delay_ch->avg_read_io, bio_to_abort) ||
	    abort_delayed_io(&delay_ch->avg_write_io, bio_to_abort) ||
	    abort_delayed_io(&delay_ch->p99_read_io, bio_to_abort) ||
	    abort_delayed_io(&delay_ch->p99_write_io, bio_to_abort)) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	}

	return spdk_bdev_abort(delay_node->base_desc, delay_ch->base_ch, bio_to_abort,
			       _delay_complete_io, bdev_io);
}

static void
vbdev_delay_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_delay, delay_bdev);
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev_ext_io_opts io_opts;
	int rc = 0;
	bool is_p99;

	is_p99 = rand_r(&delay_ch->rand_seed) % 100 == 0 ? true : false;

	io_ctx->ch = ch;
	io_ctx->type = DELAY_NONE;
	if (bdev_io->type != SPDK_BDEV_IO_TYPE_ZCOPY || bdev_io->u.bdev.zcopy.start) {
		io_ctx->zcopy_bdev_io = NULL;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		io_ctx->type = is_p99 ? DELAY_P99_READ : DELAY_AVG_READ;
		spdk_bdev_io_get_buf(bdev_io, delay_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_ctx->type = is_p99 ? DELAY_P99_WRITE : DELAY_AVG_WRITE;
		delay_init_ext_io_opts(bdev_io, &io_opts);
		rc = spdk_bdev_writev_blocks_ext(delay_node->base_desc, delay_ch->base_ch, bdev_io->u.bdev.iovs,
						 bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
						 bdev_io->u.bdev.num_blocks, _delay_complete_io,
						 bdev_io, &io_opts);
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
		/* During reset, the generic bdev layer aborts all new I/Os and queues all new resets.
		 * Hence we can simply abort all I/Os delayed to complete.
		 */
		spdk_for_each_channel(delay_node, vbdev_delay_reset_channel, bdev_io,
				      vbdev_delay_reset_dev);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = vbdev_delay_abort(delay_node, delay_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.commit) {
			io_ctx->type = is_p99 ? DELAY_P99_WRITE : DELAY_AVG_WRITE;
		} else if (bdev_io->u.bdev.zcopy.populate) {
			io_ctx->type = is_p99 ? DELAY_P99_READ : DELAY_AVG_READ;
		}
		if (bdev_io->u.bdev.zcopy.start) {
			rc = spdk_bdev_zcopy_start(delay_node->base_desc, delay_ch->base_ch,
						   bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   bdev_io->u.bdev.zcopy.populate,
						   _delay_complete_io, bdev_io);
		} else {
			rc = spdk_bdev_zcopy_end(io_ctx->zcopy_bdev_io, bdev_io->u.bdev.zcopy.commit,
						 _delay_complete_io, bdev_io);
		}
		break;
	default:
		SPDK_ERRLOG("delay: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc == -ENOMEM) {
		SPDK_ERRLOG("No memory, start to queue io for delay.\n");
		vbdev_delay_queue_io(bdev_io);
	} else if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
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
				    delay_node->average_read_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
	spdk_json_write_named_int64(w, "p99_read_latency",
				    delay_node->p99_read_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
	spdk_json_write_named_int64(w, "avg_write_latency",
				    delay_node->average_write_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
	spdk_json_write_named_int64(w, "p99_write_latency",
				    delay_node->p99_write_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
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

	delay_ch->io_poller = SPDK_POLLER_REGISTER(_delay_finish_io, delay_ch, 0);
	delay_ch->base_ch = spdk_bdev_get_io_channel(delay_node->base_desc);
	delay_ch->rand_seed = time(NULL);

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
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

int
vbdev_delay_update_latency_value(char *delay_name, uint64_t latency_us, enum delay_io_type type)
{
	struct vbdev_delay *delay_node;
	uint64_t ticks_mhz = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;

	TAILQ_FOREACH(delay_node, &g_delay_nodes, link) {
		if (strcmp(delay_node->delay_bdev.name, delay_name) == 0) {
			break;
		}
	}

	if (delay_node == NULL) {
		return -ENODEV;
	}

	switch (type) {
	case DELAY_AVG_READ:
		delay_node->average_read_latency_ticks = ticks_mhz * latency_us;
		break;
	case DELAY_AVG_WRITE:
		delay_node->average_write_latency_ticks = ticks_mhz * latency_us;
		break;
	case DELAY_P99_READ:
		delay_node->p99_read_latency_ticks = ticks_mhz * latency_us;
		break;
	case DELAY_P99_WRITE:
		delay_node->p99_write_latency_ticks = ticks_mhz * latency_us;
		break;
	default:
		return -EINVAL;
	}

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

static int
vbdev_delay_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;

	/* Delay bdev doesn't work with data buffers, so it supports any memory domain used by base_bdev */
	return spdk_bdev_get_memory_domains(delay_node->base_bdev, domains, array_size);
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_delay_fn_table = {
	.destruct		= vbdev_delay_destruct,
	.submit_request		= vbdev_delay_submit_request,
	.io_type_supported	= vbdev_delay_io_type_supported,
	.get_io_channel		= vbdev_delay_get_io_channel,
	.dump_info_json		= vbdev_delay_dump_info_json,
	.write_config_json	= vbdev_delay_write_config_json,
	.get_memory_domains	= vbdev_delay_get_memory_domains,
};

static void
vbdev_delay_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_delay *delay_node, *tmp;

	TAILQ_FOREACH_SAFE(delay_node, &g_delay_nodes, link, tmp) {
		if (bdev_find == delay_node->base_bdev) {
			spdk_bdev_unregister(&delay_node->delay_bdev, NULL, NULL);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_delay_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			       void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_delay_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* Create and register the delay vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
static int
vbdev_delay_register(const char *bdev_name)
{
	struct bdev_association *assoc;
	struct vbdev_delay *delay_node;
	struct spdk_bdev *bdev;
	uint64_t ticks_mhz = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the delay_node & bdev accordingly.
	 */
	TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
		if (strcmp(assoc->bdev_name, bdev_name) != 0) {
			continue;
		}

		delay_node = calloc(1, sizeof(struct vbdev_delay));
		if (!delay_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate delay_node\n");
			break;
		}
		delay_node->delay_bdev.name = strdup(assoc->vbdev_name);
		if (!delay_node->delay_bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate delay_bdev name\n");
			free(delay_node);
			break;
		}
		delay_node->delay_bdev.product_name = "delay";

		/* The base bdev that we're attaching to. */
		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_delay_base_bdev_event_cb,
					NULL, &delay_node->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
			}
			free(delay_node->delay_bdev.name);
			free(delay_node);
			break;
		}

		bdev = spdk_bdev_desc_get_bdev(delay_node->base_desc);
		delay_node->base_bdev = bdev;

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

		/* Save the thread where the base device is opened */
		delay_node->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, delay_node->base_desc, delay_node->delay_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", bdev_name);
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
	spdk_io_device_unregister(delay_node, NULL);
	free(delay_node->delay_bdev.name);
	free(delay_node);
	return rc;
}

int
create_delay_disk(const char *bdev_name, const char *vbdev_name, uint64_t avg_read_latency,
		  uint64_t p99_read_latency, uint64_t avg_write_latency, uint64_t p99_write_latency)
{
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

	rc = vbdev_delay_register(bdev_name);
	if (rc == -ENODEV) {
		/* This is not an error, we tracked the name above and it still
		 * may show up later.
		 */
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	return rc;
}

void
delete_delay_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_association *assoc;
	int rc;

	rc = spdk_bdev_unregister_by_name(vbdev_name, &delay_if, cb_fn, cb_arg);
	if (rc == 0) {
		TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
			if (strcmp(assoc->vbdev_name, vbdev_name) == 0) {
				TAILQ_REMOVE(&g_bdev_associations, assoc, link);
				free(assoc->bdev_name);
				free(assoc->vbdev_name);
				free(assoc);
				break;
			}
		}
	} else {
		cb_fn(cb_arg, rc);
	}
}

static void
vbdev_delay_examine(struct spdk_bdev *bdev)
{
	vbdev_delay_register(bdev->name);

	spdk_bdev_module_examine_done(&delay_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_delay)
