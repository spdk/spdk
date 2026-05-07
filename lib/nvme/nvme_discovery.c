/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 */

#include "nvme_internal.h"

#include "spdk/endian.h"

struct nvme_discovery_ctx {
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvmf_discovery_log_page	*log_page;
	size_t					log_page_size;
	uint64_t				start_genctr;
	uint64_t				end_genctr;
	union spdk_nvmf_discovery_log_lsp	lsp;
	spdk_nvme_discovery_cb			cb_fn;
	void					*cb_arg;
};

static void
get_log_page_completion_final(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_discovery_ctx *ctx = cb_arg;
	struct spdk_nvme_ctrlr *ctrlr;
	union spdk_nvmf_discovery_log_lsp lsp;
	spdk_nvme_discovery_cb disc_cb_fn;
	void *disc_cb_arg;
	uint32_t tdlpl;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL);
		free(ctx);
		return;
	}

	/* Compare original genctr with latest genctr. If it changed, we need to restart. */
	if (ctx->start_genctr == ctx->end_genctr) {
		if (ctx->log_page->dlpf.extend) {
			tdlpl = from_le32(&ctx->log_page->tdlpl);
			if (tdlpl < sizeof(*ctx->log_page) || tdlpl > ctx->log_page_size) {
				NVME_CTRLR_ERRLOG(ctx->ctrlr,
						  "Invalid total discovery log page length %" PRIu32
						  " (allocated buffer size %zu)\n",
						  tdlpl, ctx->log_page_size);
				free(ctx->log_page);
				ctx->cb_fn(ctx->cb_arg, -EINVAL, NULL, NULL);
				free(ctx);
				return;
			}
		}
		ctx->cb_fn(ctx->cb_arg, 0, cpl, ctx->log_page);
	} else {
		ctrlr = ctx->ctrlr;
		lsp = ctx->lsp;
		disc_cb_fn = ctx->cb_fn;
		disc_cb_arg = ctx->cb_arg;

		free(ctx->log_page);
		rc = spdk_nvme_ctrlr_get_discovery_log_page_ext(ctrlr, lsp, disc_cb_fn, disc_cb_arg);
		if (rc != 0) {
			disc_cb_fn(disc_cb_arg, rc, NULL, NULL);
		}
	}
	free(ctx);
}

static void
get_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_discovery_ctx *ctx = cb_arg;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL);
		free(ctx);
		return;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctx->ctrlr, SPDK_NVME_LOG_DISCOVERY, 0,
					      &ctx->end_genctr, sizeof(ctx->end_genctr), 0,
					      get_log_page_completion_final, ctx);
	if (rc != 0) {
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, rc, NULL, NULL);
		free(ctx);
	}
}

static void
discovery_log_header_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvmf_discovery_log_page *new_page;
	struct nvme_discovery_ctx *ctx = cb_arg;
	size_t page_size;
	uint32_t tdlpl;
	uint16_t recfmt;
	uint64_t numrec;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* Return without printing anything - this may not be a discovery controller */
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL);
		free(ctx->log_page);
		free(ctx);
		return;
	}

	/* Got the first 4K of the discovery log page */
	recfmt = from_le16(&ctx->log_page->recfmt);
	if (recfmt != 0) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Unrecognized discovery log record format %" PRIu16 "\n", recfmt);
		ctx->cb_fn(ctx->cb_arg, -EINVAL, NULL, NULL);
		free(ctx->log_page);
		free(ctx);
		return;
	}

	ctx->start_genctr = ctx->log_page->genctr;

	numrec = from_le64(&ctx->log_page->numrec);

	if (numrec == 0) {
		/* No entries in the discovery log. So we can just return the header to the
		 * caller.
		 */
		get_log_page_completion(ctx, cpl);
		return;
	}

	/*
	 * Determine the full page size. When Extended Discovery Log Page format
	 * was requested (extdlpe=1) and the controller signals support via
	 * dlpf.extend, use tdlpl as the authoritative total length. Otherwise
	 * fall back to the standard fixed-entry sizing.
	 */
	if (ctx->lsp.bits.extdlpe && ctx->log_page->dlpf.extend) {
		tdlpl = from_le32(&ctx->log_page->tdlpl);

		if (tdlpl > 0) {
			if (tdlpl < sizeof(*ctx->log_page)) {
				NVME_CTRLR_ERRLOG(ctx->ctrlr,
						  "Invalid total discovery log page length %" PRIu32 "\n", tdlpl);
				ctx->cb_fn(ctx->cb_arg, -EINVAL, NULL, NULL);
				free(ctx->log_page);
				free(ctx);
				return;
			}
			page_size = tdlpl;
		} else {
			page_size = sizeof(struct spdk_nvmf_discovery_log_page);
			page_size += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
		}
	} else {
		page_size = sizeof(struct spdk_nvmf_discovery_log_page);
		page_size += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
	}

	new_page = realloc(ctx->log_page, page_size);
	if (new_page == NULL) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Could not allocate buffer for log page (%" PRIu64 " entries)\n",
				  numrec);
		ctx->cb_fn(ctx->cb_arg, -ENOMEM, NULL, NULL);
		free(ctx->log_page);
		free(ctx);
		return;
	}

	ctx->log_page = new_page;
	ctx->log_page_size = page_size;

	/* Retrieve the entire discovery log page */
	rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(ctx->ctrlr,
			SPDK_NVME_LOG_DISCOVERY, 0,
			ctx->log_page, page_size, 0,
			(uint32_t)ctx->lsp.raw << 8, 0, 0,
			get_log_page_completion, ctx);
	if (rc != 0) {
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, rc, NULL, NULL);
		free(ctx);
	}
}

static int
nvme_ctrlr_get_discovery_log_page_internal(struct spdk_nvme_ctrlr *ctrlr,
		union spdk_nvmf_discovery_log_lsp lsp,
		spdk_nvme_discovery_cb cb_fn, void *cb_arg)
{
	struct nvme_discovery_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->log_page = calloc(1, sizeof(*ctx->log_page));
	if (ctx->log_page == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->ctrlr = ctrlr;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->lsp = lsp;
	ctx->log_page_size = sizeof(*ctx->log_page);

	rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(ctx->ctrlr,
			SPDK_NVME_LOG_DISCOVERY, 0,
			ctx->log_page, sizeof(*ctx->log_page), 0,
			(uint32_t)ctx->lsp.raw << 8, 0, 0,
			discovery_log_header_completion, ctx);
	if (rc != 0) {
		free(ctx->log_page);
		free(ctx);
	}

	return rc;
}

int
spdk_nvme_ctrlr_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
				       spdk_nvme_discovery_cb cb_fn, void *cb_arg)
{
	union spdk_nvmf_discovery_log_lsp lsp = {};

	return nvme_ctrlr_get_discovery_log_page_internal(ctrlr, lsp, cb_fn, cb_arg);
}

int
spdk_nvme_ctrlr_get_discovery_log_page_ext(struct spdk_nvme_ctrlr *ctrlr,
		union spdk_nvmf_discovery_log_lsp lsp,
		spdk_nvme_discovery_cb cb_fn, void *cb_arg)
{
	return nvme_ctrlr_get_discovery_log_page_internal(ctrlr, lsp, cb_fn, cb_arg);
}
