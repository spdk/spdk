/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over Fabrics discovery service
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"
#include "spdk_internal/assert.h"

#include "spdk/log.h"

void
spdk_nvmf_send_discovery_log_notice(struct spdk_nvmf_tgt *tgt, const char *hostnqn)
{
	struct spdk_nvmf_subsystem *discovery_subsystem;
	struct spdk_nvmf_ctrlr *ctrlr;

	tgt->discovery_genctr++;
	discovery_subsystem = spdk_nvmf_tgt_find_subsystem(tgt, SPDK_NVMF_DISCOVERY_NQN);

	if (discovery_subsystem) {
		/** There is a change in discovery log for hosts with given hostnqn */
		TAILQ_FOREACH(ctrlr, &discovery_subsystem->ctrlrs, link) {
			if (hostnqn == NULL || strcmp(hostnqn, ctrlr->hostnqn) == 0) {
				spdk_thread_send_msg(ctrlr->thread, nvmf_ctrlr_async_event_discovery_log_change_notice, ctrlr);
			}
		}
	}
}

static bool
nvmf_discovery_compare_trtype(const struct spdk_nvme_transport_id *trid1,
			      const struct spdk_nvme_transport_id *trid2)
{
	if (trid1->trtype == SPDK_NVME_TRANSPORT_CUSTOM) {
		return strcasecmp(trid1->trstring, trid2->trstring) == 0;
	} else {
		return trid1->trtype == trid2->trtype;
	}
}

static bool
nvmf_discovery_compare_tr_addr(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return trid1->adrfam == trid2->adrfam && strcasecmp(trid1->traddr, trid2->traddr) == 0;
}

static bool
nvmf_discovery_compare_tr_svcid(const struct spdk_nvme_transport_id *trid1,
				const struct spdk_nvme_transport_id *trid2)
{
	return strcasecmp(trid1->trsvcid, trid2->trsvcid) == 0;
}

static bool
nvmf_discovery_compare_trid(uint32_t filter,
			    const struct spdk_nvme_transport_id *trid1,
			    const struct spdk_nvme_transport_id *trid2)
{
	if ((filter & SPDK_BIT(SPDK_NVMF_TGT_DISCOVERY_FILTER_TYPE)) != 0 &&
	    !nvmf_discovery_compare_trtype(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport type mismatch between %d (%s) and %d (%s)\n",
			      trid1->trtype, trid1->trstring, trid2->trtype, trid2->trstring);
		return false;
	}

	if ((filter & SPDK_BIT(SPDK_NVMF_TGT_DISCOVERY_FILTER_ADDRESS)) != 0 &&
	    !nvmf_discovery_compare_tr_addr(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport addr mismatch between %s and %s\n",
			      trid1->traddr, trid2->traddr);
		return false;
	}

	if ((filter & SPDK_BIT(SPDK_NVMF_TGT_DISCOVERY_FILTER_SVCID)) != 0 &&
	    !nvmf_discovery_compare_tr_svcid(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport svcid mismatch between %s and %s\n",
			      trid1->trsvcid, trid2->trsvcid);
		return false;
	}

	if ((filter & SPDK_BIT(SPDK_NVMF_TGT_DISCOVERY_FILTER_CUSTOM)) != 0 &&
	    g_custom_discovery_filter(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "custom discovery filter mismatch\n");
		return false;
	}

	return true;
}

/* Per NVMe spec, EXATLEN must be a non-zero multiple of 4 */
static inline uint16_t
nvmf_get_ext_attr_len(size_t admin_label_len)
{
	return SPDK_ALIGN_CEIL(admin_label_len, 4);
}

/*
 * Calculate the size of a discovery log page entry.
 * When extdlpe is set, the entry is always an Extended Discovery Log Page
 * Entry (with TEL and NUMEXAT fields). If the subsystem has an admin_label
 * the entry also includes the extended attribute.
 */
static size_t
nvmf_calc_discovery_entry_size(struct spdk_nvmf_subsystem *subsystem, bool extdlpe)
{
	if (extdlpe) {
		if (subsystem->admin_label[0] != '\0') {
			return SPDK_NVMF_DISC_EXT_ENTRY_TEL(
				       nvmf_get_ext_attr_len(strlen(subsystem->admin_label)));
		}
		return SPDK_NVMF_DISC_EXT_ENTRY_BASE_SIZE;
	}

	return SPDK_NVMF_DISC_ENTRY_SIZE;
}

static struct spdk_nvmf_discovery_log_page *
nvmf_generate_discovery_log(struct spdk_nvmf_tgt *tgt, const char *hostnqn, size_t *log_page_size,
			    struct spdk_nvme_transport_id *cmd_source_trid, bool extdlpe)
{
	assert(spdk_thread_is_app_thread(NULL));

