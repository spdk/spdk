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

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#define ACCEPT_TIMEOUT_US	10000 /* 10ms */
#define DEFAULT_CONN_SCHED CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP

enum spdk_nvmf_connect_sched {
	CONNECT_SCHED_ROUND_ROBIN = 0,
	CONNECT_SCHED_HOST_IP,
	CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP,
};

struct spdk_nvmf_admin_passthru_conf {
	bool identify_ctrlr;
};

struct spdk_nvmf_tgt_conf {
	uint32_t acceptor_poll_rate;
	enum spdk_nvmf_connect_sched conn_sched;
	struct spdk_nvmf_admin_passthru_conf admin_passthru;
};

extern struct spdk_nvmf_tgt_conf *g_spdk_nvmf_tgt_conf;

extern uint32_t g_spdk_nvmf_tgt_max_subsystems;

extern struct spdk_nvmf_tgt *g_spdk_nvmf_tgt;

typedef void (*spdk_nvmf_parse_conf_done_fn)(int status);

int spdk_nvmf_parse_conf(spdk_nvmf_parse_conf_done_fn cb_fn);

#endif
