/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 JetStream Software Inc.
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
 *     * Neither the name of JetStream Software Inc. nor the names of its
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

/**
 * TODO
 *	- Consider uuid-based discovery logic.
 *	- Switch paths only when downstream reports true path error.
 *	- Consider reworking path management from array- to list-based.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk_internal/log.h"

#include "vbdev_multipath.h"

static int vbdev_multipath_init(void);
static void vbdev_multipath_get_spdk_running_config(FILE *fp);
static int vbdev_multipath_get_ctx_size(void);
static void vbdev_multipath_examine(struct spdk_bdev *bdev);
static void vbdev_multipath_finish(void);

static struct spdk_bdev_module multipath_if = {
	.name = "multipath",
	.module_init = vbdev_multipath_init,
	.config_text = vbdev_multipath_get_spdk_running_config,
	.get_ctx_size = vbdev_multipath_get_ctx_size,
	.examine_disk = vbdev_multipath_examine,
	.module_fini = vbdev_multipath_finish
};

SPDK_BDEV_MODULE_REGISTER(&multipath_if)

/* List of multipath virtual bdev definitions. */
struct vbdev_multipath_def {
	char			*vbdev_name;
	char			*bdev_names[MULTIPATH_MAX_PATHS];
	TAILQ_ENTRY(vbdev_multipath_def)	link;
};
static TAILQ_HEAD(, vbdev_multipath_def) g_mp_defs = TAILQ_HEAD_INITIALIZER(g_mp_defs);

#define multipath_for_each_name(name, names)	\
	for (typeof(&names[0]) name = &(names)[0]; \
		*name && (name - (names) < MULTIPATH_MAX_PATHS); name ++)

static void
free_multipath_vbdev_def(struct vbdev_multipath_def *d)
{
	if (d) {
		multipath_for_each_name(bdev_name, d->bdev_names) {
			if (*bdev_name) {
				free(*bdev_name);
			}
		}
		if (d->vbdev_name) {
			free(d->vbdev_name);
		}
		free(d);
	}
}

static void
vbdev_multipath_finish(void)
{
	struct vbdev_multipath_def *d, *t;

	TAILQ_FOREACH_SAFE(d, &g_mp_defs, link, t) {
		TAILQ_REMOVE(&g_mp_defs, d, link);
		free_multipath_vbdev_def(d);
	}
}

static struct vbdev_multipath_def *
multipath_lookup_vbdev_def(const char *vbdev_name)
{
	struct vbdev_multipath_def *def;

	TAILQ_FOREACH(def, &g_mp_defs, link) {
		if (!strcmp(def->vbdev_name, vbdev_name)) {
			return def;
		}
	}

	return NULL;
}

static int
multipath_insert_vbdev_def(const char *vbdev_name, const char **bdev_names)
{
	struct vbdev_multipath_def *def;

	def = calloc(1, sizeof(*def));
	if (!def) {
		SPDK_ERRLOG("could not allocate vbdev definition.\n");
		goto err;
	}
	memset(def, 0, sizeof(*def));

	multipath_for_each_name(bdev_name, bdev_names) {
		def->bdev_names[bdev_name - bdev_names] = strdup(*bdev_name);
		if (NULL == def->bdev_names[bdev_name - bdev_names]) {
			SPDK_ERRLOG("could not strdup bdev %s name\n", *bdev_name);
			goto err;
		}
	}

	def->vbdev_name = strdup(vbdev_name);
	if (NULL == def->vbdev_name) {
		SPDK_ERRLOG("could not strdup vbdev name %s\n", vbdev_name);
		goto err;
	}

	TAILQ_INSERT_TAIL(&g_mp_defs, def, link);
	return 0;

err:
	free_multipath_vbdev_def(def);
	return -ENOMEM;
}

static int
vbdev_multipath_init(void)
{
	struct spdk_conf_section *sp = NULL;
	const char *conf_vbdev_name = NULL;
	const char *conf_bdev_names[MULTIPATH_MAX_PATHS] = { NULL, };
	int i, rc;

	sp = spdk_conf_find_section(NULL, "Multipath");
	if (NULL == sp) {
		return 0;
	}

	for (i = 0; ; i++) {
		int j = 1;

		if (NULL == spdk_conf_section_get_nval(sp, "MP", i)) {
			break;
		}

		conf_vbdev_name = spdk_conf_section_get_nmval(sp, "MP", i, 0);
		if (NULL == conf_vbdev_name) {
			SPDK_ERRLOG("Multipath configuration missing vbdev name\n");
			break;
		}

		while ((conf_bdev_names[j - 1] = spdk_conf_section_get_nmval(sp, "MP", i, j))) {
			j ++;
		}

		if (1 == j) {
			SPDK_ERRLOG("Multipath configuration %s missing bdev names\n",
				    conf_vbdev_name);
			break;
		}

		rc = multipath_insert_vbdev_def(conf_vbdev_name, conf_bdev_names);
		if (rc != 0) {
			return rc;
		}

		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
			      "config parse matched: %s\n", conf_vbdev_name);
	}

	return 0;
}

