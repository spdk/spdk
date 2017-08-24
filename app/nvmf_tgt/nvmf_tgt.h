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

#ifndef NVMF_TGT_H
#define NVMF_TGT_H

#include "spdk/stdinc.h"

#include "spdk/nvmf.h"
#include "spdk/queue.h"
#include "spdk/event.h"

struct rpc_listen_address {
	char *transport;
	char *adrfam;
	char *traddr;
	char *trsvcid;
};

struct spdk_nvmf_tgt_conf {
	uint32_t acceptor_lcore;
	uint32_t acceptor_poll_rate;
};

struct nvmf_tgt_subsystem {
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_poller *poller;

	TAILQ_ENTRY(nvmf_tgt_subsystem) tailq;

	uint32_t lcore;
};

extern struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf;

extern struct spdk_nvmf_tgt *g_tgt;

struct nvmf_tgt_subsystem *
nvmf_tgt_subsystem_first(void);

struct nvmf_tgt_subsystem *
nvmf_tgt_subsystem_next(struct nvmf_tgt_subsystem *subsystem);

int spdk_nvmf_parse_conf(void);

void nvmf_tgt_start_subsystem(struct nvmf_tgt_subsystem *subsystem);

struct nvmf_tgt_subsystem *nvmf_tgt_create_subsystem(const char *name,
		enum spdk_nvmf_subtype subtype, uint32_t num_ns,
		uint32_t lcore);

int
spdk_nvmf_construct_subsystem(const char *name,
			      int32_t lcore,
			      int num_listen_addresses, struct rpc_listen_address *addresses,
			      int num_hosts, char *hosts[],
			      const char *sn, int num_devs, char *dev_list[]);

int
nvmf_tgt_shutdown_subsystem_by_nqn(const char *nqn);

int spdk_nvmf_tgt_start(struct spdk_app_opts *opts);

#endif
