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

#include <spdk/stdinc.h>
#include <spdk/bdev.h>
#include <spdk/conf.h>
#include <spdk/env.h>
#include <spdk/io_channel.h>
#include <spdk/json.h>
#include <spdk/string.h>
#include <spdk/likely.h>
#include <spdk/util.h>
#include <spdk/ftl.h>
#include <spdk_internal/log.h>
#include <stdatomic.h>
#include "bdev_ocssd.h"

#define OCSSD_COMPLETION_RING_SIZE 4096

struct ocssd_bdev_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;

	struct spdk_nvme_transport_id	trid;

	size_t				ref_cnt;

	LIST_ENTRY(ocssd_bdev_ctrlr)	list_entry;
};

struct ocssd_bdev {
	struct spdk_bdev		bdev;

	struct ocssd_bdev_ctrlr		*ctrlr;

	struct spdk_ftl_dev		*dev;

	ocssd_bdev_init_fn		init_cb;

	void				*init_arg;

	LIST_ENTRY(ocssd_bdev)		list_entry;
};

struct ocssd_io_channel {
	struct spdk_ftl_dev		*dev;

	struct spdk_poller		*poller;

#define OCSSD_MAX_COMPLETIONS 64
	struct ocssd_bdev_io		*io[OCSSD_MAX_COMPLETIONS];

	/* Completion ring */
	struct spdk_ring		*ring;

	struct spdk_io_channel		*ioch;
};

struct ocssd_bdev_io {
	struct ocssd_bdev		*bdev;

	struct spdk_ring		*ring;

	int				status;

	struct spdk_thread		*orig_thread;
};

struct ocssd_probe_ctx {
	struct ocssd_bdev_init_opts	*opts;

	ocssd_bdev_init_fn		init_cb;

	void				*init_arg;
};

typedef void (*bdev_ocssd_finish_fn)(void);

static LIST_HEAD(, ocssd_bdev)		g_ocssd_bdevs = LIST_HEAD_INITIALIZER(g_ocssd_bdevs);
static LIST_HEAD(, ocssd_bdev_ctrlr)	g_ocssd_bdev_ctrlrs =
	LIST_HEAD_INITIALIZER(g_ocssd_bdev_ctrlrs);
static bdev_ocssd_finish_fn		g_finish_cb;
static size_t				g_num_conf_bdevs;
static size_t				g_num_init_bdevs;
static pthread_mutex_t			g_ocssd_bdev_lock;

static int bdev_ocssd_initialize(void);
static void bdev_ocssd_finish(void);
static void bdev_ocssd_get_spdk_running_config(FILE *fp);

static int
bdev_ocssd_get_ctx_size(void)
{
	return sizeof(struct ocssd_bdev_io);
}

static struct spdk_bdev_module g_ocssd_if = {
	.name		= "ocssd",
	.async_init	= true,
	.async_fini	= true,
	.module_init	= bdev_ocssd_initialize,
	.module_fini	= bdev_ocssd_finish,
	.config_text	= bdev_ocssd_get_spdk_running_config,
	.get_ctx_size	= bdev_ocssd_get_ctx_size,
};

#ifndef OCSSD_UNIT_TEST
SPDK_BDEV_MODULE_REGISTER(&g_ocssd_if)
#endif

static struct ocssd_bdev_ctrlr *
bdev_ocssd_ctrlr_find(const struct spdk_nvme_transport_id *trid)
{
	struct ocssd_bdev_ctrlr *ocssd_ctrlr = NULL;

	LIST_FOREACH(ocssd_ctrlr, &g_ocssd_bdev_ctrlrs, list_entry) {
		if (!spdk_nvme_transport_id_compare(&ocssd_ctrlr->trid, trid)) {
			break;
		}
	}

	return ocssd_ctrlr;
}