static void
vbdev_multipath_get_spdk_running_config(FILE *fp)
{
	struct vbdev_multipath_def *def;

	fprintf(fp, "\n[Multipath]\n");
	TAILQ_FOREACH(def, &g_mp_defs, link) {
		fprintf(fp, "  MP %s", def->vbdev_name);
		multipath_for_each_name(name, def->bdev_names) {
			fprintf(fp, " %s", *name);
		}
		fprintf(fp, "\n");
	}
}

enum multipath_desc_status {
	MP_DESC_INVALID,
	MP_DESC_LIVE,
	MP_DESC_REMOVING,
};

/* List of active multipath vbdevs */
struct vbdev_multipath {
	struct spdk_bdev		mp_bdev;
	struct spdk_bdev_desc	*base_desc[MULTIPATH_MAX_PATHS];
	enum multipath_desc_status base_desc_status[MULTIPATH_MAX_PATHS];
	TAILQ_ENTRY(vbdev_multipath)	link;
};
static TAILQ_HEAD(, vbdev_multipath) g_mp_nodes = TAILQ_HEAD_INITIALIZER(g_mp_nodes);

#define multipath_next_path(path, paths)	\
		(((path - (paths)) < MULTIPATH_MAX_PATHS - 1) ? path + 1 : paths)

#define multipath_for_each_path(path, paths)	\
		for (typeof(&paths[0]) path = &(paths)[0]; \
		path - (paths) < MULTIPATH_MAX_PATHS; path ++)

static void
free_mp_node(struct vbdev_multipath *node)
{
	if (node) {
		if (node->mp_bdev.name) {
			free(node->mp_bdev.name);
		}
		free(node);
	}
}

static struct vbdev_multipath *
multipath_lookup_vbdev(const char *vbdev_name)
{
	if (NULL == vbdev_name) {
		goto out;
	}

	struct vbdev_multipath *mp_node;
	TAILQ_FOREACH(mp_node, &g_mp_nodes, link) {
		if (!strcmp(spdk_bdev_get_name(&mp_node->mp_bdev), vbdev_name)) {
			return mp_node;
		}
	}

out:
	return NULL;
}

static void
multipath_release_bdevs(struct vbdev_multipath *mp_node)
{
	multipath_for_each_path(desc, mp_node->base_desc) {
		if (*desc) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(*desc);

			spdk_bdev_module_release_bdev(bdev);
			spdk_bdev_close(*desc);
			*desc = NULL;
			mp_node->base_desc_status[desc - mp_node->base_desc] = MP_DESC_INVALID;
		}
	}
}

static int
vbdev_multipath_destruct(void *ctx)
{
	struct vbdev_multipath *mp_node = (struct vbdev_multipath *)ctx;

	if (NULL == mp_node) {
		return -ENODEV;
	}

	multipath_release_bdevs(mp_node);
	TAILQ_REMOVE(&g_mp_nodes, mp_node, link);
	free_mp_node(mp_node);
	return 0;
}

enum multipath_base_ch_status {
	MP_BASE_CH_INVALID,
	MP_BASE_CH_LIVE,
	MP_BASE_CH_REMOVING,
};

struct base_io_channel {
	struct spdk_io_channel *channel;
	enum multipath_base_ch_status status;
	int in_flight_ios;
};

static inline void
base_io_channel_remove_check(struct base_io_channel *bch)
{
	assert(bch);

	if (MP_BASE_CH_REMOVING == bch->status && 0 == bch->in_flight_ios) {
		spdk_put_io_channel(bch->channel);
		bch->channel = NULL;
		bch->status = MP_BASE_CH_INVALID;
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "Removed base channel %p\n", bch);
	}
}

static void
base_io_channel_add(struct base_io_channel *bch, struct spdk_bdev_desc *desc)
{
	assert(bch);

	if (MP_BASE_CH_INVALID == bch->status) {
		bch->channel = spdk_bdev_get_io_channel(desc);
		if (NULL != bch->channel) {
			bch->status = MP_BASE_CH_LIVE;
			bch->in_flight_ios = 0;
		}
	}
}

