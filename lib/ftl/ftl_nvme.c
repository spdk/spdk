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
#include "ftl_nvme.h"
#include "ftl_core.h"

#define FTL_NSID 1

struct ftl_nvme_driver {
	struct spdk_nvme_transport_id	trid;

	struct ftl_nvme_ops		ops;

	LIST_ENTRY(ftl_nvme_driver)	list_entry;
};

struct ftl_nvme_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;

	struct spdk_nvme_transport_id	trid;

	struct spdk_nvme_ns		*ns;

	struct ftl_nvme_ops		ops;

	unsigned int			ref_cnt;

	LIST_ENTRY(ftl_nvme_ctrlr)	list_entry;
};

static pthread_mutex_t			g_nvme_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(, ftl_nvme_driver)	g_drivers;
static LIST_HEAD(, ftl_nvme_ctrlr)	g_ctrlrs;
static const struct ftl_nvme_ops	g_default = {
	.read				= spdk_nvme_ns_cmd_read,
	.read_with_md			= spdk_nvme_ns_cmd_read_with_md,
	.write				= spdk_nvme_ns_cmd_write,
	.write_with_md			= spdk_nvme_ns_cmd_write_with_md,
	.vector_reset			= spdk_nvme_ocssd_ns_cmd_vector_reset,
	.get_log_page			= spdk_nvme_ctrlr_cmd_get_log_page,
	.get_geometry			= spdk_nvme_ocssd_ctrlr_cmd_geometry,
	.register_aer_callback		= spdk_nvme_ctrlr_register_aer_callback,
	.process_completions		= spdk_nvme_qpair_process_completions,
	.process_admin_completions	= spdk_nvme_ctrlr_process_admin_completions,
	.get_ns				= spdk_nvme_ctrlr_get_ns,
	.get_md_size			= spdk_nvme_ns_get_md_size,
	.alloc_io_qpair			= spdk_nvme_ctrlr_alloc_io_qpair,
	.free_io_qpair			= spdk_nvme_ctrlr_free_io_qpair,
};

static struct ftl_nvme_driver *
ftl_nvme_find_driver(const struct spdk_nvme_transport_id *trid)
{
	struct ftl_nvme_driver *driver;

	/* Needs g_nvme_lock to be held */
	LIST_FOREACH(driver, &g_drivers, list_entry) {
		if (!spdk_nvme_transport_id_compare(trid, &driver->trid)) {
			return driver;
		}
	}

	return NULL;
}

static struct ftl_nvme_ctrlr *
ftl_nvme_find_ctrlr(const struct spdk_nvme_transport_id *trid)
{
	struct ftl_nvme_ctrlr *ctrlr;

	/* Needs g_nvme_lock to be held */
	LIST_FOREACH(ctrlr, &g_ctrlrs, list_entry) {
		if (!spdk_nvme_transport_id_compare(&ctrlr->trid, trid)) {
			return ctrlr;
		}
	}

	return NULL;
}

int
spdk_ftl_register_nvme_driver(const struct spdk_nvme_transport_id *trid,
			      const struct ftl_nvme_ops *ops)
{
	struct ftl_nvme_driver *driver;
	int rc = -1;

	pthread_mutex_lock(&g_nvme_lock);

	if (ftl_nvme_find_driver(trid)) {
		SPDK_ERRLOG("Driver already initialized for specified trid: %s\n", trid->traddr);
		goto out;
	}

	driver = calloc(1, sizeof(*driver));
	if (!driver) {
		goto out;
	}

	driver->ops = *ops;
	driver->trid = *trid;
	LIST_INSERT_HEAD(&g_drivers, driver, list_entry);

	rc = 0;
out:
	pthread_mutex_unlock(&g_nvme_lock);
	return rc;
}

void
ftl_nvme_unregister_drivers(void)
{
	struct ftl_nvme_driver *driver, *tdriver;

	pthread_mutex_lock(&g_nvme_lock);

	LIST_FOREACH_SAFE(driver, &g_drivers, list_entry, tdriver) {
		LIST_REMOVE(driver, list_entry);
		free(driver);
	}

	pthread_mutex_unlock(&g_nvme_lock);
}

struct ftl_nvme_ctrlr *
ftl_nvme_ctrlr_init(struct spdk_nvme_ctrlr *nvme_ctrlr,
		    const struct spdk_nvme_transport_id *trid)
{
	struct ftl_nvme_ctrlr *ctrlr = NULL;
	struct ftl_nvme_driver *driver;
	const struct ftl_nvme_ops *nvme_ops = &g_default;

	pthread_mutex_lock(&g_nvme_lock);

	ctrlr = ftl_nvme_find_ctrlr(trid);
	if (ctrlr) {
		ctrlr->ref_cnt++;
	} else {
		driver = ftl_nvme_find_driver(trid);
		if (driver) {
			nvme_ops = &driver->ops;
		}

		ctrlr = calloc(1, sizeof(*ctrlr));
		if (!ctrlr) {
			goto out;
		}

		ctrlr->trid = *trid;
		ctrlr->ops = *nvme_ops;
		ctrlr->ctrlr = nvme_ctrlr;
		ctrlr->ns = (struct spdk_nvme_ns *)ftl_nvme_get_ns(ctrlr);
		ctrlr->ref_cnt = 1;

		LIST_INSERT_HEAD(&g_ctrlrs, ctrlr, list_entry);
	}
out:
	pthread_mutex_unlock(&g_nvme_lock);
	return ctrlr;
}

