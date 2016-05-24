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

#ifndef __NVME_INTERNAL_H__
#define __NVME_INTERNAL_H__

#include "spdk/nvme.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include <sys/user.h>

#include "spdk/queue.h"
#include "spdk/barrier.h"
#include "spdk/mmio.h"
#include "spdk/pci_ids.h"
#include "spdk/nvme_intel.h"
#include "spdk/pci_ids.h"

/*
 * Some Intel devices support vendor-unique read latency log page even
 * though the log page directory says otherwise.
 */
#define NVME_INTEL_QUIRK_READ_LATENCY 0x1

/*
 * Some Intel devices support vendor-unique write latency log page even
 * though the log page directory says otherwise.
 */
#define NVME_INTEL_QUIRK_WRITE_LATENCY 0x2

#define NVME_MAX_PRP_LIST_ENTRIES	(506)

/*
 * For commands requiring more than 2 PRP entries, one PRP will be
 *  embedded in the command (prp1), and the rest of the PRP entries
 *  will be in a list pointed to by the command (prp2).  This means
 *  that real max number of PRP entries we support is 506+1, which
 *  results in a max xfer size of 506*PAGE_SIZE.
 */
#define NVME_MAX_XFER_SIZE	NVME_MAX_PRP_LIST_ENTRIES * PAGE_SIZE

#define NVME_ADMIN_TRACKERS	(16)
#define NVME_ADMIN_ENTRIES	(128)
/* min and max are defined in admin queue attributes section of spec */
#define NVME_MIN_ADMIN_ENTRIES	(2)
#define NVME_MAX_ADMIN_ENTRIES	(4096)

/*
 * NVME_IO_ENTRIES defines the size of an I/O qpair's submission and completion
 *  queues, while NVME_IO_TRACKERS defines the maximum number of I/O that we
 *  will allow outstanding on an I/O qpair at any time.  The only advantage in
 *  having IO_ENTRIES > IO_TRACKERS is for debugging purposes - when dumping
 *  the contents of the submission and completion queues, it will show a longer
 *  history of data.
 */
#define NVME_IO_ENTRIES		(256)
#define NVME_IO_TRACKERS	(128)
#define NVME_MIN_IO_TRACKERS	(4)
#define NVME_MAX_IO_TRACKERS	(1024)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(253)

/*
 * NVME_MAX_IO_ENTRIES is not defined, since it is specified in CC.MQES
 *  for each controller.
 */

#define NVME_MAX_ASYNC_EVENTS	(8)

#define NVME_MIN_TIMEOUT_PERIOD		(5)
#define NVME_MAX_TIMEOUT_PERIOD		(120)

/* Maximum log page size to fetch for AERs. */
#define NVME_MAX_AER_LOG_SIZE		(4096)

/*
 * NVME_MAX_IO_QUEUES in nvme_spec.h defines the 64K spec-limit, but this
 *  define specifies the maximum number of queues this driver will actually
 *  try to configure, if available.
 */
#define DEFAULT_MAX_IO_QUEUES		(1024)

enum nvme_payload_type {
	NVME_PAYLOAD_TYPE_INVALID = 0,

	/** nvme_request::u.payload.contig_buffer is valid for this request */
	NVME_PAYLOAD_TYPE_CONTIG,

	/** nvme_request::u.sgl is valid for this request */
	NVME_PAYLOAD_TYPE_SGL,
};

/*
 * Controller support flags.
 */
enum spdk_nvme_ctrlr_flags {
	SPDK_NVME_CTRLR_SGL_SUPPORTED		= 0x1, /**< The SGL is supported */
};

/**
 * Descriptor for a request data payload.
 *
 * This struct is arranged so that it fits nicely in struct nvme_request.
 */
struct __attribute__((packed)) nvme_payload {
	union {
		/** Virtual memory address of a single physically contiguous buffer */
		void *contig;

