/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   Copyright(c) 2016 6WIND S.A.
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
 *   DATA, OR PROFITS; OR BUSINESS INTERRUcryptoION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RTE_CRYPTO_H_
#define _RTE_CRYPTO_H_

#ifdef __cplusplus
extern "C" {
#endif

/* In order to mock some DPDK functions, we place headers here with the name name as the DPDK headers
 * so these definitions wil be picked up.  Only what's mocked is included.
 */

#include "rte_mbuf.h"
#include "rte_mempool.h"
#include "rte_crypto_sym.h"

enum rte_crypto_op_type {
	RTE_CRYPTO_OP_TYPE_UNDEFINED,
	RTE_CRYPTO_OP_TYPE_SYMMETRIC,
};

enum rte_crypto_op_status {
	RTE_CRYPTO_OP_STATUS_SUCCESS,
	RTE_CRYPTO_OP_STATUS_NOT_PROCESSED,
	RTE_CRYPTO_OP_STATUS_AUTH_FAILED,
	RTE_CRYPTO_OP_STATUS_INVALID_SESSION,
	RTE_CRYPTO_OP_STATUS_INVALID_ARGS,
	RTE_CRYPTO_OP_STATUS_ERROR,
};

struct rte_crypto_op {
	uint8_t type;
	uint8_t status;
	uint8_t sess_type;
	uint8_t reserved[5];
	struct rte_mempool *mempool;
	rte_iova_t phys_addr;
	__extension__
	union {
		struct rte_crypto_sym_op sym[0];
	};
};

extern struct rte_mempool *
rte_crypto_op_pool_create(const char *name, enum rte_crypto_op_type type,
			  unsigned nb_elts, unsigned cache_size, uint16_t priv_size,
			  int socket_id);

static inline unsigned
rte_crypto_op_bulk_alloc(struct rte_mempool *mempool,
			 enum rte_crypto_op_type type,
			 struct rte_crypto_op **ops, uint16_t nb_ops);

static inline int
rte_crypto_op_attach_sym_session(struct rte_crypto_op *op,
				 struct rte_cryptodev_sym_session *sess);

#ifdef __cplusplus
}
#endif

#endif