void
ftl_nvme_ctrlr_free(struct ftl_nvme_ctrlr *ctrlr)
{
	if (!ctrlr) {
		return;
	}

	pthread_mutex_lock(&g_nvme_lock);

	if (--ctrlr->ref_cnt == 0) {
		LIST_REMOVE(ctrlr, list_entry);
		free(ctrlr);
	}

	pthread_mutex_unlock(&g_nvme_lock);
}

struct spdk_nvme_transport_id
ftl_nvme_ctrlr_get_trid(const struct ftl_nvme_ctrlr *ctrlr)
{
	return ctrlr->trid;
}

int
ftl_nvme_read(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair,
	      void *payload, uint64_t lba, uint32_t lba_count,
	      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags)
{
	return ctrlr->ops.read(ctrlr->ns, (struct spdk_nvme_qpair *)qpair, payload, lba, lba_count,
			       cb_fn, cb_arg, io_flags);
}

int
ftl_nvme_write(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair,
	       void *buffer, uint64_t lba, uint32_t lba_count,
	       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags)
{
	return ctrlr->ops.write(ctrlr->ns, (struct spdk_nvme_qpair *)qpair, buffer, lba, lba_count,
				cb_fn, cb_arg, io_flags);
}

int
ftl_nvme_read_with_md(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair,
		      void *payload, void *metadata,
		      uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg, uint32_t io_flags,
		      uint16_t apptag_mask, uint16_t apptag)
{
	return ctrlr->ops.read_with_md(ctrlr->ns, (struct spdk_nvme_qpair *)qpair, payload,
				       metadata, lba, lba_count, cb_fn, cb_arg, io_flags,
				       apptag_mask, apptag);
}

int
ftl_nvme_write_with_md(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair,
		       void *buffer, void *metadata, uint64_t lba,
		       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	return ctrlr->ops.write_with_md(ctrlr->ns, (struct spdk_nvme_qpair *)qpair, buffer,
					metadata, lba, lba_count, cb_fn, cb_arg, io_flags,
					apptag_mask, apptag);
}

int
ftl_nvme_vector_reset(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair,
		      uint64_t *lba_list, uint32_t num_lbas,
		      struct spdk_ocssd_chunk_information_entry *chunk_info,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return ctrlr->ops.vector_reset(ctrlr->ns, (struct spdk_nvme_qpair *)qpair, lba_list,
				       num_lbas, chunk_info, cb_fn, cb_arg);
}

int
ftl_nvme_get_log_page(struct ftl_nvme_ctrlr *ctrlr, uint8_t log_page,
		      void *payload, uint32_t payload_size,
		      uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return ctrlr->ops.get_log_page(ctrlr->ctrlr, log_page, FTL_NSID, payload, payload_size,
				       offset, cb_fn, cb_arg);
}

int
ftl_nvme_get_geometry(struct ftl_nvme_ctrlr *ctrlr, void *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return ctrlr->ops.get_geometry(ctrlr->ctrlr, FTL_NSID, payload,
				       payload_size, cb_fn, cb_arg);
}

void
ftl_nvme_register_aer_callback(struct ftl_nvme_ctrlr *ctrlr,
			       spdk_nvme_aer_cb aer_cb_fn,
			       void *aer_cb_arg)
{
	return ctrlr->ops.register_aer_callback(ctrlr->ctrlr, aer_cb_fn, aer_cb_arg);
}

int32_t
ftl_nvme_process_completions(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair,
			     uint32_t max_completions)
{
	return ctrlr->ops.process_completions((struct spdk_nvme_qpair *)qpair, max_completions);
}

int32_t
ftl_nvme_process_admin_completions(struct ftl_nvme_ctrlr *ctrlr)
{
	return ctrlr->ops.process_admin_completions(ctrlr->ctrlr);
}

struct ftl_nvme_ns *
ftl_nvme_get_ns(struct ftl_nvme_ctrlr *ctrlr)
{
	return (struct ftl_nvme_ns *)ctrlr->ops.get_ns(ctrlr->ctrlr, FTL_NSID);
}

uint32_t
ftl_nvme_get_md_size(struct ftl_nvme_ctrlr *ctrlr)
{
	return ctrlr->ops.get_md_size(ctrlr->ns);
}

struct ftl_nvme_qpair *
ftl_nvme_alloc_io_qpair(struct ftl_nvme_ctrlr *ctrlr,
			const struct spdk_nvme_io_qpair_opts *opts,
			size_t opts_size)
{
	return (struct ftl_nvme_qpair *)ctrlr->ops.alloc_io_qpair(ctrlr->ctrlr, opts, opts_size);
}

int
ftl_nvme_free_io_qpair(struct ftl_nvme_ctrlr *ctrlr, struct ftl_nvme_qpair *qpair)
{
	return ctrlr->ops.free_io_qpair((struct spdk_nvme_qpair *)qpair);
}
