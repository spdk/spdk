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
#include <spdk/ocssd.h>
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

	struct ocssd_dev		*dev;

	TAILQ_ENTRY(ocssd_bdev)		tailq;
};

struct ocssd_io_channel {
	struct ocssd_dev		*dev;

	struct spdk_poller		*poller;

#define OCSSD_MAX_COMPLETIONS 64
	struct ocssd_bdev_io		*io[OCSSD_MAX_COMPLETIONS];

	/* Completion ring */
	struct spdk_ring		*ring;
};

struct ocssd_bdev_io {
	struct ocssd_bdev		*bdev;

	struct ocssd_io			*io;

	struct spdk_ring		*ring;

	int				status;

	struct spdk_thread		*orig_thread;
};

struct ocssd_probe_ctx {
	struct ocssd_bdev_init_opts	*opts;

	size_t				count;

	struct ocssd_bdev_info          *bdev_info;
};

enum timeout_action {
	TIMEOUT_ACTION_NONE = 0,
	TIMEOUT_ACTION_RESET,
	TIMEOUT_ACTION_ABORT,
};

static enum timeout_action		g_action_on_timeout = TIMEOUT_ACTION_NONE;
static int				g_timeout = 0;
static TAILQ_HEAD(, ocssd_bdev)		g_ocssd_bdevs = TAILQ_HEAD_INITIALIZER(g_ocssd_bdevs);
static TAILQ_HEAD(, ocssd_bdev_ctrlr)	g_ocssd_bdev_ctrlrs =
	TAILQ_HEAD_INITIALIZER(g_ocssd_bdev_ctrlrs);

static int bdev_ocssd_initialize(void);
static void bdev_ocssd_finish(void);
static void bdev_ocssd_get_spdk_running_config(FILE *fp);

static int
bdev_ocssd_get_ctx_size(void)
{
	return sizeof(struct ocssd_bdev_io);
}

static struct spdk_bdev_module ocssd_if = {
	.name		= "ocssd",
	.module_init	= bdev_ocssd_initialize,
	.module_fini	= bdev_ocssd_finish,
	.config_text	= bdev_ocssd_get_spdk_running_config,
	.get_ctx_size	= bdev_ocssd_get_ctx_size,
};

#ifndef OCSSD_UNIT_TEST
SPDK_BDEV_MODULE_REGISTER(&ocssd_if)
#endif

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

static int
bdev_ocssd_destruct(void *ctx)
{
	struct ocssd_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_ocssd_bdevs, bdev, tailq);
	--bdev->ctrlr->ref_cnt;

	ocssd_dev_free(bdev->dev);

	free(bdev->bdev.name);
	free(bdev);

	return 0;
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

	ocssd_io_free(io->io);
	io->io = NULL;

	cnt = spdk_ring_enqueue(io->ring, (void **)&io, 1);
	assert(cnt == 1);
}

static int
bdev_ocssd_fill_bio(struct ocssd_bdev *bdev, struct spdk_io_channel *ch,
		    struct ocssd_bdev_io *io)
{
	struct ocssd_io_channel *ioch = spdk_io_channel_get_ctx(ch);

	memset(io, 0, sizeof(*io));

	io->io = ocssd_io_alloc(bdev->dev);
	if (!io->io) {
		return -ENOMEM;
	}

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
	struct ocssd_cb cb;
	int rc;

	bio = spdk_bdev_io_from_ctx(io);

	rc = bdev_ocssd_fill_bio(bdev, ch, io);
	if (rc) {
		return rc;
	}

	cb.ctx = io;
	cb.fn = bdev_ocssd_cb;

	return ocssd_read(io->io,
			  bio->u.bdev.offset_blocks,
			  bio->u.bdev.num_blocks,
			  bio->u.bdev.iovs, bio->u.bdev.iovcnt, &cb);
}

