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
#include <spdk_internal/log.h>
#include <spdk/util.h>
#include <spdk/ftl.h>
#include <stdatomic.h>
#include "bdev_ocssd.h"

#define OCSSD_COMPLETION_RING_SIZE 4096

struct ocssd_bdev_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;

	struct spdk_nvme_transport_id	trid;

	size_t				ref_cnt;

	TAILQ_ENTRY(ocssd_bdev_ctrlr)	tailq;
};

struct ocssd_bdev {
	struct spdk_bdev		bdev;

	struct ocssd_bdev_ctrlr		*ctrlr;

	struct ftl_dev			*dev;

	ocssd_bdev_init_fn		init_cb;

	void				*init_arg;

	TAILQ_ENTRY(ocssd_bdev)		tailq;
};

struct ocssd_io_channel {
	struct ftl_dev			*dev;

	struct spdk_poller		*poller;

#define OCSSD_MAX_COMPLETIONS 64
	struct ocssd_bdev_io		*io[OCSSD_MAX_COMPLETIONS];

	/* Completion ring */
	struct spdk_ring		*ring;

	struct spdk_io_channel		*ftl_channel;
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

	size_t				count;
};

enum timeout_action {
	TIMEOUT_ACTION_NONE = 0,
	TIMEOUT_ACTION_RESET,
	TIMEOUT_ACTION_ABORT,
};

typedef void (*bdev_ocssd_finish_fn)(void);

static enum timeout_action		g_action_on_timeout = TIMEOUT_ACTION_NONE;
static int				g_timeout = 0;
static TAILQ_HEAD(, ocssd_bdev)		g_ocssd_bdevs = TAILQ_HEAD_INITIALIZER(g_ocssd_bdevs);
static TAILQ_HEAD(, ocssd_bdev_ctrlr)	g_ocssd_bdev_ctrlrs =
	TAILQ_HEAD_INITIALIZER(g_ocssd_bdev_ctrlrs);
static bdev_ocssd_finish_fn		g_finish_cb;
static atomic_uint			g_bdev_count;
static bool				g_module_init = true;

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

bool
bdev_ocssd_module_init_done(void)
{
	return !g_module_init;
}

static struct ocssd_bdev_ctrlr *
bdev_ocssd_ctrlr_find(const struct spdk_nvme_transport_id *trid)
{
	struct ocssd_bdev_ctrlr *ocssd_ctrlr = NULL;

	TAILQ_FOREACH(ocssd_ctrlr, &g_ocssd_bdev_ctrlrs, tailq) {
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

		TAILQ_INSERT_TAIL(&g_ocssd_bdev_ctrlrs, ocssd_ctrlr, tailq);
	}
out:
	return ocssd_ctrlr;
}

static void
bdev_ocssd_free_cb(void *ctx, int status)
{
	struct ocssd_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_ocssd_bdevs, bdev, tailq);
	bdev->ctrlr->ref_cnt--;

	spdk_bdev_destruct_done(&bdev->bdev, status);

	free(bdev->bdev.name);
	free(bdev);

	if (TAILQ_EMPTY(&g_ocssd_bdevs) && g_finish_cb) {
		g_finish_cb();
		g_finish_cb = NULL;
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
			     ioch->ftl_channel,
			     bio->u.bdev.offset_blocks,
			     bio->u.bdev.num_blocks,
			     bio->u.bdev.iovs, bio->u.bdev.iovcnt, bdev_ocssd_cb, io);
}

