/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2019-2022, Nutanix Inc. All rights reserved.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over vfio-user transport
 */

#include <sys/param.h>

#include <vfio-user/libvfio-user.h>
#include <vfio-user/pci_defs.h>

#include "spdk/barrier.h"
#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvmf_transport.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "transport.h"

#include "nvmf_internal.h"

#define SWAP(x, y)                  \
	do                          \
	{                           \
		typeof(x) _tmp = x; \
		x = y;              \
		y = _tmp;           \
	} while (0)

#define NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH 256
#define NVMF_VFIO_USER_DEFAULT_AQ_DEPTH 32
#define NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE ((NVMF_REQ_MAX_BUFFERS - 1) << SHIFT_4KB)
#define NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE

#define NVME_DOORBELLS_OFFSET	0x1000
#define NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT 2
#define NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS 3
#define NVMF_VFIO_USER_EVENTIDX_POLL UINT32_MAX

#define NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR 512
#define NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR (NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR / 4)

/* NVMe spec 1.4, section 5.21.1.7 */
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR >= 2 &&
		   NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR <= SPDK_NVME_MAX_IO_QUEUES,
		   "bad number of queues");

/*
 * NVMe driver reads 4096 bytes, which is the extended PCI configuration space
 * available on PCI-X 2.0 and PCI Express buses
 */
#define NVME_REG_CFG_SIZE       0x1000

/*
 * Doorbells must be page aligned so that they can memory mapped.
 *
 * TODO does the NVMe spec also require this? Document it.
 */
#define NVMF_VFIO_USER_DOORBELLS_SIZE \
	SPDK_ALIGN_CEIL( \
		(NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2 * SPDK_NVME_DOORBELL_REGISTER_SIZE), \
		0x1000)
#define NVME_REG_BAR0_SIZE (NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE)

/*
 * TODO check the PCI spec whether BAR4 and BAR5 really have to be at least one
 * page and a multiple of page size (maybe QEMU also needs this?). Document all
 * this.
 */

/*
 * MSI-X Pending Bit Array Size
 *
 * TODO according to the PCI spec we need one bit per vector, document the
 * relevant section.
 *
 * If the first argument to SPDK_ALIGN_CEIL is 0 then the result is 0, so we
 * would end up with a 0-size BAR5.
 */
#define NVME_IRQ_MSIX_NUM MAX(CHAR_BIT, NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR)
#define NVME_BAR5_SIZE SPDK_ALIGN_CEIL((NVME_IRQ_MSIX_NUM / CHAR_BIT), 0x1000)
SPDK_STATIC_ASSERT(NVME_BAR5_SIZE > 0, "Incorrect size");

/* MSI-X Table Size */
#define NVME_BAR4_SIZE SPDK_ALIGN_CEIL((NVME_IRQ_MSIX_NUM * 16), 0x1000)
SPDK_STATIC_ASSERT(NVME_BAR4_SIZE > 0, "Incorrect size");

struct nvmf_vfio_user_req;

typedef int (*nvmf_vfio_user_req_cb_fn)(struct nvmf_vfio_user_req *req, void *cb_arg);

/* 1 more for PRP2 list itself */
#define NVMF_VFIO_USER_MAX_IOVECS	(NVMF_REQ_MAX_BUFFERS + 1)

enum nvmf_vfio_user_req_state {
	VFIO_USER_REQUEST_STATE_FREE = 0,
	VFIO_USER_REQUEST_STATE_EXECUTING,
};

/*
 * Support for live migration in NVMf/vfio-user: live migration is implemented
 * by stopping the NVMf subsystem when the device is instructed to enter the
 * stop-and-copy state and then trivially, and most importantly safely,
 * collecting migration state and providing it to the vfio-user client. We
 * don't provide any migration state at the pre-copy state as that's too
 * complicated to do, we might support this in the future.
 */


/* NVMe device state representation */
struct nvme_migr_sq_state {
	uint16_t	sqid;
	uint16_t	cqid;
	uint32_t	head;
	uint32_t	size;
	uint32_t	reserved;
	uint64_t	dma_addr;
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_migr_sq_state) == 0x18, "Incorrect size");

struct nvme_migr_cq_state {
	uint16_t	cqid;
	uint16_t	phase;
	uint32_t	tail;
	uint32_t	size;
	uint32_t	iv;
	uint32_t	ien;
	uint32_t	reserved;
	uint64_t	dma_addr;
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_migr_cq_state) == 0x20, "Incorrect size");

#define VFIO_USER_NVME_MIGR_MAGIC	0xAFEDBC23

/* The device state is in VFIO MIGRATION BAR(9) region, keep the device state page aligned.
 *
 * NVMe device migration region is defined as below:
 * -------------------------------------------------------------------------
 * | vfio_user_nvme_migr_header | nvmf controller data | queue pairs | BARs |
 * -------------------------------------------------------------------------
 *
 * Keep vfio_user_nvme_migr_header as a fixed 0x1000 length, all new added fields
 * can use the reserved space at the end of the data structure.
 */
struct vfio_user_nvme_migr_header {
	/* Magic value to validate migration data */
	uint32_t	magic;
	/* Version to check the data is same from source to destination */
	uint32_t	version;

	/* The library uses this field to know how many fields in this
	 * structure are valid, starting at the beginning of this data
	 * structure.  New added fields in future use `unused` memory
	 * spaces.
	 */
	uint32_t	opts_size;
	uint32_t	reserved0;

	/* BARs information */
	uint64_t	bar_offset[VFU_PCI_DEV_NUM_REGIONS];
	uint64_t	bar_len[VFU_PCI_DEV_NUM_REGIONS];

	/* Queue pair start offset, starting at the beginning of this
	 * data structure.
	 */
	uint64_t	qp_offset;
	uint64_t	qp_len;

	/* Controller data structure */
	uint32_t	num_io_queues;
	uint32_t	reserved1;

	/* NVMf controller data offset and length if exist, starting at
	 * the beginning of this data structure.
	 */
	uint64_t	nvmf_data_offset;
	uint64_t	nvmf_data_len;

	/*
	 * Whether or not shadow doorbells are used in the source. 0 is a valid DMA
	 * address.
	 */
	uint32_t	sdbl;

	/* Shadow doorbell DMA addresses. */
	uint64_t	shadow_doorbell_buffer;
	uint64_t	eventidx_buffer;

	/* Reserved memory space for new added fields, the
	 * field is always at the end of this data structure.
	 */
	uint8_t		unused[3856];
};
SPDK_STATIC_ASSERT(sizeof(struct vfio_user_nvme_migr_header) == 0x1000, "Incorrect size");

struct vfio_user_nvme_migr_qp {
	struct nvme_migr_sq_state	sq;
	struct nvme_migr_cq_state	cq;
};

/* NVMe state definition used to load/restore from/to NVMe migration BAR region */
struct vfio_user_nvme_migr_state {
	struct vfio_user_nvme_migr_header	ctrlr_header;
	struct spdk_nvmf_ctrlr_migr_data	nvmf_data;
	struct vfio_user_nvme_migr_qp		qps[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];
	uint8_t					doorbells[NVMF_VFIO_USER_DOORBELLS_SIZE];
	uint8_t					cfg[NVME_REG_CFG_SIZE];
};

struct nvmf_vfio_user_req  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			rsp;
	struct spdk_nvme_cmd			cmd;

	enum nvmf_vfio_user_req_state		state;
	nvmf_vfio_user_req_cb_fn		cb_fn;
	void					*cb_arg;

	/* old CC before prop_set_cc fabric command */
	union spdk_nvme_cc_register		cc;

	TAILQ_ENTRY(nvmf_vfio_user_req)		link;

	struct iovec				iov[NVMF_VFIO_USER_MAX_IOVECS];
	uint8_t					iovcnt;

	/* NVMF_VFIO_USER_MAX_IOVECS worth of dma_sg_t. */
	uint8_t					sg[];
};

/*
 * Mapping of an NVMe queue.
 *
 * This holds the information tracking a local process mapping of an NVMe queue
 * shared by the client.
 */
struct nvme_q_mapping {
	/* iov of local process mapping. */
	struct iovec iov;
	/* Stored sg, needed for unmap. */
	dma_sg_t *sg;
	/* Client PRP of queue. */
	uint64_t prp1;
};

enum nvmf_vfio_user_sq_state {
	VFIO_USER_SQ_UNUSED = 0,
	VFIO_USER_SQ_CREATED,
	VFIO_USER_SQ_DELETED,
	VFIO_USER_SQ_ACTIVE,
	VFIO_USER_SQ_INACTIVE
};

enum nvmf_vfio_user_cq_state {
	VFIO_USER_CQ_UNUSED = 0,
	VFIO_USER_CQ_CREATED,
	VFIO_USER_CQ_DELETED,
};

enum nvmf_vfio_user_ctrlr_state {
	VFIO_USER_CTRLR_CREATING = 0,
	VFIO_USER_CTRLR_RUNNING,
	/* Quiesce requested by libvfio-user */
	VFIO_USER_CTRLR_PAUSING,
	/* NVMf subsystem is paused, it's safe to do PCI reset, memory register,
	 * memory unergister, and vfio migration state transition in this state.
	 */
	VFIO_USER_CTRLR_PAUSED,
	/*
	 * Implies that the NVMf subsystem is paused. Device will be unquiesced (PCI
	 * reset, memory register and unregister, controller in destination VM has
	 * been restored).  NVMf subsystem resume has been requested.
	 */
	VFIO_USER_CTRLR_RESUMING,
	/*
	 * Implies that the NVMf subsystem is paused. Both controller in source VM and
	 * destinatiom VM is in this state when doing live migration.
	 */
	VFIO_USER_CTRLR_MIGRATING
};

struct nvmf_vfio_user_sq {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_transport_poll_group	*group;
	struct nvmf_vfio_user_ctrlr		*ctrlr;

	uint32_t				qid;
	/* Number of entries in queue. */
	uint32_t				size;
	struct nvme_q_mapping			mapping;
	enum nvmf_vfio_user_sq_state		sq_state;

	uint32_t				head;
	volatile uint32_t			*dbl_tailp;

	/* Whether a shadow doorbell eventidx needs setting. */
	bool					need_rearm;

	/* multiple SQs can be mapped to the same CQ */
	uint16_t				cqid;

	/* handle_queue_connect_rsp() can be used both for CREATE IO SQ response
	 * and SQ re-connect response in the destination VM, for the prior case,
	 * we will post a NVMe completion to VM, we will not set this flag when
	 * re-connecting SQs in the destination VM.
	 */
	bool					post_create_io_sq_completion;
	/* Copy of Create IO SQ command, this field is used together with
	 * `post_create_io_sq_completion` flag.
	 */
	struct spdk_nvme_cmd			create_io_sq_cmd;

	struct vfio_user_delete_sq_ctx		*delete_ctx;

	/* Currently unallocated reqs. */
	TAILQ_HEAD(, nvmf_vfio_user_req)	free_reqs;
	/* Poll group entry */
	TAILQ_ENTRY(nvmf_vfio_user_sq)		link;
	/* Connected SQ entry */
	TAILQ_ENTRY(nvmf_vfio_user_sq)		tailq;
};

struct nvmf_vfio_user_cq {
	struct spdk_nvmf_transport_poll_group	*group;
	int					cq_ref;

	uint32_t				qid;
	/* Number of entries in queue. */
	uint32_t				size;
	struct nvme_q_mapping			mapping;
	enum nvmf_vfio_user_cq_state		cq_state;

	uint32_t				tail;
	volatile uint32_t			*dbl_headp;

	bool					phase;

	uint16_t				iv;
	bool					ien;

	uint32_t				last_head;
	uint32_t				last_trigger_irq_tail;
};

struct nvmf_vfio_user_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	TAILQ_ENTRY(nvmf_vfio_user_poll_group)	link;
	TAILQ_HEAD(, nvmf_vfio_user_sq)		sqs;
	struct spdk_interrupt			*intr;
	int					intr_fd;
	struct {

		/*
		 * ctrlr_intr and ctrlr_kicks will be zero for all other poll
		 * groups. However, they can be zero even for the poll group
		 * the controller belongs are if no vfio-user message has been
		 * received or the controller hasn't been kicked yet.
		 */

		/*
		 * Number of times vfio_user_ctrlr_intr() has run:
		 * vfio-user file descriptor has been ready or explicitly
		 * kicked (see below).
		 */
		uint64_t ctrlr_intr;

		/*
		 * Kicks to the controller by ctrlr_kick().
		 * ctrlr_intr - ctrlr_kicks is the number of times the
		 * vfio-user poll file descriptor has been ready.
		 */
		uint64_t ctrlr_kicks;

		/*
		 * How many times we won the race arming an SQ.
		 */
		uint64_t won;

		/*
		 * How many times we lost the race arming an SQ
		 */
		uint64_t lost;

		/*
		 * How many requests we processed in total each time we lost
		 * the rearm race.
		 */
		uint64_t lost_count;

		/*
		 * Number of attempts we attempted to rearm all the SQs in the
		 * poll group.
		 */
		uint64_t rearms;

		uint64_t pg_process_count;
		uint64_t intr;
		uint64_t polls;
		uint64_t polls_spurious;
		uint64_t poll_reqs;
		uint64_t poll_reqs_squared;
		uint64_t cqh_admin_writes;
		uint64_t cqh_io_writes;
	} stats;
};

struct nvmf_vfio_user_shadow_doorbells {
	volatile uint32_t			*shadow_doorbells;
	volatile uint32_t			*eventidxs;
	dma_sg_t				*sgs;
	struct iovec				*iovs;
};

struct nvmf_vfio_user_ctrlr {
	struct nvmf_vfio_user_endpoint		*endpoint;
	struct nvmf_vfio_user_transport		*transport;

	/* Connected SQs list */
	TAILQ_HEAD(, nvmf_vfio_user_sq)		connected_sqs;
	enum nvmf_vfio_user_ctrlr_state		state;

	/*
	 * Tells whether live migration data have been prepared. This is used
	 * by the get_pending_bytes callback to tell whether or not the
	 * previous iteration finished.
	 */
	bool migr_data_prepared;

	/* Controller is in source VM when doing live migration */
	bool					in_source_vm;

	struct spdk_thread			*thread;
	struct spdk_poller			*vfu_ctx_poller;
	struct spdk_interrupt			*intr;
	int					intr_fd;

	bool					queued_quiesce;

	bool					reset_shn;
	bool					disconnect;

	uint16_t				cntlid;
	struct spdk_nvmf_ctrlr			*ctrlr;

	struct nvmf_vfio_user_sq		*sqs[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];
	struct nvmf_vfio_user_cq		*cqs[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];

	TAILQ_ENTRY(nvmf_vfio_user_ctrlr)	link;

	volatile uint32_t			*bar0_doorbells;
	struct nvmf_vfio_user_shadow_doorbells	*sdbl;
	/*
	 * Shadow doorbells PRPs to provide during the stop-and-copy state.
	 */
	uint64_t				shadow_doorbell_buffer;
	uint64_t				eventidx_buffer;

	bool					adaptive_irqs_enabled;
};

/* Endpoint in vfio-user is associated with a socket file, which
 * is the representative of a PCI endpoint.
 */
struct nvmf_vfio_user_endpoint {
	struct nvmf_vfio_user_transport		*transport;
	vfu_ctx_t				*vfu_ctx;
	struct spdk_poller			*accept_poller;
	struct spdk_thread			*accept_thread;
	bool					interrupt_mode;
	struct msixcap				*msix;
	vfu_pci_config_space_t			*pci_config_space;
	int					devmem_fd;
	int					accept_intr_fd;
	struct spdk_interrupt			*accept_intr;

	volatile uint32_t			*bar0_doorbells;

	int					migr_fd;
	void					*migr_data;

	struct spdk_nvme_transport_id		trid;
	struct spdk_nvmf_subsystem		*subsystem;

	/* Controller is associated with an active socket connection,
	 * the lifecycle of the controller is same as the VM.
	 * Currently we only support one active connection, as the NVMe
	 * specification defines, we may support multiple controllers in
	 * future, so that it can support e.g: RESERVATION.
	 */
	struct nvmf_vfio_user_ctrlr		*ctrlr;
	pthread_mutex_t				lock;

	bool					need_async_destroy;
	/* The subsystem is in PAUSED state and need to be resumed, TRUE
	 * only when migration is done successfully and the controller is
	 * in source VM.
	 */
	bool					need_resume;
	/* Start the accept poller again after destroying the controller */
	bool					need_relisten;

	TAILQ_ENTRY(nvmf_vfio_user_endpoint)	link;
};

struct nvmf_vfio_user_transport_opts {
	bool					disable_mappable_bar0;
	bool					disable_adaptive_irq;
	bool					disable_shadow_doorbells;
	bool					disable_compare;
	bool					enable_intr_mode_sq_spreading;
};

struct nvmf_vfio_user_transport {
	struct spdk_nvmf_transport		transport;
	struct nvmf_vfio_user_transport_opts    transport_opts;
	bool					intr_mode_supported;
	pthread_mutex_t				lock;
	TAILQ_HEAD(, nvmf_vfio_user_endpoint)	endpoints;

	pthread_mutex_t				pg_lock;
	TAILQ_HEAD(, nvmf_vfio_user_poll_group)	poll_groups;
	struct nvmf_vfio_user_poll_group	*next_pg;
};

/*
 * function prototypes
 */
static int nvmf_vfio_user_req_free(struct spdk_nvmf_request *req);

static struct nvmf_vfio_user_req *get_nvmf_vfio_user_req(struct nvmf_vfio_user_sq *sq);

/*
 * Local process virtual address of a queue.
 */
static inline void *
q_addr(struct nvme_q_mapping *mapping)
{
	return mapping->iov.iov_base;
}

static inline int
queue_index(uint16_t qid, bool is_cq)
{
	return (qid * 2) + is_cq;
}

static inline volatile uint32_t *
sq_headp(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);
	return &sq->head;
}

static inline volatile uint32_t *
sq_dbl_tailp(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);
	return sq->dbl_tailp;
}

static inline volatile uint32_t *
cq_dbl_headp(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);
	return cq->dbl_headp;
}

static inline volatile uint32_t *
cq_tailp(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);
	return &cq->tail;
}

static inline void
sq_head_advance(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);

	assert(*sq_headp(sq) < sq->size);
	(*sq_headp(sq))++;

	if (spdk_unlikely(*sq_headp(sq) == sq->size)) {
		*sq_headp(sq) = 0;
	}
}

static inline void
cq_tail_advance(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);

	assert(*cq_tailp(cq) < cq->size);
	(*cq_tailp(cq))++;

	if (spdk_unlikely(*cq_tailp(cq) == cq->size)) {
		*cq_tailp(cq) = 0;
		cq->phase = !cq->phase;
	}
}

/*
 * As per NVMe Base spec 3.3.1.2.1, we are supposed to implement CQ flow
 * control: if there is no space in the CQ, we should wait until there is.
 *
 * In practice, we just fail the controller instead: as it happens, all host
 * implementations we care about right-size the CQ: this is required anyway for
 * NVMEoF support (see 3.3.2.8).
 *
 * Since reading the head doorbell is relatively expensive, we use the cached
 * value, so we only have to read it for real if it appears that we are full.
 */
static inline bool
cq_is_full(struct nvmf_vfio_user_cq *cq)
{
	uint32_t qindex;

	assert(cq != NULL);

	qindex = *cq_tailp(cq) + 1;
	if (spdk_unlikely(qindex == cq->size)) {
		qindex = 0;
	}

	if (qindex != cq->last_head) {
		return false;
	}

	cq->last_head = *cq_dbl_headp(cq);

	return qindex == cq->last_head;
}

static bool
io_q_exists(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t qid, const bool is_cq)
{
	assert(vu_ctrlr != NULL);

	if (qid == 0 || qid >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		return false;
	}

	if (is_cq) {
		if (vu_ctrlr->cqs[qid] == NULL) {
			return false;
		}

		return (vu_ctrlr->cqs[qid]->cq_state != VFIO_USER_CQ_DELETED &&
			vu_ctrlr->cqs[qid]->cq_state != VFIO_USER_CQ_UNUSED);
	}

	if (vu_ctrlr->sqs[qid] == NULL) {
		return false;
	}

	return (vu_ctrlr->sqs[qid]->sq_state != VFIO_USER_SQ_DELETED &&
		vu_ctrlr->sqs[qid]->sq_state != VFIO_USER_SQ_UNUSED);
}

static char *
endpoint_id(struct nvmf_vfio_user_endpoint *endpoint)
{
	return endpoint->trid.traddr;
}

static char *
ctrlr_id(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	if (!ctrlr || !ctrlr->endpoint) {
		return "Null Ctrlr";
	}

	return endpoint_id(ctrlr->endpoint);
}

/* Return the poll group for the admin queue of the controller. */
static inline struct nvmf_vfio_user_poll_group *
ctrlr_to_poll_group(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	return SPDK_CONTAINEROF(vu_ctrlr->sqs[0]->group,
				struct nvmf_vfio_user_poll_group,
				group);
}

static inline struct spdk_thread *
poll_group_to_thread(struct nvmf_vfio_user_poll_group *vu_pg)
{
	return vu_pg->group.group->thread;
}

static dma_sg_t *
index_to_sg_t(void *arr, size_t i)
{
	return (dma_sg_t *)((uintptr_t)arr + i * dma_sg_size());
}

static inline size_t
vfio_user_migr_data_len(void)
{
	return SPDK_ALIGN_CEIL(sizeof(struct vfio_user_nvme_migr_state), PAGE_SIZE);
}

static inline bool
in_interrupt_mode(struct nvmf_vfio_user_transport *vu_transport)
{
	return spdk_interrupt_mode_is_enabled() &&
	       vu_transport->intr_mode_supported;
}

static int vfio_user_ctrlr_intr(void *ctx);

static void
vfio_user_msg_ctrlr_intr(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx;
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);

	vu_ctrlr_group->stats.ctrlr_kicks++;

	vfio_user_ctrlr_intr(ctx);
}