		/**
		 * Functions for retrieving physical addresses for scattered payloads.
		 */
		struct {
			spdk_nvme_req_reset_sgl_cb reset_sgl_fn;
			spdk_nvme_req_next_sge_cb next_sge_fn;
			void *cb_arg;
		} sgl;
	} u;

	/** Virtual memory address of a single physically contiguous metadata buffer */
	void *md;

	/** \ref nvme_payload_type */
	uint8_t type;
};

struct nvme_request {
	struct spdk_nvme_cmd		cmd;

	/**
	 * Data payload for this request's command.
	 */
	struct nvme_payload		payload;

	uint8_t				retries;

	/**
	 * Number of children requests still outstanding for this
	 *  request which was split into multiple child requests.
	 */
	uint8_t				num_children;
	uint32_t			payload_size;

	/**
	 * Offset in bytes from the beginning of payload for this request.
	 * This is used for I/O commands that are split into multiple requests.
	 */
	uint32_t			payload_offset;
	uint32_t			md_offset;

	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;
	STAILQ_ENTRY(nvme_request)	stailq;

	/**
	 * The following members should not be reordered with members
	 *  above.  These members are only needed when splitting
	 *  requests which is done rarely, and the driver is careful
	 *  to not touch the following fields until a split operation is
	 *  needed, to avoid touching an extra cacheline.
	 */

	/**
	 * Points to the outstanding child requests for a parent request.
	 *  Only valid if a request was split into multiple children
	 *  requests, and is not initialized for non-split requests.
	 */
	TAILQ_HEAD(, nvme_request)	children;

	/**
	 * Linked-list pointers for a child request in its parent's list.
	 */
	TAILQ_ENTRY(nvme_request)	child_tailq;

	/**
	 * Points to a parent request if part of a split request,
	 *   NULL otherwise.
	 */
	struct nvme_request		*parent;

	/**
	 * Completion status for a parent request.  Initialized to all 0's
	 *  (SUCCESS) before child requests are submitted.  If a child
	 *  request completes with error, the error status is copied here,
	 *  to ensure that the parent request is also completed with error
	 *  status once all child requests are completed.
	 */
	struct spdk_nvme_cpl		parent_status;
};

struct nvme_completion_poll_status {
	struct spdk_nvme_cpl	cpl;
	bool			done;
};

struct nvme_async_event_request {
	struct spdk_nvme_ctrlr		*ctrlr;
	struct nvme_request		*req;
	struct spdk_nvme_cpl		cpl;
};

struct nvme_tracker {
	LIST_ENTRY(nvme_tracker)	list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			rsvd1: 15;
	uint16_t			active: 1;

	uint32_t			rsvd2;

	uint64_t			prp_sgl_bus_addr;

	union {
		uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
		struct spdk_nvme_sgl_descriptor	sgl[NVME_MAX_SGL_DESCRIPTORS];
	} u;

	uint64_t			rsvd3;
};
/*
 * struct nvme_tracker must be exactly 4K so that the prp[] array does not cross a page boundary
 * and so that there is no padding required to meet alignment requirements.
 */
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) == 4096, "nvme_tracker is not 4K");
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, u.sgl) & 7) == 0, "SGL must be Qword aligned");


struct spdk_nvme_qpair {
	volatile uint32_t		*sq_tdbl;
	volatile uint32_t		*cq_hdbl;

	/**
	 * Submission queue
	 */
	struct spdk_nvme_cmd		*cmd;

	/**
	 * Completion queue
	 */
	struct spdk_nvme_cpl		*cpl;

	LIST_HEAD(, nvme_tracker)	free_tr;
	LIST_HEAD(, nvme_tracker)	outstanding_tr;

	/**
	 * Array of trackers indexed by command ID.
	 */
	struct nvme_tracker		*tr;

	STAILQ_HEAD(, nvme_request)	queued_req;

	uint16_t			id;

	uint16_t			num_entries;
	uint16_t			sq_tail;
	uint16_t			cq_head;

	uint8_t				phase;

	bool				is_enabled;
	bool				sq_in_cmb;

	/*
	 * Fields below this point should not be touched on the normal I/O happy path.
	 */

