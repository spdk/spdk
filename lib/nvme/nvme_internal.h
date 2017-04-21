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

#include "spdk/stdinc.h"

#include "spdk/nvme.h"

#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "spdk/queue.h"
#include "spdk/barrier.h"
#include "spdk/bit_array.h"
#include "spdk/mmio.h"
#include "spdk/pci_ids.h"
#include "spdk/util.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"

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

/*
 * The controller needs a delay before starts checking the device
 * readiness, which is done by reading the NVME_CSTS_RDY bit.
 */
#define NVME_QUIRK_DELAY_BEFORE_CHK_RDY	0x4

/*
 * The controller performs best when I/O is split on particular
 * LBA boundaries.
 */
#define NVME_INTEL_QUIRK_STRIPING 0x8

/*
 * The controller needs a delay after allocating an I/O queue pair
 * before it is ready to accept I/O commands.
 */
#define NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC 0x10

/*
 * Earlier NVMe devices do not indicate whether unmapped blocks
 * will read all zeroes or not. This define indicates that the
 * device does in fact read all zeroes after an unmap event
 */
#define NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE 0x20

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
#define DEFAULT_IO_QUEUE_SIZE		(256)

#define DEFAULT_ADMIN_QUEUE_REQUESTS	(32)
#define DEFAULT_IO_QUEUE_REQUESTS	(512)

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
		struct nvme_sgl_args {
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

	struct spdk_nvme_qpair		*qpair;

	/**
	 * The active admin request can be moved to a per process pending
	 *  list based on the saved pid to tell which process it belongs
	 *  to. The cpl saves the original completion information which
	 *  is used in the completion callback.
	 * NOTE: these below two fields are only used for admin request.
	 */
	pid_t				pid;
	struct spdk_nvme_cpl		cpl;

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

	/**
	 * The user_cb_fn and user_cb_arg fields are used for holding the original
	 * callback data when using nvme_allocate_request_user_copy.
	 */
	spdk_nvme_cmd_cb		user_cb_fn;
	void				*user_cb_arg;
	void				*user_buffer;
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

struct spdk_nvme_qpair {
	STAILQ_HEAD(, nvme_request)	free_req;
	STAILQ_HEAD(, nvme_request)	queued_req;

	enum spdk_nvme_transport_type	trtype;

	uint16_t			id;

	uint8_t				qprio;

	/*
	 * Members for handling IO qpair deletion inside of a completion context.
	 * These are specifically defined as single bits, so that they do not
	 *  push this data structure out to another cacheline.
	 */
	uint8_t				in_completion_context : 1;
	uint8_t				delete_after_completion_context: 1;

	/*
	 * Set when no deletion notification is needed. For example, the process
	 * which allocated this qpair exited unexpectedly.
	 */
	uint8_t				no_deletion_notification_needed: 1;

	struct spdk_nvme_ctrlr		*ctrlr;

	/* List entry for spdk_nvme_ctrlr::active_io_qpairs */
	TAILQ_ENTRY(spdk_nvme_qpair)	tailq;

	/* List entry for spdk_nvme_ctrlr_process::allocated_io_qpairs */
	TAILQ_ENTRY(spdk_nvme_qpair)	per_process_tailq;

	void				*req_buf;
};

struct spdk_nvme_ns {
	struct spdk_nvme_ctrlr		*ctrlr;
	uint32_t			sector_size;

	/*
	 * Size of data transferred as part of each block,
	 * including metadata if FLBAS indicates the metadata is transferred
	 * as part of the data buffer at the end of each LBA.
	 */
	uint32_t			extended_lba_size;

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
	 * Enable the controller by writing CC.EN to 1
	 */
	NVME_CTRLR_STATE_ENABLE,

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
 * Used to track properties for all processes accessing the controller.
 */
struct spdk_nvme_ctrlr_process {
	/** Whether it is the primary process  */
	bool						is_primary;

	/** Process ID */
	pid_t						pid;

	/** Active admin requests to be completed */
	STAILQ_HEAD(, nvme_request)			active_reqs;

	TAILQ_ENTRY(spdk_nvme_ctrlr_process)		tailq;

	/** Per process PCI device handle */
	struct spdk_pci_device				*devhandle;

	/** Reference to track the number of attachment to this controller. */
	int						ref;

	/** Allocated IO qpairs */
	TAILQ_HEAD(, spdk_nvme_qpair)			allocated_io_qpairs;
};

/*
 * One of these per allocated PCI device.
 */
struct spdk_nvme_ctrlr {
	/* Hot data (accessed in I/O path) starts here. */

	/** Array of namespaces indexed by nsid - 1 */
	struct spdk_nvme_ns		*ns;

	struct spdk_nvme_transport_id	trid;

	uint32_t			num_ns;

	bool				is_removed;

	bool				is_resetting;

	bool				is_failed;

	uint16_t			max_sges;

	/** Controller support flags */
	uint64_t			flags;

	/* Cold data (not accessed in normal I/O path) is after this point. */

	union spdk_nvme_cap_register	cap;

	enum nvme_ctrlr_state		state;
	uint64_t			state_timeout_tsc;

	uint64_t			next_keep_alive_tick;
	uint64_t			keep_alive_interval_ticks;

	TAILQ_ENTRY(spdk_nvme_ctrlr)	tailq;

	/** All the log pages supported */
	bool				log_page_supported[256];

	/** All the features supported */
	bool				feature_supported[256];

	/** maximum i/o size in bytes */
	uint32_t			max_xfer_size;

	/** minimum page size supported by this controller in bytes */
	uint32_t			min_page_size;

	/** selected memory page size for this controller in bytes */
	uint32_t			page_size;

	uint32_t			num_aers;
	struct nvme_async_event_request	aer[NVME_MAX_ASYNC_EVENTS];
	spdk_nvme_aer_cb		aer_cb_fn;
	void				*aer_cb_arg;

	/** guards access to the controller itself, including admin queues */
	pthread_mutex_t			ctrlr_lock;


	struct spdk_nvme_qpair		*adminq;

	/** shadow doorbell buffer */
	uint32_t			*shadow_doorbell;
	/** eventidx buffer */
	uint32_t			*eventidx;

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

	struct spdk_bit_array		*free_io_qids;
	TAILQ_HEAD(, spdk_nvme_qpair)	active_io_qpairs;

	struct spdk_nvme_ctrlr_opts	opts;

	uint64_t			quirks;

	/* Extra sleep time during controller initialization */
	uint64_t			sleep_timeout_tsc;

	/** Track all the processes manage this controller */
	TAILQ_HEAD(, spdk_nvme_ctrlr_process)	active_procs;

	/**
	 * A function pointer to timeout callback function
	 */
	spdk_nvme_timeout_cb		timeout_cb_fn;
	void				*timeout_cb_arg;
	uint64_t			timeout_ticks;

	STAILQ_HEAD(, nvme_request)	queued_aborts;
	uint32_t			outstanding_aborts;
};

struct nvme_driver {
	pthread_mutex_t			lock;

	/** Multi-process shared attached controller list */
	TAILQ_HEAD(, spdk_nvme_ctrlr)	shared_attached_ctrlrs;

	bool				initialized;
	uint8_t				default_extended_host_id[16];
};

extern struct nvme_driver *g_spdk_nvme_driver;

/*
 * Used for the spdk_nvme_connect() public API to save user specified opts.
 */
struct spdk_nvme_ctrlr_connect_opts {
	const struct spdk_nvme_ctrlr_opts	*opts;
	size_t					opts_size;
};

#define nvme_delay		usleep

static inline bool
nvme_qpair_is_admin_queue(struct spdk_nvme_qpair *qpair)
{
	return qpair->id == 0;
}

static inline bool
nvme_qpair_is_io_queue(struct spdk_nvme_qpair *qpair)
{
	return qpair->id != 0;
}

static inline int
nvme_robust_mutex_lock(pthread_mutex_t *mtx)
{
	int rc = pthread_mutex_lock(mtx);

#ifndef __FreeBSD__
	if (rc == EOWNERDEAD) {
		rc = pthread_mutex_consistent(mtx);
	}
#endif

	return rc;
}

static inline int
nvme_robust_mutex_unlock(pthread_mutex_t *mtx)
{
	return pthread_mutex_unlock(mtx);
}

/* Admin functions */
int	nvme_ctrlr_cmd_identify_controller(struct spdk_nvme_ctrlr *ctrlr,
		void *payload,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_identify_namespace(struct spdk_nvme_ctrlr *ctrlr,
		uint16_t nsid, void *payload,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
				      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg);
int	nvme_ctrlr_cmd_get_num_queues(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
		union spdk_nvme_critical_warning_state state,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_set_host_id(struct spdk_nvme_ctrlr *ctrlr, void *host_id, uint32_t host_id_size,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int	nvme_ctrlr_cmd_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t prp1, uint64_t prp2,
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

int	nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle);
void	nvme_ctrlr_free_processes(struct spdk_nvme_ctrlr *ctrlr);
struct spdk_pci_device *nvme_ctrlr_proc_get_devhandle(struct spdk_nvme_ctrlr *ctrlr);

int	nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid, void *devhandle,
			 spdk_nvme_probe_cb probe_cb, void *cb_ctx);

int	nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove);
int	nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr);
int	nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ctrlr_connected(struct spdk_nvme_ctrlr *ctrlr);

int	nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
					struct nvme_request *req);