struct multipath_io_channel {
	struct base_io_channel	base_ch[MULTIPATH_MAX_PATHS];
	struct base_io_channel	*curr_ch;
};

/*
 * Base bdev removal handling.
 *
 * Once all the channels associated with the mp_node have been iterated,
 * with removed base bdev's io channels fenced, the below proceeds with
 * (now safe to attempt) base device removal.
 *
 * SPDK will unregister the associated I/O device once all iterated channels
 * are done with in-flight I/Os and have its underlying base channel put.
 */
static void
base_io_channel_remove_done_cb(struct spdk_io_channel_iter *i, int status)
{
	struct vbdev_multipath *mp_node = spdk_io_channel_iter_get_io_device(i);
	struct spdk_bdev_desc **dsp = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(*dsp);

	SPDK_NOTICELOG("\nDisabled multipath vbdev %s path bdev %s.\n",
		       spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev));

	spdk_bdev_module_release_bdev(bdev);
	spdk_bdev_close(*dsp);

	*dsp = NULL;
	mp_node->base_desc_status[dsp - mp_node->base_desc] = MP_DESC_INVALID;

	int live_paths = 0;
	multipath_for_each_path(desc, mp_node->base_desc) {
		live_paths += (*desc != NULL);
	}

	/* FIXME
	 * Don't attempt to unregister here if bdev subsystem shutdown
	 * is in progress as unregister iterator will manage it anyway.
	 */
	if (0 == live_paths &&
	    SPDK_BDEV_STATUS_REMOVING != mp_node->mp_bdev.internal.status) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "Unregistering vbdev %s\n",
			      spdk_bdev_get_name(&mp_node->mp_bdev));
		spdk_bdev_unregister(&mp_node->mp_bdev, NULL, NULL);
	}
}

static void
base_io_channel_remove_cb(struct spdk_io_channel_iter *i)
{
	struct vbdev_multipath *mp_node = spdk_io_channel_iter_get_io_device(i);
	struct spdk_bdev_desc **dsp = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct multipath_io_channel *mp_ch = spdk_io_channel_get_ctx(_ch);
	struct base_io_channel *bch = &mp_ch->base_ch[dsp - mp_node->base_desc];

	if (MP_BASE_CH_LIVE == bch->status) {
		bch->status = MP_BASE_CH_REMOVING;

		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
			      "Disabled base channel %p/%p\n", mp_ch, bch);

		base_io_channel_remove_check(bch);
	}
	spdk_for_each_channel_continue(i, 0);
}

/*
 * Base bdev addition handling.
 *
 * Since the procedure is opposite to that of removal, with the descriptor
 * first added and paths then enabled, iterator completion here is a no-op.
 */
static void
base_io_channel_add_done_cb(struct spdk_io_channel_iter *i, int status)
{
	struct vbdev_multipath *mp_node = spdk_io_channel_iter_get_io_device(i);
	struct spdk_bdev_desc **dsp = spdk_io_channel_iter_get_ctx(i);

	SPDK_NOTICELOG("\nEnabled multipath vbdev %s path bdev %s, status %d.\n",
		       spdk_bdev_get_name(&mp_node->mp_bdev),
		       spdk_bdev_get_name(spdk_bdev_desc_get_bdev(*dsp)), status);
}

static void
base_io_channel_add_cb(struct spdk_io_channel_iter *i)
{
	struct vbdev_multipath *mp_node = spdk_io_channel_iter_get_io_device(i);
	struct spdk_bdev_desc **dsp = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct multipath_io_channel *mp_ch = spdk_io_channel_get_ctx(_ch);
	struct base_io_channel *bch = &mp_ch->base_ch[dsp - mp_node->base_desc];

	base_io_channel_add(bch, *dsp);
	spdk_for_each_channel_continue(i, 0);
}

/*
 * Targeting hot-remove/hot-plug, the below schedules the iteration of the vbdev
 * channels, to either put the underlying base bdev I/O channel so that
 * the underlying descriptor may be safely removed, or get io_channel for the
 * descriptor that has just went live.
 */
static void
multipath_start_vbdev_channel_iter(struct vbdev_multipath *mp_node,
				   struct spdk_bdev_desc **desc, spdk_channel_msg fn, spdk_channel_for_each_cpl cpl)
{
	spdk_for_each_channel((void *)mp_node, fn, (void *)desc, cpl);
}