	uint8_t				qprio;

	struct spdk_nvme_ctrlr		*ctrlr;

	/* List entry for spdk_nvme_ctrlr::free_io_qpairs and active_io_qpairs */
	TAILQ_ENTRY(spdk_nvme_qpair)	tailq;

	uint64_t			cmd_bus_addr;
	uint64_t			cpl_bus_addr;
};

struct spdk_nvme_ns {
	struct spdk_nvme_ctrlr		*ctrlr;
	uint32_t			stripe_size;
	uint32_t			sector_size;
	uint32_t			md_size;
	uint32_t			pi_type;
	uint32_t			sectors_per_max_io;
	uint32_t			sectors_per_stripe;
	uint16_t			id;
	uint16_t			flags;
};

/**
 * State of struct spdk_nvme_ctrlr (in particular, during initialization).
 */
enum nvme_ctrlr_state {
	/**
	 * Controller has not been initialized yet.
	 */
	NVME_CTRLR_STATE_INIT,

	/**
	 * Waiting for CSTS.RDY to transition from 0 to 1 so that CC.EN may be set to 0.
	 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,

	/**
	 * Waiting for CSTS.RDY to transition from 1 to 0 so that CC.EN may be set to 1.
	 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,

	/**
	 * Waiting for CSTS.RDY to transition from 0 to 1 after enabling the controller.
	 */
	NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,

	/**
	 * Controller initialization has completed and the controller is ready.
	 */
	NVME_CTRLR_STATE_READY
};

#define NVME_TIMEOUT_INFINITE	UINT64_MAX

/*
 * One of these per allocated PCI device.
 */
struct spdk_nvme_ctrlr {
	/* Hot data (accessed in I/O path) starts here. */

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers	*regs;

	/** I/O queue pairs */
	struct spdk_nvme_qpair		*ioq;

	/** Array of namespaces indexed by nsid - 1 */
	struct spdk_nvme_ns		*ns;

	uint32_t			num_ns;

	bool				is_resetting;

	bool				is_failed;

	/** Controller support flags */
	uint64_t			flags;

	/* Cold data (not accessed in normal I/O path) is after this point. */

	enum nvme_ctrlr_state		state;
	uint64_t			state_timeout_tsc;

	TAILQ_ENTRY(spdk_nvme_ctrlr)	tailq;

	/** All the log pages supported */
	bool				log_page_supported[256];

	/** All the features supported */
	bool				feature_supported[256];

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device		*devhandle;

	/** maximum i/o size in bytes */
	uint32_t			max_xfer_size;

	/** minimum page size supported by this controller in bytes */
	uint32_t			min_page_size;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t			doorbell_stride_u32;

	uint32_t			num_aers;
	struct nvme_async_event_request	aer[NVME_MAX_ASYNC_EVENTS];
	spdk_nvme_aer_cb		aer_cb_fn;
	void				*aer_cb_arg;

	/** guards access to the controller itself, including admin queues */
	nvme_mutex_t			ctrlr_lock;


	struct spdk_nvme_qpair		adminq;

	/**
	 * Identify Controller data.
	 */
	struct spdk_nvme_ctrlr_data	cdata;

	/**
	 * Array of Identify Namespace data.
	 *
	 * Stored separately from ns since nsdata should not normally be accessed during I/O.
	 */
	struct spdk_nvme_ns_data	*nsdata;

	TAILQ_HEAD(, spdk_nvme_qpair)	free_io_qpairs;
	TAILQ_HEAD(, spdk_nvme_qpair)	active_io_qpairs;

	struct spdk_nvme_ctrlr_opts	opts;

	/** BAR mapping address which contains controller memory buffer */
	void				*cmb_bar_virt_addr;
	/** BAR physical address which contains controller memory buffer */
	uint64_t			cmb_bar_phys_addr;
	/** Controller memory buffer size in Bytes */
	uint64_t			cmb_size;
	/** Current offset of controller memory buffer */
	uint64_t			cmb_current_offset;
};

