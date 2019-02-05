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

#ifndef SPDK_COMMON_BDEV_NVME_H
#define SPDK_COMMON_BDEV_NVME_H

#include "spdk/nvme.h"
#include "spdk/ftl.h"
#include "spdk/rpc.h"

struct spdk_bdev_nvme_construct_opts {
	/* NVMe controller's transport ID */
	struct spdk_nvme_transport_id		trid;
	/* Bdev's name */
	const char				*name;
	/* Transport address to be used by the host when connecting to the NVMe-oF endpoint */
	struct spdk_nvme_host_id		hostid;
	/* Host NQN */
	const char				*hostnqn;
	/* Parallel unit range (FTL bdev specific) */
	struct spdk_ftl_punit_range		range;
	/* UUID if device is restored from SSD (FTL bdev specific) */
	struct spdk_uuid			*uuid;
	uint32_t				prchk_flags;
};

struct rpc_construct_nvme {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
	char *hostnqn;
	char *hostaddr;
	char *hostsvcid;
	char *punits;
	char *uuid;
	char *mode;
	bool prchk_reftag;
	bool prchk_guard;
};

typedef void (*spdk_rpc_construct_bdev_fn)(struct spdk_bdev_nvme_construct_opts *opts,
		struct spdk_jsonrpc_request *request);
typedef int (*spdk_rpc_parse_args_fn)(struct rpc_construct_nvme *req,
				      struct spdk_bdev_nvme_construct_opts *opts);

void spdk_rpc_register_nvme_construct_methods(const char *bdev_type,
		spdk_rpc_construct_bdev_fn construct_fn, spdk_rpc_parse_args_fn parse_fn);

#define SPDK_RPC_REGISTER_CONSTRUCT_FNS(bdev_type, construct_fn, parse_fn) \
static void __attribute__((constructor)) rpc_register_##construct_fn(void) \
{ \
	spdk_rpc_register_nvme_construct_methods(bdev_type, construct_fn, parse_fn); \
}

#endif /* SPDK_COMMON_BDEV_NVME_H */
