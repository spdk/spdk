/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

#ifndef __NVME_PCIE_INTERNAL_H__
#define __NVME_PCIE_INTERNAL_H__

/*
 * Number of completion queue entries to process before ringing the
 *  completion queue doorbell.
 */
#define NVME_MIN_COMPLETIONS	(1)
#define NVME_MAX_COMPLETIONS	(128)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(250)

#define NVME_MAX_PRP_LIST_ENTRIES	(503)

/* Minimum admin queue size */
#define NVME_PCIE_MIN_ADMIN_QUEUE_SIZE	(256)

/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;

	/** NVMe MMIO register size */
	uint64_t regs_size;

	struct {
		/* BAR mapping address which contains controller memory buffer */
		void *bar_va;

		/* BAR physical address which contains controller memory buffer */
		uint64_t bar_pa;

		/* Controller memory buffer size in Bytes */
		uint64_t size;

		/* Current offset of controller memory buffer, relative to start of BAR virt addr */
		uint64_t current_offset;

		void *mem_register_addr;
		size_t mem_register_size;
	} cmb;

	struct {
		/* BAR mapping address which contains persistent memory region */
		void *bar_va;

		/* BAR physical address which contains persistent memory region */
		uint64_t bar_pa;

		/* Persistent memory region size in Bytes */
		uint64_t size;

		void *mem_register_addr;
		size_t mem_register_size;
	} pmr;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device *devhandle;

	/* Flag to indicate the MMIO register has been remapped */
	bool is_remapped;

	volatile uint32_t *doorbell_base;
};

extern __thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr;

struct nvme_tracker {
	TAILQ_ENTRY(nvme_tracker)       tq_list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			bad_vtophys : 1;
	uint16_t			rsvd0 : 15;
	uint32_t			rsvd1;

	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;

	uint64_t			prp_sgl_bus_addr;

	/* Don't move, metadata SGL is always contiguous with Data Block SGL */
	struct spdk_nvme_sgl_descriptor		meta_sgl;
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
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, meta_sgl) & 7) == 0, "SGL must be Qword aligned");

struct nvme_pcie_poll_group {
	struct spdk_nvme_transport_poll_group group;
	struct spdk_nvme_pcie_stat stats;
};

enum nvme_pcie_qpair_state {
	NVME_PCIE_QPAIR_WAIT_FOR_CQ = 1,
	NVME_PCIE_QPAIR_WAIT_FOR_SQ,
	NVME_PCIE_QPAIR_READY,
	NVME_PCIE_QPAIR_FAILED,
};

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;

	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;

	/* Completion queue */
	struct spdk_nvme_cpl *cpl;

	TAILQ_HEAD(, nvme_tracker) free_tr;
	TAILQ_HEAD(nvme_outstanding_tr_head, nvme_tracker) outstanding_tr;

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;

	struct spdk_nvme_pcie_stat *stat;

	uint16_t num_entries;

	uint8_t pcie_state;

	uint8_t retry_count;

	uint16_t max_completions_cap;

	uint16_t last_sq_tail;
	uint16_t sq_tail;
	uint16_t cq_head;
	uint16_t sq_head;

	struct {
		uint8_t phase			: 1;
		uint8_t delay_cmd_submit	: 1;
		uint8_t has_shadow_doorbell	: 1;
		uint8_t has_pending_vtophys_failures : 1;
		uint8_t defer_destruction	: 1;
	} flags;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;

	struct {
		/* Submission queue shadow tail doorbell */
		volatile uint32_t *sq_tdbl;

		/* Completion queue shadow head doorbell */
		volatile uint32_t *cq_hdbl;

		/* Submission queue event index */
		volatile uint32_t *sq_eventidx;

		/* Completion queue event index */
		volatile uint32_t *cq_eventidx;
	} shadow_doorbell;

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */

	bool sq_in_cmb;
	bool shared_stats;

	uint64_t cmd_bus_addr;
	uint64_t cpl_bus_addr;

	struct spdk_nvme_cmd *sq_vaddr;
	struct spdk_nvme_cpl *cq_vaddr;
};

static inline struct nvme_pcie_qpair *
nvme_pcie_qpair(struct spdk_nvme_qpair *qpair)
{
	return SPDK_CONTAINEROF(qpair, struct nvme_pcie_qpair, qpair);
}

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	return SPDK_CONTAINEROF(ctrlr, struct nvme_pcie_ctrlr, ctrlr);
}

static inline int
nvme_pcie_qpair_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old)
{
	return (uint16_t)(new_idx - event_idx) <= (uint16_t)(new_idx - old);
}

static inline bool
nvme_pcie_qpair_update_mmio_required(uint16_t value,
				     volatile uint32_t *shadow_db,
				     volatile uint32_t *eventidx)
{
	uint16_t old;

	spdk_wmb();

	old = *shadow_db;
	*shadow_db = value;

	/*
	 * Ensure that the doorbell is updated before reading the EventIdx from
	 * memory
	 */
	spdk_mb();

	if (!nvme_pcie_qpair_need_event(*eventidx, value, old)) {
		return false;
	}

	return true;
}

static inline void
nvme_pcie_qpair_ring_sq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;

	if (qpair->last_fuse == SPDK_NVME_IO_FLAGS_FUSE_FIRST) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		return;
	}

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		pqpair->stat->sq_shadow_doorbell_updates++;
		need_mmio = nvme_pcie_qpair_update_mmio_required(
				    pqpair->sq_tail,
				    pqpair->shadow_doorbell.sq_tdbl,
				    pqpair->shadow_doorbell.sq_eventidx);
	}

	if (spdk_likely(need_mmio)) {
		spdk_wmb();
		pqpair->stat->sq_mmio_doorbell_updates++;
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->sq_tdbl, pqpair->sq_tail);
		g_thread_mmio_ctrlr = NULL;
	}
}

static inline void
nvme_pcie_qpair_ring_cq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		pqpair->stat->cq_shadow_doorbell_updates++;
		need_mmio = nvme_pcie_qpair_update_mmio_required(
				    pqpair->cq_head,
				    pqpair->shadow_doorbell.cq_hdbl,
				    pqpair->shadow_doorbell.cq_eventidx);
	}

	if (spdk_likely(need_mmio)) {
		pqpair->stat->cq_mmio_doorbell_updates++;
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->cq_hdbl, pqpair->cq_head);
		g_thread_mmio_ctrlr = NULL;
	}
}

int nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair);
int nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			      const struct spdk_nvme_io_qpair_opts *opts);
int nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t num_entries);
void nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl);
void nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair);
int nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				     void *cb_arg);
int nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
void nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
void nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr);
void nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
		struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
		bool print_on_error);
void nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				      struct spdk_nvme_cpl *cpl, bool print_on_error);
void nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr);
void nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair);
void nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair);
void nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);
int32_t nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);
int nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair);
struct spdk_nvme_qpair *nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
		const struct spdk_nvme_io_qpair_opts *opts);
int nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
int nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
int nvme_pcie_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
				   struct spdk_nvme_transport_poll_group_stat **_stats);
void nvme_pcie_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				     struct spdk_nvme_transport_poll_group_stat *stats);

struct spdk_nvme_transport_poll_group *nvme_pcie_poll_group_create(void);
int nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			     struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_qpair *qpair);
int64_t nvme_pcie_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
int nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup);

#endif