struct nvme_driver {
	nvme_mutex_t	lock;
	TAILQ_HEAD(, spdk_nvme_ctrlr)	init_ctrlrs;
	TAILQ_HEAD(, spdk_nvme_ctrlr)	attached_ctrlrs;
};

struct pci_id {
	uint16_t	vendor_id;
	uint16_t	dev_id;
	uint16_t	sub_vendor_id;
	uint16_t	sub_dev_id;
};

extern struct nvme_driver g_nvme_driver;

#define nvme_min(a,b) (((a)<(b))?(a):(b))

#define INTEL_DC_P3X00_DEVID	0x09538086

#define nvme_mmio_read_4(sc, reg) \
	spdk_mmio_read_4(&(sc)->regs->reg)

#define nvme_mmio_write_4(sc, reg, val) \
	spdk_mmio_write_4(&(sc)->regs->reg, val)

#define nvme_mmio_write_8(sc, reg, val) \
	spdk_mmio_write_8(&(sc)->regs->reg, val)

#define nvme_delay		usleep

static inline uint32_t
nvme_u32log2(uint32_t x)
{
	if (x == 0) {
		/* __builtin_clz(0) is undefined, so just bail */
		return 0;
	}
	return 31u - __builtin_clz(x);
}

static inline uint32_t
nvme_align32pow2(uint32_t x)
{
	return 1u << (1 + nvme_u32log2(x - 1));
}

/* Admin functions */
int	nvme_ctrlr_cmd_identify_controller(struct spdk_nvme_ctrlr *ctrlr,
		void *payload,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_identify_namespace(struct spdk_nvme_ctrlr *ctrlr,
		uint16_t nsid, void *payload,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				    struct spdk_nvme_qpair *io_que,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				    struct spdk_nvme_qpair *io_que,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
				      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg);
int	nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
		union spdk_nvme_critical_warning_state state,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, uint16_t cid,
			     uint16_t sqid, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg);
int	nvme_ctrlr_cmd_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			      struct spdk_nvme_format *format, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_fw_commit(struct spdk_nvme_ctrlr *ctrlr,
				 const struct spdk_nvme_fw_commit *fw_commit,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
		uint32_t size, uint32_t offset, void *payload,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
void	nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl);

int	nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr, void *devhandle);
void	nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);
int	nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr);
int	nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr);

int	nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
					struct nvme_request *req);
int	nvme_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t length, uint64_t aligned,
			     uint64_t *offset);
int	nvme_qpair_construct(struct spdk_nvme_qpair *qpair, uint16_t id,
			     uint16_t num_entries,
			     uint16_t num_trackers,
			     struct spdk_nvme_ctrlr *ctrlr);
void	nvme_qpair_destroy(struct spdk_nvme_qpair *qpair);
void	nvme_qpair_enable(struct spdk_nvme_qpair *qpair);
void	nvme_qpair_disable(struct spdk_nvme_qpair *qpair);
int	nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair,
				  struct nvme_request *req);
void	nvme_qpair_reset(struct spdk_nvme_qpair *qpair);
void	nvme_qpair_fail(struct spdk_nvme_qpair *qpair);

int	nvme_ns_construct(struct spdk_nvme_ns *ns, uint16_t id,
			  struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ns_destruct(struct spdk_nvme_ns *ns);

struct nvme_request *nvme_allocate_request(const struct nvme_payload *payload,
		uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
struct nvme_request *nvme_allocate_request_null(spdk_nvme_cmd_cb cb_fn, void *cb_arg);
struct nvme_request *nvme_allocate_request_contig(void *buffer, uint32_t payload_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
void	nvme_free_request(struct nvme_request *req);
void	nvme_request_remove_child(struct nvme_request *parent, struct nvme_request *child);
bool	nvme_intel_has_quirk(struct pci_id *id, uint64_t quirk);

void	spdk_nvme_ctrlr_opts_set_defaults(struct spdk_nvme_ctrlr_opts *opts);

#endif /* __NVME_INTERNAL_H__ */