static int
bdev_ocssd_writev(struct ocssd_bdev *bdev, struct spdk_io_channel *ch,
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

	rc = spdk_ftl_write(bdev->dev,
			    ioch->ftl_channel,
			    bio->u.bdev.offset_blocks,
			    bio->u.bdev.num_blocks,
			    bio->u.bdev.iovs,
			    bio->u.bdev.iovcnt, bdev_ocssd_cb, io);

	return rc;
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

static void
bdev_ocssd_read_drive_config(struct spdk_conf_section *sp)
{
	const char *val;

	/* TODO: those parameters are set in bdev_nvme, do we need this also? */
	/* (wmalikow) */
	int retry_count;
	if ((retry_count = spdk_conf_section_get_intval(sp, "RetryCount")) < 0) {
		if ((retry_count = spdk_conf_section_get_intval(sp, "NvmeRetryCount")) < 0) {
			retry_count = SPDK_NVME_DEFAULT_RETRY_COUNT;
		} else {
			SPDK_WARNLOG("NvmeRetryCount was renamed to RetryCount\n");
			SPDK_WARNLOG("Please update your configuration file\n");
		}
	}

	spdk_nvme_retry_count = retry_count;

	if ((g_timeout = spdk_conf_section_get_intval(sp, "Timeout")) < 0) {
		/* Check old name for backward compatibility */
		if ((g_timeout = spdk_conf_section_get_intval(sp, "NvmeTimeoutValue")) < 0) {
			g_timeout = 0;
		} else {
			SPDK_WARNLOG("NvmeTimeoutValue was renamed to Timeout\n");
			SPDK_WARNLOG("Please update your configuration file\n");
		}
	}

	if (g_timeout > 0) {
		val = spdk_conf_section_get_val(sp, "ActionOnTimeout");
		if (val != NULL) {
			if (!strcasecmp(val, "Reset")) {
				g_action_on_timeout = TIMEOUT_ACTION_RESET;
			} else if (!strcasecmp(val, "Abort")) {
				g_action_on_timeout = TIMEOUT_ACTION_ABORT;
			}
		} else {
			/* Handle old name for backward compatibility */
			val = spdk_conf_section_get_val(sp, "ResetControllerOnTimeout");
			if (val) {
				SPDK_WARNLOG("ResetControllerOnTimeout was renamed to"
					     "ActionOnTimeout\n");
				SPDK_WARNLOG("Please update your configuration file\n");

				if (spdk_conf_section_get_boolval(sp, "ResetControllerOnTimeout",
								  false)) {
					g_action_on_timeout = TIMEOUT_ACTION_RESET;
				}
			}
		}
	}
}

/* TODO: user iface for creating bdev still needs to be defined (wmalikow) */
int
bdev_ocssd_parse_punits(struct ftl_punit_range *range_array, size_t num_ranges,
			const char *range_string)
{
	regex_t range_regex;
	regmatch_t range_match;
	size_t count = 0, offset = 0;
	unsigned long begin = 0, end = 0;
	size_t str_len = strlen(range_string);
	char *str_ptr;

	/* Catch for "number-number" */
	int ret = regcomp(&range_regex, "\\b[[:digit:]]+-[[:digit:]]+\\b", REG_EXTENDED);
	if (ret) {
		SPDK_WARNLOG("Regex init error\n");
		goto out;
	}

	while (regexec(&range_regex, range_string + offset, 1, &range_match, 0) == 0) {
		if (count >= num_ranges || offset > str_len) {
			break;
		}

		errno = 0;
		begin = strtoul(range_string + offset + range_match.rm_so, &str_ptr, 10);
		offset += range_match.rm_eo;

		if (begin == ULONG_MAX && errno == ERANGE) {
			continue;
		}

		errno = 0;
		/* +1 to skip the '-' delimiter */
		end = strtoul(str_ptr + 1, NULL, 10);
		if (end == ULONG_MAX && errno == ERANGE) {
			continue;
		}

		if (begin > UINT_MAX || end > UINT_MAX) {
			continue;
		}

		range_array[count].begin = begin;
		range_array[count].end = end;
		count++;
	}

	regfree(&range_regex);
out:
	return count;
}

static int
bdev_ocssd_read_bdev_config(struct spdk_conf_section *sp, struct ocssd_bdev_init_opts *opts)
{
	int rc = 0;
	size_t i;
	const char *val;

	for (i = 0; i < OCSSD_MAX_CONTROLLERS; i++) {
		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 0);
		if (!val) {
			break;
		}

		rc = spdk_nvme_transport_id_parse(&opts->trids[i], val);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to parse TransportID: %s\n", val);
			rc = -1;
			break;
		}

		if (opts->trids[i].trtype != SPDK_NVME_TRANSPORT_PCIE) {
			SPDK_ERRLOG("Not supported transport type\n");
			continue;
		}

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 1);
		if (!val) {
			SPDK_ERRLOG("No name provided for TransportID\n");
			rc = -1;
			break;
		}
		opts->names[i] = val;

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 2);
		if (!val) {
			SPDK_ERRLOG("No punit range provided for TransportID\n");
			rc = -1;
			break;
		}
		opts->range_count[opts->count] = bdev_ocssd_parse_punits(
				opts->punit_ranges[opts->count],
				OCSSD_MAX_INSTANCES, val);

		opts->count++;

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 3);
		if (!val) {
			SPDK_ERRLOG("No mode provided for TransportID\n");
			rc = -1;
			break;
		}
		opts->mode = strtoul(val, NULL, 10);

		if (!(opts->mode & FTL_MODE_CREATE)) {
			val = spdk_conf_section_get_nmval(sp, "TransportID", i, 4);
			if (!val) {
				SPDK_ERRLOG("No UUID provided for TransportID\n");
				rc = -1;
				break;
			}
			rc = spdk_uuid_parse(&opts->uuids[i], val);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to parse uuid: %s\n", val);
				rc = -1;
				break;
			}
		}
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
bdev_ocssd_create_cb(void *io_device, void *ctx)
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

	ch->ftl_channel = spdk_get_io_channel(bdev->dev);

	return 0;
}