/*
 * Kick (force a wakeup) of all poll groups for this controller.
 * vfio_user_ctrlr_intr() itself arranges for kicking other poll groups if
 * needed.
 */
static void
ctrlr_kick(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group;

	SPDK_DEBUGLOG(vfio_user_db, "%s: kicked\n", ctrlr_id(vu_ctrlr));

	vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);

	spdk_thread_send_msg(poll_group_to_thread(vu_ctrlr_group),
			     vfio_user_msg_ctrlr_intr, vu_ctrlr);
}

/*
 * Make the given DMA address and length available (locally mapped) via iov.
 */
static void *
map_one(vfu_ctx_t *ctx, uint64_t addr, uint64_t len, dma_sg_t *sg,
	struct iovec *iov, int prot)
{
	int ret;

	assert(ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	ret = vfu_addr_to_sgl(ctx, (void *)(uintptr_t)addr, len, sg, 1, prot);
	if (ret < 0) {
		return NULL;
	}

	ret = vfu_sgl_get(ctx, sg, iov, 1, 0);
	if (ret != 0) {
		return NULL;
	}

	assert(iov->iov_base != NULL);
	return iov->iov_base;
}

static int
nvme_cmd_map_prps(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs,
		  uint32_t max_iovcnt, uint32_t len, size_t mps,
		  void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, int prot))
{
	uint64_t prp1, prp2;
	void *vva;
	uint32_t i;
	uint32_t residue_len, nents;
	uint64_t *prp_list;
	uint32_t iovcnt;

	assert(max_iovcnt > 0);

	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;

	/* PRP1 may started with unaligned page address */
	residue_len = mps - (prp1 % mps);
	residue_len = spdk_min(len, residue_len);

	vva = gpa_to_vva(prv, prp1, residue_len, PROT_READ | PROT_WRITE);
	if (spdk_unlikely(vva == NULL)) {
		SPDK_ERRLOG("GPA to VVA failed\n");
		return -EINVAL;
	}
	len -= residue_len;
	if (len && max_iovcnt < 2) {
		SPDK_ERRLOG("Too many page entries, at least two iovs are required\n");
		return -ERANGE;
	}
	iovs[0].iov_base = vva;
	iovs[0].iov_len = residue_len;

	if (len) {
		if (spdk_unlikely(prp2 == 0)) {
			SPDK_ERRLOG("no PRP2, %d remaining\n", len);
			return -EINVAL;
		}

		if (len <= mps) {
			/* 2 PRP used */
			iovcnt = 2;
			vva = gpa_to_vva(prv, prp2, len, PROT_READ | PROT_WRITE);
			if (spdk_unlikely(vva == NULL)) {
				SPDK_ERRLOG("no VVA for %#" PRIx64 ", len%#x\n",
					    prp2, len);
				return -EINVAL;
			}
			iovs[1].iov_base = vva;
			iovs[1].iov_len = len;
		} else {
			/* PRP list used */
			nents = (len + mps - 1) / mps;
			if (spdk_unlikely(nents + 1 > max_iovcnt)) {
				SPDK_ERRLOG("Too many page entries\n");
				return -ERANGE;
			}

			vva = gpa_to_vva(prv, prp2, nents * sizeof(*prp_list), PROT_READ);
			if (spdk_unlikely(vva == NULL)) {
				SPDK_ERRLOG("no VVA for %#" PRIx64 ", nents=%#x\n",
					    prp2, nents);
				return -EINVAL;
			}
			prp_list = vva;
			i = 0;
			while (len != 0) {
				residue_len = spdk_min(len, mps);
				vva = gpa_to_vva(prv, prp_list[i], residue_len, PROT_READ | PROT_WRITE);
				if (spdk_unlikely(vva == NULL)) {
					SPDK_ERRLOG("no VVA for %#" PRIx64 ", residue_len=%#x\n",
						    prp_list[i], residue_len);
					return -EINVAL;
				}
				iovs[i + 1].iov_base = vva;
				iovs[i + 1].iov_len = residue_len;
				len -= residue_len;
				i++;
			}
			iovcnt = i + 1;
		}
	} else {
		/* 1 PRP used */
		iovcnt = 1;
	}

	assert(iovcnt <= max_iovcnt);
	return iovcnt;
}

static int
nvme_cmd_map_sgls_data(void *prv, struct spdk_nvme_sgl_descriptor *sgls, uint32_t num_sgls,
		       struct iovec *iovs, uint32_t max_iovcnt,
		       void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, int prot))
{
	uint32_t i;
	void *vva;

	if (spdk_unlikely(max_iovcnt < num_sgls)) {
		return -ERANGE;
	}

	for (i = 0; i < num_sgls; i++) {
		if (spdk_unlikely(sgls[i].unkeyed.type != SPDK_NVME_SGL_TYPE_DATA_BLOCK)) {
			SPDK_ERRLOG("Invalid SGL type %u\n", sgls[i].unkeyed.type);
			return -EINVAL;
		}
		vva = gpa_to_vva(prv, sgls[i].address, sgls[i].unkeyed.length, PROT_READ | PROT_WRITE);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}
		iovs[i].iov_base = vva;
		iovs[i].iov_len = sgls[i].unkeyed.length;
	}

	return num_sgls;
}

static int
nvme_cmd_map_sgls(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs, uint32_t max_iovcnt,
		  uint32_t len, size_t mps,
		  void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, int prot))
{
	struct spdk_nvme_sgl_descriptor *sgl, *last_sgl;
	uint32_t num_sgls, seg_len;
	void *vva;
	int ret;
	uint32_t total_iovcnt = 0;

	/* SGL cases */
	sgl = &cmd->dptr.sgl1;

	/* only one SGL segment */
	if (sgl->unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
		assert(max_iovcnt > 0);
		vva = gpa_to_vva(prv, sgl->address, sgl->unkeyed.length, PROT_READ | PROT_WRITE);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}
		iovs[0].iov_base = vva;
		iovs[0].iov_len = sgl->unkeyed.length;
		assert(sgl->unkeyed.length == len);

		return 1;
	}

	for (;;) {
		if (spdk_unlikely((sgl->unkeyed.type != SPDK_NVME_SGL_TYPE_SEGMENT) &&
				  (sgl->unkeyed.type != SPDK_NVME_SGL_TYPE_LAST_SEGMENT))) {
			SPDK_ERRLOG("Invalid SGL type %u\n", sgl->unkeyed.type);
			return -EINVAL;
		}

		seg_len = sgl->unkeyed.length;
		if (spdk_unlikely(seg_len % sizeof(struct spdk_nvme_sgl_descriptor))) {
			SPDK_ERRLOG("Invalid SGL segment len %u\n", seg_len);
			return -EINVAL;
		}

		num_sgls = seg_len / sizeof(struct spdk_nvme_sgl_descriptor);
		vva = gpa_to_vva(prv, sgl->address, sgl->unkeyed.length, PROT_READ);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}

		/* sgl point to the first segment */
		sgl = (struct spdk_nvme_sgl_descriptor *)vva;
		last_sgl = &sgl[num_sgls - 1];

		/* we are done */
		if (last_sgl->unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			/* map whole sgl list */
			ret = nvme_cmd_map_sgls_data(prv, sgl, num_sgls, &iovs[total_iovcnt],
						     max_iovcnt - total_iovcnt, gpa_to_vva);
			if (spdk_unlikely(ret < 0)) {
				return ret;
			}
			total_iovcnt += ret;

			return total_iovcnt;
		}

		if (num_sgls > 1) {
			/* map whole sgl exclude last_sgl */
			ret = nvme_cmd_map_sgls_data(prv, sgl, num_sgls - 1, &iovs[total_iovcnt],
						     max_iovcnt - total_iovcnt, gpa_to_vva);
			if (spdk_unlikely(ret < 0)) {
				return ret;
			}
			total_iovcnt += ret;
		}

		/* move to next level's segments */
		sgl = last_sgl;
	}

	return 0;
}

static int
nvme_map_cmd(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs, uint32_t max_iovcnt,
	     uint32_t len, size_t mps,
	     void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, int prot))
{
	if (cmd->psdt == SPDK_NVME_PSDT_PRP) {
		return nvme_cmd_map_prps(prv, cmd, iovs, max_iovcnt, len, mps, gpa_to_vva);
	}

	return nvme_cmd_map_sgls(prv, cmd, iovs, max_iovcnt, len, mps, gpa_to_vva);
}

/*
 * For each queue, update the location of its doorbell to the correct location:
 * either our own BAR0, or the guest's configured shadow doorbell area.
 *
 * The Admin queue (qid: 0) does not ever use shadow doorbells.
 */
static void
vfio_user_ctrlr_switch_doorbells(struct nvmf_vfio_user_ctrlr *ctrlr, bool shadow)
{
	volatile uint32_t *doorbells = shadow ? ctrlr->sdbl->shadow_doorbells :
				       ctrlr->bar0_doorbells;

	assert(doorbells != NULL);

	for (size_t i = 1; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; i++) {
		struct nvmf_vfio_user_sq *sq = ctrlr->sqs[i];
		struct nvmf_vfio_user_cq *cq = ctrlr->cqs[i];

		if (sq != NULL) {
			sq->dbl_tailp = doorbells + queue_index(sq->qid, false);

			ctrlr->sqs[i]->need_rearm = shadow;
		}

		if (cq != NULL) {
			cq->dbl_headp = doorbells + queue_index(cq->qid, true);
		}
	}
}

static void
unmap_sdbl(vfu_ctx_t *vfu_ctx, struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	assert(vfu_ctx != NULL);
	assert(sdbl != NULL);

	/*
	 * An allocation error would result in only one of the two being
	 * non-NULL.  If that is the case, no memory should have been mapped.
	 */
	if (sdbl->iovs == NULL || sdbl->sgs == NULL) {
		return;
	}

	for (size_t i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT; ++i) {
		struct iovec *iov;
		dma_sg_t *sg;

		if (!sdbl->iovs[i].iov_len) {
			continue;
		}

		sg = index_to_sg_t(sdbl->sgs, i);
		iov = sdbl->iovs + i;

		vfu_sgl_put(vfu_ctx, sg, iov, 1);
	}
}

static void
free_sdbl(vfu_ctx_t *vfu_ctx, struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	if (sdbl == NULL) {
		return;
	}

	unmap_sdbl(vfu_ctx, sdbl);

	/*
	 * sdbl->shadow_doorbells and sdbl->eventidxs were mapped,
	 * not allocated, so don't free() them.
	 */
	free(sdbl->sgs);
	free(sdbl->iovs);
	free(sdbl);
}

static struct nvmf_vfio_user_shadow_doorbells *
map_sdbl(vfu_ctx_t *vfu_ctx, uint64_t prp1, uint64_t prp2, size_t len)
{
	struct nvmf_vfio_user_shadow_doorbells *sdbl = NULL;
	dma_sg_t *sg2 = NULL;
	void *p;

	assert(vfu_ctx != NULL);

	sdbl = calloc(1, sizeof(*sdbl));
	if (sdbl == NULL) {
		goto err;
	}

	sdbl->sgs = calloc(NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT, dma_sg_size());
	sdbl->iovs = calloc(NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT, sizeof(*sdbl->iovs));
	if (sdbl->sgs == NULL || sdbl->iovs == NULL) {
		goto err;
	}

	/* Map shadow doorbell buffer (PRP1). */
	p = map_one(vfu_ctx, prp1, len, sdbl->sgs, sdbl->iovs,
		    PROT_READ | PROT_WRITE);

	if (p == NULL) {
		goto err;
	}

	/*
	 * Map eventidx buffer (PRP2).
	 * Should only be written to by the controller.
	 */

	sg2 = index_to_sg_t(sdbl->sgs, 1);

	p = map_one(vfu_ctx, prp2, len, sg2, sdbl->iovs + 1,
		    PROT_READ | PROT_WRITE);

	if (p == NULL) {
		goto err;
	}

	sdbl->shadow_doorbells = (uint32_t *)sdbl->iovs[0].iov_base;
	sdbl->eventidxs = (uint32_t *)sdbl->iovs[1].iov_base;

	return sdbl;

err:
	free_sdbl(vfu_ctx, sdbl);
	return NULL;
}

/*
 * Copy doorbells from one buffer to the other, during switches betweeen BAR0
 * doorbells and shadow doorbells.
 */
static void
copy_doorbells(struct nvmf_vfio_user_ctrlr *ctrlr,
	       const volatile uint32_t *from, volatile uint32_t *to)
{
	assert(ctrlr != NULL);
	assert(from != NULL);
	assert(to != NULL);

	SPDK_DEBUGLOG(vfio_user_db,
		      "%s: migrating shadow doorbells from %p to %p\n",
		      ctrlr_id(ctrlr), from, to);

	/* Can't use memcpy because it doesn't respect volatile semantics. */
	for (size_t i = 0; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; ++i) {
		if (ctrlr->sqs[i] != NULL) {
			to[queue_index(i, false)] = from[queue_index(i, false)];
		}

		if (ctrlr->cqs[i] != NULL) {
			to[queue_index(i, true)] = from[queue_index(i, true)];
		}
	}
}

static void
fail_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	const struct spdk_nvmf_registers *regs;

	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	regs = spdk_nvmf_ctrlr_get_regs(vu_ctrlr->ctrlr);
	if (regs->csts.bits.cfs == 0) {
		SPDK_ERRLOG(":%s failing controller\n", ctrlr_id(vu_ctrlr));
	}

	nvmf_ctrlr_set_fatal_status(vu_ctrlr->ctrlr);
}

static inline bool
ctrlr_interrupt_enabled(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->endpoint != NULL);

	vfu_pci_config_space_t *pci = vu_ctrlr->endpoint->pci_config_space;

	return (!pci->hdr.cmd.id || vu_ctrlr->endpoint->msix->mxc.mxe);
}

static void
nvmf_vfio_user_destroy_endpoint(struct nvmf_vfio_user_endpoint *endpoint)
{
	SPDK_DEBUGLOG(nvmf_vfio, "destroy endpoint %s\n", endpoint_id(endpoint));

	spdk_interrupt_unregister(&endpoint->accept_intr);
	spdk_poller_unregister(&endpoint->accept_poller);

	if (endpoint->bar0_doorbells) {
		munmap((void *)endpoint->bar0_doorbells, NVMF_VFIO_USER_DOORBELLS_SIZE);
	}

	if (endpoint->devmem_fd > 0) {
		close(endpoint->devmem_fd);
	}

	if (endpoint->migr_data) {
		munmap(endpoint->migr_data, vfio_user_migr_data_len());
	}

	if (endpoint->migr_fd > 0) {
		close(endpoint->migr_fd);
	}

	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);
	}

	pthread_mutex_destroy(&endpoint->lock);
	free(endpoint);
}

/* called when process exits */
static int
nvmf_vfio_user_destroy(struct spdk_nvmf_transport *transport,
		       spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;

	SPDK_DEBUGLOG(nvmf_vfio, "destroy transport\n");

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_destroy(&vu_transport->lock);
	pthread_mutex_destroy(&vu_transport->pg_lock);

	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		TAILQ_REMOVE(&vu_transport->endpoints, endpoint, link);
		nvmf_vfio_user_destroy_endpoint(endpoint);
	}

	free(vu_transport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}

	return 0;
}

static const struct spdk_json_object_decoder vfio_user_transport_opts_decoder[] = {
	{
		"disable_mappable_bar0",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_mappable_bar0),
		spdk_json_decode_bool, true
	},
	{
		"disable_adaptive_irq",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_adaptive_irq),
		spdk_json_decode_bool, true
	},
	{
		"disable_shadow_doorbells",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_shadow_doorbells),
		spdk_json_decode_bool, true
	},
	{
		"disable_compare",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_compare),
		spdk_json_decode_bool, true
	},
	{
		"enable_intr_mode_sq_spreading",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.enable_intr_mode_sq_spreading),
		spdk_json_decode_bool, true
	},
};

static struct spdk_nvmf_transport *
nvmf_vfio_user_create(struct spdk_nvmf_transport_opts *opts)
{
	struct nvmf_vfio_user_transport *vu_transport;
	int err;

	if (opts->max_qpairs_per_ctrlr > NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		SPDK_ERRLOG("Invalid max_qpairs_per_ctrlr=%d, supported max_qpairs_per_ctrlr=%d\n",
			    opts->max_qpairs_per_ctrlr, NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR);
		return NULL;
	}

	vu_transport = calloc(1, sizeof(*vu_transport));
	if (vu_transport == NULL) {
		SPDK_ERRLOG("Transport alloc fail: %m\n");
		return NULL;
	}

	err = pthread_mutex_init(&vu_transport->lock, NULL);
	if (err != 0) {
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;
	}
	TAILQ_INIT(&vu_transport->endpoints);

	err = pthread_mutex_init(&vu_transport->pg_lock, NULL);
	if (err != 0) {
		pthread_mutex_destroy(&vu_transport->lock);
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;
	}
	TAILQ_INIT(&vu_transport->poll_groups);

	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, vfio_user_transport_opts_decoder,
					    SPDK_COUNTOF(vfio_user_transport_opts_decoder),
					    vu_transport)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		goto cleanup;
	}

	/*
	 * To support interrupt mode, the transport must be configured with
	 * mappable BAR0 disabled: we need a vfio-user message to wake us up
	 * when a client writes new doorbell values to BAR0, via the
	 * libvfio-user socket fd.
	 */
	vu_transport->intr_mode_supported =
		vu_transport->transport_opts.disable_mappable_bar0;

	/*
	 * If BAR0 is mappable, it doesn't make sense to support shadow
	 * doorbells, so explicitly turn it off.
	 */
	if (!vu_transport->transport_opts.disable_mappable_bar0) {
		vu_transport->transport_opts.disable_shadow_doorbells = true;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		if (!vu_transport->intr_mode_supported) {
			SPDK_ERRLOG("interrupt mode not supported\n");
			goto cleanup;
		}

		/*
		 * If we are in interrupt mode, we cannot support adaptive IRQs,
		 * as there is no guarantee the SQ poller will run subsequently
		 * to send pending IRQs.
		 */
		vu_transport->transport_opts.disable_adaptive_irq = true;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_mappable_bar0=%d\n",
		      vu_transport->transport_opts.disable_mappable_bar0);
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_adaptive_irq=%d\n",
		      vu_transport->transport_opts.disable_adaptive_irq);
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_shadow_doorbells=%d\n",
		      vu_transport->transport_opts.disable_shadow_doorbells);

	return &vu_transport->transport;

cleanup:
	pthread_mutex_destroy(&vu_transport->lock);
	pthread_mutex_destroy(&vu_transport->pg_lock);
err:
	free(vu_transport);
	return NULL;
}

static uint32_t
max_queue_size(struct nvmf_vfio_user_ctrlr const *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	return vu_ctrlr->ctrlr->vcprop.cap.bits.mqes + 1;
}

static uint32_t
doorbell_stride(const struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	return vu_ctrlr->ctrlr->vcprop.cap.bits.dstrd;
}

static uintptr_t
memory_page_size(const struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	uint32_t memory_page_shift = vu_ctrlr->ctrlr->vcprop.cc.bits.mps + 12;
	return 1ul << memory_page_shift;
}

static uintptr_t
memory_page_mask(const struct nvmf_vfio_user_ctrlr *ctrlr)
{
	return ~(memory_page_size(ctrlr) - 1);
}

static int
map_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q_mapping *mapping,
      uint32_t q_size, bool is_cq, bool unmap)
{
	uint64_t len;
	void *ret;

	assert(q_size);
	assert(q_addr(mapping) == NULL);

	if (is_cq) {
		len = q_size * sizeof(struct spdk_nvme_cpl);
	} else {
		len = q_size * sizeof(struct spdk_nvme_cmd);
	}

	ret = map_one(vu_ctrlr->endpoint->vfu_ctx, mapping->prp1, len,
		      mapping->sg, &mapping->iov,
		      is_cq ? PROT_READ | PROT_WRITE : PROT_READ);
	if (ret == NULL) {
		return -EFAULT;
	}

	if (unmap) {
		memset(q_addr(mapping), 0, len);
	}

	return 0;
}

static inline void
unmap_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q_mapping *mapping)
{
	if (q_addr(mapping) != NULL) {
		vfu_sgl_put(vu_ctrlr->endpoint->vfu_ctx, mapping->sg,
			    &mapping->iov, 1);
		mapping->iov.iov_base = NULL;
	}
}

static int
asq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_sq *sq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);

	sq = ctrlr->sqs[0];

	assert(sq != NULL);
	assert(q_addr(&sq->mapping) == NULL);
	/* XXX ctrlr->asq == 0 is a valid memory address */

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->ctrlr);
	sq->qid = 0;
	sq->size = regs->aqa.bits.asqs + 1;
	sq->mapping.prp1 = regs->asq;
	*sq_headp(sq) = 0;
	sq->cqid = 0;

	ret = map_q(ctrlr, &sq->mapping, sq->size, false, true);
	if (ret) {
		return ret;
	}

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */
	sq->dbl_tailp = ctrlr->bar0_doorbells + queue_index(0, false);

	*sq_dbl_tailp(sq) = 0;

	return 0;
}

/*
 * Updates eventidx to set an SQ into interrupt or polling mode.
 *
 * Returns false if the current SQ tail does not match the SQ head, as
 * this means that the host has submitted more items to the queue while we were
 * not looking - or during the event index update. In that case, we must retry,
 * or otherwise make sure we are going to wake up again.
 */
