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

#ifndef OCSSD_NVME_H
#define OCSSD_NVME_H

#include <spdk/nvme.h>
#include <spdk/ocssd.h>

struct ocssd_nvme_ctrlr;
struct ocssd_nvme_ns;
struct ocssd_nvme_qpair;

struct ocssd_nvme_ctrlr *ocssd_nvme_ctrlr_init(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_transport_id *trid);
void	ocssd_nvme_ctrlr_free(struct ocssd_nvme_ctrlr *ctrlr);
struct spdk_nvme_transport_id ocssd_nvme_ctrlr_get_trid(const struct ocssd_nvme_ctrlr *ctrlr);
void	ocssd_nvme_unregister_drivers(void);
int	ocssd_nvme_read(struct ocssd_nvme_ctrlr *ctrlr, struct ocssd_nvme_qpair *qpair,
			void *payload, uint64_t lba, uint32_t lba_count,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags);
int	ocssd_nvme_write(struct ocssd_nvme_ctrlr *ctrlr, struct ocssd_nvme_qpair *qpair,
			 void *buffer, uint64_t lba, uint32_t lba_count,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags);
int	ocssd_nvme_read_with_md(struct ocssd_nvme_ctrlr *ctrlr, struct ocssd_nvme_qpair *qpair,
				void *payload, void *metadata,
				uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				void *cb_arg, uint32_t io_flags,
				uint16_t apptag_mask, uint16_t apptag);
int	ocssd_nvme_write_with_md(struct ocssd_nvme_ctrlr *ctrlr, struct ocssd_nvme_qpair *qpair,
				 void *buffer, void *metadata, uint64_t lba,
				 uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				 uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag);
int	ocssd_nvme_vector_reset(struct ocssd_nvme_ctrlr *ctrlr, struct ocssd_nvme_qpair *qpair,
				uint64_t *lba_list, uint32_t num_lbas,
				struct spdk_ocssd_chunk_information_entry *chunk_info,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	ocssd_nvme_get_log_page(struct ocssd_nvme_ctrlr *ctrlr, uint8_t log_page,
				void *payload, uint32_t payload_size,
				uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	ocssd_nvme_get_geometry(struct ocssd_nvme_ctrlr *ctrlr, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);
void	ocssd_nvme_register_aer_callback(struct ocssd_nvme_ctrlr *ctrlr, spdk_nvme_aer_cb aer_cb_fn,
		void *aer_cb_arg);
int32_t	ocssd_nvme_process_completions(struct ocssd_nvme_ctrlr *ctrlr,
				       struct ocssd_nvme_qpair *qpair,
				       uint32_t max_completions);
int32_t	ocssd_nvme_process_admin_completions(struct ocssd_nvme_ctrlr *ctrlr);
struct ocssd_nvme_ns *ocssd_nvme_get_ns(struct ocssd_nvme_ctrlr *ctrlr);
uint32_t ocssd_nvme_get_md_size(struct ocssd_nvme_ctrlr *ctrlr);
struct	ocssd_nvme_qpair *ocssd_nvme_alloc_io_qpair(struct ocssd_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size);
int	ocssd_nvme_free_io_qpair(struct ocssd_nvme_ctrlr *ctrlr, struct ocssd_nvme_qpair *qpair);

#endif /* OCSSD_NVME_H */