static void
bdev_ocssd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ocssd_io_channel *ch = ctx_buf;

	spdk_ring_free(ch->ring);
	spdk_poller_unregister(&ch->poller);
}

static bool
bdev_ocssd_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		    struct spdk_nvme_ctrlr_opts *opts)
{
	size_t i;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "Probing device %s\n", trid->traddr);

	struct ocssd_probe_ctx *ctx = cb_ctx;

	for (i = 0; i < ctx->opts->count; i++) {
		if (spdk_nvme_transport_id_compare(trid, &ctx->opts->trids[i]) == 0) {
			return true;
		}
	}

	return false;
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	/* TODO: wmalikow */
}

static void
bdev_ocssd_dev_init_cb(struct ftl_dev *dev, void *ctx, int status)
{
	struct ocssd_bdev	*bdev = ctx;
	struct ocssd_bdev_info	info = {};
	struct ftl_attrs	attrs;
	ocssd_bdev_init_fn	init_cb = bdev->init_cb;
	void			*init_arg = bdev->init_arg;
	int			rc;

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

	spdk_io_device_register(bdev, bdev_ocssd_create_cb, bdev_ocssd_destroy_cb,
				sizeof(struct ocssd_io_channel),
				bdev->bdev.name);

	rc = spdk_bdev_register(&bdev->bdev);
	if (rc) {
		goto error_unregister;
	}

	info.name = bdev->bdev.name;
	info.uuid = bdev->bdev.uuid;

	TAILQ_INSERT_TAIL(&g_ocssd_bdevs, bdev, tailq);

	init_cb(&info, init_arg, 0);
	return;

error_unregister:
	spdk_io_device_unregister(bdev, NULL);
	free(bdev->bdev.name);
error_dev:
	spdk_ftl_dev_free(dev, NULL, NULL);

	init_cb(NULL, init_arg, rc);
}