static void
_bdev_ocssd_write(void *ctx)
{
	struct ocssd_bdev_io *io = ctx;
	struct spdk_bdev_io *bio;
	struct ocssd_cb cb;
	int rc;

	bio = spdk_bdev_io_from_ctx(io);
	cb.ctx = io;
	cb.fn = bdev_ocssd_cb;

	rc = ocssd_write(io->io,
			 bio->u.bdev.offset_blocks,
			 bio->u.bdev.num_blocks,
			 bio->u.bdev.iovs, bio->u.bdev.iovcnt, &cb);

	if (rc == -EAGAIN) {
		spdk_thread_send_msg(io->orig_thread, _bdev_ocssd_write, io);
	} else if (spdk_unlikely(rc != 0)) {
		/* We can fail the request immediately and not worry about the */
		/* completion routines, as it's never called when the ocssd_write */
		/* returns an error. */
		bdev_ocssd_complete_io(io, rc);
	}
}

static int
bdev_ocssd_writev(struct ocssd_bdev *bdev, struct spdk_io_channel *ch,
		  struct ocssd_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ocssd_cb cb;
	int rc;

	bio = spdk_bdev_io_from_ctx(io);

	rc = bdev_ocssd_fill_bio(bdev, ch, io);
	if (rc) {
		return rc;
	}

	cb.ctx = io;
	cb.fn = bdev_ocssd_cb;

	rc = ocssd_write(io->io,
			 bio->u.bdev.offset_blocks,
			 bio->u.bdev.num_blocks,
			 bio->u.bdev.iovs,
			 bio->u.bdev.iovcnt, &cb);

	if (rc == -EAGAIN) {
		spdk_thread_send_msg(io->orig_thread, _bdev_ocssd_write, io);
		return 0;
	}

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
	struct ocssd_cb cb;
	int rc;

	rc = bdev_ocssd_fill_bio(bdev, ch, io);
	if (rc) {
		return rc;
	}

	cb.ctx = io;
	cb.fn = bdev_ocssd_cb;

	return ocssd_flush(bdev->dev, &cb);
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
bdev_ocssd_parse_punits(struct ocssd_punit_range *range_array, size_t num_ranges,
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

		if (!(opts->mode & OCSSD_MODE_CREATE)) {
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

static struct ocssd_bdev *
bdev_ocssd_create(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid,
		  const char *name, struct ocssd_punit_range *range,
		  unsigned int mode, const struct spdk_uuid *uuid)
{
	struct ocssd_bdev	*bdev;
	struct ocssd_init_opts	opts;
	struct ocssd_attrs	attrs;
	int			rc;

	bdev = calloc(1, sizeof(*bdev));
	if (!bdev) {
		SPDK_ERRLOG("Could not allocate ocssd_bdev\n");
		return NULL;
	}

	bdev->bdev.name = strdup(name);
	if (!bdev->bdev.name) {
		goto error_dev;
	}

	opts.conf = NULL;
	opts.ctrlr = ctrlr;
	opts.trid = *trid;
	opts.range = *range;
	opts.mode = mode;
	opts.uuid = *uuid;
	opts.name = bdev->bdev.name;

	bdev->dev = ocssd_dev_init(&opts);
	if (!bdev->dev) {
		SPDK_ERRLOG("Could not create OCSSD device\n");
		goto error_bdev;
	}

	if (ocssd_dev_get_attrs(bdev->dev, &attrs)) {
		SPDK_ERRLOG("Failed to retrieve OCSSD device's attrs\n");
		goto error_dev;
	}

	bdev->bdev.product_name = "OCSSD disk";
	bdev->bdev.write_cache = 0;
	bdev->bdev.blocklen = attrs.lbk_size;
	bdev->bdev.blockcnt = attrs.lbk_cnt;
	bdev->bdev.need_aligned_buffer = 1;
	bdev->bdev.uuid = attrs.uuid;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "Creating bdev %s:\n", bdev->bdev.name);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "\tblock_len:\t%zu\n", attrs.lbk_size);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "\tblock_cnt:\t%"PRIu64"\n", attrs.lbk_cnt);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "\tpunits:\t\t%u-%u\n", range->begin, range->end);

	bdev->bdev.ctxt = bdev;
	bdev->bdev.fn_table = &ocssd_fn_table;
	bdev->bdev.module = &ocssd_if;

	spdk_io_device_register(bdev, bdev_ocssd_create_cb, bdev_ocssd_destroy_cb,
				sizeof(struct ocssd_io_channel),
				bdev->bdev.name);

	rc = spdk_bdev_register(&bdev->bdev);
	if (rc) {
		goto error_unregister;
	}

	bdev->ctrlr = bdev_ocssd_add_ctrlr(ctrlr, trid);
	if (!bdev->ctrlr) {
		spdk_bdev_unregister(&bdev->bdev, NULL, NULL);
		goto error_unregister;
	}

	return bdev;