static struct ocssd_bdev_ctrlr *
bdev_ocssd_add_ctrlr(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid)
{
	struct ocssd_bdev_ctrlr *ocssd_ctrlr = NULL;

	pthread_mutex_lock(&g_ocssd_bdev_lock);

	ocssd_ctrlr = bdev_ocssd_ctrlr_find(trid);
	if (ocssd_ctrlr) {
		ocssd_ctrlr->ref_cnt++;
	} else {
		ocssd_ctrlr = calloc(1, sizeof(*ocssd_ctrlr));
		if (!ocssd_ctrlr) {
			goto out;
		}

		ocssd_ctrlr->ctrlr = ctrlr;
		ocssd_ctrlr->trid = *trid;
		ocssd_ctrlr->ref_cnt = 1;

		LIST_INSERT_HEAD(&g_ocssd_bdev_ctrlrs, ocssd_ctrlr, list_entry);
	}
out:
	pthread_mutex_unlock(&g_ocssd_bdev_lock);
	return ocssd_ctrlr;
}

static void
bdev_ocssd_remove_ctrlr(struct ocssd_bdev_ctrlr *ctrlr)
{
	pthread_mutex_lock(&g_ocssd_bdev_lock);

	if (--ctrlr->ref_cnt == 0) {
		if (spdk_nvme_detach(ctrlr->ctrlr)) {
			SPDK_ERRLOG("Failed to detach the controller\n");
			goto out;
		}

		LIST_REMOVE(ctrlr, list_entry);
		free(ctrlr);
	}
out:
	pthread_mutex_unlock(&g_ocssd_bdev_lock);
}

static void
bdev_ocssd_free_cb(void *ctx, int status)
{
	struct ocssd_bdev *bdev = ctx;
	bool finish_done;

	pthread_mutex_lock(&g_ocssd_bdev_lock);
	LIST_REMOVE(bdev, list_entry);
	finish_done = LIST_EMPTY(&g_ocssd_bdevs);
	pthread_mutex_unlock(&g_ocssd_bdev_lock);

	spdk_io_device_unregister(bdev, NULL);

	bdev_ocssd_remove_ctrlr(bdev->ctrlr);

	spdk_bdev_destruct_done(&bdev->bdev, status);
	free(bdev->bdev.name);
	free(bdev);

	if (finish_done && g_finish_cb) {
		g_finish_cb();
	}
}

static int
bdev_ocssd_destruct(void *ctx)
{
	struct ocssd_bdev *bdev = ctx;
	spdk_ftl_dev_free(bdev->dev, bdev_ocssd_free_cb, bdev);

	/* return 1 to indicate that the destruction is asynchronous */
	return 1;
}

static void
bdev_ocssd_complete_io(struct ocssd_bdev_io *io, int rc)
{
	enum spdk_bdev_io_status status;

	switch (rc) {
	case 0:
		status = SPDK_BDEV_IO_STATUS_SUCCESS;
		break;
	case -ENOMEM:
		status = SPDK_BDEV_IO_STATUS_NOMEM;
		break;
	default:
		status = SPDK_BDEV_IO_STATUS_FAILED;
		break;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), status);
}

static void
bdev_ocssd_cb(void *arg, int status)
{
	struct ocssd_bdev_io *io = arg;
	size_t cnt __attribute__((unused));

	io->status = status;

	cnt = spdk_ring_enqueue(io->ring, (void **)&io, 1);
	assert(cnt == 1);
}

static int
bdev_ocssd_fill_bio(struct ocssd_bdev *bdev, struct spdk_io_channel *ch,
		    struct ocssd_bdev_io *io)
{
	struct ocssd_io_channel *ioch = spdk_io_channel_get_ctx(ch);

	memset(io, 0, sizeof(*io));

	io->orig_thread = spdk_io_channel_get_thread(ch);
	io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	io->ring = ioch->ring;
	io->bdev = bdev;
	return 0;
}

