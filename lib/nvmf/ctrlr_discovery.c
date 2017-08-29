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

/*
 * NVMe over Fabrics discovery service
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

static void
nvmf_update_discovery_log(struct spdk_nvmf_tgt *tgt)
{
	uint64_t numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_listener *listener;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_discovery_log_page *disc_log;
	size_t cur_size;

	SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Generating log page for genctr %" PRIu64 "\n",
		      tgt->discovery_genctr);

	cur_size = sizeof(struct spdk_nvmf_discovery_log_page);
	disc_log = calloc(1, cur_size);
	if (disc_log == NULL) {
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return;
	}

	TAILQ_FOREACH(subsystem, &tgt->subsystems, entries) {
		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
			continue;
		}

		for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
		     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
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
			entry->portid = numrec;
			entry->cntlid = 0xffff;
			entry->asqsz = tgt->opts.max_queue_depth;
			entry->subtype = subsystem->subtype;
			snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

			transport = spdk_nvmf_tgt_get_transport(tgt, listener->trid.trtype);
			assert(transport != NULL);

			spdk_nvmf_transport_listener_discover(transport, &listener->trid, entry);

			numrec++;
		}
	}

	disc_log->numrec = numrec;
	disc_log->genctr = tgt->discovery_genctr;

	free(tgt->discovery_log_page);

	tgt->discovery_log_page = disc_log;
	tgt->discovery_log_page_size = cur_size;
}

void
spdk_nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, void *buffer,
				 uint64_t offset, uint32_t length)
{
	size_t copy_len = 0;
	size_t zero_len = length;

	if (tgt->discovery_log_page == NULL ||
	    tgt->discovery_log_page->genctr != tgt->discovery_genctr) {
		nvmf_update_discovery_log(tgt);
	}

	/* Copy the valid part of the discovery log page, if any */
	if (tgt->discovery_log_page && offset < tgt->discovery_log_page_size) {
		copy_len = spdk_min(tgt->discovery_log_page_size - offset, length);
		zero_len -= copy_len;
		memcpy(buffer, (char *)tgt->discovery_log_page + offset, copy_len);
	}

	/* Zero out the rest of the buffer */
	if (zero_len) {
		memset((char *)buffer + copy_len, 0, zero_len);
	}

	/* We should have copied or zeroed every byte of the output buffer. */
	assert(copy_len + zero_len == length);
}