error_unregister:
	spdk_io_device_unregister(bdev, NULL);
	free(bdev->bdev.name);
error_dev:
	ocssd_dev_free(bdev->dev);
error_bdev:
	free(bdev);
	return NULL;
}

static size_t
bdev_ocssd_create_bdevs(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid,
			const char *name, size_t nranges,
			struct ocssd_punit_range *ranges, unsigned int mode,
			const struct spdk_uuid *uuid, struct ocssd_bdev **bdevs)
{
	size_t i, num_bdevs = 0;

	for (i = 0; i < nranges; ++i, ++num_bdevs) {
		bdevs[i] = bdev_ocssd_create(ctrlr, trid, name, &ranges[i], mode, uuid);
		if (!bdevs[i]) {
			break;
		}

		TAILQ_INSERT_TAIL(&g_ocssd_bdevs, bdevs[i], tailq);
	}

	return num_bdevs;
}

static size_t
bdev_ocssd_ctrlr_create(struct ocssd_probe_ctx *ctx, struct spdk_nvme_ctrlr *ctrlr,
			const struct spdk_nvme_transport_id *trid)
{
	struct ocssd_bdev *bdevs[OCSSD_MAX_INSTANCES];
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

	num_bdevs = bdev_ocssd_create_bdevs(ctrlr, trid, opts->names[i], opts->range_count[i],
					    opts->punit_ranges[i], opts->mode, &opts->uuids[i],
					    bdevs);

	for (j = 0; j < num_bdevs; ++j) {
		if (ctx->bdev_info) {
			ctx->bdev_info[ctx->count].name = bdevs[j]->bdev.name;
			ctx->bdev_info[ctx->count].uuid = bdevs[j]->bdev.uuid;
		}
		ctx->count++;
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
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_timeout,
				timeout_cb, NULL);
	}
}

static int
bdev_ocssd_initialize(void)
{
	struct spdk_conf_section *sp;
	struct ocssd_bdev_init_opts *opts = NULL;
	int rc = 0;

	rc = ocssd_init();
	if (rc) {
		goto end;
	}

	sp = spdk_conf_find_section(NULL, "Ocssd");
	if (!sp) {
		return 0;
	}

	opts = calloc(1, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("Failed to allocate bdev init opts\n");
		rc = -1;
	}

	bdev_ocssd_read_drive_config(sp);

	rc = bdev_ocssd_read_bdev_config(sp, opts);
	if (rc) {
		goto end;
	}

	if (opts->count > 0) {
		if (bdev_ocssd_init_bdevs(opts, NULL, NULL)) {
			rc = -1;
		}
	}
end:
	free(opts);
	return rc;
}

int
bdev_ocssd_init_bdevs(struct ocssd_bdev_init_opts *opts,
		      size_t *count, struct ocssd_bdev_info *bdev_info)
{
	struct ocssd_probe_ctx *probe_ctx;
	struct ocssd_bdev_ctrlr *ctrlr;
	int rc = 0;

	if (!opts) {
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
	probe_ctx->bdev_info = bdev_info;
	probe_ctx->count = 0;

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
bdev_ocssd_finish(void)
{
	struct ocssd_bdev *bdev, *tmp;

	TAILQ_FOREACH_SAFE(bdev, &g_ocssd_bdevs, tailq, tmp) {
		spdk_bdev_unregister(&bdev->bdev, NULL, NULL);
	}

	ocssd_deinit();
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