static struct ocssd_bdev *
bdev_ocssd_create(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid,
		  const char *name, struct ftl_punit_range *range, unsigned int mode,
		  const struct spdk_uuid *uuid, ocssd_bdev_init_fn cb, void *cb_arg)
{
	struct ocssd_bdev *bdev;
	struct ftl_dev_init_opts opts = {};

	bdev = calloc(1, sizeof(*bdev));
	if (!bdev) {
		SPDK_ERRLOG("Could not allocate ocssd_bdev\n");
		return NULL;
	}

	if (g_module_init) {
		atomic_fetch_add(&g_bdev_count, 1);
	}

	bdev->bdev.name = strdup(name);
	if (!bdev->bdev.name) {
		goto error;
	}

	bdev->init_cb = cb;
	bdev->init_arg = cb_arg;
	bdev->ctrlr = bdev_ocssd_add_ctrlr(ctrlr, trid);
	if (!bdev->ctrlr) {
		SPDK_ERRLOG("Could not initialize OCSSD controller\n");
		goto error;
	}

	opts.conf = NULL;
	opts.ctrlr = ctrlr;
	opts.trid = *trid;
	opts.range = *range;
	opts.mode = mode;
	opts.uuid = *uuid;
	opts.name = bdev->bdev.name;
	/* TODO: set threads based on config */
	opts.core_thread = opts.read_thread = spdk_get_thread();

	if (spdk_ftl_dev_init(&opts, bdev_ocssd_dev_init_cb, bdev)) {
		SPDK_ERRLOG("Could not create OCSSD device\n");
		goto error;
	}

	return bdev;
error:
	if (g_module_init) {
		atomic_fetch_sub(&g_bdev_count, 1);
	}

	free(bdev->bdev.name);
	free(bdev);
	return NULL;
}

static size_t
bdev_ocssd_ctrlr_create(struct ocssd_probe_ctx *ctx, struct spdk_nvme_ctrlr *ctrlr,
			const struct spdk_nvme_transport_id *trid)
{
	struct ocssd_bdev *bdev;
	struct ocssd_bdev_init_opts *opts = ctx->opts;
	size_t i, j, num_bdevs = 0;

	for (i = 0; i < opts->count; i++) {
		if (spdk_nvme_transport_id_compare(trid, &opts->trids[i]) == 0) {
			break;
		}
	}

	if (i == opts->count) {
		return 0;
	}

	for (j = 0; j < opts->range_count[i]; ++j) {
		bdev = bdev_ocssd_create(ctrlr, trid, opts->names[i], &opts->punit_ranges[i][j],
					 opts->mode, &opts->uuids[i], ctx->init_cb, ctx->init_arg);
		if (!bdev) {
			SPDK_ERRLOG("Failed to create OCSSD bdev\n");
			ctx->init_cb(NULL, ctx->init_arg, -ENODEV);
			continue;
		}

		num_bdevs++;
	}

	return num_bdevs;
}

static void
bdev_ocssd_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct ocssd_probe_ctx *ctx = cb_ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "Attached to %s\n", trid->traddr);

	if (!bdev_ocssd_ctrlr_create(ctx, ctrlr, trid)) {
		return;
	}

	if (g_action_on_timeout != TIMEOUT_ACTION_NONE) {
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_timeout, timeout_cb, NULL);
	}
}

static void
bdev_ocssd_init_cb(const struct ocssd_bdev_info *info, void *ctx, int status)
{
	unsigned int bdev_cnt;

	(void) info;
	(void) ctx;

	if (status) {
		SPDK_ERRLOG("Failed to create OCSSD bdev\n");
	}

	bdev_cnt = atomic_fetch_sub(&g_bdev_count, 1);
	assert(bdev_cnt > 0);

	if (bdev_cnt == 1) {
		spdk_bdev_module_init_done(&g_ocssd_if);
	}
}

