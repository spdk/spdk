#ifndef NVME_PCIE_H
#define NVME_PCIE_H

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "nvme_internal.h"
#include "nvme_uevent.h"

/*
 * Number of completion queue entries to process before ringing the
 *  completion queue doorbell.
 */
#define NVME_MIN_COMPLETIONS	(1)
#define NVME_MAX_COMPLETIONS	(128)

#define NVME_ADMIN_ENTRIES	(128)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(253)

#define NVME_MAX_PRP_LIST_ENTRIES	(506)

struct nvme_pcie_enum_ctx {
	spdk_nvme_probe_cb probe_cb;
	void *cb_ctx;
	struct spdk_pci_addr pci_addr;
	bool has_pci_addr;
};

/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;

	/** NVMe MMIO register size */
	uint64_t regs_size;

	/* BAR mapping address which contains controller memory buffer */
	void *cmb_bar_virt_addr;

	/* BAR physical address which contains controller memory buffer */
	uint64_t cmb_bar_phys_addr;

	/* Controller memory buffer size in Bytes */
	uint64_t cmb_size;

	/* Current offset of controller memory buffer, relative to start of BAR virt addr */
	uint64_t cmb_current_offset;

	/* Last valid offset into CMB, this differs if CMB memory registration occurs or not */
	uint64_t cmb_max_offset;

	void *cmb_mem_register_addr;
	size_t cmb_mem_register_size;

	bool cmb_io_data_supported;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device *devhandle;

	/* File descriptor returned from spdk_pci_device_claim().  Closed when ctrlr is detached. */
	int claim_fd;

	/* Flag to indicate the MMIO register has been remapped */
	bool is_remapped;
};

struct nvme_tracker {
	TAILQ_ENTRY(nvme_tracker)       tq_list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			rsvd1: 15;
	uint16_t			active: 1;

	uint32_t			rsvd2;

	uint64_t			rsvd3;

	uint64_t			prp_sgl_bus_addr;

	union {
		uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
		struct spdk_nvme_sgl_descriptor	sgl[NVME_MAX_SGL_DESCRIPTORS];
	} u;
};
/*
 * struct nvme_tracker must be exactly 4K so that the prp[] array does not cross a page boundary
 * and so that there is no padding required to meet alignment requirements.
 */
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) == 4096, "nvme_tracker is not 4K");
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, u.sgl) & 7) == 0, "SGL must be Qword aligned");

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;

	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;

	/* Submission queue shadow tail doorbell */
	volatile uint32_t *sq_shadow_tdbl;

	/* Completion queue shadow head doorbell */
	volatile uint32_t *cq_shadow_hdbl;

	/* Submission queue event index */
	volatile uint32_t *sq_eventidx;

	/* Completion queue event index */
	volatile uint32_t *cq_eventidx;

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;

	/* Completion queue */
	struct spdk_nvme_cpl *cpl;

	TAILQ_HEAD(, nvme_tracker) free_tr;
	TAILQ_HEAD(nvme_outstanding_tr_head, nvme_tracker) outstanding_tr;

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;

	uint16_t num_entries;

	uint16_t max_completions_cap;

	uint16_t sq_tail;
	uint16_t cq_head;
	uint16_t sq_head;

	uint8_t phase;

	bool is_enabled;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */

	bool sq_in_cmb;

	uint64_t cmd_bus_addr;
	uint64_t cpl_bus_addr;
};

#endif /* NVME_PCIE_H */