int	nvme_ctrlr_get_cap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap);
void	nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_cap_register *cap);
int	nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
			struct spdk_nvme_ctrlr *ctrlr,
			enum spdk_nvme_qprio qprio,
			uint32_t num_requests);
void	nvme_qpair_enable(struct spdk_nvme_qpair *qpair);
void	nvme_qpair_disable(struct spdk_nvme_qpair *qpair);
int	nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair,
				  struct nvme_request *req);

int	nvme_ns_construct(struct spdk_nvme_ns *ns, uint16_t id,
			  struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ns_destruct(struct spdk_nvme_ns *ns);

struct nvme_request *nvme_allocate_request(struct spdk_nvme_qpair *qpair,
		const struct nvme_payload *payload,
		uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
struct nvme_request *nvme_allocate_request_null(struct spdk_nvme_qpair *qpair,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
struct nvme_request *nvme_allocate_request_contig(struct spdk_nvme_qpair *qpair,
		void *buffer, uint32_t payload_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
struct nvme_request *nvme_allocate_request_user_copy(struct spdk_nvme_qpair *qpair,
		void *buffer, uint32_t payload_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg, bool host_to_controller);
void	nvme_free_request(struct nvme_request *req);
void	nvme_request_remove_child(struct nvme_request *parent, struct nvme_request *child);
uint64_t nvme_get_quirks(const struct spdk_pci_id *id);

int	nvme_robust_mutex_init_shared(pthread_mutex_t *mtx);
int	nvme_robust_mutex_init_recursive_shared(pthread_mutex_t *mtx);

bool	nvme_completion_is_retry(const struct spdk_nvme_cpl *cpl);
void	nvme_qpair_print_command(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd);
void	nvme_qpair_print_completion(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cpl *cpl);

struct spdk_nvme_ctrlr *spdk_nvme_get_ctrlr_by_trid_unsafe(
	const struct spdk_nvme_transport_id *trid);

/* Transport specific functions */
#define DECLARE_TRANSPORT(name) \
	struct spdk_nvme_ctrlr *nvme_ ## name ## _ctrlr_construct(const struct spdk_nvme_transport_id *trid, const struct spdk_nvme_ctrlr_opts *opts, \
		void *devhandle); \
	int nvme_ ## name ## _ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr); \
	int nvme_ ## name ## _ctrlr_scan(const struct spdk_nvme_transport_id *trid, void *cb_ctx, spdk_nvme_probe_cb probe_cb, spdk_nvme_remove_cb remove_cb, bool direct_connect); \
	int nvme_ ## name ## _ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr); \
	int nvme_ ## name ## _ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value); \
	int nvme_ ## name ## _ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value); \
	int nvme_ ## name ## _ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value); \
	int nvme_ ## name ## _ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value); \
	uint32_t nvme_ ## name ## _ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr); \
	uint16_t nvme_ ## name ## _ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr); \
	struct spdk_nvme_qpair *nvme_ ## name ## _ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid, const struct spdk_nvme_io_qpair_opts *opts); \
	void *nvme_ ## name ## _ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size); \
	int nvme_ ## name ## _ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size); \
	int nvme_ ## name ## _ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair); \
	int nvme_ ## name ## _ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair); \
	int nvme_ ## name ## _qpair_enable(struct spdk_nvme_qpair *qpair); \
	int nvme_ ## name ## _qpair_disable(struct spdk_nvme_qpair *qpair); \
	int nvme_ ## name ## _qpair_reset(struct spdk_nvme_qpair *qpair); \
	int nvme_ ## name ## _qpair_fail(struct spdk_nvme_qpair *qpair); \
	int nvme_ ## name ## _qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req); \
	int32_t nvme_ ## name ## _qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions);

DECLARE_TRANSPORT(transport) /* generic transport dispatch functions */
DECLARE_TRANSPORT(pcie)
#ifdef  SPDK_CONFIG_RDMA
DECLARE_TRANSPORT(rdma)
#endif

#undef DECLARE_TRANSPORT

/*
 * Below ref related functions must be called with the global
 *  driver lock held for the multi-process condition.
 *  Within these functions, the per ctrlr ctrlr_lock is also
 *  acquired for the multi-thread condition.
 */
void	nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr);
int	nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr);

static inline bool
_is_page_aligned(uint64_t address, uint64_t page_size)
{
	return (address & (page_size - 1)) == 0;
}

#endif /* __NVME_INTERNAL_H__ */
