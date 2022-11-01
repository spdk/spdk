/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
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
 * \brief Determine if OpenChannel is supported by the given NVMe controller.
 * \param ctrlr NVMe controller to check.
 *
 * \return true if support OpenChannel
 */
bool spdk_nvme_ctrlr_is_ocssd_supported(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Identify geometry of the given namespace.
 * \param ctrlr NVMe controller to query.
 * \param nsid Id of the given namespace.
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
 * \param lba_list an array of LBAs for processing.
 * LBAs must correspond to the start of chunks to reset.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param chunk_info an array of chunk info on DMA-able memory
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					uint64_t *lba_list, uint32_t num_lbas,
					struct spdk_ocssd_chunk_information_entry *chunk_info,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a vector write command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_write(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *buffer,
					uint64_t *lba_list, uint32_t num_lbas,
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
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

/**
 * \brief Submits a vector read command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_read(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *buffer,
				       uint64_t *lba_list, uint32_t num_lbas,
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
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

/**
 * \brief Submits a vector copy command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param dst_lba_list an array of destination LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param src_lba_list an array of source LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in src_lba_list and dst_lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
int spdk_nvme_ocssd_ns_cmd_vector_copy(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       uint64_t *dst_lba_list, uint64_t *src_lba_list,
				       uint32_t num_lbas,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

#ifdef __cplusplus
}
#endif

#endif