static int
bdev_ocssd_readv(struct ocssd_bdev *bdev, struct spdk_io_channel *ch,
		 struct ocssd_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ocssd_io_channel *ioch = spdk_io_channel_get_ctx(ch);
	int rc;

	bio = spdk_bdev_io_from_ctx(io);

	rc = bdev_ocssd_fill_bio(bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_read(bdev->dev,
			     ioch->ioch,
			     bio->u.bdev.offset_blocks,
			     bio->u.bdev.num_blocks,
			     bio->u.bdev.iovs, bio->u.bdev.iovcnt, bdev_ocssd_cb, io);
}

static int
bdev_ocssd_writev(struct ocssd_bdev *bdev, struct spdk_io_channel *ch,
		  struct ocssd_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ocssd_io_channel *ioch;
	int rc;

	bio = spdk_bdev_io_from_ctx(io);
	ioch = spdk_io_channel_get_ctx(ch);

	rc = bdev_ocssd_fill_bio(bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_write(bdev->dev,
			      ioch->ioch,
			      bio->u.bdev.offset_blocks,
			      bio->u.bdev.num_blocks,
			      bio->u.bdev.iovs,
			      bio->u.bdev.iovcnt, bdev_ocssd_cb, io);
}

static void
bdev_ocssd_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int rc = bdev_ocssd_readv((struct ocssd_bdev *)bdev_io->bdev->ctxt,
				  ch, (struct ocssd_bdev_io *)bdev_io->driver_ctx);

	if (spdk_unlikely(rc != 0)) {
		bdev_ocssd_complete_io((struct ocssd_bdev_io *)bdev_io->driver_ctx, rc);
	}
}

static int
bdev_ocssd_flush(struct ocssd_bdev *bdev, struct spdk_io_channel *ch, struct ocssd_bdev_io *io)
{
	int rc;

	rc = bdev_ocssd_fill_bio(bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_flush(bdev->dev, bdev_ocssd_cb, io);
}

static int
_bdev_ocssd_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct ocssd_bdev *bdev = (struct ocssd_bdev *)bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_ocssd_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_ocssd_writev(bdev, ch, (struct ocssd_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_ocssd_flush(bdev, ch, (struct ocssd_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return -ENOTSUP;
		break;
	}
}

static void
bdev_ocssd_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int rc = _bdev_ocssd_submit_request(ch, bdev_io);

	if (spdk_likely(rc != 0)) {
		bdev_ocssd_complete_io((struct ocssd_bdev_io *)bdev_io->driver_ctx, rc);
	}
}

static bool
bdev_ocssd_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_ocssd_get_io_channel(void *ctx)
{
	struct ocssd_bdev *bdev = ctx;

	return spdk_get_io_channel(bdev);
}

static void
bdev_ocssd_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* TODO: implement me! (wmalikow) */
}

static const struct spdk_bdev_fn_table ocssd_fn_table = {
	.destruct		= bdev_ocssd_destruct,
	.submit_request		= bdev_ocssd_submit_request,
	.io_type_supported	= bdev_ocssd_io_type_supported,
	.get_io_channel		= bdev_ocssd_get_io_channel,
	.write_config_json	= bdev_ocssd_write_config_json,
};

int
bdev_ocssd_parse_punits(struct spdk_ftl_punit_range *range, const char *range_string)
{
	regex_t range_regex;
	regmatch_t range_match;
	unsigned long begin = 0, end = 0;
	char *str_ptr;
	int rc = -1;

	if (regcomp(&range_regex, "\\b[[:digit:]]+-[[:digit:]]+\\b", REG_EXTENDED)) {
		SPDK_ERRLOG("Regex init error\n");
		return -1;
	}

	if (regexec(&range_regex, range_string, 1, &range_match, 0)) {
		SPDK_WARNLOG("Invalid range\n");
		goto out;
	}

	errno = 0;
	begin = strtoul(range_string + range_match.rm_so, &str_ptr, 10);
	if ((begin == ULONG_MAX && errno == ERANGE) || (begin == 0 && errno == EINVAL)) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	errno = 0;
	/* +1 to skip the '-' delimiter */
	end = strtoul(str_ptr + 1, NULL, 10);
	if ((end == ULONG_MAX && errno == ERANGE) || (end == 0 && errno == EINVAL)) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	if (begin > UINT_MAX || end > UINT_MAX) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	range->begin = (unsigned int)begin;
	range->end = (unsigned int)end;

	rc = 0;
out:
	regfree(&range_regex);
	return rc;
}

static int
bdev_ocssd_read_bdev_config(struct spdk_conf_section *sp,
			    struct ocssd_bdev_init_opts *opts,
			    size_t *num_bdevs)
{
	const char *val;
	unsigned long mode;
	size_t i;
	int rc = 0;

	*num_bdevs = 0;

	for (i = 0; i < OCSSD_MAX_BDEVS; i++, opts++) {
		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 0);
		if (!val) {
			break;
		}

		rc = spdk_nvme_transport_id_parse(&opts->trid, val);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to parse TransportID: %s\n", val);
			rc = -1;
			break;
		}

		if (opts->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
			SPDK_ERRLOG("Unsupported transport type\n");
			continue;
		}

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 1);
		if (!val) {
			SPDK_ERRLOG("No name provided for TransportID\n");
			rc = -1;
			break;
		}
		opts->name = val;

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 2);
		if (!val) {
			SPDK_ERRLOG("No punit range provided for TransportID\n");
			rc = -1;
			break;
		}

		if (bdev_ocssd_parse_punits(&opts->range, val)) {
			SPDK_ERRLOG("Invalid punit range\n");
			rc = -1;
			break;
		}

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 3);
		if (!val) {
			SPDK_ERRLOG("No mode provided for TransportID\n");
			rc = -1;
			break;
		}

		mode = strtoul(val, NULL, 10);
		if ((mode == ULONG_MAX  && errno == ERANGE) || (mode == 0 && errno == EINVAL)) {
			SPDK_ERRLOG("Invalid mode\n");
			rc = -1;
			break;
		}

		opts->mode = (unsigned int)mode;

		if (!(opts->mode & SPDK_FTL_MODE_CREATE)) {
			val = spdk_conf_section_get_nmval(sp, "TransportID", i, 4);
			if (!val) {
				SPDK_ERRLOG("No UUID provided for TransportID\n");
				rc = -1;
				break;
			}
			rc = spdk_uuid_parse(&opts->uuid, val);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to parse uuid: %s\n", val);
				rc = -1;
				break;
			}
		}
	}

	if (!rc) {
		*num_bdevs = i;
	}

	return rc;
}

