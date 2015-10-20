/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include "spdk/nvme.h"

#include "spdk/queue.h"
#include "spdk/barrier.h"

#define NVME_MAX_PRP_LIST_ENTRIES	(32)

/*
 * For commands requiring more than 2 PRP entries, one PRP will be
 *  embedded in the command (prp1), and the rest of the PRP entries
 *  will be in a list pointed to by the command (prp2).  This means
 *  that real max number of PRP entries we support is 32+1, which
 *  results in a max xfer size of 32*PAGE_SIZE.
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
 * NVME_MAX_IO_ENTRIES is not defined, since it is specified in CC.MQES
 *  for each controller.
 */

#define NVME_MAX_ASYNC_EVENTS	(8)

#define NVME_MIN_TIMEOUT_PERIOD		(5)
#define NVME_MAX_TIMEOUT_PERIOD		(120)

/* Maximum log page size to fetch for AERs. */
#define NVME_MAX_AER_LOG_SIZE		(4096)

struct nvme_request {
	struct nvme_command		cmd;

	/**
	 * Points to a parent request if part of a split request,
	 *   NULL otherwise.
	 */
	struct nvme_request		*parent;
	union {
		void			*payload;
	} u;

	uint8_t				timeout;
	uint8_t				retries;

	/**
	 * Number of children requests still outstanding for this
	 *  request which was split into multiple child requests.
	 */
	uint8_t				num_children;
	uint32_t			payload_size;
	nvme_cb_fn_t			cb_fn;
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
	 * Completion status for a parent request.  Initialized to all 0's
	 *  (SUCCESS) before child requests are submitted.  If a child
	 *  request completes with error, the error status is copied here,
	 *  to ensure that the parent request is also completed with error
	 *  status once all child requests are completed.
	 */
	struct nvme_completion		parent_status;
};

struct nvme_completion_poll_status {
	struct nvme_completion	cpl;
	bool			done;
};

struct nvme_async_event_request {
	struct nvme_controller		*ctrlr;
	struct nvme_request		*req;
	struct nvme_completion		cpl;
};

struct nvme_tracker {
	LIST_ENTRY(nvme_tracker)	list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint64_t			prp_bus_addr;
	uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
};

struct nvme_qpair {
	volatile uint32_t		*sq_tdbl;
	volatile uint32_t		*cq_hdbl;

	/**
	 * Submission queue
	 */
	struct nvme_command		*cmd;

	/**
	 * Completion queue
	 */
	struct nvme_completion		*cpl;

	LIST_HEAD(, nvme_tracker)	free_tr;
	LIST_HEAD(, nvme_tracker)	outstanding_tr;

	STAILQ_HEAD(, nvme_request)	queued_req;

	struct nvme_tracker		**act_tr;

	uint16_t			id;

	uint16_t			num_entries;
	uint16_t			sq_tail;
	uint16_t			cq_head;

	uint8_t				phase;

	bool				is_enabled;

	/*
	 * Fields below this point should not be touched on the normal I/O happy path.
	 */
	struct nvme_controller		*ctrlr;

	uint64_t			cmd_bus_addr;
	uint64_t			cpl_bus_addr;
};

struct nvme_namespace {
	struct nvme_controller		*ctrlr;
	uint32_t			stripe_size;
	uint32_t			sector_size;
	uint32_t			sectors_per_max_io;
	uint32_t			sectors_per_stripe;
	uint16_t			id;
	uint16_t			flags;
};

/*
 * One of these per allocated PCI device.
 */
struct nvme_controller {
	/* Hot data (accessed in I/O path) starts here. */

	/** NVMe MMIO register space */
	volatile struct nvme_registers	*regs;

	/** I/O queue pairs */
	struct nvme_qpair		*ioq;

	uint32_t			is_resetting;

	uint32_t			num_ns;

	/** Array of namespaces indexed by nsid - 1 */
	struct nvme_namespace		*ns;


	/* Cold data (not accessed in normal I/O path) is after this point. */

	/* Opaque handle to associated PCI device. */
	void				*devhandle;

	uint32_t			num_io_queues;

	/** maximum i/o size in bytes */
	uint32_t			max_xfer_size;

	/** minimum page size supported by this controller in bytes */
	uint32_t			min_page_size;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t			doorbell_stride_u32;

	uint32_t			num_aers;
	struct nvme_async_event_request	aer[NVME_MAX_ASYNC_EVENTS];
	nvme_aer_cb_fn_t		aer_cb_fn;
	void				*aer_cb_arg;

	bool				is_failed;

	/** guards access to the controller itself, including admin queues */
	nvme_mutex_t			ctrlr_lock;


	struct nvme_qpair		adminq;

	/**
	 * Identify Controller data.
	 */
	struct nvme_controller_data	cdata;

	/**
	 * Array of Identify Namespace data.
	 *
	 * Stored separately from ns since nsdata should not normally be accessed during I/O.
	 */
	struct nvme_namespace_data	*nsdata;
};

extern int __thread nvme_thread_ioq_index;

struct nvme_driver {
	nvme_mutex_t	lock;
	uint16_t	*ioq_index_pool;
	uint32_t	max_io_queues;
	uint16_t	ioq_index_pool_next;
};

extern struct nvme_driver g_nvme_driver;

#define nvme_min(a,b) (((a)<(b))?(a):(b))

#define INTEL_DC_P3X00_DEVID	0x09538086