	uint64_t numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_discovery_log_page *disc_log;
	size_t cur_size;
	struct spdk_nvmf_referral *referral;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	struct spdk_nvmf_discovery_log_page_entry_extended *ext_hdr;
	struct spdk_nvmf_discovery_extended_attribute *attr;
	size_t entry_size;
	size_t new_size;
	uint8_t *entry_ptr;
	void *new_log_page;

	SPDK_DEBUGLOG(nvmf, "Generating%s discovery log page for genctr %" PRIu64 "\n",
		      extdlpe ? " extended" : "", tgt->discovery_genctr);

	cur_size = SPDK_NVMF_DISC_LOG_PAGE_HEADER_SIZE;
	disc_log = calloc(1, cur_size);
	if (disc_log == NULL) {
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return NULL;
	}

	NVMF_SUBSYSTEM_FOREACH(tgt, subsystem) {
		if ((subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE) ||
		    (subsystem->state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING)) {
			continue;
		}

		if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
			continue;
		}

		TAILQ_FOREACH(listener, &subsystem->listeners, link) {
			if (!nvmf_subsystem_listener_is_active(listener)) {
				continue;
			}

			if (!nvmf_discovery_compare_trid(tgt->discovery_filter, listener->trid, cmd_source_trid)) {
				continue;
			}

			SPDK_DEBUGLOG(nvmf, "listener %s:%s trtype %s\n", listener->trid->traddr, listener->trid->trsvcid,
				      listener->trid->trstring);

			entry_size = nvmf_calc_discovery_entry_size(subsystem, extdlpe);
			new_size = cur_size + entry_size;
			new_log_page = realloc(disc_log, new_size);

			if (new_log_page == NULL) {
				SPDK_ERRLOG("Discovery log page memory allocation error\n");
				break;
			}

			disc_log = new_log_page;
			entry_ptr = (uint8_t *)disc_log + cur_size;
			cur_size = new_size;

			entry = (struct spdk_nvmf_discovery_log_page_entry *)entry_ptr;
			memset(entry, 0, entry_size);
			entry->portid = listener->id;
			entry->cntlid = 0xffff;
			entry->asqsz = listener->transport->opts.max_aq_depth;
			entry->subtype = subsystem->opts.type;
			snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

			if (subsystem->opts.type == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT) {
				/* Each listener in the Current Discovery Subsystem provides access
				 * to the same Discovery Log Pages, so set the Duplicate Returned
				 * Information flag. */
				entry->eflags |= SPDK_NVMF_DISCOVERY_LOG_EFLAGS_DUPRETINFO;
				/* Since the SPDK NVMeoF target supports Asynchronous Event Request
				 * and Keep Alive commands, set the Explicit Persistent Connection
				 * Support for Discovery flag. */
				entry->eflags |= SPDK_NVMF_DISCOVERY_LOG_EFLAGS_EPCSD;
			}

			nvmf_transport_listener_discover(listener->transport, listener->trid, entry);

			/* Add extended entry header (and attributes if present) when requested */
			if (extdlpe) {
				ext_hdr = (struct spdk_nvmf_discovery_log_page_entry_extended *)
					  (entry_ptr + SPDK_NVMF_DISC_ENTRY_SIZE);
				ext_hdr->tel = entry_size;

				if (subsystem->admin_label[0] != '\0') {
					ext_hdr->numexat = 1;
					attr = (struct spdk_nvmf_discovery_extended_attribute *)
					       (entry_ptr + SPDK_NVMF_DISC_EXT_ENTRY_BASE_SIZE);
					attr->exattype = SPDK_NVMF_EXTAT_ADMIN_LABEL;
					attr->exatlen = nvmf_get_ext_attr_len(strlen(subsystem->admin_label));
					memcpy(attr->exatval, subsystem->admin_label, strlen(subsystem->admin_label));
				}
			}

			numrec++;
		}
	}

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		entry_size = extdlpe ? SPDK_NVMF_DISC_EXT_ENTRY_BASE_SIZE : SPDK_NVMF_DISC_ENTRY_SIZE;
		SPDK_DEBUGLOG(nvmf, "referral %s:%s trtype %s\n", referral->trid.traddr, referral->trid.trsvcid,
			      referral->trid.trstring);

		if (!spdk_nvmf_referral_host_allowed(referral, hostnqn)) {
			continue;
		}

		new_size = cur_size + entry_size;
		new_log_page = realloc(disc_log, new_size);

		if (new_log_page == NULL) {
			SPDK_ERRLOG("Discovery log page memory allocation error\n");
			break;
		}

		disc_log = new_log_page;
		entry_ptr = (uint8_t *)disc_log + cur_size;
		cur_size = new_size;

		memcpy(entry_ptr, &referral->entry, sizeof(referral->entry));

		if (extdlpe) {
			ext_hdr = (struct spdk_nvmf_discovery_log_page_entry_extended *)
				  (entry_ptr + SPDK_NVMF_DISC_ENTRY_SIZE);
			*ext_hdr = (struct spdk_nvmf_discovery_log_page_entry_extended) {
				.tel = entry_size,
				.numexat = 0,
			};
		}

		numrec++;
	}

	disc_log->numrec = numrec;
	disc_log->genctr = tgt->discovery_genctr;

	if (extdlpe) {
		disc_log->dlpf.extend = 1;
		disc_log->tdlpl = cur_size;
	}

	*log_page_size = cur_size;

	return disc_log;
}