static int
bdev_ocssd_poll(void *arg)
{
	struct ocssd_io_channel *ch = arg;
	size_t cnt, i;

	cnt = spdk_ring_dequeue(ch->ring, (void **)&ch->io, OCSSD_MAX_COMPLETIONS);

	for (i = 0; i < cnt; ++i) {
		bdev_ocssd_complete_io(ch->io[i], ch->io[i]->status);
	}

	return cnt;
}

static int
bdev_ocssd_io_channel_create_cb(void *io_device, void *ctx)
{
	struct ocssd_io_channel *ch = ctx;
	struct ocssd_bdev *bdev = (struct ocssd_bdev *)io_device;

	ch->dev = bdev->dev;
	ch->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, OCSSD_COMPLETION_RING_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);

	if (!ch->ring) {
		return -ENOMEM;
	}

	ch->poller = spdk_poller_register(bdev_ocssd_poll, ch, 0);
	if (!ch->poller) {
		spdk_ring_free(ch->ring);
		return -ENOMEM;
	}

	ch->ioch = spdk_get_io_channel(bdev->dev);

	return 0;
}

static void
bdev_ocssd_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ocssd_io_channel *ch = ctx_buf;

	spdk_ring_free(ch->ring);
	spdk_poller_unregister(&ch->poller);
	spdk_put_io_channel(ch->ioch);
}