static int
bdev_ocssd_initialize(void)
{
	struct spdk_conf_section *sp;
	struct ocssd_bdev_init_opts *opts = NULL;
	struct ftl_module_init_opts ftl_opts = {};
	unsigned int bdev_cnt = 0;
	int rc = 0;

	/* TODO: retrieve this from config */
	ftl_opts.anm_thread = spdk_get_thread();

	rc = spdk_ftl_module_init(&ftl_opts);
	if (rc) {
		goto end;
	}

	sp = spdk_conf_find_section(NULL, "Ocssd");
	if (!sp) {
		spdk_bdev_module_init_done(&g_ocssd_if);
		goto end;
	}

	opts = calloc(1, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("Failed to allocate bdev init opts\n");
		rc = -1;
		goto end;
	}

	bdev_ocssd_read_drive_config(sp);

	rc = bdev_ocssd_read_bdev_config(sp, opts);
	if (rc) {
		goto end;
	}

	if (opts->count > 0) {
		/* Keep bdev_count at 1, so only after all bdevs are initialized
		 * module_init_done can be called */
		atomic_store(&g_bdev_count, 1);

		if (bdev_ocssd_init_bdevs(opts, NULL, bdev_ocssd_init_cb, NULL)) {
			rc = -1;
		}

		bdev_cnt = atomic_fetch_sub(&g_bdev_count, 1);
		assert(bdev_cnt > 0);

		if (bdev_cnt == 1) {
			spdk_bdev_module_init_done(&g_ocssd_if);
		}
	}
end:
	g_module_init = false;
	free(opts);
	return rc;
}

int
bdev_ocssd_init_bdevs(struct ocssd_bdev_init_opts *opts, size_t *count,
		      ocssd_bdev_init_fn cb, void *cb_arg)
{
	struct ocssd_probe_ctx *probe_ctx;
	struct ocssd_bdev_ctrlr *ctrlr;
	int rc = 0;

	if (!opts || !cb) {
		return -EINVAL;
	}

	if (opts->count == 0) {
		return -ENODEV;
	}

	probe_ctx = calloc(1, sizeof(*probe_ctx));
	if (!probe_ctx) {
		return -ENOMEM;
	}

	probe_ctx->opts = opts;
	probe_ctx->count = 0;
	probe_ctx->init_cb = cb;
	probe_ctx->init_arg = cb_arg;

	/* Initialize bdevs on existing ctrlrs */
	TAILQ_FOREACH(ctrlr, &g_ocssd_bdev_ctrlrs, tailq) {
		bdev_ocssd_ctrlr_create(probe_ctx, ctrlr->ctrlr, &ctrlr->trid);
	}

	if (spdk_nvme_probe(NULL, probe_ctx, bdev_ocssd_probe_cb, bdev_ocssd_attach_cb, NULL)) {
		rc = -ENODEV;
		goto out;
	}
out:
	if (count) {
		*count = probe_ctx->count;
	}

	free(probe_ctx);
	return rc;
}

void
bdev_ocssd_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct ocssd_bdev *bdev, *tmp;

	TAILQ_FOREACH_SAFE(bdev, &g_ocssd_bdevs, tailq, tmp) {
		if (strcmp(bdev->bdev.name, name) == 0) {
			spdk_bdev_unregister(&bdev->bdev, cb_fn, cb_arg);
			return;
		}
	}

	cb_fn(cb_arg, -ENODEV);
}

static void
bdev_ocssd_finish_cb(void)
{
	spdk_ftl_module_deinit();
	spdk_bdev_module_finish_done();
}

static void
bdev_ocssd_finish(void)
{
	if (TAILQ_EMPTY(&g_ocssd_bdevs)) {
		bdev_ocssd_finish_cb();
	} else {
		g_finish_cb = bdev_ocssd_finish_cb;
	}
}

static void
bdev_ocssd_get_spdk_running_config(FILE *fp)
{
	struct ocssd_bdev *bdev;
	uint64_t ocssd_bdev_size;

	fprintf(fp, "\n[Ocssd]\n");

	TAILQ_FOREACH(bdev, &g_ocssd_bdevs, tailq) {
		ocssd_bdev_size = bdev->bdev.blocklen * bdev->bdev.blockcnt;
		ocssd_bdev_size /= (1024 * 1024);
		fprintf(fp, "  %s %" PRIu64 " %u\n",
			bdev->bdev.name, ocssd_bdev_size, bdev->bdev.blocklen);
	}
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