static bool
set_sq_eventidx(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	volatile uint32_t *sq_tail_eidx;
	uint32_t old_tail, new_tail;

	assert(sq != NULL);
	assert(sq->ctrlr != NULL);
	assert(sq->ctrlr->sdbl != NULL);
	assert(sq->need_rearm);
	assert(sq->qid != 0);

	ctrlr = sq->ctrlr;

	SPDK_DEBUGLOG(vfio_user_db, "%s: updating eventidx of sqid:%u\n",
		      ctrlr_id(ctrlr), sq->qid);

	sq_tail_eidx = ctrlr->sdbl->eventidxs + queue_index(sq->qid, false);

	assert(ctrlr->endpoint != NULL);

	if (!ctrlr->endpoint->interrupt_mode) {
		/* No synchronisation necessary. */
		*sq_tail_eidx = NVMF_VFIO_USER_EVENTIDX_POLL;
		return true;
	}

	old_tail = *sq_dbl_tailp(sq);
	*sq_tail_eidx = old_tail;

	/*
	 * Ensure that the event index is updated before re-reading the tail
	 * doorbell. If it's not, then the host might race us and update the
	 * tail after the second read but before the event index is written, so
	 * it won't write to BAR0 and we'll miss the update.
	 *
	 * The driver should provide similar ordering with an mb().
	 */
	spdk_mb();

	/*
	 * Check if the host has updated the tail doorbell after we've read it
	 * for the first time, but before the event index was written. If that's
	 * the case, then we've lost the race and we need to update the event
	 * index again (after polling the queue, since the host won't write to
	 * BAR0).
	 */
	new_tail = *sq_dbl_tailp(sq);

	/*
	 * We might poll the queue straight after this function returns if the
	 * tail has been updated, so we need to ensure that any changes to the
	 * queue will be visible to us if the doorbell has been updated.
	 *
	 * The driver should provide similar ordering with a wmb() to ensure
	 * that the queue is written before it updates the tail doorbell.
	 */
	spdk_rmb();

	SPDK_DEBUGLOG(vfio_user_db, "%s: sqid:%u, old_tail=%u, new_tail=%u, "
		      "sq_head=%u\n", ctrlr_id(ctrlr), sq->qid, old_tail,
		      new_tail, *sq_headp(sq));

	if (new_tail == *sq_headp(sq)) {
		sq->need_rearm = false;
		return true;
	}

	/*
	 * We've lost the race: the tail was updated since we last polled,
	 * including if it happened within this routine.
	 *
	 * The caller should retry after polling (think of this as a cmpxchg
	 * loop); if we go to sleep while the SQ is not empty, then we won't
	 * process the remaining events.
	 */
	return false;
}

static int nvmf_vfio_user_sq_poll(struct nvmf_vfio_user_sq *sq);

/*
 * Arrange for an SQ to interrupt us if written. Returns non-zero if we
 * processed some SQ entries.
 */
static int
vfio_user_sq_rearm(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct nvmf_vfio_user_sq *sq,
		   struct nvmf_vfio_user_poll_group *vu_group)
{
	int count = 0;
	size_t i;

	assert(sq->need_rearm);

	for (i = 0; i < NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS; i++) {
		int ret;

		if (set_sq_eventidx(sq)) {
			/* We won the race and set eventidx; done. */
			vu_group->stats.won++;
			return count;
		}

		ret = nvmf_vfio_user_sq_poll(sq);

		count += (ret < 0) ? 1 : ret;

		/*
		 * set_sq_eventidx() hit the race, so we expected
		 * to process at least one command from this queue.
		 * If there were no new commands waiting for us, then
		 * we must have hit an unexpected race condition.
		 */
		if (ret == 0) {
			SPDK_ERRLOG("%s: unexpected race condition detected "
				    "while updating the shadow doorbell buffer\n",
				    ctrlr_id(ctrlr));

			fail_ctrlr(ctrlr);
			return count;
		}
	}

	SPDK_DEBUGLOG(vfio_user_db,
		      "%s: set_sq_eventidx() lost the race %zu times\n",
		      ctrlr_id(ctrlr), i);

	vu_group->stats.lost++;
	vu_group->stats.lost_count += count;

	/*
	 * We couldn't arrange an eventidx guaranteed to cause a BAR0 write, as
	 * we raced with the producer too many times; force ourselves to wake up
	 * instead. We'll process all queues at that point.
	 */
	ctrlr_kick(ctrlr);

	return count;
}

/*
 * We're in interrupt mode, and potentially about to go to sleep. We need to
 * make sure any further I/O submissions are guaranteed to wake us up: for
 * shadow doorbells that means we may need to go through set_sq_eventidx() for
 * every SQ that needs re-arming.
 *
 * Returns non-zero if we processed something.
 */
static int
vfio_user_poll_group_rearm(struct nvmf_vfio_user_poll_group *vu_group)
{
	struct nvmf_vfio_user_sq *sq;
	int count = 0;

	vu_group->stats.rearms++;

	TAILQ_FOREACH(sq, &vu_group->sqs, link) {
		if (spdk_unlikely(sq->sq_state != VFIO_USER_SQ_ACTIVE || !sq->size)) {
			continue;
		}

		if (sq->need_rearm) {
			count += vfio_user_sq_rearm(sq->ctrlr, sq, vu_group);
		}
	}

	return count;
}

static int
acq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_cq *cq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);

	cq = ctrlr->cqs[0];

	assert(cq != NULL);

	assert(q_addr(&cq->mapping) == NULL);

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->ctrlr);
	assert(regs != NULL);
	cq->qid = 0;
	cq->size = regs->aqa.bits.acqs + 1;
	cq->mapping.prp1 = regs->acq;
	*cq_tailp(cq) = 0;
	cq->ien = true;
	cq->phase = true;

	ret = map_q(ctrlr, &cq->mapping, cq->size, true, true);
	if (ret) {
		return ret;
	}

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */
	cq->dbl_headp = ctrlr->bar0_doorbells + queue_index(0, true);

	*cq_dbl_headp(cq) = 0;

	return 0;
}

static void *
_map_one(void *prv, uint64_t addr, uint64_t len, int prot)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)prv;
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_sq *sq;
	void *ret;

	assert(req != NULL);
	qpair = req->qpair;
	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);

	assert(vu_req->iovcnt < NVMF_VFIO_USER_MAX_IOVECS);
	ret = map_one(sq->ctrlr->endpoint->vfu_ctx, addr, len,
		      index_to_sg_t(vu_req->sg, vu_req->iovcnt),
		      &vu_req->iov[vu_req->iovcnt], prot);
	if (spdk_likely(ret != NULL)) {
		vu_req->iovcnt++;
	}
	return ret;
}

static int
vfio_user_map_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req,
		  struct iovec *iov, uint32_t length)
{
	/* Map PRP list to from Guest physical memory to
	 * virtual memory address.
	 */
	return nvme_map_cmd(req, &req->cmd->nvme_cmd, iov, NVMF_REQ_MAX_BUFFERS,
			    length, 4096, _map_one);
}

static int handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
			  struct nvmf_vfio_user_sq *sq);

/*
 * Posts a CQE in the completion queue.
 *
 * @ctrlr: the vfio-user controller
 * @cq: the completion queue
 * @cdw0: cdw0 as reported by NVMf
 * @sqid: submission queue ID
 * @cid: command identifier in NVMe command
 * @sc: the NVMe CQE status code
 * @sct: the NVMe CQE status code type
 */
static int
post_completion(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq,
		uint32_t cdw0, uint16_t sqid, uint16_t cid, uint16_t sc, uint16_t sct)
{
	struct spdk_nvme_status cpl_status = { 0 };
	struct spdk_nvme_cpl *cpl;
	int err;

	assert(ctrlr != NULL);

	if (spdk_unlikely(cq == NULL || q_addr(&cq->mapping) == NULL)) {
		return 0;
	}

	if (cq->qid == 0) {
		assert(spdk_get_thread() == cq->group->group->thread);
	}

	if (cq_is_full(cq)) {
		SPDK_ERRLOG("%s: cqid:%d full (tail=%d, head=%d)\n",
			    ctrlr_id(ctrlr), cq->qid, *cq_tailp(cq),
			    *cq_dbl_headp(cq));
		return -1;
	}

	cpl = ((struct spdk_nvme_cpl *)q_addr(&cq->mapping)) + *cq_tailp(cq);

	assert(ctrlr->sqs[sqid] != NULL);
	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: request complete sqid:%d cid=%d status=%#x "
		      "sqhead=%d cq tail=%d\n", ctrlr_id(ctrlr), sqid, cid, sc,
		      *sq_headp(ctrlr->sqs[sqid]), *cq_tailp(cq));

	cpl->sqhd = *sq_headp(ctrlr->sqs[sqid]);
	cpl->sqid = sqid;
	cpl->cid = cid;
	cpl->cdw0 = cdw0;

	/*
	 * This is a bitfield: instead of setting the individual bits we need
	 * directly in cpl->status, which would cause a read-modify-write cycle,
	 * we'll avoid reading from the CPL altogether by filling in a local
	 * cpl_status variable, then writing the whole thing.
	 */
	cpl_status.sct = sct;
	cpl_status.sc = sc;
	cpl_status.p = cq->phase;
	cpl->status = cpl_status;

	/* Ensure the Completion Queue Entry is visible. */
	spdk_wmb();
	cq_tail_advance(cq);

	if ((cq->qid == 0 || !ctrlr->adaptive_irqs_enabled) &&
	    cq->ien && ctrlr_interrupt_enabled(ctrlr)) {
		err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
			return err;
		}
	}

	return 0;
}

static void
free_sq_reqs(struct nvmf_vfio_user_sq *sq)
{
	while (!TAILQ_EMPTY(&sq->free_reqs)) {
		struct nvmf_vfio_user_req *vu_req = TAILQ_FIRST(&sq->free_reqs);
		TAILQ_REMOVE(&sq->free_reqs, vu_req, link);
		free(vu_req);
	}
}

static void
delete_cq_done(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq)
{
	assert(cq->cq_ref == 0);
	unmap_q(ctrlr, &cq->mapping);
	cq->size = 0;
	cq->cq_state = VFIO_USER_CQ_DELETED;
	cq->group = NULL;
}

/* Deletes a SQ, if this SQ is the last user of the associated CQ
 * and the controller is being shut down/reset or vfio-user client disconnects,
 * then the CQ is also deleted.
 */
static void
delete_sq_done(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_cq *cq;
	uint16_t cqid;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete sqid:%d=%p done\n", ctrlr_id(vu_ctrlr),
		      sq->qid, sq);

	/* Free SQ resources */
	unmap_q(vu_ctrlr, &sq->mapping);

	free_sq_reqs(sq);

	sq->size = 0;

	sq->sq_state = VFIO_USER_SQ_DELETED;

	/* Controller RESET and SHUTDOWN are special cases,
	 * VM may not send DELETE IO SQ/CQ commands, NVMf library
	 * will disconnect IO queue pairs.
	 */
	if (vu_ctrlr->reset_shn || vu_ctrlr->disconnect) {
		cqid = sq->cqid;
		cq = vu_ctrlr->cqs[cqid];

		SPDK_DEBUGLOG(nvmf_vfio, "%s: try to delete cqid:%u=%p\n", ctrlr_id(vu_ctrlr),
			      cq->qid, cq);

		assert(cq->cq_ref > 0);
		if (--cq->cq_ref == 0) {
			delete_cq_done(vu_ctrlr, cq);
		}
	}
}

static void
free_qp(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	if (ctrlr == NULL) {
		return;
	}

	sq = ctrlr->sqs[qid];
	if (sq) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: Free sqid:%u\n", ctrlr_id(ctrlr), qid);
		unmap_q(ctrlr, &sq->mapping);

		free_sq_reqs(sq);

		free(sq->mapping.sg);
		free(sq);
		ctrlr->sqs[qid] = NULL;
	}

	cq = ctrlr->cqs[qid];
	if (cq) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: Free cqid:%u\n", ctrlr_id(ctrlr), qid);
		unmap_q(ctrlr, &cq->mapping);
		free(cq->mapping.sg);
		free(cq);
		ctrlr->cqs[qid] = NULL;
	}
}

static int
init_sq(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_transport *transport,
	const uint16_t id)
{
	struct nvmf_vfio_user_sq *sq;

	assert(ctrlr != NULL);
	assert(transport != NULL);
	assert(ctrlr->sqs[id] == NULL);

	sq = calloc(1, sizeof(*sq));
	if (sq == NULL) {
		return -ENOMEM;
	}
	sq->mapping.sg = calloc(1, dma_sg_size());
	if (sq->mapping.sg == NULL) {
		free(sq);
		return -ENOMEM;
	}

	sq->qid = id;
	sq->qpair.qid = id;
	sq->qpair.transport = transport;
	sq->ctrlr = ctrlr;
	ctrlr->sqs[id] = sq;

	TAILQ_INIT(&sq->free_reqs);

	return 0;
}

static int
init_cq(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t id)
{
	struct nvmf_vfio_user_cq *cq;

	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->cqs[id] == NULL);

	cq = calloc(1, sizeof(*cq));
	if (cq == NULL) {
		return -ENOMEM;
	}
	cq->mapping.sg = calloc(1, dma_sg_size());
	if (cq->mapping.sg == NULL) {
		free(cq);
		return -ENOMEM;
	}

	cq->qid = id;
	vu_ctrlr->cqs[id] = cq;

	return 0;
}

static int
alloc_sq_reqs(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_req *vu_req, *tmp;
	size_t req_size;
	uint32_t i;

	req_size = sizeof(struct nvmf_vfio_user_req) +
		   (dma_sg_size() * NVMF_VFIO_USER_MAX_IOVECS);

	for (i = 0; i < sq->size; i++) {
		struct spdk_nvmf_request *req;

		vu_req = calloc(1, req_size);
		if (vu_req == NULL) {
			goto err;
		}

		req = &vu_req->req;
		req->qpair = &sq->qpair;
		req->rsp = (union nvmf_c2h_msg *)&vu_req->rsp;
		req->cmd = (union nvmf_h2c_msg *)&vu_req->cmd;
		req->stripped_data = NULL;

		TAILQ_INSERT_TAIL(&sq->free_reqs, vu_req, link);
	}

	return 0;

err:
	TAILQ_FOREACH_SAFE(vu_req, &sq->free_reqs, link, tmp) {
		free(vu_req);
	}
	return -ENOMEM;
}

static volatile uint32_t *
ctrlr_doorbell_ptr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	return ctrlr->sdbl != NULL ?
	       ctrlr->sdbl->shadow_doorbells :
	       ctrlr->bar0_doorbells;
}

static uint16_t
handle_create_io_sq(struct nvmf_vfio_user_ctrlr *ctrlr,
		    struct spdk_nvme_cmd *cmd, uint16_t *sct)
{
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;
	struct nvmf_vfio_user_sq *sq;
	uint32_t qsize;
	uint16_t cqid;
	uint16_t qid;
	int err;

