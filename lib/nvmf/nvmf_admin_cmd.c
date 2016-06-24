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

#include "nvmf.h"
#include "nvmf_internal.h"
#include "session.h"
#include "subsystem_grp.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"
#include "spdk/pci.h"
#include "spdk/trace.h"

void
nvmf_check_admin_completions(struct nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	struct spdk_nvme_ctrlr *ctrlr, *prev_ctrlr = NULL;
	int i;

	for (i = 0; i < MAX_PER_SUBSYSTEM_NAMESPACES; i++) {
		ctrlr = subsystem->ns_list_map[i].ctrlr;
		if (ctrlr == NULL)
			continue;
		if (ctrlr != NULL && ctrlr != prev_ctrlr) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr);
			prev_ctrlr = ctrlr;
		}
	}
}

