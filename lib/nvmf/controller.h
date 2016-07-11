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

#ifndef NVMF_CONTROLLER_H
#define NVMF_CONTROLLER_H

#include <stdbool.h>

#include "nvmf_internal.h"

#define MAX_NVME_NAME_LENGTH 64

struct nvme_bdf_whitelist {
	uint16_t	domain;
	uint8_t		bus;
	uint8_t		dev;
	uint8_t		func;
	char		name[MAX_NVME_NAME_LENGTH];
};

struct spdk_nvmf_ctrlr {
	struct spdk_nvme_ctrlr *ctrlr;
	char 			name[MAX_NVME_NAME_LENGTH];
	bool			claimed;
	TAILQ_ENTRY(spdk_nvmf_ctrlr) entry;
};

int spdk_nvmf_init_nvme(struct nvme_bdf_whitelist *whitelist, size_t whitelist_count,
			bool claim_all, bool unbind_from_kernel);
int spdk_nvmf_shutdown_nvme(void);

struct spdk_nvmf_ctrlr *
spdk_nvmf_ctrlr_claim(const char *name);


#endif