	qid = cmd->cdw10_bits.create_io_q.qid;
	cqid = cmd->cdw11_bits.create_io_sq.cqid;
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	if (ctrlr->sqs[qid] == NULL) {
		err = init_sq(ctrlr, ctrlr->sqs[0]->qpair.transport, qid);
		if (err != 0) {
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (cqid == 0 || cqid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid cqid:%u\n", ctrlr_id(ctrlr), cqid);
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
	}

	/* CQ must be created before SQ. */
	if (!io_q_exists(ctrlr, cqid, true)) {
		SPDK_ERRLOG("%s: cqid:%u does not exist\n", ctrlr_id(ctrlr), cqid);
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_COMPLETION_QUEUE_INVALID;
	}

	if (cmd->cdw11_bits.create_io_sq.pc != 0x1) {
		SPDK_ERRLOG("%s: non-PC SQ not supported\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	sq = ctrlr->sqs[qid];
	sq->size = qsize;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: sqid:%d cqid:%d\n", ctrlr_id(ctrlr),
		      qid, cqid);

	sq->mapping.prp1 = cmd->dptr.prp.prp1;

	err = map_q(ctrlr, &sq->mapping, sq->size, false, true);
	if (err) {
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped sqid:%d IOVA=%#lx vaddr=%p\n",
		      ctrlr_id(ctrlr), qid, cmd->dptr.prp.prp1,
		      q_addr(&sq->mapping));

	err = alloc_sq_reqs(ctrlr, sq);
	if (err < 0) {
		SPDK_ERRLOG("%s: failed to allocate SQ requests: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	sq->cqid = cqid;
	ctrlr->cqs[sq->cqid]->cq_ref++;
	sq->sq_state = VFIO_USER_SQ_CREATED;
	*sq_headp(sq) = 0;

	sq->dbl_tailp = ctrlr_doorbell_ptr(ctrlr) + queue_index(qid, false);

	/*
	 * We should always reset the doorbells.
	 *
	 * The Specification prohibits the controller from writing to the shadow
	 * doorbell buffer, however older versions of the Linux NVMe driver
	 * don't reset the shadow doorbell buffer after a Queue-Level or
	 * Controller-Level reset, which means that we're left with garbage
	 * doorbell values.
	 */
	*sq_dbl_tailp(sq) = 0;

	if (ctrlr->sdbl != NULL) {
		sq->need_rearm = true;

		if (!set_sq_eventidx(sq)) {
			SPDK_ERRLOG("%s: host updated SQ tail doorbell before "
				    "sqid:%hu was initialized\n",
				    ctrlr_id(ctrlr), qid);
			fail_ctrlr(ctrlr);
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	/*
	 * Create our new I/O qpair. This asynchronously invokes, on a suitable
	 * poll group, the nvmf_vfio_user_poll_group_add() callback, which will
	 * call spdk_nvmf_request_exec_fabrics() with a generated fabrics
	 * connect command. This command is then eventually completed via
	 * handle_queue_connect_rsp().
	 */
	sq->create_io_sq_cmd = *cmd;
	sq->post_create_io_sq_completion = true;

	spdk_nvmf_tgt_new_qpair(ctrlr->transport->transport.tgt,
				&sq->qpair);

	*sct = SPDK_NVME_SCT_GENERIC;
	return SPDK_NVME_SC_SUCCESS;
}

static uint16_t
handle_create_io_cq(struct nvmf_vfio_user_ctrlr *ctrlr,
		    struct spdk_nvme_cmd *cmd, uint16_t *sct)
{
	struct nvmf_vfio_user_cq *cq;
	uint32_t qsize;
	uint16_t qid;
	int err;

	qid = cmd->cdw10_bits.create_io_q.qid;
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	if (ctrlr->cqs[qid] == NULL) {
		err = init_cq(ctrlr, qid);
		if (err != 0) {
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (cmd->cdw11_bits.create_io_cq.pc != 0x1) {
		SPDK_ERRLOG("%s: non-PC CQ not supported\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	if (cmd->cdw11_bits.create_io_cq.iv > NVME_IRQ_MSIX_NUM - 1) {
		SPDK_ERRLOG("%s: IV is too big\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR;
	}

	cq = ctrlr->cqs[qid];
	cq->size = qsize;

	cq->mapping.prp1 = cmd->dptr.prp.prp1;

	cq->dbl_headp = ctrlr_doorbell_ptr(ctrlr) + queue_index(qid, true);

	err = map_q(ctrlr, &cq->mapping, cq->size, true, true);
	if (err) {
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped cqid:%u IOVA=%#lx vaddr=%p\n",
		      ctrlr_id(ctrlr), qid, cmd->dptr.prp.prp1,
		      q_addr(&cq->mapping));

	cq->ien = cmd->cdw11_bits.create_io_cq.ien;
	cq->iv = cmd->cdw11_bits.create_io_cq.iv;
	cq->phase = true;
	cq->cq_state = VFIO_USER_CQ_CREATED;

	*cq_tailp(cq) = 0;

	/*
	 * We should always reset the doorbells.
	 *
	 * The Specification prohibits the controller from writing to the shadow
	 * doorbell buffer, however older versions of the Linux NVMe driver
	 * don't reset the shadow doorbell buffer after a Queue-Level or
	 * Controller-Level reset, which means that we're left with garbage
	 * doorbell values.
	 */
	*cq_dbl_headp(cq) = 0;

	*sct = SPDK_NVME_SCT_GENERIC;
	return SPDK_NVME_SC_SUCCESS;
}

/*
 * Creates a completion or submission I/O queue. Returns 0 on success, -errno
 * on error.
 */
static int
handle_create_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	uint32_t qsize;
	uint16_t qid;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	qid = cmd->cdw10_bits.create_io_q.qid;
	if (qid == 0 || qid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid qid=%d, max=%d\n", ctrlr_id(ctrlr),
			    qid, vu_transport->transport.opts.max_qpairs_per_ctrlr);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (io_q_exists(ctrlr, qid, is_cq)) {
		SPDK_ERRLOG("%s: %cqid:%d already exists\n", ctrlr_id(ctrlr),
			    is_cq ? 'c' : 's', qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;
	if (qsize == 1 || qsize > max_queue_size(ctrlr)) {
		SPDK_ERRLOG("%s: invalid I/O queue size %u\n", ctrlr_id(ctrlr), qsize);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_SIZE;
		goto out;
	}

	if (is_cq) {
		sc = handle_create_io_cq(ctrlr, cmd, &sct);
	} else {
		sc = handle_create_io_sq(ctrlr, cmd, &sct);

		if (sct == SPDK_NVME_SCT_GENERIC &&
		    sc == SPDK_NVME_SC_SUCCESS) {
			/* Completion posted asynchronously. */
			return 0;
		}
	}

out:
	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/* For ADMIN I/O DELETE SUBMISSION QUEUE the NVMf library will disconnect and free
 * queue pair, so save the command id and controller in a context.
 */
struct vfio_user_delete_sq_ctx {
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	uint16_t cid;
};

static void
vfio_user_qpair_delete_cb(void *cb_arg)
{
	struct vfio_user_delete_sq_ctx *ctx = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx->vu_ctrlr;
	struct nvmf_vfio_user_cq *admin_cq = vu_ctrlr->cqs[0];

	assert(admin_cq != NULL);
	assert(admin_cq->group != NULL);
	assert(admin_cq->group->group->thread != NULL);
	if (admin_cq->group->group->thread != spdk_get_thread()) {
		spdk_thread_send_msg(admin_cq->group->group->thread,
				     vfio_user_qpair_delete_cb,
				     cb_arg);
	} else {
		post_completion(vu_ctrlr, admin_cq, 0, 0,
				ctx->cid,
				SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
		free(ctx);
	}
}

/*
 * Deletes a completion or submission I/O queue.
 */
static int
handle_del_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete I/O %cqid:%d\n",
		      ctrlr_id(ctrlr), is_cq ? 'c' : 's',
		      cmd->cdw10_bits.delete_io_q.qid);

	if (!io_q_exists(ctrlr, cmd->cdw10_bits.delete_io_q.qid, is_cq)) {
		SPDK_ERRLOG("%s: I/O %cqid:%d does not exist\n", ctrlr_id(ctrlr),
			    is_cq ? 'c' : 's', cmd->cdw10_bits.delete_io_q.qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (is_cq) {
		cq = ctrlr->cqs[cmd->cdw10_bits.delete_io_q.qid];
		if (cq->cq_ref) {
			SPDK_ERRLOG("%s: the associated SQ must be deleted first\n", ctrlr_id(ctrlr));
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_DELETION;
			goto out;
		}
		delete_cq_done(ctrlr, cq);
	} else {
		/*
		 * Deletion of the CQ is only deferred to delete_sq_done() on
		 * VM reboot or CC.EN change, so we have to delete it in all
		 * other cases.
		 */
		sq = ctrlr->sqs[cmd->cdw10_bits.delete_io_q.qid];
		sq->delete_ctx = calloc(1, sizeof(*sq->delete_ctx));
		if (!sq->delete_ctx) {
			sct = SPDK_NVME_SCT_GENERIC;
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		sq->delete_ctx->vu_ctrlr = ctrlr;
		sq->delete_ctx->cid = cmd->cid;
		sq->sq_state = VFIO_USER_SQ_DELETED;
		assert(ctrlr->cqs[sq->cqid]->cq_ref);
		ctrlr->cqs[sq->cqid]->cq_ref--;

		spdk_nvmf_qpair_disconnect(&sq->qpair, NULL, NULL);
		return 0;
	}

out:
	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/*
 * Configures Shadow Doorbells.
 */
static int
handle_doorbell_buffer_config(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	struct nvmf_vfio_user_shadow_doorbells *sdbl = NULL;
	uint32_t dstrd;
	uintptr_t page_size, page_mask;
	uint64_t prp1, prp2;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_INVALID_FIELD;

	assert(ctrlr != NULL);
	assert(ctrlr->endpoint != NULL);
	assert(cmd != NULL);

	dstrd = doorbell_stride(ctrlr);
	page_size = memory_page_size(ctrlr);
	page_mask = memory_page_mask(ctrlr);

	/* FIXME: we don't check doorbell stride when setting queue doorbells. */
	if ((4u << dstrd) * NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR > page_size) {
		SPDK_ERRLOG("%s: doorbells do not fit in a single host page",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* Verify guest physical addresses passed as PRPs. */
	if (cmd->psdt != SPDK_NVME_PSDT_PRP) {
		SPDK_ERRLOG("%s: received Doorbell Buffer Config without PRPs",
			    ctrlr_id(ctrlr));

		goto out;
	}

	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: configuring shadow doorbells with PRP1=%#lx and PRP2=%#lx (GPAs)\n",
		      ctrlr_id(ctrlr), prp1, prp2);

	if (prp1 == prp2
	    || prp1 != (prp1 & page_mask)
	    || prp2 != (prp2 & page_mask)) {
		SPDK_ERRLOG("%s: invalid shadow doorbell GPAs\n",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* Map guest physical addresses to our virtual address space. */
	sdbl = map_sdbl(ctrlr->endpoint->vfu_ctx, prp1, prp2, page_size);
	if (sdbl == NULL) {
		SPDK_ERRLOG("%s: failed to map shadow doorbell buffers\n",
			    ctrlr_id(ctrlr));

		goto out;
	}

	ctrlr->shadow_doorbell_buffer = prp1;
	ctrlr->eventidx_buffer = prp2;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: mapped shadow doorbell buffers [%p, %p) and [%p, %p)\n",
		      ctrlr_id(ctrlr),
		      sdbl->iovs[0].iov_base,
		      sdbl->iovs[0].iov_base + sdbl->iovs[0].iov_len,
		      sdbl->iovs[1].iov_base,
		      sdbl->iovs[1].iov_base + sdbl->iovs[1].iov_len);


	/*
	 * Set all possible CQ head doorbells to polling mode now, such that we
	 * don't have to worry about it later if the host creates more queues.
	 *
	 * We only ever want interrupts for writes to the SQ tail doorbells
	 * (which are initialised in set_ctrlr_intr_mode() below).
	 */
	for (uint16_t i = 0; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; ++i) {
		sdbl->eventidxs[queue_index(i, true)] = NVMF_VFIO_USER_EVENTIDX_POLL;
	}

	/* Update controller. */
	SWAP(ctrlr->sdbl, sdbl);

	/*
	 * Copy doorbells from either the previous shadow doorbell buffer or the
	 * BAR0 doorbells and make I/O queue doorbells point to the new buffer.
	 *
	 * This needs to account for older versions of the Linux NVMe driver,
	 * which don't clear out the buffer after a controller reset.
	 */
	copy_doorbells(ctrlr, sdbl != NULL ?
		       sdbl->shadow_doorbells : ctrlr->bar0_doorbells,
		       ctrlr->sdbl->shadow_doorbells);

	vfio_user_ctrlr_switch_doorbells(ctrlr, true);

	ctrlr_kick(ctrlr);

	sc = SPDK_NVME_SC_SUCCESS;

out:
	/*
	 * Unmap existing buffers, in case Doorbell Buffer Config was sent
	 * more than once (pointless, but not prohibited by the spec), or
	 * in case of an error.
	 *
	 * If this is the first time Doorbell Buffer Config was processed,
	 * then we've just swapped a NULL from ctrlr->sdbl into sdbl, so
	 * free_sdbl() becomes a noop.
	 */
	free_sdbl(ctrlr->endpoint->vfu_ctx, sdbl);

	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/* Returns 0 on success and -errno on error. */
static int
consume_admin_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	assert(ctrlr != NULL);
	assert(cmd != NULL);

	if (cmd->fuse != 0) {
		/* Fused admin commands are not supported. */
		return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid,
				       SPDK_NVME_SC_INVALID_FIELD,
				       SPDK_NVME_SCT_GENERIC);
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		return handle_create_io_q(ctrlr, cmd,
					  cmd->opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		return handle_del_io_q(ctrlr, cmd,
				       cmd->opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		if (!ctrlr->transport->transport_opts.disable_shadow_doorbells) {
			return handle_doorbell_buffer_config(ctrlr, cmd);
		}
	/* FALLTHROUGH */
	default:
		return handle_cmd_req(ctrlr, cmd, ctrlr->sqs[0]);
	}
}

static int
handle_cmd_rsp(struct nvmf_vfio_user_req *vu_req, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = sq->ctrlr;
	uint16_t sqid, cqid;

	assert(sq != NULL);
	assert(vu_req != NULL);
	assert(vu_ctrlr != NULL);

	if (spdk_likely(vu_req->iovcnt)) {
		vfu_sgl_put(vu_ctrlr->endpoint->vfu_ctx,
			    index_to_sg_t(vu_req->sg, 0),
			    vu_req->iov, vu_req->iovcnt);
	}
	sqid = sq->qid;
	cqid = sq->cqid;

	return post_completion(vu_ctrlr, vu_ctrlr->cqs[cqid],
			       vu_req->req.rsp->nvme_cpl.cdw0,
			       sqid,
			       vu_req->req.cmd->nvme_cmd.cid,
			       vu_req->req.rsp->nvme_cpl.status.sc,
			       vu_req->req.rsp->nvme_cpl.status.sct);
}

static int
consume_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq,
	    struct spdk_nvme_cmd *cmd)
{
	assert(sq != NULL);
	if (spdk_unlikely(nvmf_qpair_is_admin_queue(&sq->qpair))) {
		return consume_admin_cmd(ctrlr, cmd);
	}

	return handle_cmd_req(ctrlr, cmd, sq);
}

/* Returns the number of commands processed, or a negative value on error. */
static int
handle_sq_tdbl_write(struct nvmf_vfio_user_ctrlr *ctrlr, const uint32_t new_tail,
		     struct nvmf_vfio_user_sq *sq)
{
	struct spdk_nvme_cmd *queue;
	int count = 0;

	assert(ctrlr != NULL);
	assert(sq != NULL);

	if (ctrlr->sdbl != NULL && sq->qid != 0) {
		/*
		 * Submission queue index has moved past the event index, so it
		 * needs to be re-armed before we go to sleep.
		 */
		sq->need_rearm = true;
	}

	queue = q_addr(&sq->mapping);
	while (*sq_headp(sq) != new_tail) {
		int err;
		struct spdk_nvme_cmd *cmd = &queue[*sq_headp(sq)];

		count++;

		/*
		 * SQHD must contain the new head pointer, so we must increase
		 * it before we generate a completion.
		 */
		sq_head_advance(sq);

		err = consume_cmd(ctrlr, sq, cmd);
		if (spdk_unlikely(err != 0)) {
			return err;
		}
	}

	return count;
}

/* Checks whether endpoint is connected from the same process */
static bool
is_peer_same_process(struct nvmf_vfio_user_endpoint *endpoint)
{
	struct ucred ucred;
	socklen_t ucredlen = sizeof(ucred);

	if (endpoint == NULL) {
		return false;
	}

	if (getsockopt(vfu_get_poll_fd(endpoint->vfu_ctx), SOL_SOCKET, SO_PEERCRED, &ucred,
		       &ucredlen) < 0) {
		SPDK_ERRLOG("getsockopt(SO_PEERCRED): %s\n", strerror(errno));
		return false;
	}

	return ucred.pid == getpid();
}

static void
memory_region_add_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	void *map_start, *map_end;
	int ret;

	/*
	 * We're not interested in any DMA regions that aren't mappable (we don't
	 * support clients that don't share their memory).
	 */
	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	assert(endpoint != NULL);
	if (endpoint->ctrlr == NULL) {
		return;
	}
	ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: map IOVA %p-%p\n", endpoint_id(endpoint),
		      map_start, map_end);

	/* VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE are enabled when registering to VFIO, here we also
	 * check the protection bits before registering. When vfio client and server are run in same process
	 * there is no need to register the same memory again.
	 */
	if (info->prot == (PROT_WRITE | PROT_READ) && !is_peer_same_process(endpoint)) {
		ret = spdk_mem_register(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region register %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		if (sq->sq_state != VFIO_USER_SQ_INACTIVE) {
			continue;
		}

		cq = ctrlr->cqs[sq->cqid];

		/* For shared CQ case, we will use q_addr() to avoid mapping CQ multiple times */
		if (cq->size && q_addr(&cq->mapping) == NULL) {
			ret = map_q(ctrlr, &cq->mapping, cq->size, true, false);
			if (ret) {
				SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap cqid:%d %#lx-%#lx\n",
					      cq->qid, cq->mapping.prp1,
					      cq->mapping.prp1 + cq->size * sizeof(struct spdk_nvme_cpl));
				continue;
			}
		}

		if (sq->size) {
			ret = map_q(ctrlr, &sq->mapping, sq->size, false, false);
			if (ret) {
				SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap sqid:%d %#lx-%#lx\n",
					      sq->qid, sq->mapping.prp1,
					      sq->mapping.prp1 + sq->size * sizeof(struct spdk_nvme_cmd));
				continue;
			}
		}
		sq->sq_state = VFIO_USER_SQ_ACTIVE;
		SPDK_DEBUGLOG(nvmf_vfio, "Remap sqid:%u successfully\n", sq->qid);
	}
	pthread_mutex_unlock(&endpoint->lock);
}

static void
memory_region_remove_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	void *map_start, *map_end;
	int ret = 0;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	assert(endpoint != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: unmap IOVA %p-%p\n", endpoint_id(endpoint),
		      map_start, map_end);

	if (endpoint->ctrlr != NULL) {
		struct nvmf_vfio_user_ctrlr *ctrlr;
		ctrlr = endpoint->ctrlr;

		pthread_mutex_lock(&endpoint->lock);
		TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
			if (q_addr(&sq->mapping) >= map_start && q_addr(&sq->mapping) <= map_end) {
				unmap_q(ctrlr, &sq->mapping);
				sq->sq_state = VFIO_USER_SQ_INACTIVE;
			}

			cq = ctrlr->cqs[sq->cqid];
			if (q_addr(&cq->mapping) >= map_start && q_addr(&cq->mapping) <= map_end) {
				unmap_q(ctrlr, &cq->mapping);
			}
		}

		if (ctrlr->sdbl != NULL) {
			size_t i;

			for (i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT; i++) {
				const void *const iov_base = ctrlr->sdbl->iovs[i].iov_base;

				if (iov_base >= map_start && iov_base < map_end) {
					copy_doorbells(ctrlr,
						       ctrlr->sdbl->shadow_doorbells,
						       ctrlr->bar0_doorbells);
					vfio_user_ctrlr_switch_doorbells(ctrlr, false);
					free_sdbl(endpoint->vfu_ctx, ctrlr->sdbl);
					ctrlr->sdbl = NULL;
					break;
				}
			}
		}

		pthread_mutex_unlock(&endpoint->lock);
	}

	if (info->prot == (PROT_WRITE | PROT_READ) && !is_peer_same_process(endpoint)) {
		ret = spdk_mem_unregister(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region unregister %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}
}

/* Used to initiate a controller-level reset or a controller shutdown. */
static void
disable_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	SPDK_DEBUGLOG(nvmf_vfio, "%s: disabling controller\n",
		      ctrlr_id(vu_ctrlr));

	/* Unmap Admin queue. */

	assert(vu_ctrlr->sqs[0] != NULL);
	assert(vu_ctrlr->cqs[0] != NULL);

	unmap_q(vu_ctrlr, &vu_ctrlr->sqs[0]->mapping);
	unmap_q(vu_ctrlr, &vu_ctrlr->cqs[0]->mapping);

	vu_ctrlr->sqs[0]->size = 0;
	*sq_headp(vu_ctrlr->sqs[0]) = 0;

	vu_ctrlr->sqs[0]->sq_state = VFIO_USER_SQ_INACTIVE;

	vu_ctrlr->cqs[0]->size = 0;
	*cq_tailp(vu_ctrlr->cqs[0]) = 0;

	/*
	 * For PCIe controller reset or shutdown, we will drop all AER
	 * responses.
	 */
	nvmf_ctrlr_abort_aer(vu_ctrlr->ctrlr);

	/* Free the shadow doorbell buffer. */
	vfio_user_ctrlr_switch_doorbells(vu_ctrlr, false);
	free_sdbl(vu_ctrlr->endpoint->vfu_ctx, vu_ctrlr->sdbl);
	vu_ctrlr->sdbl = NULL;
}

/* Used to re-enable the controller after a controller-level reset. */
static int
enable_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	int err;

	assert(vu_ctrlr != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: enabling controller\n",
		      ctrlr_id(vu_ctrlr));

	err = acq_setup(vu_ctrlr);
	if (err != 0) {
		return err;
	}

	err = asq_setup(vu_ctrlr);
	if (err != 0) {
		return err;
	}

	vu_ctrlr->sqs[0]->sq_state = VFIO_USER_SQ_ACTIVE;

	return 0;
}

static int
nvmf_vfio_user_prop_req_rsp_set(struct nvmf_vfio_user_req *req,
				struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	union spdk_nvme_cc_register cc, diff;

	assert(req->req.cmd->prop_set_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET);
	assert(sq->ctrlr != NULL);
	vu_ctrlr = sq->ctrlr;

	if (req->req.cmd->prop_set_cmd.ofst != offsetof(struct spdk_nvme_registers, cc)) {
		return 0;
	}

	cc.raw = req->req.cmd->prop_set_cmd.value.u64;
	diff.raw = cc.raw ^ req->cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			int ret = enable_ctrlr(vu_ctrlr);
			if (ret) {
				SPDK_ERRLOG("%s: failed to enable ctrlr\n", ctrlr_id(vu_ctrlr));
				return ret;
			}
			vu_ctrlr->reset_shn = false;
		} else {
			vu_ctrlr->reset_shn = true;
		}
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL || cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			vu_ctrlr->reset_shn = true;
		}
	}

	if (vu_ctrlr->reset_shn) {
		disable_ctrlr(vu_ctrlr);
	}
	return 0;
}

static int
nvmf_vfio_user_prop_req_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq = cb_arg;

	assert(sq != NULL);
	assert(req != NULL);

	if (req->req.cmd->prop_get_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET) {
		assert(sq->ctrlr != NULL);
		assert(req != NULL);

		memcpy(req->req.data,
		       &req->req.rsp->prop_get_rsp.value.u64,
		       req->req.length);
		return 0;
	}

	return nvmf_vfio_user_prop_req_rsp_set(req, sq);
}

/*
 * Handles a write at offset 0x1000 or more; this is the non-mapped path when a
 * doorbell is written via access_bar0_fn().
 *
 * DSTRD is set to fixed value 0 for NVMf.
 *
 */
static int
handle_dbl_access(struct nvmf_vfio_user_ctrlr *ctrlr, uint32_t *buf,
		  const size_t count, loff_t pos, const bool is_write)
{
	struct nvmf_vfio_user_poll_group *group;

	assert(ctrlr != NULL);
	assert(buf != NULL);

	if (spdk_unlikely(!is_write)) {
		SPDK_WARNLOG("%s: host tried to read BAR0 doorbell %#lx\n",
			     ctrlr_id(ctrlr), pos);
		errno = EPERM;
		return -1;
	}

	if (spdk_unlikely(count != sizeof(uint32_t))) {
		SPDK_ERRLOG("%s: bad doorbell buffer size %ld\n",
			    ctrlr_id(ctrlr), count);
		errno = EINVAL;
		return -1;
	}

	pos -= NVME_DOORBELLS_OFFSET;

	/* pos must be dword aligned */
	if (spdk_unlikely((pos & 0x3) != 0)) {
		SPDK_ERRLOG("%s: bad doorbell offset %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	/* convert byte offset to array index */
	pos >>= 2;

	if (spdk_unlikely(pos >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2)) {
		SPDK_ERRLOG("%s: bad doorbell index %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	ctrlr->bar0_doorbells[pos] = *buf;
	spdk_wmb();

	group = ctrlr_to_poll_group(ctrlr);
	if (pos == 1) {
		group->stats.cqh_admin_writes++;
	} else if (pos & 1) {
		group->stats.cqh_io_writes++;
	}

	SPDK_DEBUGLOG(vfio_user_db, "%s: updating BAR0 doorbell %s:%ld to %u\n",
		      ctrlr_id(ctrlr), (pos & 1) ? "cqid" : "sqid",
		      pos / 2, *buf);


	return 0;
}

static size_t
vfio_user_property_access(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
			  char *buf, size_t count, loff_t pos,
			  bool is_write)
{
	struct nvmf_vfio_user_req *req;
	const struct spdk_nvmf_registers *regs;

	if ((count != 4) && (count != 8)) {
		errno = EINVAL;
		return -1;
	}

	/* Construct a Fabric Property Get/Set command and send it */
	req = get_nvmf_vfio_user_req(vu_ctrlr->sqs[0]);
	if (req == NULL) {
		errno = ENOBUFS;
		return -1;
	}
	regs = spdk_nvmf_ctrlr_get_regs(vu_ctrlr->ctrlr);
	req->cc.raw = regs->cc.raw;

	req->cb_fn = nvmf_vfio_user_prop_req_rsp;
	req->cb_arg = vu_ctrlr->sqs[0];
	req->req.cmd->prop_set_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->req.cmd->prop_set_cmd.cid = 0;
	if (count == 4) {
		req->req.cmd->prop_set_cmd.attrib.size = 0;
	} else {
		req->req.cmd->prop_set_cmd.attrib.size = 1;
	}
	req->req.cmd->prop_set_cmd.ofst = pos;
	if (is_write) {
		req->req.cmd->prop_set_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
		if (req->req.cmd->prop_set_cmd.attrib.size) {
			req->req.cmd->prop_set_cmd.value.u64 = *(uint64_t *)buf;
		} else {
			req->req.cmd->prop_set_cmd.value.u32.high = 0;
			req->req.cmd->prop_set_cmd.value.u32.low = *(uint32_t *)buf;
		}
	} else {
		req->req.cmd->prop_get_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	}
	req->req.length = count;
	req->req.data = buf;

	spdk_nvmf_request_exec_fabrics(&req->req);

	return count;
}

static ssize_t
access_bar0_fn(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
	       bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int ret;

	ctrlr = endpoint->ctrlr;
	if (spdk_unlikely(endpoint->need_async_destroy || !ctrlr)) {
		errno = EIO;
		return -1;
	}

	if (pos >= NVME_DOORBELLS_OFFSET) {
		/*
		 * The fact that the doorbells can be memory mapped doesn't mean
		 * that the client (VFIO in QEMU) is obliged to memory map them,
		 * it might still elect to access them via regular read/write;
		 * we might also have had disable_mappable_bar0 set.
		 */
		ret = handle_dbl_access(ctrlr, (uint32_t *)buf, count,
					pos, is_write);
		if (ret == 0) {
			return count;
		}
		return ret;
	}

	return vfio_user_property_access(ctrlr, buf, count, pos, is_write);
}

static ssize_t
access_pci_config(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset,
		  bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);

	if (is_write) {
		SPDK_ERRLOG("%s: write %#lx-%#lx not supported\n",
			    endpoint_id(endpoint), offset, offset + count);
		errno = EINVAL;
		return -1;
	}

	if (offset + count > NVME_REG_CFG_SIZE) {
		SPDK_ERRLOG("%s: access past end of extended PCI configuration space, want=%ld+%ld, max=%d\n",
			    endpoint_id(endpoint), offset, count,
			    NVME_REG_CFG_SIZE);
		errno = ERANGE;
		return -1;
	}

	memcpy(buf, ((unsigned char *)endpoint->pci_config_space) + offset, count);

	return count;
}

static void
vfio_user_log(vfu_ctx_t *vfu_ctx, int level, char const *msg)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);

	if (level >= LOG_DEBUG) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_INFO) {
		SPDK_INFOLOG(nvmf_vfio, "%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_NOTICE) {
		SPDK_NOTICELOG("%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_WARNING) {
		SPDK_WARNLOG("%s: %s\n", endpoint_id(endpoint), msg);
	} else {
		SPDK_ERRLOG("%s: %s\n", endpoint_id(endpoint), msg);
	}
}

static int
vfio_user_get_log_level(void)
{
	int level;

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf_vfio")) {
		return LOG_DEBUG;
	}

	level = spdk_log_to_syslog_level(spdk_log_get_level());
	if (level < 0) {
		return LOG_ERR;
	}

	return level;
}

static void
init_pci_config_space(vfu_pci_config_space_t *p)
{
	/* MLBAR */
	p->hdr.bars[0].raw = 0x0;
	/* MUBAR */
	p->hdr.bars[1].raw = 0x0;

	/* vendor specific, let's set them to zero for now */
	p->hdr.bars[3].raw = 0x0;
	p->hdr.bars[4].raw = 0x0;
	p->hdr.bars[5].raw = 0x0;

	/* enable INTx */
	p->hdr.intr.ipin = 0x1;
}

struct ctrlr_quiesce_ctx {
	struct nvmf_vfio_user_endpoint *endpoint;
	struct nvmf_vfio_user_poll_group *group;
	int status;
};

static void ctrlr_quiesce(struct nvmf_vfio_user_ctrlr *vu_ctrlr);

static void
_vfio_user_endpoint_resume_done_msg(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	endpoint->need_resume = false;

	if (!vu_ctrlr) {
		return;
	}

	if (!vu_ctrlr->queued_quiesce) {
		vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;

		/*
		 * We might have ignored new SQ entries while we were quiesced:
		 * kick ourselves so we'll definitely check again while in
		 * VFIO_USER_CTRLR_RUNNING state.
		 */
		if (in_interrupt_mode(endpoint->transport)) {
			ctrlr_kick(vu_ctrlr);
		}
		return;
	}


	/*
	 * Basically, once we call `vfu_device_quiesced` the device is
	 * unquiesced from libvfio-user's perspective so from the moment
	 * `vfio_user_quiesce_done` returns libvfio-user might quiesce the device
	 * again. However, because the NVMf subsytem is an asynchronous
	 * operation, this quiesce might come _before_ the NVMf subsystem has
	 * been resumed, so in the callback of `spdk_nvmf_subsystem_resume` we
	 * need to check whether a quiesce was requested.
	 */
	SPDK_DEBUGLOG(nvmf_vfio, "%s has queued quiesce event, quiesce again\n",
		      ctrlr_id(vu_ctrlr));
	ctrlr_quiesce(vu_ctrlr);
}

static void
vfio_user_endpoint_resume_done(struct spdk_nvmf_subsystem *subsystem,
			       void *cb_arg, int status)
{
	struct nvmf_vfio_user_endpoint *endpoint = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s resumed done with status %d\n", endpoint_id(endpoint), status);

	if (!vu_ctrlr) {
		return;
	}

	spdk_thread_send_msg(vu_ctrlr->thread, _vfio_user_endpoint_resume_done_msg, endpoint);
}

static void
vfio_user_quiesce_done(void *ctx)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	int ret;

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s device quiesced\n", ctrlr_id(vu_ctrlr));

	assert(vu_ctrlr->state == VFIO_USER_CTRLR_PAUSING);
	vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
	vfu_device_quiesced(endpoint->vfu_ctx, quiesce_ctx->status);
	vu_ctrlr->queued_quiesce = false;
	free(quiesce_ctx);

	/* `vfu_device_quiesced` can change the migration state,
	 * so we need to re-check `vu_ctrlr->state`.
	 */
	if (vu_ctrlr->state == VFIO_USER_CTRLR_MIGRATING) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s is in MIGRATION state\n", ctrlr_id(vu_ctrlr));
		return;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s start to resume\n", ctrlr_id(vu_ctrlr));
	vu_ctrlr->state = VFIO_USER_CTRLR_RESUMING;
	ret = spdk_nvmf_subsystem_resume((struct spdk_nvmf_subsystem *)endpoint->subsystem,
					 vfio_user_endpoint_resume_done, endpoint);
	if (ret < 0) {
		vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
		SPDK_ERRLOG("%s: failed to resume, ret=%d\n", endpoint_id(endpoint), ret);
	}
}

static void
vfio_user_pause_done(struct spdk_nvmf_subsystem *subsystem,
		     void *ctx, int status)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	quiesce_ctx->status = status;

	SPDK_DEBUGLOG(nvmf_vfio, "%s pause done with status %d\n",
		      ctrlr_id(vu_ctrlr), status);

	spdk_thread_send_msg(vu_ctrlr->thread,
			     vfio_user_quiesce_done, ctx);
}

/*
 * Ensure that, for this PG, we've stopped running in nvmf_vfio_user_sq_poll();
 * we've already set ctrlr->state, so we won't process new entries, but we need
 * to ensure that this PG is quiesced. This only works because there's no
 * callback context set up between polling the SQ and spdk_nvmf_request_exec().
 *
 * Once we've walked all PGs, we need to pause any submitted I/O via
 * spdk_nvmf_subsystem_pause(SPDK_NVME_GLOBAL_NS_TAG).
 */
static void
vfio_user_quiesce_pg(void *ctx)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	struct nvmf_vfio_user_poll_group *vu_group = quiesce_ctx->group;
	struct spdk_nvmf_subsystem *subsystem = endpoint->subsystem;
	int ret;

	SPDK_DEBUGLOG(nvmf_vfio, "quiesced pg:%p\n", vu_group);

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	quiesce_ctx->group = TAILQ_NEXT(vu_group, link);
	if (quiesce_ctx->group != NULL)  {
		spdk_thread_send_msg(poll_group_to_thread(quiesce_ctx->group),
				     vfio_user_quiesce_pg, quiesce_ctx);
		return;
	}

	ret = spdk_nvmf_subsystem_pause(subsystem, SPDK_NVME_GLOBAL_NS_TAG,
					vfio_user_pause_done, quiesce_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("%s: failed to pause, ret=%d\n",
			    endpoint_id(endpoint), ret);
		vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;
		fail_ctrlr(vu_ctrlr);
		free(quiesce_ctx);
	}
}

static void
ctrlr_quiesce(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx;

	vu_ctrlr->state = VFIO_USER_CTRLR_PAUSING;

	quiesce_ctx = calloc(1, sizeof(*quiesce_ctx));
	if (!quiesce_ctx) {
		SPDK_ERRLOG("Failed to allocate subsystem pause context\n");
		assert(false);
		return;
	}

	quiesce_ctx->endpoint = vu_ctrlr->endpoint;
	quiesce_ctx->status = 0;
	quiesce_ctx->group = TAILQ_FIRST(&vu_ctrlr->transport->poll_groups);

	spdk_thread_send_msg(poll_group_to_thread(quiesce_ctx->group),
			     vfio_user_quiesce_pg, quiesce_ctx);
}

static int
vfio_user_dev_quiesce_cb(vfu_ctx_t *vfu_ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct spdk_nvmf_subsystem *subsystem = endpoint->subsystem;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		return 0;
	}

	/* NVMf library will destruct controller when no
	 * connected queue pairs.
	 */
	if (!nvmf_subsystem_get_ctrlr(subsystem, vu_ctrlr->cntlid)) {
		return 0;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s starts to quiesce\n", ctrlr_id(vu_ctrlr));

	/* There is no race condition here as device quiesce callback
	 * and nvmf_prop_set_cc() are running in the same thread context.
	 */
	if (!vu_ctrlr->ctrlr->vcprop.cc.bits.en) {
		return 0;
	} else if (!vu_ctrlr->ctrlr->vcprop.csts.bits.rdy) {
		return 0;
	} else if (vu_ctrlr->ctrlr->vcprop.csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
		return 0;
	}

	switch (vu_ctrlr->state) {
	case VFIO_USER_CTRLR_PAUSED:
	case VFIO_USER_CTRLR_MIGRATING:
		return 0;
	case VFIO_USER_CTRLR_RUNNING:
		ctrlr_quiesce(vu_ctrlr);
		break;
	case VFIO_USER_CTRLR_RESUMING:
		vu_ctrlr->queued_quiesce = true;
		SPDK_DEBUGLOG(nvmf_vfio, "%s is busy to quiesce, current state %u\n", ctrlr_id(vu_ctrlr),
			      vu_ctrlr->state);
		break;
	default:
		assert(vu_ctrlr->state != VFIO_USER_CTRLR_PAUSING);
		break;
	}

	errno = EBUSY;
	return -1;
}

static void
vfio_user_ctrlr_dump_migr_data(const char *name,
			       struct vfio_user_nvme_migr_state *migr_data,
			       struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	struct spdk_nvmf_registers *regs;
	struct nvme_migr_sq_state *sq;
	struct nvme_migr_cq_state *cq;
	uint32_t *doorbell_base;
	uint32_t i;

	SPDK_NOTICELOG("Dump %s\n", name);

	regs = &migr_data->nvmf_data.regs;
	doorbell_base = (uint32_t *)&migr_data->doorbells;

	SPDK_NOTICELOG("Registers\n");
	SPDK_NOTICELOG("CSTS 0x%x\n", regs->csts.raw);
	SPDK_NOTICELOG("CAP  0x%"PRIx64"\n", regs->cap.raw);
	SPDK_NOTICELOG("VS   0x%x\n", regs->vs.raw);
	SPDK_NOTICELOG("CC   0x%x\n", regs->cc.raw);
	SPDK_NOTICELOG("AQA  0x%x\n", regs->aqa.raw);
	SPDK_NOTICELOG("ASQ  0x%"PRIx64"\n", regs->asq);
	SPDK_NOTICELOG("ACQ  0x%"PRIx64"\n", regs->acq);

	SPDK_NOTICELOG("Number of IO Queues %u\n", migr_data->ctrlr_header.num_io_queues);

	if (sdbl != NULL) {
		SPDK_NOTICELOG("shadow doorbell buffer=%#lx\n",
			       migr_data->ctrlr_header.shadow_doorbell_buffer);
		SPDK_NOTICELOG("eventidx buffer=%#lx\n",
			       migr_data->ctrlr_header.eventidx_buffer);
	}

	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		sq = &migr_data->qps[i].sq;
		cq = &migr_data->qps[i].cq;

		if (sq->size) {
			SPDK_NOTICELOG("sqid:%u, bar0_doorbell:%u\n", sq->sqid, doorbell_base[i * 2]);
			if (i > 0 && sdbl != NULL) {
				SPDK_NOTICELOG("sqid:%u, shadow_doorbell:%u, eventidx:%u\n",
					       sq->sqid,
					       sdbl->shadow_doorbells[queue_index(i, false)],
					       sdbl->eventidxs[queue_index(i, false)]);
			}
			SPDK_NOTICELOG("SQ sqid:%u, cqid:%u, sqhead:%u, size:%u, dma_addr:0x%"PRIx64"\n",
				       sq->sqid, sq->cqid, sq->head, sq->size, sq->dma_addr);
		}

		if (cq->size) {
			SPDK_NOTICELOG("cqid:%u, bar0_doorbell:%u\n", cq->cqid, doorbell_base[i * 2 + 1]);
			if (i > 0 && sdbl != NULL) {
				SPDK_NOTICELOG("cqid:%u, shadow_doorbell:%u, eventidx:%u\n",
					       cq->cqid,
					       sdbl->shadow_doorbells[queue_index(i, true)],
					       sdbl->eventidxs[queue_index(i, true)]);
			}
			SPDK_NOTICELOG("CQ cqid:%u, phase:%u, cqtail:%u, size:%u, iv:%u, ien:%u, dma_addr:0x%"PRIx64"\n",
				       cq->cqid, cq->phase, cq->tail, cq->size, cq->iv, cq->ien, cq->dma_addr);
		}
	}

	SPDK_NOTICELOG("%s Dump Done\n", name);
}

/* Read region 9 content and restore it to migration data structures */
static int
vfio_user_migr_stream_to_data(struct nvmf_vfio_user_endpoint *endpoint,
			      struct vfio_user_nvme_migr_state *migr_state)
{
	void *data_ptr = endpoint->migr_data;

	/* Load vfio_user_nvme_migr_header first */
	memcpy(&migr_state->ctrlr_header, data_ptr, sizeof(struct vfio_user_nvme_migr_header));
	/* TODO: version check */
	if (migr_state->ctrlr_header.magic != VFIO_USER_NVME_MIGR_MAGIC) {
		SPDK_ERRLOG("%s: bad magic number %x\n", endpoint_id(endpoint), migr_state->ctrlr_header.magic);
		return -EINVAL;
	}

	/* Load nvmf controller data */
	data_ptr = endpoint->migr_data + migr_state->ctrlr_header.nvmf_data_offset;
	memcpy(&migr_state->nvmf_data, data_ptr, migr_state->ctrlr_header.nvmf_data_len);

	/* Load queue pairs */
	data_ptr = endpoint->migr_data + migr_state->ctrlr_header.qp_offset;
	memcpy(&migr_state->qps, data_ptr, migr_state->ctrlr_header.qp_len);

	/* Load doorbells */
	data_ptr = endpoint->migr_data + migr_state->ctrlr_header.bar_offset[VFU_PCI_DEV_BAR0_REGION_IDX];
	memcpy(&migr_state->doorbells, data_ptr,
	       migr_state->ctrlr_header.bar_len[VFU_PCI_DEV_BAR0_REGION_IDX]);

	/* Load CFG */
	data_ptr = endpoint->migr_data + migr_state->ctrlr_header.bar_offset[VFU_PCI_DEV_CFG_REGION_IDX];
	memcpy(&migr_state->cfg, data_ptr, migr_state->ctrlr_header.bar_len[VFU_PCI_DEV_CFG_REGION_IDX]);

	return 0;
}


static void
vfio_user_migr_ctrlr_save_data(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct spdk_nvmf_ctrlr *ctrlr = vu_ctrlr->ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint = vu_ctrlr->endpoint;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	uint64_t data_offset;
	void *data_ptr;
	uint32_t *doorbell_base;
	uint32_t i = 0;
	uint16_t sqid, cqid;
	struct vfio_user_nvme_migr_state migr_state = {
		.nvmf_data = {
			.data_size = offsetof(struct spdk_nvmf_ctrlr_migr_data, unused),
			.regs_size = sizeof(struct spdk_nvmf_registers),
			.feat_size = sizeof(struct spdk_nvmf_ctrlr_feat)
		}
	};

	/* Save all data to vfio_user_nvme_migr_state first, then we will
	 * copy it to device migration region at last.
	 */

	/* save magic number */
	migr_state.ctrlr_header.magic = VFIO_USER_NVME_MIGR_MAGIC;

	/* save controller data */
	spdk_nvmf_ctrlr_save_migr_data(ctrlr, &migr_state.nvmf_data);

	/* save connected queue pairs */
	TAILQ_FOREACH(sq, &vu_ctrlr->connected_sqs, tailq) {
		/* save sq */
		sqid = sq->qid;
		migr_state.qps[sqid].sq.sqid = sq->qid;
		migr_state.qps[sqid].sq.cqid = sq->cqid;
		migr_state.qps[sqid].sq.head = *sq_headp(sq);
		migr_state.qps[sqid].sq.size = sq->size;
		migr_state.qps[sqid].sq.dma_addr = sq->mapping.prp1;

		/* save cq, for shared cq case, cq may be saved multiple times */
		cqid = sq->cqid;
		cq = vu_ctrlr->cqs[cqid];
		migr_state.qps[cqid].cq.cqid = cqid;
		migr_state.qps[cqid].cq.tail = *cq_tailp(cq);
		migr_state.qps[cqid].cq.ien = cq->ien;
		migr_state.qps[cqid].cq.iv = cq->iv;
		migr_state.qps[cqid].cq.size = cq->size;
		migr_state.qps[cqid].cq.phase = cq->phase;
		migr_state.qps[cqid].cq.dma_addr = cq->mapping.prp1;
		i++;
	}

	assert(i > 0);
	migr_state.ctrlr_header.num_io_queues = i - 1;

	/* Save doorbells */
	doorbell_base = (uint32_t *)&migr_state.doorbells;
	memcpy(doorbell_base, (void *)vu_ctrlr->bar0_doorbells, NVMF_VFIO_USER_DOORBELLS_SIZE);

	/* Save PCI configuration space */
	memcpy(&migr_state.cfg, (void *)endpoint->pci_config_space, NVME_REG_CFG_SIZE);

	/* Save all data to device migration region */
	data_ptr = endpoint->migr_data;

	/* Copy nvmf controller data */
	data_offset = sizeof(struct vfio_user_nvme_migr_header);
	data_ptr += data_offset;
	migr_state.ctrlr_header.nvmf_data_offset = data_offset;
	migr_state.ctrlr_header.nvmf_data_len = sizeof(struct spdk_nvmf_ctrlr_migr_data);
	memcpy(data_ptr, &migr_state.nvmf_data, sizeof(struct spdk_nvmf_ctrlr_migr_data));

	/* Copy queue pairs */
	data_offset += sizeof(struct spdk_nvmf_ctrlr_migr_data);
	data_ptr += sizeof(struct spdk_nvmf_ctrlr_migr_data);
	migr_state.ctrlr_header.qp_offset = data_offset;
	migr_state.ctrlr_header.qp_len = i * (sizeof(struct nvme_migr_sq_state) + sizeof(
			struct nvme_migr_cq_state));
	memcpy(data_ptr, &migr_state.qps, migr_state.ctrlr_header.qp_len);

	/* Copy doorbells */
	data_offset += migr_state.ctrlr_header.qp_len;
	data_ptr += migr_state.ctrlr_header.qp_len;
	migr_state.ctrlr_header.bar_offset[VFU_PCI_DEV_BAR0_REGION_IDX] = data_offset;
	migr_state.ctrlr_header.bar_len[VFU_PCI_DEV_BAR0_REGION_IDX] = NVMF_VFIO_USER_DOORBELLS_SIZE;
	memcpy(data_ptr, &migr_state.doorbells, NVMF_VFIO_USER_DOORBELLS_SIZE);

	/* Copy CFG */
	data_offset += NVMF_VFIO_USER_DOORBELLS_SIZE;
	data_ptr += NVMF_VFIO_USER_DOORBELLS_SIZE;
	migr_state.ctrlr_header.bar_offset[VFU_PCI_DEV_CFG_REGION_IDX] = data_offset;
	migr_state.ctrlr_header.bar_len[VFU_PCI_DEV_CFG_REGION_IDX] = NVME_REG_CFG_SIZE;
	memcpy(data_ptr, &migr_state.cfg, NVME_REG_CFG_SIZE);

	/* copy shadow doorbells */
	if (vu_ctrlr->sdbl != NULL) {
		migr_state.ctrlr_header.sdbl = true;
		migr_state.ctrlr_header.shadow_doorbell_buffer = vu_ctrlr->shadow_doorbell_buffer;
		migr_state.ctrlr_header.eventidx_buffer = vu_ctrlr->eventidx_buffer;
	}

	/* Copy nvme migration header finally */
	memcpy(endpoint->migr_data, &migr_state.ctrlr_header, sizeof(struct vfio_user_nvme_migr_header));

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf_vfio")) {
		vfio_user_ctrlr_dump_migr_data("SAVE", &migr_state, vu_ctrlr->sdbl);
	}
}

/*
 * If we are about to close the connection, we need to unregister the interrupt,
 * as the library will subsequently close the file descriptor we registered.
 */
static int
vfio_user_device_reset(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "Device reset type %u\n", type);

	if (type == VFU_RESET_LOST_CONN) {
		if (ctrlr != NULL) {
			spdk_interrupt_unregister(&ctrlr->intr);
			ctrlr->intr_fd = -1;
		}
		return 0;
	}

	/* FIXME: LOST_CONN case ? */
	if (ctrlr->sdbl != NULL) {
		vfio_user_ctrlr_switch_doorbells(ctrlr, false);
		free_sdbl(vfu_ctx, ctrlr->sdbl);
		ctrlr->sdbl = NULL;
	}

	/* FIXME: much more needed here. */

	return 0;
}

static int
vfio_user_migr_ctrlr_construct_qps(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
				   struct vfio_user_nvme_migr_state *migr_state)
{
	uint32_t i, qsize = 0;
	uint16_t sqid, cqid;
	struct vfio_user_nvme_migr_qp migr_qp;
	void *addr;
	uint32_t cqs_ref[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR] = {};
	int ret;

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf_vfio")) {
		vfio_user_ctrlr_dump_migr_data("RESUME", migr_state, vu_ctrlr->sdbl);
	}

	/* restore submission queues */
	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		migr_qp =  migr_state->qps[i];

		qsize = migr_qp.sq.size;
		if (qsize) {
			struct nvmf_vfio_user_sq *sq;

			sqid = migr_qp.sq.sqid;
			if (sqid != i) {
				SPDK_ERRLOG("Expected sqid %u while got %u", i, sqid);
				return -EINVAL;
			}

			/* allocate sq if necessary */
			if (vu_ctrlr->sqs[sqid] == NULL) {
				ret = init_sq(vu_ctrlr, &vu_ctrlr->transport->transport, sqid);
				if (ret) {
					SPDK_ERRLOG("Construct qpair with qid %u failed\n", sqid);
					return -EFAULT;
				}
			}

			sq = vu_ctrlr->sqs[sqid];
			sq->size = qsize;

			ret = alloc_sq_reqs(vu_ctrlr, sq);
			if (ret) {
				SPDK_ERRLOG("Construct sq with qid %u failed\n", sqid);
				return -EFAULT;
			}

			/* restore sq */
			sq->sq_state = VFIO_USER_SQ_CREATED;
			sq->cqid = migr_qp.sq.cqid;
			*sq_headp(sq) = migr_qp.sq.head;
			sq->mapping.prp1 = migr_qp.sq.dma_addr;
			addr = map_one(vu_ctrlr->endpoint->vfu_ctx,
				       sq->mapping.prp1, sq->size * 64,
				       sq->mapping.sg, &sq->mapping.iov,
				       PROT_READ);
			if (addr == NULL) {
				SPDK_ERRLOG("Restore sq with qid %u PRP1 0x%"PRIx64" with size %u failed\n",
					    sqid, sq->mapping.prp1, sq->size);
				return -EFAULT;
			}
			cqs_ref[sq->cqid]++;
		}
	}

	/* restore completion queues */
	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		migr_qp =  migr_state->qps[i];

		qsize = migr_qp.cq.size;
		if (qsize) {
			struct nvmf_vfio_user_cq *cq;

			/* restore cq */
			cqid = migr_qp.sq.cqid;
			assert(cqid == i);

			/* allocate cq if necessary */
			if (vu_ctrlr->cqs[cqid] == NULL) {
				ret = init_cq(vu_ctrlr, cqid);
				if (ret) {
					SPDK_ERRLOG("Construct qpair with qid %u failed\n", cqid);
					return -EFAULT;
				}
			}

			cq = vu_ctrlr->cqs[cqid];

			cq->size = qsize;

			cq->cq_state = VFIO_USER_CQ_CREATED;
			cq->cq_ref = cqs_ref[cqid];
			*cq_tailp(cq) = migr_qp.cq.tail;
			cq->mapping.prp1 = migr_qp.cq.dma_addr;
			cq->ien = migr_qp.cq.ien;
			cq->iv = migr_qp.cq.iv;
			cq->phase = migr_qp.cq.phase;
			addr = map_one(vu_ctrlr->endpoint->vfu_ctx,
				       cq->mapping.prp1, cq->size * 16,
				       cq->mapping.sg, &cq->mapping.iov,
				       PROT_READ | PROT_WRITE);
			if (addr == NULL) {
				SPDK_ERRLOG("Restore cq with qid %u PRP1 0x%"PRIx64" with size %u failed\n",
					    cqid, cq->mapping.prp1, cq->size);
				return -EFAULT;
			}
		}
	}

