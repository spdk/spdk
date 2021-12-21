/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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

#include "nvme_internal.h"

#include "spdk/endian.h"

struct nvme_discovery_ctx {
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvmf_discovery_log_page	*log_page;
	uint64_t				genctr;
	spdk_nvme_discovery_cb			cb_fn;
	void					*cb_arg;
	struct spdk_nvme_cpl			cpl;
	uint32_t				outstanding_commands;
};

static void
get_log_page_completion_final(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_discovery_ctx *ctx = cb_arg;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL);
		free(ctx);
		return;
	}

	/* Compare original genctr with latest genctr. If it changed, we need to restart. */
	if (ctx->log_page->genctr == ctx->genctr) {
		ctx->cb_fn(ctx->cb_arg, 0, cpl, ctx->log_page);
	} else {
		free(ctx->log_page);
		rc = spdk_nvme_ctrlr_get_discovery_log_page(ctx->ctrlr, ctx->cb_fn, ctx->cb_arg);
		if (rc != 0) {
			ctx->cb_fn(ctx->cb_arg, rc, NULL, NULL);
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
		/* Only save the cpl for the first error that we encounter. */
		if (!spdk_nvme_cpl_is_error(&ctx->cpl)) {
			ctx->cpl = *cpl;
		}
	}
	ctx->outstanding_commands--;
	if (ctx->outstanding_commands > 0) {
		return;
	}

	if (spdk_nvme_cpl_is_error(&ctx->cpl)) {
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, 0, &ctx->cpl, NULL);
		free(ctx);
		return;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctx->ctrlr, SPDK_NVME_LOG_DISCOVERY, 0,
					      &ctx->genctr, sizeof(ctx->genctr), 0,
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
	uint16_t recfmt;
	uint64_t numrec;
	uint64_t remaining;
	uint64_t offset;
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
		SPDK_ERRLOG("Unrecognized discovery log record format %" PRIu16 "\n", recfmt);
		ctx->cb_fn(ctx->cb_arg, -EINVAL, NULL, NULL);
		free(ctx->log_page);
		free(ctx);
		return;
	}

	numrec = from_le64(&ctx->log_page->numrec);

	if (numrec == 0) {
		/* No entries in the discovery log. So we can just return the header to the
		 * caller. Increment outstanding_commands and use the get_log_page_completion()
		 * function to avoid duplicating that code here.
		 */
		ctx->outstanding_commands++;
		get_log_page_completion(ctx, cpl);
		return;
	}

	/*
	 * Now that we know how many entries should be in the log page, we can allocate
	 * the full log page buffer.
	 */
	page_size = sizeof(struct spdk_nvmf_discovery_log_page);
	page_size += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
	new_page = realloc(ctx->log_page, page_size);
	if (new_page == NULL) {
		SPDK_ERRLOG("Could not allocate buffer for log page (%" PRIu64 " entries)\n",
			    numrec);
		ctx->cb_fn(ctx->cb_arg, -ENOMEM, NULL, NULL);
		free(ctx->log_page);
		free(ctx);
		return;
	}

	ctx->log_page = new_page;

	/* Retrieve the rest of the discovery log page */
	offset = offsetof(struct spdk_nvmf_discovery_log_page, entries);
	remaining = page_size - offset;
	while (remaining) {
		uint32_t size;

		/* Retrieve up to 4 KB at a time */
		size = spdk_min(remaining, 4096);

		ctx->outstanding_commands++;
		rc = spdk_nvme_ctrlr_cmd_get_log_page(ctx->ctrlr, SPDK_NVME_LOG_DISCOVERY,
						      0, (char *)ctx->log_page + offset, size, offset,
						      get_log_page_completion, ctx);
		if (rc != 0) {
			/* We may have already successfully submitted some get_log_page commands,
			 * so we cannot just call the user's callback with error status and free
			 * the log page here.  Simulate a completion instead, so that we keep
			 * all of the cleanup code in the get_log_page_completion() function.
			 */
			struct spdk_nvme_cpl cpl = { 0 };

			SPDK_ERRLOG("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
			cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			cpl.status.dnr = 1;
			get_log_page_completion(ctx, &cpl);
			return;
		}

		offset += size;
		remaining -= size;
	}
}

int
spdk_nvme_ctrlr_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
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

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0,
					      ctx->log_page, sizeof(*ctx->log_page), 0,
					      discovery_log_header_completion, ctx);
	if (rc != 0) {
		free(ctx->log_page);
		free(ctx);
	}

	return rc;
}