static void
bdev_ocssd_create_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ocssd_bdev	*bdev = ctx;
	struct ocssd_bdev_info	info = {};
	struct spdk_ftl_attrs	attrs;
	ocssd_bdev_init_fn	init_cb = bdev->init_cb;
	void			*init_arg = bdev->init_arg;
	int			rc = -ENODEV;

	if (status) {
		SPDK_ERRLOG("Failed to create OCSSD FTL device (%d)\n", status);
		rc = status;
		goto error_dev;
	}

	if (spdk_ftl_dev_get_attrs(dev, &attrs)) {
		SPDK_ERRLOG("Failed to retrieve OCSSD FTL device's attrs\n");
		rc = -ENODEV;
		goto error_dev;
	}

	bdev->dev = dev;
	bdev->bdev.product_name = "OCSSD disk";
	bdev->bdev.write_cache = 0;
	bdev->bdev.blocklen = attrs.lbk_size;
	bdev->bdev.blockcnt = attrs.lbk_cnt;
	bdev->bdev.required_alignment = spdk_u32log2(attrs.lbk_size);
	bdev->bdev.uuid = attrs.uuid;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "Creating bdev %s:\n", bdev->bdev.name);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "\tblock_len:\t%zu\n", attrs.lbk_size);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "\tblock_cnt:\t%"PRIu64"\n", attrs.lbk_cnt);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "\tpunits:\t\t%u-%u\n", attrs.range.begin,
		      attrs.range.end);

	bdev->bdev.ctxt = bdev;
	bdev->bdev.fn_table = &ocssd_fn_table;
	bdev->bdev.module = &g_ocssd_if;

	spdk_io_device_register(bdev, bdev_ocssd_io_channel_create_cb,
				bdev_ocssd_io_channel_destroy_cb,
				sizeof(struct ocssd_io_channel),
				bdev->bdev.name);

	if (spdk_bdev_register(&bdev->bdev)) {
		goto error_unregister;
	}

	info.name = bdev->bdev.name;
	info.uuid = bdev->bdev.uuid;

	pthread_mutex_lock(&g_ocssd_bdev_lock);
	LIST_INSERT_HEAD(&g_ocssd_bdevs, bdev, list_entry);
	pthread_mutex_unlock(&g_ocssd_bdev_lock);

	init_cb(&info, init_arg, 0);
	return;

error_unregister:
	spdk_io_device_unregister(bdev, NULL);
error_dev:
	spdk_ftl_dev_free(dev, NULL, NULL);

	bdev_ocssd_remove_ctrlr(bdev->ctrlr);

	free(bdev->bdev.name);
	free(bdev);

	init_cb(NULL, init_arg, rc);
}

static int
bdev_ocssd_create(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid,
		  const char *name, struct spdk_ftl_punit_range *range, unsigned int mode,
		  const struct spdk_uuid *uuid, ocssd_bdev_init_fn cb, void *cb_arg)
{
	struct ocssd_bdev *bdev = NULL;
	struct ocssd_bdev_ctrlr *ocssd_ctrlr;
	struct spdk_ftl_dev_init_opts opts = {};
	int rc;

	ocssd_ctrlr = bdev_ocssd_add_ctrlr(ctrlr, trid);
	if (!ocssd_ctrlr) {
		spdk_nvme_detach(ctrlr);
		return -ENOMEM;
	}

	bdev = calloc(1, sizeof(*bdev));
	if (!bdev) {
		SPDK_ERRLOG("Could not allocate ocssd_bdev\n");
		rc = -ENOMEM;
		goto error_ctrlr;
	}

	bdev->bdev.name = strdup(name);
	if (!bdev->bdev.name) {
		rc = -ENOMEM;
		goto error_ctrlr;
	}

	bdev->ctrlr = ocssd_ctrlr;
	bdev->init_cb = cb;
	bdev->init_arg = cb_arg;

	opts.conf = NULL;
	opts.ctrlr = ctrlr;
	opts.trid = *trid;
	opts.range = *range;
	opts.mode = mode;
	opts.uuid = *uuid;
	opts.name = bdev->bdev.name;
	/* TODO: set threads based on config */
	opts.core_thread = opts.read_thread = spdk_get_thread();

	rc = spdk_ftl_dev_init(&opts, bdev_ocssd_create_cb, bdev);
	if (rc) {
		SPDK_ERRLOG("Could not create OCSSD device\n");
		goto error_name;
	}

	return 0;

error_name:
	free(bdev->bdev.name);
error_ctrlr:
	bdev_ocssd_remove_ctrlr(ocssd_ctrlr);
	free(bdev);
	return rc;
}