	return 0;
}

static int
vfio_user_migr_ctrlr_restore(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct nvmf_vfio_user_endpoint *endpoint = vu_ctrlr->endpoint;
	struct spdk_nvmf_ctrlr *ctrlr = vu_ctrlr->ctrlr;
	uint32_t *doorbell_base;
	struct spdk_nvme_cmd cmd;
	uint16_t i;
	int rc = 0;
	struct vfio_user_nvme_migr_state migr_state = {
		.nvmf_data = {
			.data_size = offsetof(struct spdk_nvmf_ctrlr_migr_data, unused),
			.regs_size = sizeof(struct spdk_nvmf_registers),
			.feat_size = sizeof(struct spdk_nvmf_ctrlr_feat)
		}
	};

	assert(endpoint->migr_data != NULL);
	assert(ctrlr != NULL);
	rc = vfio_user_migr_stream_to_data(endpoint, &migr_state);
	if (rc) {
		return rc;
	}

	/* restore shadow doorbells */
	if (migr_state.ctrlr_header.sdbl) {
		struct nvmf_vfio_user_shadow_doorbells *sdbl;
		sdbl = map_sdbl(vu_ctrlr->endpoint->vfu_ctx,
				migr_state.ctrlr_header.shadow_doorbell_buffer,
				migr_state.ctrlr_header.eventidx_buffer,
				memory_page_size(vu_ctrlr));
		if (sdbl == NULL) {
			SPDK_ERRLOG("%s: failed to re-map shadow doorbell buffers\n",
				    ctrlr_id(vu_ctrlr));
			return -1;
		}

		vu_ctrlr->shadow_doorbell_buffer = migr_state.ctrlr_header.shadow_doorbell_buffer;
		vu_ctrlr->eventidx_buffer = migr_state.ctrlr_header.eventidx_buffer;

		SWAP(vu_ctrlr->sdbl, sdbl);
	}

	rc = vfio_user_migr_ctrlr_construct_qps(vu_ctrlr, &migr_state);
	if (rc) {
		return rc;
	}

	/* restore PCI configuration space */
	memcpy((void *)endpoint->pci_config_space, &migr_state.cfg, NVME_REG_CFG_SIZE);

	doorbell_base = (uint32_t *)&migr_state.doorbells;
	/* restore doorbells from saved registers */
	memcpy((void *)vu_ctrlr->bar0_doorbells, doorbell_base, NVMF_VFIO_USER_DOORBELLS_SIZE);

	/* restore nvmf controller data */
	rc = spdk_nvmf_ctrlr_restore_migr_data(ctrlr, &migr_state.nvmf_data);
	if (rc) {
		return rc;
	}

	/* resubmit pending AERs */
	for (i = 0; i < migr_state.nvmf_data.num_aer_cids; i++) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s AER resubmit, CID %u\n", ctrlr_id(vu_ctrlr),
			      migr_state.nvmf_data.aer_cids[i]);
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
		cmd.cid = migr_state.nvmf_data.aer_cids[i];
		rc = handle_cmd_req(vu_ctrlr, &cmd, vu_ctrlr->sqs[0]);
		if (spdk_unlikely(rc)) {
			break;
		}
	}

	return rc;
}

