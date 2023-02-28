/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * SPDK cuse
 */


#ifndef SPDK_NVME_IO_MSG_H_
#define SPDK_NVME_IO_MSG_H_

typedef void (*spdk_nvme_io_msg_fn)(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				    void *arg);

struct spdk_nvme_io_msg {
	struct spdk_nvme_ctrlr	*ctrlr;
	uint32_t		nsid;

	spdk_nvme_io_msg_fn	fn;
	void			*arg;
};

struct nvme_io_msg_producer {
	const char *name;
	void (*update)(struct spdk_nvme_ctrlr *ctrlr);
	void (*stop)(struct spdk_nvme_ctrlr *ctrlr);
	STAILQ_ENTRY(nvme_io_msg_producer) link;
};

int nvme_io_msg_send(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_io_msg_fn fn,
		     void *arg);

/**
 * Process IO message sent to controller from external module.
 *
 * This call process requests from the ring, send IO to an allocated qpair or
 * admin commands in its context. This call is non-blocking and intended to be
 * polled by SPDK thread to provide safe environment for NVMe request
 * completion sent by external module to controller.
 *
 * The caller must ensure that each controller is polled by only one thread at
 * a time.
 *
 * This function may be called at any point while the controller is attached to
 * the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return number of processed external IO messages.
 */
int nvme_io_msg_process(struct spdk_nvme_ctrlr *ctrlr);

int nvme_io_msg_ctrlr_register(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_io_msg_producer *io_msg_producer);
void nvme_io_msg_ctrlr_unregister(struct spdk_nvme_ctrlr *ctrlr,
				  struct nvme_io_msg_producer *io_msg_producer);
void nvme_io_msg_ctrlr_detach(struct spdk_nvme_ctrlr *ctrlr);
void nvme_io_msg_ctrlr_update(struct spdk_nvme_ctrlr *ctrlr);

#endif /* SPDK_NVME_IO_MSG_H_ */
