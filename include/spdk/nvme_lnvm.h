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

/** \file
 * LightNVM driver public API
 */

#ifndef SPDK_NVME_LNVM_H
#define SPDK_NVME_LNVM_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme_lnvm_spec.h"
#include "spdk/nvme.h"

bool spdk_nvme_ctrlr_is_lightnvm_supported(struct spdk_nvme_ctrlr *ctrlr);

int spdk_nvme_lnvm_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);

int spdk_nvme_ns_lnvm_cmd_vector_reset(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *metadata, uint64_t *lbal, uint32_t nlb,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

int spdk_nvme_ns_lnvm_cmd_vector_copy(struct spdk_nvme_ns *ns,
				      struct spdk_nvme_qpair *qpair,
				      uint64_t *dlbal, uint64_t *slbal, uint32_t nlb,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				      uint32_t io_flags);

int spdk_nvme_ns_lnvm_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lbal, uint32_t nlb,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

int spdk_nvme_ns_lnvm_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lbal, uint32_t nlb,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

int spdk_nvme_ns_lnvm_cmd_vector_write(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *buffer,
				       uint64_t *lbal, uint32_t nlb,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

int spdk_nvme_ns_lnvm_cmd_vector_read(struct spdk_nvme_ns *ns,
				      struct spdk_nvme_qpair *qpair,
				      void *buffer,
				      uint64_t *lbal, uint32_t nlb,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				      uint32_t io_flags);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_LNVM_H */