static int
vbdev_multipath_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct multipath_io_channel *mp_ch = ctx_buf;
	struct vbdev_multipath *mp_node = io_device;

	memset(mp_ch, 0, sizeof(*mp_ch));
	multipath_for_each_path(dsp, mp_node->base_desc) {
		if (*dsp && mp_node->base_desc_status[dsp - mp_node->base_desc] == MP_DESC_LIVE) {
			struct base_io_channel *bch = &mp_ch->base_ch[dsp - mp_node->base_desc];

			base_io_channel_add(bch, *dsp);
			SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
				      "Enabled base channel %p/%p\n", mp_ch, bch);
		}
	}

	mp_ch->curr_ch = &mp_ch->base_ch[0];
	return 0;
}

static void
vbdev_multipath_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct multipath_io_channel *mp_ch = ctx_buf;

	multipath_for_each_path(chp, mp_ch->base_ch) {
		if (MP_BASE_CH_LIVE == chp->status) {
			chp->status = MP_BASE_CH_REMOVING;

			SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
				      "Disabled base channel %p/%p\n", mp_ch, chp);

			base_io_channel_remove_check(chp);
		}
	}
}

static struct spdk_io_channel *
vbdev_multipath_get_io_channel(void *ctx)
{
	struct vbdev_multipath *mp_node = (struct vbdev_multipath *)ctx;

	return spdk_get_io_channel(mp_node);
}

struct multipath_io_ctx {
	struct multipath_io_channel *mp_ch;
	struct base_io_channel *orig_ch, *curr_ch;
};

static int
vbdev_multipath_get_ctx_size(void)
{
	return sizeof(struct multipath_io_ctx);
}

static bool
mp_setup_io_ctx(struct multipath_io_channel *mp_ch, struct multipath_io_ctx *ctx)
{
	struct base_io_channel *chp;

	ctx->mp_ch = mp_ch;
	chp = mp_ch->curr_ch;
	do {
		if (MP_BASE_CH_LIVE == chp->status) {
			ctx->orig_ch = ctx->curr_ch = chp;
			mp_ch->curr_ch = multipath_next_path(chp, mp_ch->base_ch);
			return true;
		}
		chp = multipath_next_path(chp, mp_ch->base_ch);
	} while (chp != mp_ch->curr_ch);

	return false;
}

static bool
mp_advance_io_ctx(struct multipath_io_ctx *ctx)
{
	struct multipath_io_channel *mp_ch = ctx->mp_ch;
	struct base_io_channel *chp = multipath_next_path(ctx->curr_ch, mp_ch->base_ch);

	do {
		if (chp == ctx->orig_ch) {
			break;
		}

		if (MP_BASE_CH_LIVE == chp->status) {
			ctx->curr_ch = chp;
			return true;
		}
	} while (chp != ctx->curr_ch);

	return false;
}

static void
multipath_submit_request(struct multipath_io_ctx *, struct spdk_bdev_io *);

static void
base_channel_io_done(struct base_io_channel *bch)
{
	assert(bch->in_flight_ios > 0);
	bch->in_flight_ios --;
	base_io_channel_remove_check(bch);
}

static bool
multipath_path_error(struct spdk_bdev_io *io)
{
	/* FIXME Truly differentiate between path and any other base bdev errors. */
	return SPDK_BDEV_IO_STATUS_SUCCESS != io->internal.status;
}

static void
multipath_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct multipath_io_ctx *io_ctx = (struct multipath_io_ctx *)orig_io->driver_ctx;
	struct base_io_channel *bch = io_ctx->curr_ch;

	if (spdk_likely(bdev_io)) {
		orig_io->internal.status = bdev_io->internal.status;
		spdk_bdev_free_io(bdev_io);
	}

	base_channel_io_done(bch);
	if (spdk_likely(success)) {
		goto io_done;
	}

	if (multipath_path_error(orig_io) && mp_advance_io_ctx(io_ctx)) {
		/* Give the next live path a chance */
		multipath_submit_request(io_ctx, orig_io);
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "Failing I/O (%lu,%lu)\n",
		      orig_io->u.bdev.offset_blocks, orig_io->u.bdev.num_blocks);

io_done:
	/* If orig_io failed on every path w/o being issued downstream, its status
	 * will still correctly reflect the issue at the last submission.
	 */
	spdk_bdev_io_complete(orig_io, orig_io->internal.status);
}

static inline struct vbdev_multipath *
mp_node_from_bdev(struct spdk_bdev *bdev)
{
	return SPDK_CONTAINEROF(bdev, struct vbdev_multipath, mp_bdev);
}

