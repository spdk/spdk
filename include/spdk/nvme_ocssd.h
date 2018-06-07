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

/**
 * \file
 * NVMe driver public API extension for Open-Channel
 */

#ifndef SPDK_NVME_OCSSD_H
#define SPDK_NVME_OCSSD_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/nvme.h"
#include "spdk/nvme_ocssd_spec.h"

/**
 * \brief Identify geometry of the given namespace.
 * \param ctrlr NVMe controller to query.
 * \param nsid Id of the given namesapce.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer. Shall be multiple of 4K.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be
 * allocated for this request, EINVAL if wrong payload size.
 *
 */
int spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				       void *payload, uint32_t payload_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a vector reset command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param lba_arr an array of LBAs of sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param arr_size number of LBAs stored in lba_arr
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *lba_arr, uint32_t arr_size,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a vector write command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param lba_arr an array of LBAs of sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param arr_size number of LBAs stored in lba_arr
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_LNVM_IO_FLAGS_* entries
 * in spdk/lnvm_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_write(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *buffer,
					void *lba_arr, uint32_t arr_size,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg,
					uint32_t io_flags);

/**
 * \brief Submits a vector write command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param lba_arr an array of LBAs of sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param arr_size number of LBAs stored in lba_arr
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_LNVM_IO_FLAGS_* entries
 * in spdk/lnvm_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		void *lba_arr, uint32_t arr_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

/**
 * \brief Submits a vector read command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param lba_arr an array of LBAs of sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param arr_size number of LBAs stored in lba_arr
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_LNVM_IO_FLAGS_* entries
 * in spdk/lnvm_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_read(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *buffer,
				       void *lba_arr, uint32_t arr_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

/**
 * \brief Submits a vector read command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param lba_arr an array of LBAs of sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param arr_size number of LBAs stored in lba_arr
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_LNVM_IO_FLAGS_* entries
 * in spdk/lnvm_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		void *lba_arr, uint32_t arr_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

/**
 * \brief Submits a vector copy command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param dst_lba_arr an array of LBAs of source sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param src_lba_arr an array of LBAs of destination sectors for processing.
 * This memory must be allocated through spdk_dma_malloc() or its variants
 * \param arr_size number of LBAs stored in lba_arr
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_LNVM_IO_FLAGS_* entries
 * in spdk/lnvm_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_copy(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *dst_lba_arr, void *src_lba_arr, uint32_t arr_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

#ifdef __cplusplus
}
#endif

#endif
