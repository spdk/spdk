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

#ifndef SPDK_NVMF_SUBSYSTEM_H
#define SPDK_NVMF_SUBSYSTEM_H

#include "spdk/nvme.h"
#include "spdk/queue.h"

struct spdk_nvmf_conn;

#define MAX_PER_SUBSYSTEM_ACCESS_MAP 2
#define MAX_NQN_SIZE 255

struct spdk_nvmf_access_map {
	struct spdk_nvmf_port	*port;
	struct spdk_nvmf_host	*host;
};

/*
 * The NVMf subsystem, as indicated in the specification, is a collection
 * of virtual controller sessions.  Any individual controller session has
 * access to all the NVMe device/namespaces maintained by the subsystem.
 */
struct spdk_nvmf_subsystem {
	uint16_t num;
	char subnqn[MAX_NQN_SIZE];
	enum spdk_nvmf_subsystem_types subtype;
	struct nvmf_session *session;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_qpair *io_qpair;

	int map_count;
	struct spdk_nvmf_access_map map[MAX_PER_SUBSYSTEM_ACCESS_MAP];

	TAILQ_ENTRY(spdk_nvmf_subsystem) entries;
};

struct spdk_nvmf_subsystem *
nvmf_create_subsystem(int num, const char *name, enum spdk_nvmf_subsystem_types sub_type);

int
nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem);

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn);

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr);

int
spdk_nvmf_subsystem_add_map(struct spdk_nvmf_subsystem *subsystem,
			    int port_tag, int host_tag);

int
spdk_shutdown_nvmf_subsystems(void);

int
spdk_add_nvmf_discovery_subsystem(void);

void
spdk_format_discovery_log(struct spdk_nvmf_discovery_log_page *disc_log, uint32_t length);

#endif /* SPDK_NVMF_SUBSYSTEM_H */