static void
vfio_user_migr_ctrlr_enable_sqs(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	uint32_t i;
	struct nvmf_vfio_user_sq *sq;

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */

	if (vu_ctrlr->sqs[0] != NULL) {
		vu_ctrlr->sqs[0]->dbl_tailp = vu_ctrlr->bar0_doorbells +
					      queue_index(0, false);
	}

	if (vu_ctrlr->cqs[0] != NULL) {
		vu_ctrlr->cqs[0]->dbl_headp = vu_ctrlr->bar0_doorbells +
					      queue_index(0, true);
	}

	vfio_user_ctrlr_switch_doorbells(vu_ctrlr, vu_ctrlr->sdbl != NULL);

	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		sq = vu_ctrlr->sqs[i];
		if (!sq || !sq->size) {
			continue;
		}

		if (nvmf_qpair_is_admin_queue(&sq->qpair)) {
			/* ADMIN queue pair is always in the poll group, just enable it */
			sq->sq_state = VFIO_USER_SQ_ACTIVE;
		} else {
			spdk_nvmf_tgt_new_qpair(vu_ctrlr->transport->transport.tgt, &sq->qpair);
		}
	}
}

/*
 * We are in stop-and-copy state, but still potentially have some current dirty
 * sgls: while we're quiesced and thus should have no active requests, we still
 * have potentially dirty maps of the shadow doorbells and the CQs (SQs are
 * mapped read only).
 *
 * Since we won't be calling vfu_sgl_put() for them, we need to explicitly
 * mark them dirty now.
 */
static void
vfio_user_migr_ctrlr_mark_dirty(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct nvmf_vfio_user_endpoint *endpoint = vu_ctrlr->endpoint;

	assert(vu_ctrlr->state == VFIO_USER_CTRLR_MIGRATING);

	for (size_t i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		struct nvmf_vfio_user_cq *cq = vu_ctrlr->cqs[i];

		if (cq == NULL || q_addr(&cq->mapping) == NULL) {
			continue;
		}

		vfu_sgl_mark_dirty(endpoint->vfu_ctx, cq->mapping.sg, 1);
	}

	if (vu_ctrlr->sdbl != NULL) {
		dma_sg_t *sg;
		size_t i;

		for (i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT;
		     ++i) {

			if (!vu_ctrlr->sdbl->iovs[i].iov_len) {
				continue;
			}

			sg = index_to_sg_t(vu_ctrlr->sdbl->sgs, i);

			vfu_sgl_mark_dirty(endpoint->vfu_ctx, sg, 1);
		}
	}
}

static int
vfio_user_migration_device_state_transition(vfu_ctx_t *vfu_ctx, vfu_migr_state_t state)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	struct nvmf_vfio_user_sq *sq;
	int ret = 0;

	SPDK_DEBUGLOG(nvmf_vfio, "%s controller state %u, migration state %u\n", endpoint_id(endpoint),
		      vu_ctrlr->state, state);

	switch (state) {
	case VFU_MIGR_STATE_STOP_AND_COPY:
		vu_ctrlr->in_source_vm = true;
		vu_ctrlr->state = VFIO_USER_CTRLR_MIGRATING;
		vfio_user_migr_ctrlr_mark_dirty(vu_ctrlr);
		vfio_user_migr_ctrlr_save_data(vu_ctrlr);
		break;
	case VFU_MIGR_STATE_STOP:
		vu_ctrlr->state = VFIO_USER_CTRLR_MIGRATING;
		/* The controller associates with source VM is dead now, we will resume
		 * the subsystem after destroying the controller data structure, then the
		 * subsystem can be re-used for another new client.
		 */
		if (vu_ctrlr->in_source_vm) {
			endpoint->need_resume = true;
		}
		break;
	case VFU_MIGR_STATE_PRE_COPY:
		assert(vu_ctrlr->state == VFIO_USER_CTRLR_PAUSED);
		break;
	case VFU_MIGR_STATE_RESUME:
		/*
		 * Destination ADMIN queue pair is connected when starting the VM,
		 * but the ADMIN queue pair isn't enabled in destination VM, the poll
		 * group will do nothing to ADMIN queue pair for now.
		 */
		if (vu_ctrlr->state != VFIO_USER_CTRLR_RUNNING) {
			break;
		}

		assert(!vu_ctrlr->in_source_vm);
		vu_ctrlr->state = VFIO_USER_CTRLR_MIGRATING;

		sq = TAILQ_FIRST(&vu_ctrlr->connected_sqs);
		assert(sq != NULL);
		assert(sq->qpair.qid == 0);
		sq->sq_state = VFIO_USER_SQ_INACTIVE;

		/* Free ADMIN SQ resources first, SQ resources will be
		 * allocated based on queue size from source VM.
		 */
		free_sq_reqs(sq);
		sq->size = 0;
		break;
	case VFU_MIGR_STATE_RUNNING:

		if (vu_ctrlr->state != VFIO_USER_CTRLR_MIGRATING) {
			break;
		}

		if (!vu_ctrlr->in_source_vm) {
			/* Restore destination VM from BAR9 */
			ret = vfio_user_migr_ctrlr_restore(vu_ctrlr);
			if (ret) {
				break;
			}

			vfio_user_ctrlr_switch_doorbells(vu_ctrlr, false);
			vfio_user_migr_ctrlr_enable_sqs(vu_ctrlr);
			vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;
			/* FIXME where do we resume nvmf? */
		} else {
			/* Rollback source VM */
			vu_ctrlr->state = VFIO_USER_CTRLR_RESUMING;
			ret = spdk_nvmf_subsystem_resume((struct spdk_nvmf_subsystem *)endpoint->subsystem,
							 vfio_user_endpoint_resume_done, endpoint);
			if (ret < 0) {
				/* TODO: fail controller with CFS bit set */
				vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
				SPDK_ERRLOG("%s: failed to resume, ret=%d\n", endpoint_id(endpoint), ret);
			}
		}
		vu_ctrlr->migr_data_prepared = false;
		vu_ctrlr->in_source_vm = false;
		break;

	default:
		return -EINVAL;
	}

	return ret;
}

static uint64_t
vfio_user_migration_get_pending_bytes(vfu_ctx_t *vfu_ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr = endpoint->ctrlr;
	uint64_t pending_bytes;

	if (ctrlr->migr_data_prepared) {
		assert(ctrlr->state == VFIO_USER_CTRLR_MIGRATING);
		pending_bytes = 0;
	} else {
		pending_bytes = vfio_user_migr_data_len();
	}

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s current state %u, pending bytes 0x%"PRIx64"\n",
		      endpoint_id(endpoint), ctrlr->state, pending_bytes);

	return pending_bytes;
}

static int
vfio_user_migration_prepare_data(vfu_ctx_t *vfu_ctx, uint64_t *offset, uint64_t *size)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr = endpoint->ctrlr;

	/*
	 * When transitioning to pre-copy state we set pending_bytes to 0,
	 * so the vfio-user client shouldn't attempt to read any migration
	 * data. This is not yet guaranteed by libvfio-user.
	 */
	if (ctrlr->state != VFIO_USER_CTRLR_MIGRATING) {
		assert(size != NULL);
		*offset = 0;
		*size = 0;
		return 0;
	}

	if (ctrlr->in_source_vm) { /* migration source */
		assert(size != NULL);
		*size = vfio_user_migr_data_len();
		vfio_user_migr_ctrlr_save_data(ctrlr);
	} else { /* migration destination */
		assert(size == NULL);
		assert(!ctrlr->migr_data_prepared);
	}
	*offset = 0;
	ctrlr->migr_data_prepared = true;

	SPDK_DEBUGLOG(nvmf_vfio, "%s current state %u\n", endpoint_id(endpoint), ctrlr->state);

	return 0;
}

static ssize_t
vfio_user_migration_read_data(vfu_ctx_t *vfu_ctx __attribute__((unused)),
			      void *buf __attribute__((unused)),
			      uint64_t count __attribute__((unused)),
			      uint64_t offset __attribute__((unused)))
{
	SPDK_DEBUGLOG(nvmf_vfio, "%s: migration read data not supported\n",
		      endpoint_id(vfu_get_private(vfu_ctx)));
	errno = ENOTSUP;
	return -1;
}

static ssize_t
vfio_user_migration_write_data(vfu_ctx_t *vfu_ctx __attribute__((unused)),
			       void *buf __attribute__((unused)),
			       uint64_t count __attribute__((unused)),
			       uint64_t offset __attribute__((unused)))
{
	SPDK_DEBUGLOG(nvmf_vfio, "%s: migration write data not supported\n",
		      endpoint_id(vfu_get_private(vfu_ctx)));
	errno = ENOTSUP;
	return -1;
}

static int
vfio_user_migration_data_written(vfu_ctx_t *vfu_ctx __attribute__((unused)),
				 uint64_t count)
{
	SPDK_DEBUGLOG(nvmf_vfio, "write 0x%"PRIx64"\n", (uint64_t)count);

	if (count != vfio_user_migr_data_len()) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s bad count %#lx\n",
			      endpoint_id(vfu_get_private(vfu_ctx)), count);
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static int
vfio_user_dev_info_fill(struct nvmf_vfio_user_transport *vu_transport,
			struct nvmf_vfio_user_endpoint *endpoint)
{
	int ret;
	ssize_t cap_offset;
	vfu_ctx_t *vfu_ctx = endpoint->vfu_ctx;
	struct iovec migr_sparse_mmap = {};

	struct pmcap pmcap = { .hdr.id = PCI_CAP_ID_PM, .pmcs.nsfrst = 0x1 };
	struct pxcap pxcap = {
		.hdr.id = PCI_CAP_ID_EXP,
		.pxcaps.ver = 0x2,
		.pxdcap = {.rer = 0x1, .flrc = 0x1},
		.pxdcap2.ctds = 0x1
	};

	struct msixcap msixcap = {
		.hdr.id = PCI_CAP_ID_MSIX,
		.mxc.ts = NVME_IRQ_MSIX_NUM - 1,
		.mtab = {.tbir = 0x4, .to = 0x0},
		.mpba = {.pbir = 0x5, .pbao = 0x0}
	};

	struct iovec sparse_mmap[] = {
		{
			.iov_base = (void *)NVME_DOORBELLS_OFFSET,
			.iov_len = NVMF_VFIO_USER_DOORBELLS_SIZE,
		},
	};

	const vfu_migration_callbacks_t migr_callbacks = {
		.version = VFU_MIGR_CALLBACKS_VERS,
		.transition = &vfio_user_migration_device_state_transition,
		.get_pending_bytes = &vfio_user_migration_get_pending_bytes,
		.prepare_data = &vfio_user_migration_prepare_data,
		.read_data = &vfio_user_migration_read_data,
		.data_written = &vfio_user_migration_data_written,
		.write_data = &vfio_user_migration_write_data
	};

	ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to initialize PCI\n", vfu_ctx);
		return ret;
	}
	vfu_pci_set_id(vfu_ctx, SPDK_PCI_VID_NUTANIX, 0x0001, SPDK_PCI_VID_NUTANIX, 0);
	/*
	 * 0x02, controller uses the NVM Express programming interface
	 * 0x08, non-volatile memory controller
	 * 0x01, mass storage controller
	 */
	vfu_pci_set_class(vfu_ctx, 0x01, 0x08, 0x02);

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &pmcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pmcap\n", vfu_ctx);
		return ret;
	}

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &pxcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pxcap\n", vfu_ctx);
		return ret;
	}

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &msixcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add msixcap\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_CFG_REGION_IDX, NVME_REG_CFG_SIZE,
			       access_pci_config, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup cfg\n", vfu_ctx);
		return ret;
	}

	if (vu_transport->transport_opts.disable_mappable_bar0) {
		ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_REG_BAR0_SIZE,
				       access_bar0_fn, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
				       NULL, 0, -1, 0);
	} else {
		ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_REG_BAR0_SIZE,
				       access_bar0_fn, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
				       sparse_mmap, 1, endpoint->devmem_fd, 0);
	}

	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 0\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR4_REGION_IDX, NVME_BAR4_SIZE,
			       NULL, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 4\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR5_REGION_IDX, NVME_BAR5_SIZE,
			       NULL, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 5\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_dma(vfu_ctx, memory_region_add_cb, memory_region_remove_cb);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup dma callback\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_reset_cb(vfu_ctx, vfio_user_device_reset);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup reset callback\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_INTX_IRQ, 1);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup INTX\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSIX_IRQ, NVME_IRQ_MSIX_NUM);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup MSIX\n", vfu_ctx);
		return ret;
	}

	vfu_setup_device_quiesce_cb(vfu_ctx, vfio_user_dev_quiesce_cb);

	migr_sparse_mmap.iov_base = (void *)4096;
	migr_sparse_mmap.iov_len = vfio_user_migr_data_len();
	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_MIGR_REGION_IDX,
			       vfu_get_migr_register_area_size() + vfio_user_migr_data_len(),
			       NULL, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, &migr_sparse_mmap,
			       1, endpoint->migr_fd, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup migration region\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_migration_callbacks(vfu_ctx, &migr_callbacks,
			vfu_get_migr_register_area_size());
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup migration callbacks\n", vfu_ctx);
		return ret;
	}

	ret = vfu_realize_ctx(vfu_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to realize\n", vfu_ctx);
		return ret;
	}

	endpoint->pci_config_space = vfu_pci_get_config_space(endpoint->vfu_ctx);
	assert(endpoint->pci_config_space != NULL);
	init_pci_config_space(endpoint->pci_config_space);

	assert(cap_offset != 0);
	endpoint->msix = (struct msixcap *)((uint8_t *)endpoint->pci_config_space + cap_offset);

	return 0;
}

static int nvmf_vfio_user_accept(void *ctx);

static void
set_intr_mode_noop(struct spdk_poller *poller, void *arg, bool interrupt_mode)
{
	/* Nothing for us to do here. */
}

/*
 * Register an "accept" poller: this is polling for incoming vfio-user socket
 * connections (on the listening socket).
 *
 * We need to do this on first listening, and also after destroying a
 * controller, so we can accept another connection.
 */
static int
vfio_user_register_accept_poller(struct nvmf_vfio_user_endpoint *endpoint)
{
	uint64_t poll_rate_us = endpoint->transport->transport.opts.acceptor_poll_rate;

	SPDK_DEBUGLOG(nvmf_vfio, "registering accept poller\n");

	endpoint->accept_poller = SPDK_POLLER_REGISTER(nvmf_vfio_user_accept,
				  endpoint, poll_rate_us);

	if (!endpoint->accept_poller) {
		return -1;
	}

	endpoint->accept_thread = spdk_get_thread();
	endpoint->need_relisten = false;

	if (!spdk_interrupt_mode_is_enabled()) {
		return 0;
	}

	endpoint->accept_intr_fd = vfu_get_poll_fd(endpoint->vfu_ctx);
	assert(endpoint->accept_intr_fd != -1);

	endpoint->accept_intr = SPDK_INTERRUPT_REGISTER(endpoint->accept_intr_fd,
				nvmf_vfio_user_accept, endpoint);

	assert(endpoint->accept_intr != NULL);

	spdk_poller_register_interrupt(endpoint->accept_poller,
				       set_intr_mode_noop, NULL);
	return 0;
}

static void
_vfio_user_relisten(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;

	vfio_user_register_accept_poller(endpoint);
}

static void
_free_ctrlr(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = ctrlr->endpoint;

	free_sdbl(endpoint->vfu_ctx, ctrlr->sdbl);

	spdk_interrupt_unregister(&ctrlr->intr);
	ctrlr->intr_fd = -1;
	spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

	free(ctrlr);

	if (endpoint->need_async_destroy) {
		nvmf_vfio_user_destroy_endpoint(endpoint);
	} else if (endpoint->need_relisten) {
		spdk_thread_send_msg(endpoint->accept_thread,
				     _vfio_user_relisten, endpoint);
	}
}

static void
free_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	int i;
	assert(ctrlr != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "free %s\n", ctrlr_id(ctrlr));

	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		free_qp(ctrlr, i);
	}

	spdk_thread_exec_msg(ctrlr->thread, _free_ctrlr, ctrlr);
}

static int
nvmf_vfio_user_create_ctrlr(struct nvmf_vfio_user_transport *transport,
			    struct nvmf_vfio_user_endpoint *endpoint)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int err = 0;

	SPDK_DEBUGLOG(nvmf_vfio, "%s\n", endpoint_id(endpoint));

	/* First, construct a vfio-user CUSTOM transport controller */
	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		err = -ENOMEM;
		goto out;
	}
	/* We can only support one connection for now */
	ctrlr->cntlid = 0x1;
	ctrlr->intr_fd = -1;
	ctrlr->transport = transport;
	ctrlr->endpoint = endpoint;
	ctrlr->bar0_doorbells = endpoint->bar0_doorbells;
	TAILQ_INIT(&ctrlr->connected_sqs);

	ctrlr->adaptive_irqs_enabled =
		!transport->transport_opts.disable_adaptive_irq;

	/* Then, construct an admin queue pair */
	err = init_sq(ctrlr, &transport->transport, 0);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}

	err = init_cq(ctrlr, 0);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}

	ctrlr->sqs[0]->size = NVMF_VFIO_USER_DEFAULT_AQ_DEPTH;

	err = alloc_sq_reqs(ctrlr, ctrlr->sqs[0]);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}
	endpoint->ctrlr = ctrlr;

	/* Notify the generic layer about the new admin queue pair */
	spdk_nvmf_tgt_new_qpair(transport->transport.tgt, &ctrlr->sqs[0]->qpair);

