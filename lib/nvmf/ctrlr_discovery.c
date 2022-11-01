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
	uint32_t sid;

	SPDK_DEBUGLOG(nvmf, "Generating log page for genctr %" PRIu64 "\n",
		      tgt->discovery_genctr);

	cur_size = sizeof(struct spdk_nvmf_discovery_log_page);
	disc_log = calloc(1, cur_size);
	if (disc_log == NULL) {
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return NULL;
	}

	for (sid = 0; sid < tgt->max_subsystems; sid++) {
		subsystem = tgt->subsystems[sid];
		if ((subsystem == NULL) ||
		    (subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE) ||
		    (subsystem->state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING)) {
			continue;
		}

		if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
			continue;
		}

		for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
		     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
			if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
				struct spdk_nvme_transport_id source_trid = *cmd_source_trid;
				struct spdk_nvme_transport_id listener_trid = *listener->trid;

				/* Do not generate an entry for the transport ID for the listener
				 * entry associated with the discovery controller that generated
				 * this command.  We compare a copy of the trids, since the trids
				 * here don't contain the subnqn, and the transport_id_compare()
				 * function will compare the subnqns.
				 */
				source_trid.subnqn[0] = '\0';
				listener_trid.subnqn[0] = '\0';
				if (!spdk_nvme_transport_id_compare(&listener_trid, &source_trid)) {
					continue;
				}
			}

			if ((tgt->discovery_filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE) != 0 &&
			    !nvmf_discovery_compare_trtype(listener->trid, cmd_source_trid)) {
				SPDK_DEBUGLOG(nvmf, "ignore listener type %d (%s) due to type mismatch\n",
					      listener->trid->trtype, listener->trid->trstring);
				continue;
			}
			if ((tgt->discovery_filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS) != 0 &&
			    !nvmf_discovery_compare_tr_addr(listener->trid, cmd_source_trid)) {
				SPDK_DEBUGLOG(nvmf, "ignore listener addr %s due to addr mismatch\n",
					      listener->trid->traddr);
				continue;
			}
			if ((tgt->discovery_filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID) != 0 &&
			    !nvmf_discovery_compare_tr_svcid(listener->trid, cmd_source_trid)) {
				SPDK_DEBUGLOG(nvmf, "ignore listener svcid %s due to svcid mismatch\n",
					      listener->trid->trsvcid);
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

			nvmf_transport_listener_discover(listener->transport, listener->trid, entry);

			numrec++;
		}
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