static void
multipath_submit_request(struct multipath_io_ctx *ctx, struct spdk_bdev_io *orig_io)
{
	struct vbdev_multipath *mp_node = mp_node_from_bdev(orig_io->bdev);
	struct spdk_bdev_desc *desc = mp_node->base_desc[ctx->curr_ch - ctx->mp_ch->base_ch];
	struct base_io_channel *bch = ctx->curr_ch;
	struct spdk_io_channel *ch = bch->channel;
	int rc;

	bch->in_flight_ios ++;

	/*
	 * Since this vbdev does nothing with the upstream I/O, just calling
	 * spdk_bdev_io_submit would suffice (and render the below obsolete).
	 */
	switch (orig_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rc = spdk_bdev_readv_blocks(desc, ch, orig_io->u.bdev.iovs,
					    orig_io->u.bdev.iovcnt, orig_io->u.bdev.offset_blocks,
					    orig_io->u.bdev.num_blocks, multipath_io_complete,
					    orig_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_writev_blocks(desc, ch, orig_io->u.bdev.iovs,
					     orig_io->u.bdev.iovcnt, orig_io->u.bdev.offset_blocks,
					     orig_io->u.bdev.num_blocks, multipath_io_complete,
					     orig_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(desc, ch,
						   orig_io->u.bdev.offset_blocks,
						   orig_io->u.bdev.num_blocks,
						   multipath_io_complete, orig_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(desc, ch,
					    orig_io->u.bdev.offset_blocks,
					    orig_io->u.bdev.num_blocks,
					    multipath_io_complete, orig_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(desc, ch,
					    orig_io->u.bdev.offset_blocks,
					    orig_io->u.bdev.num_blocks,
					    multipath_io_complete, orig_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(desc, ch, multipath_io_complete, orig_io);
		break;
	default:
		SPDK_ERRLOG("multipath: unknown I/O type %d\n", orig_io->type);
		rc = -ENOTSUP;
		break;
	}

	if (rc != 0) {
		orig_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		multipath_io_complete(NULL, false, orig_io);
	}
}

static void
vbdev_multipath_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct multipath_io_channel *mp_ch = spdk_io_channel_get_ctx(ch);
	struct multipath_io_ctx *io_ctx = (struct multipath_io_ctx *)bdev_io->driver_ctx;

	if (!mp_setup_io_ctx(mp_ch, io_ctx)) {
		/* The below may actually fail only if there's no active path,
		 * and that should be already handled by device removal logic.
		 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	multipath_submit_request(io_ctx, bdev_io);
}

static bool
vbdev_multipath_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_multipath *mp_node = (struct vbdev_multipath *)ctx;

	multipath_for_each_path(desc, mp_node->base_desc) {
		if (*desc) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(*desc);

			/* All paths lead to the same base device */
			return spdk_bdev_io_type_supported(bdev, io_type);
		}
	}
	return false;
}

/* Output single vbdev json config */
static int
vbdev_multipath_info_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_multipath *mp_node = (struct vbdev_multipath *)ctx;

	if (NULL == mp_node) {
		return -ENODEV;
	}

	spdk_json_write_named_object_begin(w, "multipath");
	spdk_json_write_named_string(w, "mp_bdev_name",
				     spdk_bdev_get_name(&mp_node->mp_bdev));

	spdk_json_write_named_array_begin(w, "base_bdev_names");
	for (int i = 0; i < MULTIPATH_MAX_PATHS; i ++) {
		struct spdk_bdev_desc *desc = mp_node->base_desc[i];
		if (desc) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

			spdk_json_write_string(w, spdk_bdev_get_name(bdev));
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
	return 0;
}

static void
vbdev_multipath_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct vbdev_multipath *mp_node = SPDK_CONTAINEROF(bdev, struct vbdev_multipath, mp_bdev);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_multipath_bdev");
	spdk_json_write_named_object_begin(w, "params");

	spdk_json_write_named_string(w, "multipath_bdev_name",
				     spdk_bdev_get_name(&mp_node->mp_bdev));

	spdk_json_write_named_array_begin(w, "base_bdev_names");
	for (int i = 0; i < MULTIPATH_MAX_PATHS; i ++) {
		struct spdk_bdev_desc *desc = mp_node->base_desc[i];
		if (desc) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

			spdk_json_write_string(w, spdk_bdev_get_name(bdev));
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table vbdev_multipath_fn_table = {
	.destruct		= vbdev_multipath_destruct,
	.submit_request		= vbdev_multipath_submit_request,
	.io_type_supported	= vbdev_multipath_io_type_supported,
	.get_io_channel		= vbdev_multipath_get_io_channel,
	.dump_info_json		= vbdev_multipath_info_config_json,
	.write_config_json	= vbdev_multipath_write_json_config,
};

/*
 * Remove the given bdev from the multipath vbdev. If this is the last path
 * bdev going away, unregister the multipath virtual bdev.
 *
 * This can be called by either hot-remove or RPC.
 *
 * Targeting hot-remove, the below schedules the iteration of the vbdev channels,
 * to put the underlying base bdev I/O channel so that the underlying descriptor
 * could be safely removed.
 */
static void
multipath_path_down(struct vbdev_multipath *mp_node, struct spdk_bdev_desc **desc)
{
	SPDK_NOTICELOG("\nDisabling multipath vbdev %s path bdev %s\n",
		       spdk_bdev_get_name(&mp_node->mp_bdev),
		       spdk_bdev_get_name(spdk_bdev_desc_get_bdev(*desc)));

	mp_node->base_desc_status[desc - mp_node->base_desc] = MP_DESC_REMOVING;
	multipath_start_vbdev_channel_iter(mp_node, desc,
					   base_io_channel_remove_cb, base_io_channel_remove_done_cb);
}

/* Called when the base bdev opened by a multipath vbdev goes away. */
static void
vbdev_multipath_base_bdev_hotremove_cb(void *ctx)
{
	struct vbdev_multipath *mp_node, *tmp;
	struct spdk_bdev *hr_bdev = ctx;

	TAILQ_FOREACH_SAFE(mp_node, &g_mp_nodes, link, tmp) {
		multipath_for_each_path(desc, mp_node->base_desc) {
			if (*desc) {
				struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(*desc);

				if (hr_bdev == bdev) {
					multipath_path_down(mp_node, desc);
				}
			}
		}
	}
}

/* RPC to deactivate the given path bdev of the active multipath vbdev. */
int
vbdev_multipath_path_down(const char *vbdev_name, const char **bdev_names)
{
	struct vbdev_multipath *mp_node, *tmp;
	bool found = false;
	int count = 0, rc = 0;

	if (NULL == vbdev_name || NULL == bdev_names || NULL == *bdev_names) {
		rc = -EINVAL;
		goto out;
	}

	TAILQ_FOREACH_SAFE(mp_node, &g_mp_nodes, link, tmp) {
		if (strcmp(vbdev_name, spdk_bdev_get_name(&mp_node->mp_bdev))) {
			continue;
		}

		found = true;
		multipath_for_each_path(desc, mp_node->base_desc) {
			if (*desc) {
				const char *name = spdk_bdev_get_name(spdk_bdev_desc_get_bdev(*desc));

				multipath_for_each_name(bdev_name, bdev_names) {
					if (!strcmp(*bdev_name, name) &&
					    mp_node->base_desc_status[desc - mp_node->base_desc] == MP_DESC_LIVE) {
						multipath_path_down(mp_node, desc);
						count ++;
						break;
					}
				}
			}
		}

		if (!count) {
			SPDK_ERRLOG("No requested bdevs found under vbdev %s.\n", vbdev_name);
			rc = -ENODEV;
			goto out;
		}
	}

	if (!found) {
		SPDK_ERRLOG("vbdev %s not found.\n", vbdev_name);
		rc = -ENOTTY;
	}

out:
	return rc;
}

static int
multipath_add_path_bdev(struct vbdev_multipath *mp_node,
			struct spdk_bdev *bdev, struct spdk_bdev_desc **dsp, bool vbdev_exists)
{
	int rc;

	rc = spdk_bdev_open(bdev, true, vbdev_multipath_base_bdev_hotremove_cb,
			    bdev, dsp);
	if (rc) {
		SPDK_ERRLOG("vbdev %s: could not open bdev %s.\n",
			    spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev));
		goto out;
	}
	SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "vbdev %s: bdev %s opened, desc %p.\n",
		      spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev), *dsp);

	rc = spdk_bdev_module_claim_bdev(bdev, *dsp, &multipath_if);
	if (rc) {
		SPDK_ERRLOG("vbdev %s: could not claim bdev %s.\n",
			    spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev));
		goto out;
	}
	SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "vbdev %s: bdev %s claimed.\n",
		      spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev));

	/* Otherwise base bdev will be added during vbdev registration */
	if (vbdev_exists) {
		rc = spdk_vbdev_add_base_bdev(&mp_node->mp_bdev, bdev);
		if (rc) {
			SPDK_ERRLOG("vbdev %s: could not add bdev %s.\n",
				    spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev));
			goto out;
		}
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
			      "Registered bdev %s with multipath vbdev %s\n",
			      spdk_bdev_get_name(bdev), spdk_bdev_get_name(&mp_node->mp_bdev));
	}

	mp_node->base_desc_status[dsp - mp_node->base_desc] = MP_DESC_LIVE;
	SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "Added bdev %s to mp vbdev %s\n",
		      spdk_bdev_get_name(bdev), spdk_bdev_get_name(&mp_node->mp_bdev));