out:
	if (err != 0) {
		SPDK_ERRLOG("%s: failed to create vfio-user controller: %s\n",
			    endpoint_id(endpoint), strerror(-err));
	}

	return err;
}

static int
nvmf_vfio_user_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvmf_listen_opts *listen_opts)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;
	char path[PATH_MAX] = {};
	char uuid[PATH_MAX] = {};
	int ret;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		/* Only compare traddr */
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			pthread_mutex_unlock(&vu_transport->lock);
			return -EEXIST;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	endpoint = calloc(1, sizeof(*endpoint));
	if (!endpoint) {
		return -ENOMEM;
	}

	pthread_mutex_init(&endpoint->lock, NULL);
	endpoint->devmem_fd = -1;
	memcpy(&endpoint->trid, trid, sizeof(endpoint->trid));
	endpoint->transport = vu_transport;

	ret = snprintf(path, PATH_MAX, "%s/bar0", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get socket path: %s.\n", endpoint_id(endpoint), spdk_strerror(errno));
		ret = -1;
		goto out;
	}

	ret = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (ret == -1) {
		SPDK_ERRLOG("%s: failed to open device memory at %s: %s.\n",
			    endpoint_id(endpoint), path, spdk_strerror(errno));
		goto out;
	}
	unlink(path);

	endpoint->devmem_fd = ret;
	ret = ftruncate(endpoint->devmem_fd,
			NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE);
	if (ret != 0) {
		SPDK_ERRLOG("%s: error to ftruncate file %s: %s.\n", endpoint_id(endpoint), path,
			    spdk_strerror(errno));
		goto out;
	}

	endpoint->bar0_doorbells = mmap(NULL, NVMF_VFIO_USER_DOORBELLS_SIZE,
					PROT_READ | PROT_WRITE, MAP_SHARED, endpoint->devmem_fd, NVME_DOORBELLS_OFFSET);
	if (endpoint->bar0_doorbells == MAP_FAILED) {
		SPDK_ERRLOG("%s: error to mmap file %s: %s.\n", endpoint_id(endpoint), path, spdk_strerror(errno));
		endpoint->bar0_doorbells = NULL;
		ret = -1;
		goto out;
	}

	ret = snprintf(path, PATH_MAX, "%s/migr", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get migration file path: %s.\n", endpoint_id(endpoint),
			    spdk_strerror(errno));
		ret = -1;
		goto out;
	}
	ret = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (ret == -1) {
		SPDK_ERRLOG("%s: failed to open device memory at %s: %s.\n",
			    endpoint_id(endpoint), path, spdk_strerror(errno));
		goto out;
	}
	unlink(path);

	endpoint->migr_fd = ret;
	ret = ftruncate(endpoint->migr_fd,
			vfu_get_migr_register_area_size() + vfio_user_migr_data_len());
	if (ret != 0) {
		SPDK_ERRLOG("%s: error to ftruncate migration file %s: %s.\n", endpoint_id(endpoint), path,
			    spdk_strerror(errno));
		goto out;
	}

	endpoint->migr_data = mmap(NULL, vfio_user_migr_data_len(),
				   PROT_READ | PROT_WRITE, MAP_SHARED, endpoint->migr_fd, vfu_get_migr_register_area_size());
	if (endpoint->migr_data == MAP_FAILED) {
		SPDK_ERRLOG("%s: error to mmap file %s: %s.\n", endpoint_id(endpoint), path, spdk_strerror(errno));
		endpoint->migr_data = NULL;
		ret = -1;
		goto out;
	}

	ret = snprintf(uuid, PATH_MAX, "%s/cntrl", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get ctrlr file path: %s\n", endpoint_id(endpoint), spdk_strerror(errno));
		ret = -1;
		goto out;
	}

	endpoint->vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, uuid, LIBVFIO_USER_FLAG_ATTACH_NB,
					   endpoint, VFU_DEV_TYPE_PCI);
	if (endpoint->vfu_ctx == NULL) {
		SPDK_ERRLOG("%s: error creating libmuser context: %m\n",
			    endpoint_id(endpoint));
		ret = -1;
		goto out;
	}

	ret = vfu_setup_log(endpoint->vfu_ctx, vfio_user_log,
			    vfio_user_get_log_level());
	if (ret < 0) {
		goto out;
	}


	ret = vfio_user_dev_info_fill(vu_transport, endpoint);
	if (ret < 0) {
		goto out;
	}

	ret = vfio_user_register_accept_poller(endpoint);

	if (ret != 0) {
		goto out;
	}

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_INSERT_TAIL(&vu_transport->endpoints, endpoint, link);
	pthread_mutex_unlock(&vu_transport->lock);

out:
	if (ret != 0) {
		nvmf_vfio_user_destroy_endpoint(endpoint);
	}

	return ret;
}

static void
nvmf_vfio_user_stop_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;

	assert(trid != NULL);
	assert(trid->traddr != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: stop listen\n", trid->traddr);

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		if (strcmp(trid->traddr, endpoint->trid.traddr) == 0) {
			TAILQ_REMOVE(&vu_transport->endpoints, endpoint, link);
			/* Defer to free endpoint resources until the controller
			 * is freed.  There are two cases when running here:
			 * 1. kill nvmf target while VM is connected
			 * 2. remove listener via RPC call
			 * nvmf library will disconnect all queue paris.
			 */
			if (endpoint->ctrlr) {
				assert(!endpoint->need_async_destroy);
				endpoint->need_async_destroy = true;
				pthread_mutex_unlock(&vu_transport->lock);
				return;
			}

			nvmf_vfio_user_destroy_endpoint(endpoint);
			pthread_mutex_unlock(&vu_transport->lock);
			return;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: not found\n", trid->traddr);
}

static void
nvmf_vfio_user_cdata_init(struct spdk_nvmf_transport *transport,
			  struct spdk_nvmf_subsystem *subsystem,
			  struct spdk_nvmf_ctrlr_data *cdata)
{
	struct nvmf_vfio_user_transport *vu_transport;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	cdata->vid = SPDK_PCI_VID_NUTANIX;
	cdata->ssvid = SPDK_PCI_VID_NUTANIX;
	cdata->ieee[0] = 0x8d;
	cdata->ieee[1] = 0x6b;
	cdata->ieee[2] = 0x50;
	memset(&cdata->sgls, 0, sizeof(struct spdk_nvme_cdata_sgls));
	cdata->sgls.supported = SPDK_NVME_SGLS_SUPPORTED_DWORD_ALIGNED;
	cdata->oncs.compare = !vu_transport->transport_opts.disable_compare;
	/* libvfio-user can only support 1 connection for now */
	cdata->oncs.reservations = 0;
	cdata->oacs.doorbell_buffer_config = !vu_transport->transport_opts.disable_shadow_doorbells;
	cdata->fuses.compare_and_write = !vu_transport->transport_opts.disable_compare;
}

static int
nvmf_vfio_user_listen_associate(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem,
				const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH(endpoint, &vu_transport->endpoints, link) {
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			break;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	if (endpoint == NULL) {
		return -ENOENT;
	}

	/* Drop const - we will later need to pause/unpause. */
	endpoint->subsystem = (struct spdk_nvmf_subsystem *)subsystem;

	return 0;
}

/*
 * Executed periodically at a default SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US
 * frequency.
 *
 * For this endpoint (which at the libvfio-user level corresponds to a socket),
 * if we don't currently have a controller set up, peek to see if the socket is
 * able to accept a new connection.
 */
static int
nvmf_vfio_user_accept(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_transport *vu_transport;
	int err;

	vu_transport = endpoint->transport;

	if (endpoint->ctrlr != NULL) {
		return SPDK_POLLER_IDLE;
	}

	/* While we're here, the controller is already destroyed,
	 * subsystem may still be in RESUMING state, we will wait
	 * until the subsystem is in RUNNING state.
	 */
	if (endpoint->need_resume) {
		return SPDK_POLLER_IDLE;
	}

	err = vfu_attach_ctx(endpoint->vfu_ctx);
	if (err == 0) {
		SPDK_DEBUGLOG(nvmf_vfio, "attach succeeded\n");
		err = nvmf_vfio_user_create_ctrlr(vu_transport, endpoint);
		if (err == 0) {
			/*
			 * Unregister ourselves: now we've accepted a
			 * connection, there is nothing for us to poll for, and
			 * we will poll the connection via vfu_run_ctx()
			 * instead.
			 */
			spdk_interrupt_unregister(&endpoint->accept_intr);
			spdk_poller_unregister(&endpoint->accept_poller);
		}
		return SPDK_POLLER_BUSY;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

static void
nvmf_vfio_user_discover(struct spdk_nvmf_transport *transport,
			struct spdk_nvme_transport_id *trid,
			struct spdk_nvmf_discovery_log_page_entry *entry)
{ }

static int vfio_user_poll_group_intr(void *ctx);

static void
vfio_user_poll_group_add_intr(struct nvmf_vfio_user_poll_group *vu_group,
			      struct spdk_nvmf_poll_group *group)
{
	vu_group->intr_fd = eventfd(0, EFD_NONBLOCK);
	assert(vu_group->intr_fd != -1);

	vu_group->intr = SPDK_INTERRUPT_REGISTER(vu_group->intr_fd,
			 vfio_user_poll_group_intr, vu_group);
	assert(vu_group->intr != NULL);

	spdk_poller_register_interrupt(group->poller, set_intr_mode_noop,
				       vu_group);
}

static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_poll_group *vu_group;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	SPDK_DEBUGLOG(nvmf_vfio, "create poll group\n");

	vu_group = calloc(1, sizeof(*vu_group));
	if (vu_group == NULL) {
		SPDK_ERRLOG("Error allocating poll group: %m");
		return NULL;
	}

	if (in_interrupt_mode(vu_transport)) {
		vfio_user_poll_group_add_intr(vu_group, group);
	}

	TAILQ_INIT(&vu_group->sqs);

	pthread_mutex_lock(&vu_transport->pg_lock);
	TAILQ_INSERT_TAIL(&vu_transport->poll_groups, vu_group, link);
	if (vu_transport->next_pg == NULL) {
		vu_transport->next_pg = vu_group;
	}
	pthread_mutex_unlock(&vu_transport->pg_lock);

	return &vu_group->group;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_poll_group **vu_group;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	struct spdk_nvmf_transport_poll_group *result = NULL;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	cq = sq->ctrlr->cqs[sq->cqid];
	assert(cq != NULL);
	vu_transport = SPDK_CONTAINEROF(qpair->transport, struct nvmf_vfio_user_transport, transport);

	pthread_mutex_lock(&vu_transport->pg_lock);
	if (TAILQ_EMPTY(&vu_transport->poll_groups)) {
		goto out;
	}

	if (!nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * If this is shared IO CQ case, just return the used CQ's poll
		 * group, so I/O completions don't have to use
		 * spdk_thread_send_msg().
		 */
		if (cq->group != NULL) {
			result = cq->group;
			goto out;
		}

		/*
		 * If we're in interrupt mode, align all qpairs for a controller
		 * on the same poll group by default, unless requested. This can
		 * be lower in performance than running on a single poll group,
		 * so we disable spreading by default.
		 */
		if (in_interrupt_mode(vu_transport) &&
		    !vu_transport->transport_opts.enable_intr_mode_sq_spreading) {
			result = sq->ctrlr->sqs[0]->group;
			goto out;
		}

	}

	vu_group = &vu_transport->next_pg;
	assert(*vu_group != NULL);

	result = &(*vu_group)->group;
	*vu_group = TAILQ_NEXT(*vu_group, link);
	if (*vu_group == NULL) {
		*vu_group = TAILQ_FIRST(&vu_transport->poll_groups);
	}

out:
	if (cq->group == NULL) {
		cq->group = result;
	}

	pthread_mutex_unlock(&vu_transport->pg_lock);
	return result;
}

static void
vfio_user_poll_group_del_intr(struct nvmf_vfio_user_poll_group *vu_group)
{
	assert(vu_group->intr_fd != -1);

	spdk_interrupt_unregister(&vu_group->intr);

	close(vu_group->intr_fd);
	vu_group->intr_fd = -1;
}

/* called when process exits */
static void
nvmf_vfio_user_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group, *next_tgroup;
	struct nvmf_vfio_user_transport *vu_transport;

	SPDK_DEBUGLOG(nvmf_vfio, "destroy poll group\n");

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);
	vu_transport = SPDK_CONTAINEROF(vu_group->group.transport, struct nvmf_vfio_user_transport,
					transport);

	if (in_interrupt_mode(vu_transport)) {
		vfio_user_poll_group_del_intr(vu_group);
	}

	pthread_mutex_lock(&vu_transport->pg_lock);
	next_tgroup = TAILQ_NEXT(vu_group, link);
	TAILQ_REMOVE(&vu_transport->poll_groups, vu_group, link);
	if (next_tgroup == NULL) {
		next_tgroup = TAILQ_FIRST(&vu_transport->poll_groups);
	}
	if (vu_transport->next_pg == vu_group) {
		vu_transport->next_pg = next_tgroup;
	}
	pthread_mutex_unlock(&vu_transport->pg_lock);

	free(vu_group);
}

static void
_vfio_user_qpair_disconnect(void *ctx)
{
	struct nvmf_vfio_user_sq *sq = ctx;

	spdk_nvmf_qpair_disconnect(&sq->qpair, NULL, NULL);
}

/* The function is used when socket connection is destroyed */
static int
vfio_user_destroy_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_endpoint *endpoint;

	SPDK_DEBUGLOG(nvmf_vfio, "%s stop processing\n", ctrlr_id(ctrlr));

	endpoint = ctrlr->endpoint;
	assert(endpoint != NULL);

	pthread_mutex_lock(&endpoint->lock);
	endpoint->need_relisten = true;
	ctrlr->disconnect = true;
	if (TAILQ_EMPTY(&ctrlr->connected_sqs)) {
		endpoint->ctrlr = NULL;
		free_ctrlr(ctrlr);
		pthread_mutex_unlock(&endpoint->lock);
		return 0;
	}

	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		/* add another round thread poll to avoid recursive endpoint lock */
		spdk_thread_send_msg(ctrlr->thread, _vfio_user_qpair_disconnect, sq);
	}
	pthread_mutex_unlock(&endpoint->lock);

	return 0;
}

/*
 * Poll for and process any incoming vfio-user messages.
 */
static int
vfio_user_poll_vfu_ctx(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	int ret;

	assert(ctrlr != NULL);

	/* This will call access_bar0_fn() if there are any writes
	 * to the portion of the BAR that is not mmap'd */
	ret = vfu_run_ctx(ctrlr->endpoint->vfu_ctx);
	if (spdk_unlikely(ret == -1)) {
		if (errno == EBUSY) {
			return SPDK_POLLER_IDLE;
		}

		spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

		/*
		 * We lost the client; the reset callback will already have
		 * unregistered the interrupt.
		 */
		if (errno == ENOTCONN) {
			vfio_user_destroy_ctrlr(ctrlr);
			return SPDK_POLLER_BUSY;
		}

		/*
		 * We might not have got a reset callback in this case, so
		 * explicitly unregister the interrupt here.
		 */
		spdk_interrupt_unregister(&ctrlr->intr);
		ctrlr->intr_fd = -1;
		fail_ctrlr(ctrlr);
	}

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

struct vfio_user_post_cpl_ctx {
	struct nvmf_vfio_user_ctrlr	*ctrlr;
	struct nvmf_vfio_user_cq	*cq;
	struct spdk_nvme_cpl		cpl;
};

static void
_post_completion_msg(void *ctx)
{
	struct vfio_user_post_cpl_ctx *cpl_ctx = ctx;

	post_completion(cpl_ctx->ctrlr, cpl_ctx->cq, cpl_ctx->cpl.cdw0, cpl_ctx->cpl.sqid,
			cpl_ctx->cpl.cid, cpl_ctx->cpl.status.sc, cpl_ctx->cpl.status.sct);
	free(cpl_ctx);
}

static int nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

static int
vfio_user_poll_group_process(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_group = ctx;
	int ret = 0;

	SPDK_DEBUGLOG(vfio_user_db, "pg:%p got intr\n", vu_group);

	ret |= nvmf_vfio_user_poll_group_poll(&vu_group->group);

	/*
	 * Re-arm the event indexes. NB: this also could rearm other
	 * controller's SQs.
	 */
	ret |= vfio_user_poll_group_rearm(vu_group);

	vu_group->stats.pg_process_count++;
	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
vfio_user_poll_group_intr(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_group = ctx;
	eventfd_t val;

	eventfd_read(vu_group->intr_fd, &val);

	vu_group->stats.intr++;

	return vfio_user_poll_group_process(ctx);
}

/*
 * Handle an interrupt for the given controller: we must poll the vfu_ctx, and
 * the SQs assigned to our own poll group. Other poll groups are handled via
 * vfio_user_poll_group_intr().
 */
static int
vfio_user_ctrlr_intr(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx;
	struct nvmf_vfio_user_poll_group *vu_group;
	int ret = SPDK_POLLER_IDLE;

	vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);

	SPDK_DEBUGLOG(vfio_user_db, "ctrlr pg:%p got intr\n", vu_ctrlr_group);

	vu_ctrlr_group->stats.ctrlr_intr++;

	/*
	 * Poll vfio-user for this controller. We need to do this before polling
	 * any SQs, as this is where doorbell writes may be handled.
	 */
	ret = vfio_user_poll_vfu_ctx(vu_ctrlr);

	/*
	 * `sqs[0]` could be set to NULL in vfio_user_poll_vfu_ctx() context,
	 * just return for this case.
	 */
	if (vu_ctrlr->sqs[0] == NULL) {
		return ret;
	}

	if (vu_ctrlr->transport->transport_opts.enable_intr_mode_sq_spreading) {
		/*
		 * We may have just written to a doorbell owned by another
		 * reactor: we need to prod them to make sure its SQs are polled
		 * *after* the doorbell value is updated.
		 */
		TAILQ_FOREACH(vu_group, &vu_ctrlr->transport->poll_groups, link) {
			if (vu_group != vu_ctrlr_group) {
				SPDK_DEBUGLOG(vfio_user_db, "prodding pg:%p\n", vu_group);
				eventfd_write(vu_group->intr_fd, 1);
			}
		}
	}

	ret |= vfio_user_poll_group_process(vu_ctrlr_group);

	return ret;
}

static void
vfio_user_ctrlr_set_intr_mode(struct spdk_poller *poller, void *ctx,
			      bool interrupt_mode)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	assert(ctrlr != NULL);
	assert(ctrlr->endpoint != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: setting interrupt mode to %d\n",
		      ctrlr_id(ctrlr), interrupt_mode);

	/*
	 * interrupt_mode needs to persist across controller resets, so store
	 * it in the endpoint instead.
	 */
	ctrlr->endpoint->interrupt_mode = interrupt_mode;

	vfio_user_poll_group_rearm(ctrlr_to_poll_group(ctrlr));
}

/*
 * In response to the nvmf_vfio_user_create_ctrlr() path, the admin queue is now
 * set up and we can start operating on this controller.
 */
static void
start_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
	    struct spdk_nvmf_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_endpoint *endpoint = vu_ctrlr->endpoint;

	vu_ctrlr->ctrlr = ctrlr;
	vu_ctrlr->cntlid = ctrlr->cntlid;
	vu_ctrlr->thread = spdk_get_thread();
	vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;

	if (!in_interrupt_mode(endpoint->transport)) {
		vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx,
					   vu_ctrlr, 1000);
		return;
	}

	vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx,
				   vu_ctrlr, 0);

	vu_ctrlr->intr_fd = vfu_get_poll_fd(vu_ctrlr->endpoint->vfu_ctx);
	assert(vu_ctrlr->intr_fd != -1);

	vu_ctrlr->intr = SPDK_INTERRUPT_REGISTER(vu_ctrlr->intr_fd,
			 vfio_user_ctrlr_intr, vu_ctrlr);

	assert(vu_ctrlr->intr != NULL);

	spdk_poller_register_interrupt(vu_ctrlr->vfu_ctx_poller,
				       vfio_user_ctrlr_set_intr_mode,
				       vu_ctrlr);
}