static inline uint32_t
_nvme_mmio_read_4(const volatile uint32_t *addr)
{
	return *addr;
}

static inline void
_nvme_mmio_write_4(volatile uint32_t *addr, uint32_t val)
{
	*addr = val;
}

static inline void
_nvme_mmio_write_8(volatile uint64_t *addr, uint64_t val)
{
	*addr = val;
}

#define nvme_mmio_read_4(sc, reg) \
	_nvme_mmio_read_4(&(sc)->regs->reg)

#define nvme_mmio_write_4(sc, reg, val) \
	_nvme_mmio_write_4(&(sc)->regs->reg, val)

#define nvme_mmio_write_8(sc, reg, val) \
	_nvme_mmio_write_8(&(sc)->regs->reg, val)

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
void	nvme_ctrlr_cmd_set_feature(struct nvme_controller *ctrlr,
				   uint8_t feature, uint32_t cdw11,
				   void *payload, uint32_t payload_size,
				   nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_get_feature(struct nvme_controller *ctrlr,
				   uint8_t feature, uint32_t cdw11,
				   void *payload, uint32_t payload_size,
				   nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_get_log_page(struct nvme_controller *ctrlr,
				    uint8_t log_page, uint32_t nsid,
				    void *payload, uint32_t payload_size,
				    nvme_cb_fn_t cb_fn, void *cb_arg);

void	nvme_ctrlr_cmd_identify_controller(struct nvme_controller *ctrlr,
		void *payload,
		nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_identify_namespace(struct nvme_controller *ctrlr,
		uint16_t nsid, void *payload,
		nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_get_error_page(struct nvme_controller *ctrlr,
				      struct nvme_error_information_entry *payload,
				      uint32_t num_entries, /* 0 = max */
				      nvme_cb_fn_t cb_fn,
				      void *cb_arg);
void	nvme_ctrlr_cmd_get_health_information_page(struct nvme_controller *ctrlr,
		uint32_t nsid,
		struct nvme_health_information_page *payload,
		nvme_cb_fn_t cb_fn,
		void *cb_arg);
void	nvme_ctrlr_cmd_get_firmware_page(struct nvme_controller *ctrlr,
		struct nvme_firmware_page *payload,
		nvme_cb_fn_t cb_fn,
		void *cb_arg);
void	nvme_ctrlr_cmd_create_io_cq(struct nvme_controller *ctrlr,
				    struct nvme_qpair *io_que,
				    nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_create_io_sq(struct nvme_controller *ctrlr,
				    struct nvme_qpair *io_que,
				    nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_set_num_queues(struct nvme_controller *ctrlr,
				      uint32_t num_queues, nvme_cb_fn_t cb_fn,
				      void *cb_arg);
void	nvme_ctrlr_cmd_set_async_event_config(struct nvme_controller *ctrlr,
		union nvme_critical_warning_state state,
		nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_ctrlr_cmd_abort(struct nvme_controller *ctrlr, uint16_t cid,
			     uint16_t sqid, nvme_cb_fn_t cb_fn, void *cb_arg);

void	nvme_completion_poll_cb(void *arg, const struct nvme_completion *cpl);

int	nvme_ctrlr_construct(struct nvme_controller *ctrlr, void *devhandle);
void	nvme_ctrlr_destruct(struct nvme_controller *ctrlr);
int	nvme_ctrlr_start(struct nvme_controller *ctrlr);
int	nvme_ctrlr_hw_reset(struct nvme_controller *ctrlr);

void	nvme_ctrlr_submit_admin_request(struct nvme_controller *ctrlr,
					struct nvme_request *req);
void	nvme_ctrlr_submit_io_request(struct nvme_controller *ctrlr,
				     struct nvme_request *req);
void	nvme_ctrlr_post_failed_request(struct nvme_controller *ctrlr,
				       struct nvme_request *req);

int	nvme_qpair_construct(struct nvme_qpair *qpair, uint16_t id,
			     uint16_t num_entries,
			     uint16_t num_trackers,
			     struct nvme_controller *ctrlr);
void	nvme_qpair_destroy(struct nvme_qpair *qpair);
void	nvme_qpair_enable(struct nvme_qpair *qpair);
void	nvme_qpair_disable(struct nvme_qpair *qpair);
void	nvme_qpair_submit_tracker(struct nvme_qpair *qpair,
				  struct nvme_tracker *tr);
void	nvme_qpair_process_completions(struct nvme_qpair *qpair);
void	nvme_qpair_submit_request(struct nvme_qpair *qpair,
				  struct nvme_request *req);
void	nvme_qpair_reset(struct nvme_qpair *qpair);
void	nvme_qpair_fail(struct nvme_qpair *qpair);
void	nvme_qpair_manual_complete_request(struct nvme_qpair *qpair,
		struct nvme_request *req,
		uint32_t sct, uint32_t sc,
		bool print_on_error);

int	nvme_ns_construct(struct nvme_namespace *ns, uint16_t id,
			  struct nvme_controller *ctrlr);
void	nvme_ns_destruct(struct nvme_namespace *ns);

struct nvme_request *
nvme_allocate_request(void *payload, uint32_t payload_size,
		      nvme_cb_fn_t cb_fn, void *cb_arg);
void	nvme_free_request(struct nvme_request *req);

#endif /* __NVME_INTERNAL_H__ */