/* Async discovery log page generation context */
struct nvmf_discovery_log_ctx {
	struct spdk_nvmf_request *req;
	struct spdk_nvmf_tgt *tgt;
	char *hostnqn;
	uint64_t offset;
	uint32_t length;
	struct spdk_nvme_transport_id cmd_source_trid;
	bool rae;
	bool extdlpe;
};

static void
nvmf_get_discovery_log_page(void *arg)
{
	struct nvmf_discovery_log_ctx *ctx = arg;
	struct spdk_nvmf_request *req = ctx->req;
	struct spdk_nvmf_discovery_log_page *discovery_log_page;
	size_t log_page_size = 0;
	size_t copy_len = 0;
	size_t zero_len = 0;
	struct iovec *tmp;
	uint64_t offset = ctx->offset;
	uint32_t length = ctx->length;
	int rc = 0;

	assert(spdk_thread_is_app_thread(NULL));

	discovery_log_page = nvmf_generate_discovery_log(ctx->tgt, ctx->hostnqn,
			     &log_page_size, &ctx->cmd_source_trid, ctx->extdlpe);

	if (offset >= log_page_size) {
		SPDK_ERRLOG("Invalid Get log page discovery offset: (%" PRIu64 "), log page size (%zu)\n",
			    offset, log_page_size);
		rc = -EINVAL;
		free(discovery_log_page);
		goto complete;
	}

	/* Copy the valid part of the discovery log page, if any */
	if (discovery_log_page) {
		for (tmp = req->iov; tmp < req->iov + req->iovcnt; tmp++) {
			copy_len = spdk_min(tmp->iov_len, length);
			copy_len = spdk_min(log_page_size - offset, copy_len);

			memcpy(tmp->iov_base, (char *)discovery_log_page + offset, copy_len);

			offset += copy_len;
			length -= copy_len;
			zero_len = tmp->iov_len - copy_len;
			if (log_page_size <= offset || length == 0) {
				break;
			}
		}
		/* Zero out the rest of the payload */
		if (zero_len) {
			memset((char *)tmp->iov_base + copy_len, 0, zero_len);
		}

		for (++tmp; tmp < req->iov + req->iovcnt; tmp++) {
			memset((char *)tmp->iov_base, 0, tmp->iov_len);
		}

		free(discovery_log_page);
	}

complete:
	if (rc == 0 && !ctx->rae) {
		nvmf_ctrlr_unmask_aen(req->qpair->ctrlr, SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT);
	}

	if (rc != 0) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
	}

	free(ctx->hostnqn);
	free(ctx);

	spdk_nvmf_request_complete(req);
}

void
nvmf_get_discovery_log_page_async(struct spdk_nvmf_request *req,
				  uint64_t offset, uint32_t length,
				  struct spdk_nvme_transport_id *cmd_source_trid,
				  bool rae, bool extdlpe)
{
	struct nvmf_discovery_log_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate discovery log context\n");
		goto error;
	}

	ctx->req = req;
	ctx->tgt = req->qpair->ctrlr->subsys->tgt;
	ctx->hostnqn = strdup(req->qpair->ctrlr->hostnqn);
	if (!ctx->hostnqn) {
		SPDK_ERRLOG("Failed to duplicate hostnqn\n");
		free(ctx);
		goto error;
	}
	ctx->offset = offset;
	ctx->length = length;
	ctx->cmd_source_trid = *cmd_source_trid;
	ctx->rae = rae;
	ctx->extdlpe = extdlpe;

	spdk_thread_send_msg(spdk_thread_get_app_thread(), nvmf_get_discovery_log_page, ctx);
	return;

error:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	spdk_nvmf_request_complete(req);
}