out:
	return rc;
}

/* Create and register the multipath vbdev based on the (potentially partially
 * activated) subset of bdevs.
 *
 * This can be called by either examine or one of the RPC methods.
 */
static int
multipath_register_vbdev(const char *vbdev_name)
{
	struct vbdev_multipath_def *def =  multipath_lookup_vbdev_def(vbdev_name);
	struct vbdev_multipath *mp_node = multipath_lookup_vbdev(vbdev_name);
	bool vbdev_registered = (mp_node != NULL);
	struct spdk_bdev *bdevs[MULTIPATH_MAX_PATHS] = { NULL, };
	size_t total_bdevs = 0, active_bdevs = 0;
	int rc = 0;

	multipath_for_each_name(bdev_name, def->bdev_names) {
		struct spdk_bdev *bdev = spdk_bdev_get_by_name(*bdev_name);

		bdevs[total_bdevs] = bdev;
		total_bdevs ++;
		if (NULL == bdev) {
			continue;
		}

		if (!mp_node) {
			mp_node = calloc(1, sizeof(*mp_node));
			if (!mp_node) {
				SPDK_ERRLOG("could not allocate multipath vbdev %s node.\n",
					    vbdev_name);
				rc = -ENOMEM;
				goto out;
			}

			mp_node->mp_bdev.name = strdup(vbdev_name);
			if (!mp_node->mp_bdev.name) {
				SPDK_ERRLOG("could not allocate multipath vbdev %s name.\n", vbdev_name);
				rc = -ENOMEM;
				goto free_node;
			}

			mp_node->mp_bdev.product_name = "multipath";
			mp_node->mp_bdev.write_cache = bdev->write_cache;
			mp_node->mp_bdev.need_aligned_buffer = bdev->need_aligned_buffer;
			mp_node->mp_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
			mp_node->mp_bdev.blocklen = bdev->blocklen;
			mp_node->mp_bdev.blockcnt = bdev->blockcnt;

			mp_node->mp_bdev.ctxt = mp_node;
			mp_node->mp_bdev.fn_table = &vbdev_multipath_fn_table;
			mp_node->mp_bdev.module = &multipath_if;

			TAILQ_INSERT_TAIL(&g_mp_nodes, mp_node, link);
		}
		active_bdevs ++;

		if (mp_node->base_desc[bdev_name - def->bdev_names]) {
			SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
				      "vbdev %s: skipping alredy opened bdev %s.\n",
				      vbdev_name, *bdev_name);
			continue;
		}

		struct spdk_bdev_desc **desc = &mp_node->base_desc[bdev_name - def->bdev_names];
		rc = multipath_add_path_bdev(mp_node, bdev, desc, vbdev_registered);
		if (rc) {
			SPDK_ERRLOG("vbdev %s: could not add bdev %s.\n",
				    vbdev_name, *bdev_name);
			goto release_bdevs;
		}
	}

	if (!vbdev_registered) {
		spdk_io_device_register(mp_node, vbdev_multipath_ch_create_cb,
					vbdev_multipath_ch_destroy_cb, sizeof(struct multipath_io_channel));

		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
			      "io_device for %s created at: 0x%p\n", vbdev_name, mp_node);

		rc = spdk_vbdev_register(&mp_node->mp_bdev, bdevs, active_bdevs);
		if (rc) {
			SPDK_ERRLOG("could not register multipath vbdev %s.\n", vbdev_name);
			goto unreg_iodev;
		}
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH, "%s: mp_bdev registered.\n", vbdev_name);

		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_MULTIPATH,
			      "created multipath vbdev %s.\n", spdk_bdev_get_name(&mp_node->mp_bdev));
	}

	goto out;

