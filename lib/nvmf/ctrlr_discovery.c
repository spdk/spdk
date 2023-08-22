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
nvmf_update_discovery_log(struct spdk_nvmf_tgt *tgt, const char *hostnqn)
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
nvmf_discovery_compare_trid(const enum spdk_nvmf_tgt_discovery_filter filter,
			    const struct spdk_nvme_transport_id *trid1,
			    const struct spdk_nvme_transport_id *trid2)
{
	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE) != 0 &&
	    !nvmf_discovery_compare_trtype(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport type mismatch between %d (%s) and %d (%s)\n",
			      trid1->trtype, trid1->trstring, trid2->trtype, trid2->trstring);
		return false;
	}

	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS) != 0 &&
	    !nvmf_discovery_compare_tr_addr(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport addr mismatch between %s and %s\n",
			      trid1->traddr, trid2->traddr);
		return false;
	}

	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID) != 0 &&
	    !nvmf_discovery_compare_tr_svcid(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport svcid mismatch between %s and %s\n",
			      trid1->trsvcid, trid2->trsvcid);
		return false;
	}

	return true;
}

static struct spdk_nvmf_discovery_log_page *
nvmf_generate_discovery_log(struct spdk_nvmf_tgt *tgt, const char *hostnqn, size_t *log_page_size,
			    struct spdk_nvme_transport_id *cmd_source_trid)
{
	uint64_t numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	struct spdk_nvmf_discovery_log_page *disc_log;
	size_t cur_size;
	struct spdk_nvmf_referral *referral;

	SPDK_DEBUGLOG(nvmf, "Generating log page for genctr %" PRIu64 "\n",
		      tgt->discovery_genctr);

	cur_size = sizeof(struct spdk_nvmf_discovery_log_page);
	disc_log = calloc(1, cur_size);
	if (disc_log == NULL) {
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return NULL;
	}

	for (subsystem = spdk_nvmf_subsystem_get_first(tgt);
	     subsystem != NULL;
	     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
		if ((subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE) ||
		    (subsystem->state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING)) {
			continue;
		}

		if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
			continue;
		}

		for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
		     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {

			if (!nvmf_discovery_compare_trid(tgt->discovery_filter, listener->trid, cmd_source_trid)) {
				continue;
			}

			SPDK_DEBUGLOG(nvmf, "listener %s:%s trtype %s\n", listener->trid->traddr, listener->trid->trsvcid,
				      listener->trid->trstring);

			size_t new_size = cur_size + sizeof(*entry);
			void *new_log_page = realloc(disc_log, new_size);

			if (new_log_page == NULL) {
				SPDK_ERRLOG("Discovery log page memory allocation error\n");
				break;
			}

			disc_log = new_log_page;
			cur_size = new_size;

			entry = &disc_log->entries[numrec];
			memset(entry, 0, sizeof(*entry));
			entry->portid = listener->id;
			entry->cntlid = 0xffff;
			entry->asqsz = listener->transport->opts.max_aq_depth;
			entry->subtype = subsystem->subtype;
			snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

			if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT) {
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

			numrec++;
		}
	}

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		SPDK_DEBUGLOG(nvmf, "referral %s:%s trtype %s\n", referral->trid.traddr, referral->trid.trsvcid,
			      referral->trid.trstring);

		size_t new_size = cur_size + sizeof(*entry);
		void *new_log_page = realloc(disc_log, new_size);

		if (new_log_page == NULL) {
			SPDK_ERRLOG("Discovery log page memory allocation error\n");
			break;
		}

		disc_log = new_log_page;
		cur_size = new_size;

		entry = &disc_log->entries[numrec];
		memcpy(entry, &referral->entry, sizeof(*entry));

		numrec++;
	}


	disc_log->numrec = numrec;
	disc_log->genctr = tgt->discovery_genctr;
	*log_page_size = cur_size;

	return disc_log;
}

void
nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, const char *hostnqn, struct iovec *iov,
			    uint32_t iovcnt, uint64_t offset, uint32_t length,
			    struct spdk_nvme_transport_id *cmd_source_trid)
{
	size_t copy_len = 0;
	size_t zero_len = 0;
	struct iovec *tmp;
	size_t log_page_size = 0;
	struct spdk_nvmf_discovery_log_page *discovery_log_page;

	discovery_log_page = nvmf_generate_discovery_log(tgt, hostnqn, &log_page_size, cmd_source_trid);

	/* Copy the valid part of the discovery log page, if any */
	if (discovery_log_page) {
		for (tmp = iov; tmp < iov + iovcnt; tmp++) {
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

		for (++tmp; tmp < iov + iovcnt; tmp++) {
			memset((char *)tmp->iov_base, 0, tmp->iov_len);
		}

		free(discovery_log_page);
	}
}