static void
bdev_ocssd_attach_cb(void *ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *nvme_opts)
{
	struct ocssd_probe_ctx *probe_ctx = ctx;
	struct ocssd_bdev_init_opts *opts = probe_ctx->opts;

	if (!spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)) {
		spdk_nvme_detach(ctrlr);
		probe_ctx->init_cb(NULL, probe_ctx->init_arg, -EPERM);
		return;
	}

	if (bdev_ocssd_create(ctrlr, trid, opts->name, &opts->range, opts->mode, &opts->uuid,
			      probe_ctx->init_cb, probe_ctx->init_arg)) {
		probe_ctx->init_cb(NULL, probe_ctx->init_arg, -ENODEV);
		return;
	}
}

static void
bdev_ocssd_bdev_init_done(void)
{
	pthread_mutex_lock(&g_ocssd_bdev_lock);

	if (++g_num_init_bdevs != g_num_conf_bdevs) {
		pthread_mutex_unlock(&g_ocssd_bdev_lock);
		return;
	}

	pthread_mutex_unlock(&g_ocssd_bdev_lock);

	spdk_bdev_module_init_done(&g_ocssd_if);
}

static void
bdev_ocssd_init_cb(const struct ocssd_bdev_info *info, void *ctx, int status)
{
	(void) info;
	(void) ctx;

	if (status) {
		SPDK_ERRLOG("Failed to initialize OCSSD bdev\n");
	}

	bdev_ocssd_bdev_init_done();
}

static void
bdev_ocssd_initialize_cb(void *ctx, int status)
{
	struct spdk_conf_section *sp;
	struct ocssd_bdev_init_opts *opts = NULL;

	if (status) {
		SPDK_ERRLOG("Failed to initialize FTL module\n");
		goto out;
	}

	sp = spdk_conf_find_section(NULL, "Ocssd");
	if (!sp) {
		goto out;
	}

	opts = calloc(OCSSD_MAX_BDEVS, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("Failed to allocate bdev init opts\n");
		goto out;
	}

	if (bdev_ocssd_read_bdev_config(sp, opts, &g_num_conf_bdevs)) {
		goto out;
	}

	for (size_t i = 0; i < g_num_conf_bdevs; ++i) {
		if (bdev_ocssd_init_bdev(&opts[i], bdev_ocssd_init_cb, NULL)) {
			SPDK_ERRLOG("Failed to create bdev '%s'\n", opts[i].name);
			bdev_ocssd_bdev_init_done();
		}
	}
out:
	if (g_num_conf_bdevs == 0) {
		spdk_bdev_module_init_done(&g_ocssd_if);
	}

	free(opts);
}

static int
bdev_ocssd_initialize(void)
{
	struct ftl_module_init_opts ftl_opts = {};
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		SPDK_ERRLOG("Mutex initialization failed\n");
		return -1;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		SPDK_ERRLOG("Mutex initialization failed\n");
		rc = -1;
		goto error;
	}

	if (pthread_mutex_init(&g_ocssd_bdev_lock, &attr)) {
		SPDK_ERRLOG("Mutex initialization failed\n");
		rc = -1;
		goto error;
	}

	/* TODO: retrieve this from config */
	ftl_opts.anm_thread = spdk_get_thread();
	rc = spdk_ftl_module_init(&ftl_opts, bdev_ocssd_initialize_cb, NULL);