unreg_iodev:
	spdk_io_device_unregister(mp_node, NULL);

release_bdevs:
	TAILQ_REMOVE(&g_mp_nodes, mp_node, link);
	multipath_release_bdevs(mp_node);

free_node:
	free_mp_node(mp_node);

out:
	return rc;
}

/* RPC to create the multipath vbdev from the given bdevs. */
int
vbdev_multipath_create_vbdev(const char *vbdev_name, const char **bdev_names)
{
	int rc = 0;

	if (NULL == vbdev_name || NULL == bdev_names || NULL == *bdev_names) {
		rc = -EINVAL;
		goto out;
	}

	struct vbdev_multipath_def *def = multipath_lookup_vbdev_def(vbdev_name);
	if (def) {
		SPDK_ERRLOG("multipath vbdev %s is already defined.\n", vbdev_name);
		rc = -EEXIST;
		goto out;
	}

	multipath_for_each_name(bdev_name, bdev_names) {
		struct spdk_bdev *bdev = spdk_bdev_get_by_name(*bdev_name);

		if (!bdev) {
			SPDK_ERRLOG("multipath vbdev %s path bdev %s not found.\n",
				    vbdev_name, *bdev_name);
			rc = -ENODEV;
			goto out;
		}
	}

	rc = multipath_insert_vbdev_def(vbdev_name, bdev_names);
	if (rc) {
		goto out;
	}

	rc = multipath_register_vbdev(vbdev_name);

out:
	return rc;
}