static int
handle_queue_connect_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_sq *sq = cb_arg;
	struct nvmf_vfio_user_cq *admin_cq;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint;

	assert(sq != NULL);
	assert(req != NULL);

	vu_ctrlr = sq->ctrlr;
	assert(vu_ctrlr != NULL);
	endpoint = vu_ctrlr->endpoint;
	assert(endpoint != NULL);

	if (spdk_nvme_cpl_is_error(&req->req.rsp->nvme_cpl)) {
		SPDK_ERRLOG("SC %u, SCT %u\n", req->req.rsp->nvme_cpl.status.sc, req->req.rsp->nvme_cpl.status.sct);
		endpoint->ctrlr = NULL;
		free_ctrlr(vu_ctrlr);
		return -1;
	}

	vu_group = SPDK_CONTAINEROF(sq->group, struct nvmf_vfio_user_poll_group, group);
	TAILQ_INSERT_TAIL(&vu_group->sqs, sq, link);

	admin_cq = vu_ctrlr->cqs[0];
	assert(admin_cq != NULL);
	assert(admin_cq->group != NULL);
	assert(admin_cq->group->group->thread != NULL);

	pthread_mutex_lock(&endpoint->lock);
	if (nvmf_qpair_is_admin_queue(&sq->qpair)) {
		assert(admin_cq->group->group->thread == spdk_get_thread());
		/*
		 * The admin queue is special as SQ0 and CQ0 are created
		 * together.
		 */
		admin_cq->cq_ref = 1;
		start_ctrlr(vu_ctrlr, sq->qpair.ctrlr);
	} else {
		/* For I/O queues this command was generated in response to an
		 * ADMIN I/O CREATE SUBMISSION QUEUE command which has not yet
		 * been completed. Complete it now.
		 */
		if (sq->post_create_io_sq_completion) {
			if (admin_cq->group->group->thread != spdk_get_thread()) {
				struct vfio_user_post_cpl_ctx *cpl_ctx;

				cpl_ctx = calloc(1, sizeof(*cpl_ctx));
				if (!cpl_ctx) {
					return -ENOMEM;
				}
				cpl_ctx->ctrlr = vu_ctrlr;
				cpl_ctx->cq = admin_cq;
				cpl_ctx->cpl.sqid = 0;
				cpl_ctx->cpl.cdw0 = 0;
				cpl_ctx->cpl.cid = sq->create_io_sq_cmd.cid;
				cpl_ctx->cpl.status.sc = SPDK_NVME_SC_SUCCESS;
				cpl_ctx->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

				spdk_thread_send_msg(admin_cq->group->group->thread,
						     _post_completion_msg,
						     cpl_ctx);
			} else {
				post_completion(vu_ctrlr, admin_cq, 0, 0,
						sq->create_io_sq_cmd.cid, SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
			}
			sq->post_create_io_sq_completion = false;
		} else if (in_interrupt_mode(endpoint->transport)) {
			/*
			 * If we're live migrating a guest, there is a window
			 * where the I/O queues haven't been set up but the
			 * device is in running state, during which the guest
			 * might write to a doorbell. This doorbell write will
			 * go unnoticed, so let's poll the whole controller to
			 * pick that up.
			 */
			ctrlr_kick(vu_ctrlr);
		}
		sq->sq_state = VFIO_USER_SQ_ACTIVE;
	}

	TAILQ_INSERT_TAIL(&vu_ctrlr->connected_sqs, sq, tailq);
	pthread_mutex_unlock(&endpoint->lock);

	free(req->req.data);
	req->req.data = NULL;

	return 0;
}

/*
 * Add the given qpair to the given poll group. New qpairs are added via
 * spdk_nvmf_tgt_new_qpair(), which picks a poll group via
 * nvmf_vfio_user_get_optimal_poll_group(), then calls back here via
 * nvmf_transport_poll_group_add().
 */
static int
nvmf_vfio_user_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct spdk_nvmf_request *req;
	struct spdk_nvmf_fabric_connect_data *data;
	bool admin;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	sq->group = group;
	ctrlr = sq->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: add QP%d=%p(%p) to poll_group=%p\n",
		      ctrlr_id(ctrlr), sq->qpair.qid,
		      sq, qpair, group);

	admin = nvmf_qpair_is_admin_queue(&sq->qpair);

	vu_req = get_nvmf_vfio_user_req(sq);
	if (vu_req == NULL) {
		return -1;
	}

	req = &vu_req->req;
	req->cmd->connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->cmd->connect_cmd.cid = 0;
	req->cmd->connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	req->cmd->connect_cmd.recfmt = 0;
	req->cmd->connect_cmd.sqsize = sq->size - 1;
	req->cmd->connect_cmd.qid = admin ? 0 : qpair->qid;

	req->length = sizeof(struct spdk_nvmf_fabric_connect_data);
	req->data = calloc(1, req->length);
	if (req->data == NULL) {
		nvmf_vfio_user_req_free(req);
		return -ENOMEM;
	}

	data = (struct spdk_nvmf_fabric_connect_data *)req->data;
	data->cntlid = ctrlr->cntlid;
	snprintf(data->subnqn, sizeof(data->subnqn), "%s",
		 spdk_nvmf_subsystem_get_nqn(ctrlr->endpoint->subsystem));

	vu_req->cb_fn = handle_queue_connect_rsp;
	vu_req->cb_arg = sq;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: sending connect fabrics command for qid:%#x cntlid=%#x\n",
		      ctrlr_id(ctrlr), qpair->qid, data->cntlid);

	spdk_nvmf_request_exec_fabrics(req);
	return 0;
}

static int
nvmf_vfio_user_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_poll_group *vu_group;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: remove NVMf QP%d=%p from NVMf poll_group=%p\n",
		      ctrlr_id(sq->ctrlr), qpair->qid, qpair, group);


	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);
	TAILQ_REMOVE(&vu_group->sqs, sq, link);

	return 0;
}

static void
_nvmf_vfio_user_req_free(struct nvmf_vfio_user_sq *sq, struct nvmf_vfio_user_req *vu_req)
{
	memset(&vu_req->cmd, 0, sizeof(vu_req->cmd));
	memset(&vu_req->rsp, 0, sizeof(vu_req->rsp));
	vu_req->iovcnt = 0;
	vu_req->state = VFIO_USER_REQUEST_STATE_FREE;

	TAILQ_INSERT_TAIL(&sq->free_reqs, vu_req, link);
}

static int
nvmf_vfio_user_req_free(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_req *vu_req;

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	sq = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);

	_nvmf_vfio_user_req_free(sq, vu_req);

	return 0;
}

static int
nvmf_vfio_user_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_req *vu_req;

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	sq = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);

	if (vu_req->cb_fn != NULL) {
		if (vu_req->cb_fn(vu_req, vu_req->cb_arg) != 0) {
			fail_ctrlr(sq->ctrlr);
		}
	}

	_nvmf_vfio_user_req_free(sq, vu_req);

	return 0;
}

static void
nvmf_vfio_user_close_qpair(struct spdk_nvmf_qpair *qpair,
			   spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint;
	struct vfio_user_delete_sq_ctx *del_ctx;

	assert(qpair != NULL);
	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	vu_ctrlr = sq->ctrlr;
	endpoint = vu_ctrlr->endpoint;
	del_ctx = sq->delete_ctx;
	sq->delete_ctx = NULL;

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_REMOVE(&vu_ctrlr->connected_sqs, sq, tailq);
	delete_sq_done(vu_ctrlr, sq);
	if (TAILQ_EMPTY(&vu_ctrlr->connected_sqs)) {
		endpoint->ctrlr = NULL;
		if (vu_ctrlr->in_source_vm && endpoint->need_resume) {
			/* The controller will be freed, we can resume the subsystem
			 * now so that the endpoint can be ready to accept another
			 * new connection.
			 */
			spdk_nvmf_subsystem_resume((struct spdk_nvmf_subsystem *)endpoint->subsystem,
						   vfio_user_endpoint_resume_done, endpoint);
		}
		free_ctrlr(vu_ctrlr);
	}
	pthread_mutex_unlock(&endpoint->lock);

	if (del_ctx) {
		vfio_user_qpair_delete_cb(del_ctx);
	}

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

/**
 * Returns a preallocated request, or NULL if there isn't one available.
 */
static struct nvmf_vfio_user_req *
get_nvmf_vfio_user_req(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_req *req;

	if (sq == NULL) {
		return NULL;
	}

	req = TAILQ_FIRST(&sq->free_reqs);
	if (req == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&sq->free_reqs, req, link);

	return req;
}

static int
get_nvmf_io_req_length(struct spdk_nvmf_request *req)
{
	uint16_t nr;
	uint32_t nlb, nsid;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_ns *ns;

	nsid = cmd->nsid;
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_ERRLOG("unsuccessful query for nsid %u\n", cmd->nsid);
		return -EINVAL;
	}

	if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		nr = cmd->cdw10_bits.dsm.nr + 1;
		return nr * sizeof(struct spdk_nvme_dsm_range);
	}

	nlb = (cmd->cdw12 & 0x0000ffffu) + 1;
	return nlb * spdk_bdev_get_block_size(ns->bdev);
}

static int
map_admin_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint32_t len = 0, numdw = 0;
	uint8_t fid;
	int iovcnt;

	req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	req->length = 0;
	req->data = NULL;

	if (req->xfer == SPDK_NVME_DATA_NONE) {
		return 0;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		len = 4096;
		break;
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		numdw = ((((uint32_t)cmd->cdw11_bits.get_log_page.numdu << 16) |
			  cmd->cdw10_bits.get_log_page.numdl) + 1);
		if (numdw > UINT32_MAX / 4) {
			return -EINVAL;
		}
		len = numdw * 4;
		break;
	case SPDK_NVME_OPC_GET_FEATURES:
	case SPDK_NVME_OPC_SET_FEATURES:
		fid = cmd->cdw10_bits.set_features.fid;
		switch (fid) {
		case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			len = 4096;
			break;
		case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
			len = 256;
			break;
		case SPDK_NVME_FEAT_TIMESTAMP:
			len = 8;
			break;
		case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
			len = 512;
			break;
		case SPDK_NVME_FEAT_HOST_IDENTIFIER:
			if (cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
				len = 16;
			} else {
				len = 8;
			}
			break;
		default:
			return 0;
		}
		break;
	default:
		return 0;
	}

	/* ADMIN command will not use SGL */
	if (cmd->psdt != 0) {
		return -EINVAL;
	}

	iovcnt = vfio_user_map_cmd(ctrlr, req, req->iov, len);
	if (iovcnt < 0) {
		SPDK_ERRLOG("%s: map Admin Opc %x failed\n",
			    ctrlr_id(ctrlr), cmd->opc);
		return -1;
	}
	req->length = len;
	req->data = req->iov[0].iov_base;
	req->iovcnt = iovcnt;

	return 0;
}

/*
 * Map an I/O command's buffers.
 *
 * Returns 0 on success and -errno on failure.
 */
static int
map_io_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req)
{
	int len, iovcnt;
	struct spdk_nvme_cmd *cmd;

	assert(ctrlr != NULL);
	assert(req != NULL);

	cmd = &req->cmd->nvme_cmd;
	req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	req->length = 0;
	req->data = NULL;

	if (spdk_unlikely(req->xfer == SPDK_NVME_DATA_NONE)) {
		return 0;
	}

	len = get_nvmf_io_req_length(req);
	if (len < 0) {
		return -EINVAL;
	}
	req->length = len;

	iovcnt = vfio_user_map_cmd(ctrlr, req, req->iov, req->length);
	if (iovcnt < 0) {
		SPDK_ERRLOG("%s: failed to map IO OPC %u\n", ctrlr_id(ctrlr), cmd->opc);
		return -EFAULT;
	}
	req->data = req->iov[0].iov_base;
	req->iovcnt = iovcnt;

	return 0;
}

static int
handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
	       struct nvmf_vfio_user_sq *sq)
{
	int err;
	struct nvmf_vfio_user_req *vu_req;
	struct spdk_nvmf_request *req;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	vu_req = get_nvmf_vfio_user_req(sq);
	if (spdk_unlikely(vu_req == NULL)) {
		SPDK_ERRLOG("%s: no request for NVMe command opc 0x%x\n", ctrlr_id(ctrlr), cmd->opc);
		return post_completion(ctrlr, ctrlr->cqs[sq->cqid], 0, 0, cmd->cid,
				       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, SPDK_NVME_SCT_GENERIC);

	}
	req = &vu_req->req;

	assert(req->qpair != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: handle sqid:%u, req opc=%#x cid=%d\n",
		      ctrlr_id(ctrlr), req->qpair->qid, cmd->opc, cmd->cid);

	vu_req->cb_fn = handle_cmd_rsp;
	vu_req->cb_arg = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);
	req->cmd->nvme_cmd = *cmd;

	if (nvmf_qpair_is_admin_queue(req->qpair)) {
		err = map_admin_cmd_req(ctrlr, req);
	} else {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
			err = -ENOTSUP;
			break;
		default:
			err = map_io_cmd_req(ctrlr, req);
			break;
		}
	}

	if (spdk_unlikely(err < 0)) {
		SPDK_ERRLOG("%s: process NVMe command opc 0x%x failed\n",
			    ctrlr_id(ctrlr), cmd->opc);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		err = handle_cmd_rsp(vu_req, vu_req->cb_arg);
		_nvmf_vfio_user_req_free(sq, vu_req);
		return err;
	}

	vu_req->state = VFIO_USER_REQUEST_STATE_EXECUTING;
	spdk_nvmf_request_exec(req);

	return 0;
}

/*
 * If we suppressed an IRQ in post_completion(), check if it needs to be fired
 * here: if the host isn't up to date, and is apparently not actively processing
 * the queue (i.e. ->last_head isn't changing), we need an IRQ.
 */
static void
handle_suppressed_irq(struct nvmf_vfio_user_ctrlr *ctrlr,
		      struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	uint32_t cq_head;
	uint32_t cq_tail;

	if (!cq->ien || cq->qid == 0 || !ctrlr_interrupt_enabled(ctrlr)) {
		return;
	}

	cq_tail = *cq_tailp(cq);

	/* Already sent? */
	if (cq_tail == cq->last_trigger_irq_tail) {
		return;
	}

	spdk_ivdt_dcache(cq_dbl_headp(cq));
	cq_head = *cq_dbl_headp(cq);

	if (cq_head != cq_tail && cq_head == cq->last_head) {
		int err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
		} else {
			cq->last_trigger_irq_tail = cq_tail;
		}
	}

	cq->last_head = cq_head;
}

/* Returns the number of commands processed, or a negative value on error. */
static int
nvmf_vfio_user_sq_poll(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	uint32_t new_tail;
	int count = 0;

	assert(sq != NULL);

	ctrlr = sq->ctrlr;

	/*
	 * A quiesced, or migrating, controller should never process new
	 * commands.
	 */
	if (ctrlr->state != VFIO_USER_CTRLR_RUNNING) {
		return SPDK_POLLER_IDLE;
	}

	if (ctrlr->adaptive_irqs_enabled) {
		handle_suppressed_irq(ctrlr, sq);
	}

	/* On aarch64 platforms, doorbells update from guest VM may not be seen
	 * on SPDK target side. This is because there is memory type mismatch
	 * situation here. That is on guest VM side, the doorbells are treated as
	 * device memory while on SPDK target side, it is treated as normal
	 * memory. And this situation cause problem on ARM platform.
	 * Refer to "https://developer.arm.com/documentation/102376/0100/
	 * Memory-aliasing-and-mismatched-memory-types". Only using spdk_mb()
	 * cannot fix this. Use "dc civac" to invalidate cache may solve
	 * this.
	 */
	spdk_ivdt_dcache(sq_dbl_tailp(sq));

	/* Load-Acquire. */
	new_tail = *sq_dbl_tailp(sq);

	new_tail = new_tail & 0xffffu;
	if (spdk_unlikely(new_tail >= sq->size)) {
		union spdk_nvme_async_event_completion event = {};

		SPDK_DEBUGLOG(nvmf_vfio, "%s: invalid sqid:%u doorbell value %u\n", ctrlr_id(ctrlr), sq->qid,
			      new_tail);
		event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_ERROR;
		event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE;
		nvmf_ctrlr_async_event_error_event(ctrlr->ctrlr, event);

		return -1;
	}

	if (*sq_headp(sq) == new_tail) {
		return 0;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: sqid:%u doorbell old=%u new=%u\n",
		      ctrlr_id(ctrlr), sq->qid, *sq_headp(sq), new_tail);
	if (ctrlr->sdbl != NULL) {
		SPDK_DEBUGLOG(nvmf_vfio,
			      "%s: sqid:%u bar0_doorbell=%u shadow_doorbell=%u eventidx=%u\n",
			      ctrlr_id(ctrlr), sq->qid,
			      ctrlr->bar0_doorbells[queue_index(sq->qid, false)],
			      ctrlr->sdbl->shadow_doorbells[queue_index(sq->qid, false)],
			      ctrlr->sdbl->eventidxs[queue_index(sq->qid, false)]);
	}

	/*
	 * Ensure that changes to the queue are visible to us.
	 * The host driver should write the queue first, do a wmb(), and then
	 * update the SQ tail doorbell (their Store-Release).
	 */
	spdk_rmb();

	count = handle_sq_tdbl_write(ctrlr, new_tail, sq);
	if (spdk_unlikely(count < 0)) {
		fail_ctrlr(ctrlr);
	}

	return count;
}

/*
 * vfio-user transport poll handler. Note that the library context is polled in
 * a separate poller (->vfu_ctx_poller), so this poller only needs to poll the
 * active SQs.
 *
 * Returns the number of commands processed, or a negative value on error.
 */
static int
nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_sq *sq, *tmp;
	int count = 0;

	assert(group != NULL);

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);

	SPDK_DEBUGLOG(vfio_user_db, "polling all SQs\n");

	TAILQ_FOREACH_SAFE(sq, &vu_group->sqs, link, tmp) {
		int ret;

		if (spdk_unlikely(sq->sq_state != VFIO_USER_SQ_ACTIVE || !sq->size)) {
			continue;
		}

		ret = nvmf_vfio_user_sq_poll(sq);

		if (spdk_unlikely(ret < 0)) {
			return ret;
		}

		count += ret;
	}

	vu_group->stats.polls++;
	vu_group->stats.poll_reqs += count;
	vu_group->stats.poll_reqs_squared += count * count;
	if (count == 0) {
		vu_group->stats.polls_spurious++;
	}

	return count;
}

static int
nvmf_vfio_user_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	ctrlr = sq->ctrlr;

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));
	return 0;
}

static int
nvmf_vfio_user_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return 0;
}

static int
nvmf_vfio_user_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	ctrlr = sq->ctrlr;

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));
	return 0;
}

static void
nvmf_vfio_user_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_request *req_to_abort = NULL;
	struct spdk_nvmf_request *temp_req = NULL;
	uint16_t cid;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;

	TAILQ_FOREACH(temp_req, &qpair->outstanding, link) {
		struct nvmf_vfio_user_req *vu_req;

		vu_req = SPDK_CONTAINEROF(temp_req, struct nvmf_vfio_user_req, req);

		if (vu_req->state == VFIO_USER_REQUEST_STATE_EXECUTING && vu_req->cmd.cid == cid) {
			req_to_abort = temp_req;
			break;
		}
	}

	if (req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = req_to_abort;
	nvmf_ctrlr_abort_request(req);
}

static void
nvmf_vfio_user_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_json_write_ctx *w)
{
	struct nvmf_vfio_user_poll_group *vu_group = SPDK_CONTAINEROF(group,
			struct nvmf_vfio_user_poll_group, group);
	uint64_t polls_denom;

	spdk_json_write_named_uint64(w, "ctrlr_intr", vu_group->stats.ctrlr_intr);
	spdk_json_write_named_uint64(w, "ctrlr_kicks", vu_group->stats.ctrlr_kicks);
	spdk_json_write_named_uint64(w, "won", vu_group->stats.won);
	spdk_json_write_named_uint64(w, "lost", vu_group->stats.lost);
	spdk_json_write_named_uint64(w, "lost_count", vu_group->stats.lost_count);
	spdk_json_write_named_uint64(w, "rearms", vu_group->stats.rearms);
	spdk_json_write_named_uint64(w, "pg_process_count", vu_group->stats.pg_process_count);
	spdk_json_write_named_uint64(w, "intr", vu_group->stats.intr);
	spdk_json_write_named_uint64(w, "polls", vu_group->stats.polls);
	spdk_json_write_named_uint64(w, "polls_spurious", vu_group->stats.polls_spurious);
	spdk_json_write_named_uint64(w, "poll_reqs", vu_group->stats.poll_reqs);
	polls_denom = vu_group->stats.polls * (vu_group->stats.polls - 1);
	if (polls_denom) {
		uint64_t n = vu_group->stats.polls * vu_group->stats.poll_reqs_squared - vu_group->stats.poll_reqs *
			     vu_group->stats.poll_reqs;
		spdk_json_write_named_double(w, "poll_reqs_variance", sqrt(n / polls_denom));
	}

	spdk_json_write_named_uint64(w, "cqh_admin_writes", vu_group->stats.cqh_admin_writes);
	spdk_json_write_named_uint64(w, "cqh_io_writes", vu_group->stats.cqh_io_writes);
}

static void
nvmf_vfio_user_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	0;
	opts->max_io_size =		NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =		NVMF_VFIO_USER_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	0;
	opts->buf_cache_size =		0;
	opts->association_timeout =	0;
	opts->transport_specific =      NULL;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_vfio_user = {
	.name = "VFIOUSER",
	.type = SPDK_NVME_TRANSPORT_VFIOUSER,
	.opts_init = nvmf_vfio_user_opts_init,
	.create = nvmf_vfio_user_create,
	.destroy = nvmf_vfio_user_destroy,

	.listen = nvmf_vfio_user_listen,
	.stop_listen = nvmf_vfio_user_stop_listen,
	.cdata_init = nvmf_vfio_user_cdata_init,
	.listen_associate = nvmf_vfio_user_listen_associate,

	.listener_discover = nvmf_vfio_user_discover,

	.poll_group_create = nvmf_vfio_user_poll_group_create,
	.get_optimal_poll_group = nvmf_vfio_user_get_optimal_poll_group,
	.poll_group_destroy = nvmf_vfio_user_poll_group_destroy,
	.poll_group_add = nvmf_vfio_user_poll_group_add,
	.poll_group_remove = nvmf_vfio_user_poll_group_remove,
	.poll_group_poll = nvmf_vfio_user_poll_group_poll,

	.req_free = nvmf_vfio_user_req_free,
	.req_complete = nvmf_vfio_user_req_complete,

	.qpair_fini = nvmf_vfio_user_close_qpair,
	.qpair_get_local_trid = nvmf_vfio_user_qpair_get_local_trid,
	.qpair_get_peer_trid = nvmf_vfio_user_qpair_get_peer_trid,
	.qpair_get_listen_trid = nvmf_vfio_user_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_vfio_user_qpair_abort_request,

	.poll_group_dump_stat = nvmf_vfio_user_poll_group_dump_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(muser, &spdk_nvmf_transport_vfio_user);
SPDK_LOG_REGISTER_COMPONENT(nvmf_vfio)
SPDK_LOG_REGISTER_COMPONENT(vfio_user_db)