error:
	pthread_mutexattr_destroy(&attr);
	return rc;
}

int
bdev_ocssd_init_bdev(struct ocssd_bdev_init_opts *opts, ocssd_bdev_init_fn cb, void *cb_arg)
{
	struct ocssd_bdev_ctrlr *ctrlr;
	struct ocssd_probe_ctx probe_ctx;
	int rc = 0;

	if (!opts || !cb) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_ocssd_bdev_lock);

	/* Check already attached controllers first */
	LIST_FOREACH(ctrlr, &g_ocssd_bdev_ctrlrs, list_entry) {
		if (!spdk_nvme_transport_id_compare(&ctrlr->trid, &opts->trid)) {
			rc = bdev_ocssd_create(ctrlr->ctrlr, &ctrlr->trid, opts->name, &opts->range,
					       opts->mode, &opts->uuid, cb, cb_arg);
			pthread_mutex_unlock(&g_ocssd_bdev_lock);
			return rc;
		}
	}

	pthread_mutex_unlock(&g_ocssd_bdev_lock);

	probe_ctx.init_cb = cb;
	probe_ctx.init_arg = cb_arg;
	probe_ctx.opts = opts;

	if (spdk_nvme_probe(&opts->trid, &probe_ctx, NULL, bdev_ocssd_attach_cb, NULL)) {
		return -ENODEV;
	}

	return 0;
}

void
bdev_ocssd_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct ocssd_bdev *bdev, *tmp;

	pthread_mutex_lock(&g_ocssd_bdev_lock);

	LIST_FOREACH_SAFE(bdev, &g_ocssd_bdevs, list_entry, tmp) {
		if (strcmp(bdev->bdev.name, name) == 0) {
			spdk_bdev_unregister(&bdev->bdev, cb_fn, cb_arg);

			pthread_mutex_unlock(&g_ocssd_bdev_lock);
			return;
		}
	}

	pthread_mutex_unlock(&g_ocssd_bdev_lock);
	cb_fn(cb_arg, -ENODEV);
}

static void
bdev_ocssd_ftl_module_fini_cb(void *ctx, int status)
{
	(void)ctx;

	if (status) {
		SPDK_ERRLOG("Failed to deinitialize FTL module\n");
		assert(0);
	}

	spdk_bdev_module_finish_done();
}

static void
bdev_ocssd_finish_cb(void)
{
	if (spdk_ftl_module_fini(bdev_ocssd_ftl_module_fini_cb, NULL)) {
		SPDK_ERRLOG("Failed to deinitialize FTL module\n");
		assert(0);
	}
}

static void
bdev_ocssd_finish(void)
{
	pthread_mutex_lock(&g_ocssd_bdev_lock);

	if (LIST_EMPTY(&g_ocssd_bdevs)) {
		pthread_mutex_unlock(&g_ocssd_bdev_lock);
		bdev_ocssd_finish_cb();
		return;
	}

	g_finish_cb = bdev_ocssd_finish_cb;
	pthread_mutex_unlock(&g_ocssd_bdev_lock);
}

static void
bdev_ocssd_get_spdk_running_config(FILE *fp)
{
	struct ocssd_bdev *bdev;
	uint64_t ocssd_bdev_size;

	fprintf(fp, "\n[Ocssd]\n");

	pthread_mutex_lock(&g_ocssd_bdev_lock);
	LIST_FOREACH(bdev, &g_ocssd_bdevs, list_entry) {
		ocssd_bdev_size = bdev->bdev.blocklen * bdev->bdev.blockcnt;
		ocssd_bdev_size /= (1024 * 1024);
		fprintf(fp, "  %s %" PRIu64 " %u\n",
			bdev->bdev.name, ocssd_bdev_size, bdev->bdev.blocklen);
	}
	pthread_mutex_unlock(&g_ocssd_bdev_lock);
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