/*
 * Add the given known bdev to the multipath vbdev.
 *
 * This can be called by either hot-remove or RPC.
 *
 * Targeting hot-remove, the below opens, claims, and adds bdev, and then
 * schedules the iteration of the vbdev channels, to add the underlying
 * base bdev I/O channel to the multipath_io_channel array.
 */

static int
multipath_path_up(struct vbdev_multipath *mp_node,
		  struct spdk_bdev *bdev, struct spdk_bdev_desc **desc)
{
	int rc = multipath_add_path_bdev(mp_node, bdev, desc, true);
	if (rc) {
		goto out;
	}

	SPDK_NOTICELOG("\nEnabling multipath vbdev %s path bdev %s...\n",
		       spdk_bdev_get_name(&mp_node->mp_bdev), spdk_bdev_get_name(bdev));

	multipath_start_vbdev_channel_iter(mp_node, desc,
					   base_io_channel_add_cb, base_io_channel_add_done_cb);
out:
	return rc;
}

/*
 * RPC to activate path bdev of the running multipath vbdev.
 */
int
vbdev_multipath_path_up(const char *vbdev_name, const char **bdev_names)
{
	int rc = 0;

	if (NULL == vbdev_name || NULL == bdev_names || NULL == *bdev_names) {
		rc = -EINVAL;
		goto out;
	}

	struct vbdev_multipath_def *def = multipath_lookup_vbdev_def(vbdev_name);
	if (NULL == def) {
		SPDK_ERRLOG("multipath vbdev %s definition not found.\n", vbdev_name);
		rc = -ENOTTY;
		goto out;
	}

	multipath_for_each_name(bdev_name, bdev_names) {
		struct spdk_bdev *bdev = spdk_bdev_get_by_name(*bdev_name);

		if (!bdev) {
			SPDK_ERRLOG("multipath vbdev %s path bdev %s not found.\n",
				    vbdev_name, *bdev_name);
			rc = -ENODEV;
			goto out;
		}

		/* Need second loop to obtain the correct path index */
		struct vbdev_multipath *mp_node = multipath_lookup_vbdev(vbdev_name);
		if (NULL == mp_node) {
			SPDK_ERRLOG("multipath vbdev %s not opened.\n", vbdev_name);
			rc = -ENOTTY;
			goto out;
		}

		multipath_for_each_name(path_bdev_name, def->bdev_names) {
			if (0 == strcmp(*bdev_name, *path_bdev_name)) {
				struct spdk_bdev_desc **desc =
						&mp_node->base_desc[path_bdev_name - def->bdev_names];

				if (*desc) {
					SPDK_ERRLOG("multipath vbdev %s path bdev %s already present.\n",
						    vbdev_name, *path_bdev_name);
					rc = -EEXIST;
					goto out;
				}
				rc = multipath_path_up(mp_node, bdev, desc);
			}
		}
	}

out:
	return  rc;
}

/*
 * FIXME Find a better way to unify register with add_path.
 *
 * E.g., split create (a fully-assembled) vbdev path which should also
 * be triggered by examine when config is fully processed, from the
 * true hot-plug path entered via either add_bdev RPC or examine acting
 * as hotplug inspector.
 */
static void
vbdev_multipath_examine(struct spdk_bdev *bdev)
{
	struct vbdev_multipath_def *def;

	TAILQ_FOREACH(def, &g_mp_defs, link) {
		multipath_for_each_name(bdev_name, def->bdev_names) {
			if (!strcmp(*bdev_name, spdk_bdev_get_name(bdev))) {
				int rc;

				rc = multipath_register_vbdev(def->vbdev_name);
				if (rc) {
					SPDK_ERRLOG("examine error %d\n", -rc);
				}
			}
		}
	}

	spdk_bdev_module_examine_done(&multipath_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_multipath", SPDK_LOG_VBDEV_MULTIPATH)
