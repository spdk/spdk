/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2019, Nutanix Inc. All rights reserved.
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

#include <muser/muser.h>
#include <muser/caps/pm.h>
#include <muser/caps/px.h>
#include <muser/caps/msi.h>
#include <muser/caps/msix.h>

#include "spdk/barrier.h"
#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk_internal/log.h"

struct nvme_pcie_mlbar {
	uint32_t rte :	1;
	uint32_t tp :	2;
	uint32_t pf :	1;
	uint32_t res1 :	10;
	uint32_t ba :	16;
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_pcie_mlbar) == sizeof(uint32_t), "Invalid size");

struct nvme_pcie_bar2 {
	uint32_t rte :	1;
	uint32_t res1 :	2;
	uint32_t ba :	29;
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_pcie_bar2) == sizeof(uint32_t), "Bad NVMe BAR2 size");

struct spdk_log_flag SPDK_LOG_MUSER = {.enabled = true};

#define MUSER_DEFAULT_MAX_QUEUE_DEPTH 256
#define MUSER_DEFAULT_AQ_DEPTH 32
#define MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR 64
#define MUSER_DEFAULT_IN_CAPSULE_DATA_SIZE 0
#define MUSER_DEFAULT_MAX_IO_SIZE 131072
#define MUSER_DEFAULT_IO_UNIT_SIZE 131072
#define MUSER_DEFAULT_NUM_SHARED_BUFFERS 512 /* internal buf size */
#define MUSER_DEFAULT_BUFFER_CACHE_SIZE 0

#define NVME_REG_CFG_SIZE       0x1000
#define NVME_REG_BAR0_SIZE      0x4000

#define NVME_IRQ_INTX_NUM       1
#define NVME_IRQ_MSI_NUM       	2
#define NVME_IRQ_MSIX_NUM       32

enum muser_nvmf_dir {
	MUSER_NVMF_INVALID,
	MUSER_NVMF_READ,
	MUSER_NVMF_WRITE
};

struct muser_req;
struct muser_qpair;

typedef void (*muser_req_end_fn)(struct muser_qpair*, struct muser_req*);

struct muser_req  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_cmd			*cmd;

	muser_req_end_fn			end_fn;

	TAILQ_ENTRY(muser_req)			link;
};

struct muser_nvmf_prop_req {
	enum muser_nvmf_dir			dir;
	sem_t					wait;
	/*
	 * TODO either this will be a register access or an internal command
	 * (for now it's only about starting the NVMf subsystem. Convert into a
	 * union.
	 */
	char					*buf;
	size_t					count;
	loff_t					pos;
	ssize_t					ret;
	bool					delete;
	struct muser_req			muser_req;
	union nvmf_h2c_msg			cmd;
	union nvmf_c2h_msg			rsp;
};

/*
 * An I/O queue.
 *
 * TODO we use the same struct both for submission and for completion I/O
 * queues because that simplifies queue creation. However we're wasting memory
 * for submission queues, maybe rethink this approach.
 */
struct io_q {
	bool is_cq;

	void *addr;

	dma_sg_t sg;
	struct iovec iov;

	/*
	 * FIXME move to parent struct muser_qpair? There's already qsize
	 * there.
	 */
	uint32_t size;

	union {
		struct {
			uint32_t head;
			/* multiple SQs can be mapped to the same CQ */
			uint16_t cqid;
		};
		struct {
			uint32_t tail;
			uint16_t iv;
			bool ien;
		};
	};
};

struct muser_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_muser_poll_group	*group;
	struct muser_ctrlr			*ctrlr;
	struct spdk_nvme_cmd			*cmd;
	struct muser_req			*reqs_internal;
	union nvmf_h2c_msg			*cmds_internal;
	union nvmf_c2h_msg			*rsps_internal;
	uint16_t				qsize; /* TODO aren't all queues the same size? */
	struct io_q				cq;
	struct io_q				sq;
	bool					del;

	TAILQ_HEAD(, muser_req)			reqs;
	TAILQ_ENTRY(muser_qpair)		link;
};

/*
 * function prototypes
 */
static uint32_t*
hdbl(struct muser_ctrlr *ctrlr, struct io_q *q);

static uint32_t*
tdbl(struct muser_ctrlr *ctrlr, struct io_q *q);

/*
 * XXX We need a way to extract the queue ID from an io_q, which is already
 * available in muser_qpair->qpair.qid. Currently we store the type of the
 * queue within the queue, so retrieving the QID requires a comparison. Rather
 * than duplicating this information in struct io_q, we could store a pointer
 * to parent struct muser_qpair, however we would be using 8 bytes instead of
 * just 2 (uint16_t vs. pointer). This is only per-queue so it's not that bad.
 * Another approach is to define two types: struct io_cq { struct io_q q }; and
 * struct io_sq { struct io_q q; };. The downside would be that we would need
 * two almost identical functions to extract the QID.
 */
static uint16_t
io_q_id(struct io_q *q) {

	struct muser_qpair *muser_qpair;

	assert(q);

	if (q->is_cq) {
		muser_qpair = SPDK_CONTAINEROF(q, struct muser_qpair, cq);
	} else {
		muser_qpair = SPDK_CONTAINEROF(q, struct muser_qpair, sq);
	}
	assert(muser_qpair);
	return muser_qpair->qpair.qid;
}

struct muser_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	struct muser_ctrlr			*ctrlr;
	TAILQ_HEAD(, muser_qpair)		qps;
};

struct muser_ctrlr {
	struct spdk_nvme_transport_id		trid;
	char					uuid[37]; /* TODO 37 is already defined somewhere */
	pthread_t				lm_thr;
	lm_ctx_t				*lm_ctx;
	lm_pci_config_space_t			*pci_config_space;

	/* FIXME why do we need this? */
	struct muser_poll_group			*muser_group;

	/*
	 * FIXME variables that are checked by poll_group_poll to see whether
	 * commands need to be executed, in addition to checking the doorbells.
	 * We now have 3 such different commands so we should introduce a queue,
	 * or if we're going to have a single outstanding command we should
	 * group them into a union.
	 */
	bool					start; /* start subsys */
	bool					del_admin_qp; /* del admin qp */
	sem_t					sem;
	struct spdk_nvmf_subsystem		*subsys;
	struct muser_nvmf_prop_req		prop_req; /* read/write BAR0 */

	/* PCI capabilities */
	struct pmcap				pmcap;
	struct msicap				msicap;
	struct msixcap				msixcap;
	struct pxcap				pxcap;

	uint16_t				cntlid;

	struct muser_qpair			*qp[MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR];

	TAILQ_ENTRY(muser_ctrlr)		link;

	union spdk_nvme_cc_register		cc;
	union spdk_nvme_aqa_register		aqa;
	uint64_t				asq;
	uint64_t				acq;

	/* even indices are SQ, odd indices are CQ */
	uint32_t				doorbells[MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR * 2];
};

struct muser_transport {
	struct spdk_nvmf_transport		transport;
	pthread_mutex_t				lock;
	struct muser_poll_group			*group;
	TAILQ_HEAD(, muser_ctrlr)		ctrlrs;

	TAILQ_HEAD(, muser_qpair)		new_qps;
};

static int
muser_destroy(struct spdk_nvmf_transport *transport)
{
	struct muser_transport *muser_transport;

	muser_transport = SPDK_CONTAINEROF(transport, struct muser_transport,
					   transport);

	(void)pthread_mutex_destroy(&muser_transport->lock);

	free(muser_transport);

	return 0;
}

static struct spdk_nvmf_transport *
muser_create(struct spdk_nvmf_transport_opts *opts)
{
	struct muser_transport *muser_transport;
	int err;

	muser_transport = calloc(1, sizeof(*muser_transport));
	if (muser_transport == NULL) {
		SPDK_ERRLOG("Transport alloc fail: %m\n");
		return NULL;
	}

	err = pthread_mutex_init(&muser_transport->lock, NULL);
	if (err != 0) {
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;
	}

	TAILQ_INIT(&muser_transport->ctrlrs);
	TAILQ_INIT(&muser_transport->new_qps);

	return &muser_transport->transport;

err:
	free(muser_transport);

	return NULL;
}

#define MDEV_CREATE_PATH "/sys/class/muser/muser/mdev_supported_types/muser-1/create"

static void
mdev_remove(const char *uuid)
{ /* TODO: Implement me */ }

static int
mdev_create(const char *uuid)
{
	int fd;
	int err;

	fd = open(MDEV_CREATE_PATH, O_WRONLY);
	if (fd == -1) {
		SPDK_ERRLOG("Error opening '%s': %m\n", MDEV_CREATE_PATH);
		return -1;
	}

	err = write(fd, uuid, strlen(uuid));
	if (err != (int)strlen(uuid)) {
		SPDK_ERRLOG("Error creating device '%s': %m\n", uuid);
		err = -1;
	} else {
		err = 0;
	}

	(void)close(fd);

	sleep(1);

	/* TODO: Wait until ctrlr appears on /ctrlr/muser/<uuid> */

	return err;
}

static bool
is_nvme_cap(const loff_t pos)
{
	static const size_t off = offsetof(struct spdk_nvme_registers, cap);
	return (size_t)pos >= off && (size_t)pos < off + sizeof(uint64_t);
}

static int
handle_dbl_access(struct muser_ctrlr *ctrlr, uint32_t *buf,
		  const size_t count, loff_t pos, const bool is_write);

static bool
muser_spdk_nvmf_subsystem_is_active(struct muser_ctrlr *ctrlr)
{
	return ctrlr->subsys->state == SPDK_NVMF_SUBSYSTEM_ACTIVE;
}

static void
destroy_qp(struct muser_ctrlr *ctrlr, uint16_t qid);

static int
muser_request_spdk_nvmf_subsystem_resume(struct muser_ctrlr *ctrlr)
{
	int err;

	assert(ctrlr != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "requesting NVMf subsystem resume\n");

	err= sem_init(&ctrlr->sem, 0, 0);
	assert(err == 0); /* FIXME */
	ctrlr->start = true;
	spdk_wmb();
	do {
		err = sem_wait(&ctrlr->sem);
	} while (err != 0 && errno != EINTR);

	/*
	 * If it was stopped then there won't be an admin QP, we need to add it
	 * however we can't do it here as add_qp must be executed in SPDK
	 * thread context. muser_do_spdk_nvmf_subsystem_resume calls
	 * spdk_nvmf_subsystem_resume where in the callback we simply fire the
	 * semaphore which unblocks the wait above. So we'll have to either
	 * change that callback to add the queue or issue a do_prop_req here.
	 * XXX Idea: if add_qp is always expected to follow
	 * spdk_nvmf_subsystem_resume, then in its callback we can call add_qp
	 * and in handle_admin_q_connect_rsp we can fire the sem semaphore.
	 */

	return 0;
}

static int
do_prop_req(struct muser_ctrlr *ctrlr, char *buf, size_t count, loff_t pos,
         bool is_write)
{
	int err;

	assert(ctrlr != NULL);

	err = sem_init(&ctrlr->prop_req.wait, 0, 0);
	if (err != 0) {
		return err;
	}
	ctrlr->prop_req.buf = buf;
	/* TODO: count must never be more than 8, otherwise we need to split it */
	ctrlr->prop_req.count = count;
	ctrlr->prop_req.pos = pos;
	spdk_wmb();
	ctrlr->prop_req.dir = is_write ? MUSER_NVMF_WRITE : MUSER_NVMF_READ;
	err = sem_wait(&ctrlr->prop_req.wait);
	assert(err == 0); /* FIXME */
	return err;
}

/*
 * TODO read_bar0 and write_bar0 are very similar, merge
 */
static ssize_t
read_bar0(void *pvt, char *buf, size_t count, loff_t pos)
{
	struct muser_ctrlr *ctrlr = pvt;
	int err;
	char *_buf = NULL;
	size_t _count;

	SPDK_NOTICELOG("\nctrlr: %p, count=%zu, pos=%"PRIX64"\n",
		       ctrlr, count, pos);

	/*
	 * FIXME why wo we have to check from this thread whether it's active?
	 * Can we blindly forward the read and resume the subsystem if required
	 * in the SPDK thread context?
	 */
	if (!muser_spdk_nvmf_subsystem_is_active(ctrlr)) {
		err = muser_request_spdk_nvmf_subsystem_resume(ctrlr);
		assert(err == 0); /* FIXME */
	}

	/*
	 * NVMe CAP is 8 bytes long however the driver reads it 4 bytes at a
	 * time. NVMf doesn't like this.
	 */
	if (is_nvme_cap(pos)) {
		assert(count == 4 || count == 8); /* FIXME */
		if (count == 4) {
			_buf = buf;
			_count = count;
			count = 8;
			buf = alloca(count);
		}
	}

	if (pos >= DOORBELLS) {
		return handle_dbl_access(ctrlr, (uint32_t*)buf, count, 
		                         pos, false);
	}

	/*
	 * This is a PCI read from the guest so we must synchronously wait for
	 * NVMf to respond with the data.
	 */
	err = do_prop_req(ctrlr, buf, count, pos, false);
	assert(err == 0); /* FIXME */

	if (_buf != NULL) {
		memcpy(_buf,
		       buf + pos - offsetof(struct spdk_nvme_registers, cap),
		       _count);
	}

	return ctrlr->prop_req.ret;
}

static uint16_t
max_queue_size(struct muser_ctrlr const *ctrlr)
{
	assert(ctrlr != NULL);
	assert(ctrlr->qp[0] != NULL);
	assert(ctrlr->qp[0]->qpair.ctrlr != NULL);

	return ctrlr->qp[0]->qpair.ctrlr->vcprop.cap.bits.mqes + 1;
}

static ssize_t
aqa_write(struct muser_ctrlr *ctrlr,
	  union spdk_nvme_aqa_register const *from)
{
	assert(ctrlr);
	assert(from);

	if (from->bits.asqs + 1 > max_queue_size(ctrlr) ||
	    from->bits.acqs + 1 > max_queue_size(ctrlr)) {
		SPDK_ERRLOG("admin queue(s) too big, ASQS=%d, ACQS=%d, max=%d\n",
			    from->bits.asqs + 1, from->bits.acqs + 1,
			    max_queue_size(ctrlr));
		return -EINVAL;
	}
	ctrlr->aqa.raw = from->raw;
	SPDK_NOTICELOG("write to AQA %x\n", ctrlr->aqa.raw);
	return 0;
}

static void
write_partial(uint8_t const *buf, const loff_t pos, const size_t count,
	      const size_t reg_off, uint64_t *reg)
{
	memcpy(reg + pos - reg_off, buf, count);
}

/*
 * Tells whether either the lower 4 bytes are written at the beginning of the
 * 8-byte register, or the higher 4 starting at the middle.
 */
static inline bool
_is_half(const size_t p, const size_t c, const size_t o)
{
	return c == sizeof(uint32_t) && (p == o || (p == o + sizeof(uint32_t)));
}

/*
 * Tells whether the full 8 bytes are written at the correct offset.
 */
static inline bool
_is_full(const size_t p, const size_t c, const size_t o)
{
	return c == sizeof(uint64_t) && p == o;
}

/*
 * Either write or lower/upper 4 bytes, or the full 8 bytes.
 *
 * p: position
 * c: count
 * o: register offset
 */
static inline bool
is_valid_asq_or_acq_write(const size_t p, const size_t c, const size_t o)
{
	return _is_half(p, c, o) || _is_full(p, c, o);
}

static ssize_t
asq_or_acq_write(uint8_t const *buf, const loff_t pos,
		 const size_t count, uint64_t *reg, const size_t reg_off)
{
	/*
	 * The NVMe driver seems to write those only in 4 upper/lower bytes, but
	 * we still have to support writing the whole register in one go.
	 */
	if (!is_valid_asq_or_acq_write((size_t)pos, count, reg_off)) {
		SPDK_ERRLOG("bad write count %zu and/or offset 0x%lx\n",
			    count, reg_off);
		return -EINVAL;
	}

	write_partial(buf, pos, count, reg_off, reg);

	return 0;
}

static ssize_t
asq_write(uint64_t *asq, uint8_t const *buf,
	  const loff_t pos, const size_t count)
{
	int ret = asq_or_acq_write(buf, pos, count, asq,
				   offsetof(struct spdk_nvme_registers, asq));
	SPDK_NOTICELOG("ASQ=0x%lx\n", *asq);
	return ret;
}

static ssize_t
acq_write(uint64_t *acq, uint8_t const *buf,
	  const loff_t pos, const size_t count)
{
	int ret = asq_or_acq_write(buf, pos, count, acq,
				   offsetof(struct spdk_nvme_registers, acq));
	SPDK_NOTICELOG("ACQ=0x%lx\n", *acq);
	return ret;
}

#define REGISTER_RANGE(name, size) \
	offsetof(struct spdk_nvme_registers, name) ... \
		offsetof(struct spdk_nvme_registers, name) + size - 1

#define ASQ \
	REGISTER_RANGE(asq, sizeof(uint64_t))

#define ACQ \
	REGISTER_RANGE(acq, sizeof(uint64_t))

#define ADMIN_QUEUES \
	offsetof(struct spdk_nvme_registers, aqa) ... \
		offsetof(struct spdk_nvme_registers, acq) + sizeof(uint64_t) - 1

static ssize_t
admin_queue_write(struct muser_ctrlr *ctrlr, uint8_t const *buf,
		  const size_t count, const loff_t pos)
{
	switch (pos) {
	case offsetof(struct spdk_nvme_registers, aqa):
		return aqa_write(ctrlr,
				 (union spdk_nvme_aqa_register *)buf);
	case ASQ:
		return asq_write(&ctrlr->asq, buf, pos, count);
	case ACQ:
		return acq_write(&ctrlr->acq, buf, pos, count);
	default:
		break;
	}
	SPDK_ERRLOG("bad admin queue write offset 0x%lx\n", pos);
	return -EINVAL;
}

/* TODO this should be a libmuser public function */
static void *
map_one(void *prv, uint64_t addr, uint64_t len, dma_sg_t *sg, struct iovec *iov)
{
	int ret;
	lm_ctx_t *ctx = (lm_ctx_t*)prv;

	if (sg == NULL) {
		sg = alloca(sizeof(*sg));
	}
	if (iov == NULL) {
		iov = alloca(sizeof(*iov));
	}

	ret = lm_addr_to_sg(ctx, addr, len, sg, 1);
	if (ret != 1) {
		SPDK_ERRLOG("failed to map 0x%lx-0x%lx\n", addr, addr + len);
		return NULL;
	}

	ret = lm_map_sg(ctx, sg, iov, 1);
	if (ret != 0) {
		SPDK_ERRLOG("failed to map segment: %d\n", ret);
		return NULL;
	}

	return iov->iov_base;
}

static uint32_t
sq_head(struct muser_qpair *qpair)
{
	assert(qpair != NULL);
	return qpair->sq.head;
}

static void
sqhd_advance(struct muser_ctrlr *ctrlr, struct muser_qpair *qpair)
{
	assert(ctrlr != NULL);
	assert(qpair != NULL);
	qpair->sq.head = (qpair->sq.head + 1) % qpair->sq.size;
}

static void
insert_queue(struct muser_ctrlr *ctrlr, struct io_q *q,
	     const bool is_cq, const uint16_t id)
{
	struct io_q *_q;
	struct muser_qpair *qpair;

	assert(ctrlr != NULL);
	assert(q != NULL);

	qpair = ctrlr->qp[id];

	q->is_cq = is_cq;
	if (is_cq) {
		_q = &qpair->cq;
		*_q = *q;
		*hdbl(ctrlr, _q) = 0;
	} else {
		_q = &qpair->sq;
		*_q = *q;
		*tdbl(ctrlr, _q) = 0;
	}
}

static int
asq_map(struct muser_ctrlr *ctrlr)
{
	struct io_q q;

	assert(ctrlr != NULL);
	assert(ctrlr->qp[0]->sq.addr == NULL);
	/* XXX ctrlr->asq == 0 is a valid memory address */

	q.size = ctrlr->aqa.bits.asqs + 1;
	q.head = ctrlr->doorbells[0] = 0;
	q.cqid = 0;
	q.addr = map_one(ctrlr->lm_ctx, ctrlr->asq,
			 q.size * sizeof(struct spdk_nvme_cmd), NULL, NULL);
	if (!q.addr) {
		return -1;
	}
	insert_queue(ctrlr, &q, false, 0);
	return 0;
}

static uint16_t
cq_next(struct io_q *q)
{
	assert(q != NULL);
	assert(q->is_cq);
	return (q->tail + 1) % q->size;
}

static int
queue_index(uint16_t qid, int is_cq)
{
	return (qid * 2) + is_cq;
}

static uint32_t*
_dbl(struct muser_ctrlr *ctrlr, uint16_t qid, bool is_cq)
{
	assert(ctrlr != NULL);
	return &ctrlr->doorbells[queue_index(qid, is_cq)];
}

/*
 * Don't use directly, use tdbl and hdbl instead which check that queue type. */
static uint32_t*
dbl(struct muser_ctrlr *ctrlr, struct io_q *q)
{
	assert(q != NULL);
	return _dbl(ctrlr, io_q_id(q), q->is_cq);
}

static uint32_t*
tdbl(struct muser_ctrlr *ctrlr, struct io_q *q)
{
	assert(ctrlr != NULL);
	assert(q != NULL);
	assert(!q->is_cq);
	return dbl(ctrlr, q);
}


static uint32_t*
hdbl(struct muser_ctrlr *ctrlr, struct io_q *q)
{
	assert(ctrlr != NULL);
	assert(q != NULL);
	assert(q->is_cq);
	return dbl(ctrlr, q);
}

static bool
cq_is_full(struct muser_ctrlr *ctrlr, struct io_q *q)
{
	assert(ctrlr != NULL);
	assert(q != NULL);
	return cq_next(q) == *hdbl(ctrlr, q);
}

static void
cq_tail_advance(struct io_q *q)
{
	assert(q != NULL);
	q->tail = cq_next(q);
}

static int
acq_map(struct muser_ctrlr *ctrlr)
{
	struct io_q *q;

	assert(ctrlr != NULL);
	assert(ctrlr->qp[0] != NULL);
	assert(ctrlr->qp[0]->cq.addr == NULL);
	assert(ctrlr->acq != 0);

	q = &ctrlr->qp[0]->cq;

	q->size = ctrlr->aqa.bits.acqs + 1;
	q->tail = 0;
	q->addr = map_one(ctrlr->lm_ctx, ctrlr->acq,
			  q->size * sizeof(struct spdk_nvme_cpl), NULL, NULL);
	if (q->addr == NULL) {
		return -1;
	}
	q->is_cq = true;
	return 0;
}

static ssize_t
host_mem_page_size(uint8_t mps)
{
	/*
	 * only 4 lower bits can be set
	 * TODO this function could go into core SPDK
	 */
	if (0xf0 & mps) {
		return -EINVAL;
	}
	return 1 << (12 + mps);
}

static void *
_map_one(void *prv, uint64_t addr, uint64_t len)
{
	return map_one(prv, addr, len, NULL, NULL);
}

static int
muser_map_prps(struct muser_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
               struct iovec *iov, uint32_t length)
{
	return spdk_nvme_map_prps(ctrlr->lm_ctx, cmd, iov, length,
	                          host_mem_page_size(ctrlr->cc.bits.mps), /* TODO don't compute this every time, store it in ctrlr */
	                          _map_one);
}

/*
 * Maps a DPTR (currently a single page PRP) to our virtual memory.
 */
static int
dptr_remap(struct muser_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd, size_t size)
{
	struct iovec iov;

	assert(ctrlr);
	assert(cmd);

	assert(!cmd->dptr.prp.prp2); /* FIXME */

	if (muser_map_prps(ctrlr, cmd, &iov, size) != 1) {
		return -1;
	}
	cmd->dptr.prp.prp1 = (uint64_t)iov.iov_base >> ctrlr->cc.bits.mps;
	return 0;
}

#ifdef DEBUG
/* TODO does such a function already exist in SPDK? */
static bool
is_prp(struct spdk_nvme_cmd *cmd)
{
	return cmd->psdt == 0;
}
#endif

static struct spdk_nvmf_request*
get_nvmf_req(struct muser_qpair *qp);

static int
handle_cmd_req(struct muser_ctrlr * ctrlr, struct spdk_nvme_cmd * cmd,
               struct spdk_nvmf_request * req);


/* XXX SPDK thread */
/* FIXME looks very similar to consume_io_req, maybe convert this function to
 * something like 'prepare admin req' and then call consume_io_req? */
static int
handle_admin_req(struct muser_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	int err;
	struct spdk_nvmf_request *req;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	assert(is_prp(cmd));

	/*
	 * FIXME why do we specify size sizeof(struct spdk_nvme_cmd)? Check the
	 * spec.
	 */
	err = dptr_remap(ctrlr, cmd, sizeof(struct spdk_nvme_cmd));
	if (err) {
		SPDK_ERRLOG("failed to remap DPTR: %d\n", err);
		return -1;
	}

	req = get_nvmf_req(ctrlr->qp[0]);
	assert(req != NULL); /* FIXME */

	/* FIXME have handle_cmd_req to call get_nvmf_req internally */
	return handle_cmd_req(ctrlr, cmd, req);
}

/*
 * TODO move into nvme_spec.h and change nvme_pcie_ctrlr_cmd_create_io_cq to
 * use it, also check for other functions
 */
union __attribute__((packed)) spdk_nvme_create_io_q_cdw10 {
	uint32_t raw;
	struct __attribute__((packed)) {
		uint32_t qid	: 16;
		uint32_t qsize	: 16;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_create_io_q_cdw10) == 4, "Incorrect size");

union __attribute__((packed)) spdk_nvme_create_io_cq_cdw11 {
	uint32_t raw;
	struct __attribute__((packed)) {
		uint32_t pc		: 1;
		uint32_t ien		: 1;
		uint32_t reserved	: 14;
		uint32_t iv		: 16;

	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_create_io_cq_cdw11) == 4, "Incorrect size");

union __attribute__((packed)) spdk_nvme_create_io_sq_cdw11 {
	uint32_t raw;
	struct __attribute__((packed)) {
		uint32_t pc		: 1;
		uint32_t qprio		: 2;
		uint32_t reserved	: 13;
		uint32_t cqid		: 16;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_create_io_sq_cdw11) == 4, "Incorrect size");

union __attribute__((packed)) spdk_nvme_del_io_q_cdw10 {
	uint32_t raw;
	struct __attribute__((packed)) {
		uint32_t qid		: 16;
		uint32_t reserved	: 16;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_del_io_q_cdw10) == 4, "Incorrect size");

static void
handle_identify_ctrlr_rsp(struct muser_ctrlr *ctrlr,
                          struct spdk_nvme_ctrlr_data *data)
{
	assert(ctrlr != NULL);
	assert(data != NULL);

	data->sgls.supported = SPDK_NVME_SGLS_NOT_SUPPORTED;

	/*
	 * Intentionally disabled, otherwise we get a
	 * SPDK_NVME_OPC_DATASET_MANAGEMENT command we don't know how to
	 * properly handle.
	 */
	data->oncs.dsm = 0;
}

static void
handle_identify_rsp(struct muser_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	assert(ctrlr != NULL);
	assert(cmd != NULL);

	if ((cmd->cdw10 & 0xFF) == SPDK_NVME_IDENTIFY_CTRLR) {
		handle_identify_ctrlr_rsp(ctrlr,
		                          (struct spdk_nvme_ctrlr_data *)cmd->dptr.prp.prp1);
	}
}

/*
 * Completes a admin request.
 */
static int
post_completion(struct muser_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
                        struct io_q *cq, struct spdk_nvmf_request *req,
                        uint16_t sc)
{
	struct spdk_nvme_cpl *cpl;
	uint16_t qid;
	int err;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	qid = io_q_id(cq);

	if (cq_is_full(ctrlr, cq)) {
		SPDK_ERRLOG("CQ%d full (tail=%d, head=%d)\n",
			    qid, cq->tail, *hdbl(ctrlr, cq));
		return -1;
	}

	cpl = ((struct spdk_nvme_cpl *)cq->addr) + cq->tail;

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "request complete SQ%d cid=%d status=%#x SQ head=%#x CQ tail=%#x\n",
	              qid, cmd->cid, sc, ctrlr->qp[qid]->sq.head, cq->tail);

	if (qid == 0) {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_IDENTIFY:
			handle_identify_rsp(ctrlr, cmd);
			break;
		case SPDK_NVME_OPC_SET_FEATURES:
			assert(req != NULL);
			cpl->cdw0 = req->rsp->nvme_cpl.cdw0;
			break;
		}
	}

	assert(ctrlr->qp[qid] != NULL);

	cpl->sqhd = (ctrlr->qp[qid]->sq.head + 1) % ctrlr->qp[qid]->sq.size;
	cpl->cid = cmd->cid;
	cpl->status.dnr = 0x0;
	cpl->status.m = 0x0;
	cpl->status.sct = 0x0;
	cpl->status.p = ~cpl->status.p;
	cpl->status.sc = sc;

	cq_tail_advance(cq);

	/*
	 * FIXME check STS.IS "Indicates the interrupt status of the device (‘1’ = asserted)."
	 * FIXME this function now executes at SPDK thread context, we
	 * might be triggerring interrupts from MUSER thread context so
	 * check for race conditions.
	 */
	err = lm_irq_trigger(ctrlr->lm_ctx, cq->iv);
	if (err != 0) {
		SPDK_ERRLOG("failed to trigger interrupt: %m\n");
		return err;
	}

	return 0;
}

static struct io_q *
lookup_io_q(struct muser_ctrlr *ctrlr, const uint16_t qid, const bool is_cq)
{
	struct io_q *q;

	assert(ctrlr != NULL);

	if (qid > MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR) {
		return NULL;
	}

	if (ctrlr->qp[qid] == NULL) {
		return NULL;
	}

	if (is_cq) {
		q = &ctrlr->qp[qid]->cq;
	} else {
		q = &ctrlr->qp[qid]->sq;
	}

	if (q->addr == NULL) {
		return NULL;
	}

	return q;
}

static void
destroy_io_q(lm_ctx_t *lm_ctx, struct io_q *q)
{
	if (q == NULL) {
		return;
	}
	if (q->addr != NULL) {
		lm_unmap_sg(lm_ctx, &q->sg, &q->iov, 1);
		q->addr = NULL;
	}
}

static void
muser_nvmf_subsystem_paused(struct spdk_nvmf_subsystem *subsys,
                             void *cb_arg, int status)
{
	sem_t *sem = (sem_t*)cb_arg;
	int err;

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "NVMf subsystem=%p paused=%d\n",
	              subsys, status);

	assert(status == 0); /* FIXME */
	assert(sem != NULL);

	err = sem_post(sem);
	assert(err == 0); /* FIXME */
}

static void
destroy_io_qp(struct muser_qpair *qp)
{
	if (qp->ctrlr == NULL) {
		return;
	}
	destroy_io_q(qp->ctrlr->lm_ctx, &qp->sq);
	destroy_io_q(qp->ctrlr->lm_ctx, &qp->cq);
}

static void
tear_down_qpair(struct muser_qpair *qpair)
{
	free(qpair->reqs_internal);
	free(qpair->cmds_internal);
	free(qpair->rsps_internal);
}

/*
 * FIXME we can immediately remove the QP from the list because this function
 * is now executed by the SPDK thread.
 */
static void
destroy_qp(struct muser_ctrlr *ctrlr, uint16_t qid)
{
	struct muser_qpair *qpair;

	assert(ctrlr != NULL);

	qpair = ctrlr->qp[qid];
	if (qpair == NULL) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "destroy QP%d=%p, removing from group=%p\n",
	              qid, qpair, ctrlr->muser_group);

	/*
	 * FIXME Is it possible for the pointer to be accessed while we're
	 * tearing down the queue?
	 */
	destroy_io_qp(qpair);
	tear_down_qpair(qpair);
	free(qpair);
	ctrlr->qp[qid] = NULL;
}

static int
init_qp(struct muser_ctrlr *ctrlr, struct spdk_nvmf_transport *transport,
        const uint16_t qsize, const uint16_t id)
{
	int err = 0, i;
	struct muser_qpair *qpair;

	assert(ctrlr != NULL);
	assert(transport != NULL);

	qpair = calloc(1, sizeof(*qpair));
	if (qpair == NULL) {
		return -ENOMEM;
	}

	qpair->qpair.qid = id;
	qpair->qpair.transport = transport;
	qpair->ctrlr = ctrlr;
	qpair->qsize = qsize;

	TAILQ_INIT(&qpair->reqs);

	qpair->rsps_internal = calloc(qsize, sizeof(union nvmf_c2h_msg));
	if (qpair->rsps_internal == NULL) {
		SPDK_ERRLOG("Error allocating rsps: %m\n");
		err = -ENOMEM;
		goto out;
	}

	qpair->cmds_internal = calloc(qsize, sizeof(union nvmf_h2c_msg));
	if (qpair->cmds_internal == NULL) {
		SPDK_ERRLOG("Error allocating cmds: %m\n");
		err = -ENOMEM;
		goto out;
	}

	qpair->reqs_internal = calloc(qsize, sizeof(struct muser_req));
	if (qpair->reqs_internal == NULL) {
		SPDK_ERRLOG("Error allocating reqs: %m\n");
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < qsize; i++) {
		qpair->reqs_internal[i].req.qpair = &qpair->qpair;
		qpair->reqs_internal[i].req.rsp = &qpair->rsps_internal[i];
		qpair->reqs_internal[i].req.cmd = &qpair->cmds_internal[i];
		TAILQ_INSERT_TAIL(&qpair->reqs, &qpair->reqs_internal[i], link);
	}
	ctrlr->qp[id] = qpair;
out:
	if (err) {
		tear_down_qpair(qpair);
	}
	return err;
}

/* XXX SPDK thread */
static int
add_qp(struct muser_ctrlr *ctrlr, struct spdk_nvmf_transport *transport,
       const uint16_t qsize, const uint16_t qid, struct spdk_nvme_cmd *cmd)
{
	int err;
	struct muser_transport *muser_transport;

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "request add QP%d\n", qid);

	err = init_qp(ctrlr, transport, qsize, qid);
	if (err) {
		return err;
	}
	ctrlr->qp[qid]->cmd = cmd;

	muser_transport = SPDK_CONTAINEROF(transport, struct muser_transport,
					   transport);

	/*
	 * After we've returned from the muser_poll_group_poll thread, once
	 * muser_accept executes it will pick up this QP and will eventually
	 * call muser_poll_group_add. The rest of the opertions needed to
	 * complete the addition of the queue will be continued at the
	 * completion callback.
	 */
	TAILQ_INSERT_TAIL(&muser_transport->new_qps, ctrlr->qp[qid], link);

	return 0;
}

/*
 * XXX SPDK thread
 * Creates a completion or sumbission I/O queue.
 */
static int
handle_create_io_q(struct muser_ctrlr *ctrlr,
		   struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	union spdk_nvme_create_io_q_cdw10 *cdw10;
	union spdk_nvme_create_io_cq_cdw11 *cdw11_cq = NULL;
	union spdk_nvme_create_io_sq_cdw11 *cdw11_sq = NULL;
	size_t entry_size;
	int err = 0;

	/*
	 * XXX don't call io_q_id on this. Maybe operate directly on the
	 * ctrlr->qp[id].cq/sq?
	 */
	struct io_q io_q = { 0 };

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	cdw10 = (union spdk_nvme_create_io_q_cdw10 *)&cmd->cdw10;

	SPDK_NOTICELOG("create I/O %cQ: QID=0x%x, QSIZE=0x%x\n",
		       is_cq ? 'C' : 'S', cdw10->bits.qid, cdw10->bits.qsize);

	if (cdw10->bits.qid >= MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR) {
		SPDK_ERRLOG("invalid QID=%d, max=%d\n", cdw10->bits.qid,
			    MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR);
		return -EINVAL;
	}

	if (lookup_io_q(ctrlr, cdw10->bits.qid, is_cq)) {
		SPDK_ERRLOG("%cQ%d already exists\n", is_cq ? 'C' : 'S',
			    cdw10->bits.qid);

		/*
		 * FIXME once this functions fails we seem to endlessly call it,
		 * check that we queue the relevant completion.
		 */
		assert(false);

		return -EEXIST;
	}

	/* TODO break rest of this function into smaller functions */
	if (is_cq) {
		cdw11_cq = (union spdk_nvme_create_io_cq_cdw11 *)&cmd->cdw11;
		entry_size = sizeof(struct spdk_nvme_cpl);
		assert(cdw11_cq->bits.pc == 0x1); /* FIXME */
		io_q.ien = cdw11_cq->bits.ien;
		io_q.iv = cdw11_cq->bits.iv;
	} else {
		/* CQ must be created before SQ */
		cdw11_sq = (union spdk_nvme_create_io_sq_cdw11 *)&cmd->cdw11;
		if (!lookup_io_q(ctrlr, cdw11_sq->bits.cqid, true)) {
			SPDK_ERRLOG("CQ%d does not exist\n", cdw11_sq->bits.cqid);
			return -ENOENT;
		}
		entry_size = sizeof(struct spdk_nvme_cmd);
		assert(cdw11_sq->bits.pc == 0x1); /* FIXME */

		io_q.cqid = cdw11_sq->bits.cqid;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "CQID=%d\n", io_q.cqid);
	}

	io_q.size = cdw10->bits.qsize + 1;
	if (io_q.size > max_queue_size(ctrlr)) {
		SPDK_ERRLOG("queue too big, want=%d, max=%d\n", io_q.size,
			    max_queue_size(ctrlr));
		return -E2BIG;
	}
	io_q.addr = map_one(ctrlr->lm_ctx, cmd->dptr.prp.prp1,
			    io_q.size * entry_size, &io_q.sg, &io_q.iov);
	if (!io_q.addr) {
		SPDK_ERRLOG("failed to map I/O queue\n");
		return -1;
	}

	if (is_cq) {
		err = add_qp(ctrlr, ctrlr->qp[0]->qpair.transport, io_q.size,
		             cdw10->bits.qid, cmd);
		assert(err == 0); /* FIXME */
	}

	/* FIXME shouldn't we do this at completion? */
	insert_queue(ctrlr, &io_q, is_cq, cdw10->bits.qid);

	/* For CQ the completion is posted at by handle_connect_rsp. */
	if (!is_cq) {
		/* FIXME last arg sc might be a define already? */
		err = post_completion(ctrlr, cmd, &ctrlr->qp[0]->cq, NULL, 0);
	}

	return err;
}

/*
 * Deletes a completion or sumbission I/O queue.
 */
static int
handle_del_io_q(struct muser_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	union spdk_nvme_del_io_q_cdw10 *cdw10;
	struct io_q *q;
	uint16_t sc = 0;

	cdw10 = (union spdk_nvme_del_io_q_cdw10 *)&cmd->cdw10;

	SPDK_NOTICELOG("delete I/O %cQ: QID=%d\n",
		       is_cq ? 'C' : 'S', cdw10->bits.qid);

	q = lookup_io_q(ctrlr, cdw10->bits.qid, is_cq);
	if (q == NULL) {
		SPDK_ERRLOG("%cQ%d does not exist\n", is_cq ? 'C' : 'S',
			    cdw10->bits.qid);
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (is_cq) {
		/* SQ must have been deleted first */
		struct io_q *sq = lookup_io_q(ctrlr, cdw10->bits.qid, false);
		if (sq) {
			/* FIXME add error message */
			sc = SPDK_NVME_SC_INVALID_QUEUE_DELETION;
			goto out;
		}
	} else {
		/*
		 * FIXME this doesn't actually delete the I/O, we can't do that
		 * anyway because we can't do that in NVMf. We're merely telling
		 * the poll_group_poll function to skip checking this queue. The
		 * only workflow this works is when CC.EN is set to 0 and we're
		 * stopping the subsystem, so we know the relevant callbacks to
		 * destroy the queues will be called.
		 */
		ctrlr->qp[cdw10->bits.qid]->del = true;
	}

out:
	return post_completion(ctrlr, cmd, &ctrlr->qp[0]->cq, NULL, sc);
}

/*
 * XXX SPDK thread
 *
 * Returns 0 on success and -errno on error.
 */
static int
consume_admin_req(struct muser_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	assert(ctrlr != NULL);
	assert(cmd != NULL);

	SPDK_NOTICELOG("handle admin req opc=%#x cid=%d\n", cmd->opc, cmd->cid);

	switch (cmd->opc) {
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST: /* FIXME implement */
		SPDK_NOTICELOG("FIXME ignore async event request\n");
		return 0;
	/* TODO put all cases in order */
	case SPDK_NVME_OPC_IDENTIFY:
	case SPDK_NVME_OPC_SET_FEATURES:
	case SPDK_NVME_OPC_GET_LOG_PAGE:
	case SPDK_NVME_OPC_NS_MANAGEMENT:
		return handle_admin_req(ctrlr, cmd);
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		return handle_create_io_q(ctrlr, cmd,
		                          cmd->opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	/* FIXME need to queue completion response */
	case SPDK_NVME_OPC_ABORT:
		SPDK_NOTICELOG("FIXME ignore abort request\n");
		return 0;
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		return handle_del_io_q(ctrlr, cmd, cmd->opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	default:
		SPDK_ERRLOG("unsupported command 0x%x\n", cmd->opc);
		return -1;
	}
	return -1;
}

static void
handle_cmd_rsp(struct muser_qpair *qpair, struct muser_req *req)
{
	int err;

	assert(qpair != NULL);
	assert(req != NULL);

	err = post_completion(qpair->ctrlr, &req->req.cmd->nvme_cmd,
	                       &qpair->ctrlr->qp[req->req.qpair->qid]->cq,
	                       &req->req, 0);
	assert(err == 0); /* FIXME */
}

static int
consume_io_req(struct muser_ctrlr *ctrlr, struct muser_qpair *qpair,
               struct spdk_nvme_cmd *cmd)
{
	struct spdk_nvmf_request *req;

	assert(cmd != NULL);
	assert(qpair != NULL);

	req = get_nvmf_req(qpair);
	assert(req != NULL); /* FIXME */
	return handle_cmd_req(ctrlr, cmd, req);
}

/* XXX SPDK thread
 *
 * Returns 0 on success and -errno on error.
 */
static int
consume_req(struct muser_ctrlr *ctrlr, struct muser_qpair *qpair,
	    struct spdk_nvme_cmd *cmd)
{
	assert(qpair != NULL);
	if (spdk_nvmf_qpair_is_admin_queue(&qpair->qpair)) {
		return consume_admin_req(ctrlr, cmd);
	}
	return consume_io_req(ctrlr, qpair, cmd);
}

/* XXX SPDK thread */
static int
consume_reqs(struct muser_ctrlr *ctrlr, const uint32_t new_tail,
	     struct muser_qpair *qpair)
{
	struct spdk_nvme_cmd *queue;

	assert(ctrlr != NULL);
	assert(qpair != NULL);

	/*
	 * FIXME can queue size change arbitrarily? Shall we operate on a copy ?
	 *
	 * FIXME operating on an SQ is pretty much the same for admin and I/O
	 * queues. All we need is a callback to replace consume_req,
	 * depending on the type of the queue.
	 *
	 */
	queue = qpair->sq.addr;
	while (sq_head(qpair) != new_tail) {
		int err;
		struct spdk_nvme_cmd *cmd = &queue[sq_head(qpair)];

		/*
		 * SQHD must contain the new head pointer, so we must increase
		 * it before we generate a completion.
		 */
		sqhd_advance(ctrlr, qpair);

		err = consume_req(ctrlr, qpair, cmd);
		if (err != 0) {
			/* FIXME how should we proceed now? */
			SPDK_ERRLOG("failed to process request\n");
			return err;
		}
	}
	return 0;
}

/*
 * XXX SPDK thread context
 */
static ssize_t
handle_sq_tdbl_write(struct muser_ctrlr *ctrlr, const uint32_t new_tail,
		     struct muser_qpair *qpair)
{
	assert(ctrlr != NULL);
	assert(qpair != NULL);
	return consume_reqs(ctrlr, new_tail, qpair);
}

/*
 * Handles a write at offset 0x1000 or more.
 *
 * DSTRD is set to fixed value 0 for NVMf.
 */
static int
handle_dbl_access(struct muser_ctrlr *ctrlr, uint32_t *buf,
		  const size_t count, loff_t pos, const bool is_write)
{
	assert(ctrlr != NULL);
	assert(buf != NULL);

	if (count != sizeof(uint32_t)) {
		SPDK_ERRLOG("bad doorbell buffer size %ld\n", count);
		return -EINVAL;
	}

	pos -= DOORBELLS;

	/* pos must be dword aligned */
	if ((pos & 0x3) != 0) {
		SPDK_ERRLOG("bad doorbell offset %#lx\n", pos);
		return -EINVAL;
	}

	/* convert byte offset to array index */
	pos >>= 2;

	/* FIXME need to make sure index is within bounds */
	if (is_write) {
		ctrlr->doorbells[pos] = *buf;
		spdk_wmb();
	} else {
		spdk_rmb();
		*buf = ctrlr->doorbells[pos];
	}
	return 0;
}

/*
 * FIXME Is there any benefit in forwarding the write to the SPDK thread and
 * handling it there? This way we can optionally make writes posted, which may
 * or may not be a good thing. Also, if we handle writes at the the SPDK thread
 * we won't be able to synchronously wait, we'll have to execute everything in
 * callbacks and schedule the next piece of work from the callback handlers,
 * and this sounds more difficult to implement.
 */
static int
handle_cc_write(struct muser_ctrlr *ctrlr, uint8_t *buf,
                const size_t count, const loff_t pos)
{
	union spdk_nvme_cc_register *cc = (union spdk_nvme_cc_register*)buf;
	int err;

	assert(ctrlr != NULL);
	assert(cc != NULL);
	assert(count == sizeof(union spdk_nvme_cc_register));

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "write CC=%#x\n", cc->raw);

	/*
	 * FIXME is it OK to access the controller registers like this without
	 * a proper property request?
	 */

	/*
	 * Host driver attempts to reset (set CC.EN to 0), which isn't
	 * supported in NVMf. We must first shutdown the controller and then
	 * set CC.EN to 0.
	 */
	if (cc->bits.en == 0 && ctrlr->qp[0]->qpair.ctrlr->vcprop.cc.bits.en == 1) {

		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "CC.EN 1 -> 0\n");

		/*
		 * FIXME We send two requests to the SPDK thread, one after
		 * after another, synchronously waiting for them to complete.
		 * Is it better to have the SPDK thread issue the second
		 * request?
		 */

		SPDK_NOTICELOG("shutdown controller\n");
		cc->bits.en = 1;
		cc->bits.shn = SPDK_NVME_SHN_NORMAL;
		err = do_prop_req(ctrlr, buf, count, pos, true);
		assert(err == 0); /* FIXME */
		SPDK_NOTICELOG("controller shut down\n");
		/* FIXME we shouldn't expect it to shutdown immediately */
		if (ctrlr->qp[0]->qpair.ctrlr->vcprop.csts.bits.shst != SPDK_NVME_SHST_COMPLETE) {
		        SPDK_ERRLOG("controller didn't shutdown\n");
		        return -1;
		}

		/* FIXME Shouldn't CSTS.SHST be set by NVMf? */
		ctrlr->qp[0]->qpair.ctrlr->vcprop.csts.bits.shst = 0;
		cc->bits.en = 0;
		cc->bits.shn = 0;
		SPDK_NOTICELOG("disable controller\n");

	} else if (cc->bits.en == 1 &&
		   ctrlr->qp[0]->qpair.ctrlr->vcprop.cc.bits.en == 0 &&
		   !muser_spdk_nvmf_subsystem_is_active(ctrlr)) {
		/*
		 * FIXME CC.EN == 0 does not necessarily mean that NVMf subsys
		 * is inactive.  We must first tell the NVMf subsystem to resume
		 * and then set CC.EN to 1.
		 */
		err = muser_request_spdk_nvmf_subsystem_resume(ctrlr);
		assert(err == 0); /* FIXME */
	} else if (cc->bits.en == 0 &&
		   ctrlr->qp[0]->qpair.ctrlr->vcprop.cc.bits.en == 0) {
			return 0;
	}

	err = do_prop_req(ctrlr, buf, count, pos, true);
	assert(err == 0); /* FIXME */

	if (cc->bits.en == 0 && ctrlr->qp[0] != NULL) {
		/* FIXME need to delete admin queues, however destroy_qp must be
		 * called at SPDK thread context.
		 * FIXME is this really needed? Don't we get a callback for
		 * deleting the admin queue?
		 */
		err = sem_init(&ctrlr->sem, 0, 0);
		assert(err == 0); /* FIXME */
		ctrlr->del_admin_qp = true;
		err = sem_wait(&ctrlr->sem);
		assert(err == 0); /* FIXME */
	}

	return 0;
}

static ssize_t
write_bar0(void *pvt, char *buf, size_t count, loff_t pos)
{
	struct muser_ctrlr *ctrlr = pvt;

	SPDK_NOTICELOG("\nctrlr: %p, count=%zu, pos=%"PRIX64"\n",
		       ctrlr, count, pos);
	spdk_log_dump(stdout, "muser_write", buf, count);

	switch (pos) {
	/* TODO sort cases */
	case ADMIN_QUEUES:
		return admin_queue_write(ctrlr, buf, count, pos);
	case CC:
		return handle_cc_write(ctrlr, buf, count, pos);
	default:
		if (pos >= DOORBELLS) {
			return handle_dbl_access(ctrlr, (uint32_t*)buf, count,
			                         pos, true);
		}
		break;
	}
	SPDK_ERRLOG("write to 0x%lx not implemented\n", pos);
	return -ENOTSUP;
}

static ssize_t
access_bar_fn(void *pvt, char *buf, size_t count, loff_t offset,
              const bool is_write)
{
	ssize_t ret;

	/*
	 * FIXME it doesn't make sense to have separate functions for the BAR0,
	 * since a lot of the code is common, e.g. figuring out which doorbell
	 * is accessed. Merge.
	 */
	if (is_write) {
		ret = write_bar0(pvt, buf, count, offset);
	} else {
		ret = read_bar0(pvt, buf, count, offset);
	}

	if (ret != 0) {
		SPDK_WARNLOG("failed to %s %lx@%lx BAR0: %zu\n",
			     is_write ? "write" : "read", offset, count, ret);
		return -1;
	}
	return count;
}

/*
 * NVMe driver reads 4096 bytes, which is the extended PCI configuration space
 * available on PCI-X 2.0 and PCI Express buses
 */
static ssize_t
access_pci_config(void *pvt, char *buf, size_t count, loff_t offset,
		  const bool is_write)
{
	struct muser_ctrlr *ctrlr = (struct muser_ctrlr *)pvt;

	if (is_write) {
		fprintf(stderr, "writes not supported\n");
		return -EINVAL;
	}

	if (offset + count > PCI_CFG_SPACE_EXP_SIZE) {
		fprintf(stderr, "access past end of extended PCI configuration space, want=%ld+%ld, max=%d\n",
			offset, count, PCI_CFG_SPACE_EXP_SIZE);
		return -ERANGE;
	}

	memcpy(buf, ((unsigned char *)ctrlr->pci_config_space) + offset, count);

	return count;
}

static ssize_t
pmcap_access(void *pvt, const uint8_t id, char * const buf, const size_t count,
             const loff_t offset, const bool is_write)
{
	struct muser_ctrlr *ctrlr = (struct muser_ctrlr *)pvt;

	if (is_write)
		assert(false); /* TODO */

	memcpy(buf, ((char*)&ctrlr->pmcap) + offset, count);

	return count;
}

static ssize_t
handle_mxc_write(struct muser_ctrlr *ctrlr, const struct mxc * const mxc)
{
	uint16_t n;

	assert(ctrlr != NULL);
	assert(mxc != NULL);

	/* host driver writes RO field, don't know why */
	if (ctrlr->msixcap.mxc.ts == *(uint16_t*)mxc) {
		goto out;
	}

	n = ~(PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE) & *((uint16_t*)mxc);
	if (n != 0) {
		SPDK_ERRLOG("bad write 0x%x to MXC\n", n);
		return -EINVAL;
	}

	if (mxc->mxe != ctrlr->msixcap.mxc.mxe) {
		/*
		 * FIXME need to check that MSI enable bit is set to 0 before
		 * enabling MSI-X.
		 */
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "%s MSI-X\n",
		              mxc->mxe ? "enable" : "disable");
		ctrlr->msixcap.mxc.mxe = mxc->mxe;
	}

	if (mxc->fm != ctrlr->msixcap.mxc.fm) {
		if (mxc->fm) {
			SPDK_DEBUGLOG(SPDK_LOG_MUSER,
			              "all MSI-X vectors masked\n");
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_MUSER,
			              "vector's mask bit determines whether vector is masked");
		}
		ctrlr->msixcap.mxc.fm = mxc->fm;
	}
out:
	return sizeof (struct mxc);
}

static ssize_t
handle_msix_write(struct muser_ctrlr *ctrlr, char * const buf, const size_t count,
                  const loff_t offset)
{
	if (count == sizeof (struct mxc)) {
		switch (offset) {
			case offsetof(struct msixcap, mxc):
				return handle_mxc_write(ctrlr, (struct mxc*)buf);
			default:
				SPDK_ERRLOG("invalid MSI-X write offset %ld\n",
				            offset);
				return -EINVAL;
		}
	}
	SPDK_ERRLOG("invalid MSI-X write size %lu\n", count);
	return -EINVAL;
}

static ssize_t
msixcap_access(void *pvt, const uint8_t id, char * const buf, size_t count,
               loff_t offset, const bool is_write)
{
	struct muser_ctrlr *ctrlr = (struct muser_ctrlr *)pvt;

	if (is_write) {
		return handle_msix_write(ctrlr, buf, count, offset);
	}

	memcpy(buf, ((char*)&ctrlr->msixcap) + offset, count);

	return count;
}

static int
handle_msicap_mc_write(struct muser_ctrlr * const ctrlr, const struct mc * const mc)
{
	assert(ctrlr != NULL);
	assert(mc != NULL);

	if (mc->msie != ctrlr->msicap.mc.msie) {
		ctrlr->msicap.mc.msie = mc->msie;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MSI %s\n",
		              mc->msie ? "enable" : "disable");
	}

	if (mc->mmc != ctrlr->msicap.mc.mmc) {
		SPDK_ERRLOG("invalid write %#x to RO register MMC (%#x)\n",
		            mc->mmc, ctrlr->msicap.mc.mmc);
	}

	if (mc->mme != ctrlr->msicap.mc.mme) {
		ctrlr->msicap.mc.mme = mc->mme;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MME set to %#x\n",
		              ctrlr->msicap.mc.mme);
	}

	if (mc->c64 != ctrlr->msicap.mc.c64) {
		SPDK_ERRLOG("invalid write %#x to RO register C64 (%#x)\n",
		            mc->c64, ctrlr->msicap.mc.c64);
	}

	if (mc->pvm != ctrlr->msicap.mc.pvm) {
		SPDK_ERRLOG("invalid write %#x to RO register PVM (%#x)\n",
		            mc->pvm, ctrlr->msicap.mc.pvm);
	}

	if (mc->res1) {
		SPDK_ERRLOG("invalid write %#x to RO reserved\n", mc->res1);
	}

	return 0;
}

static int
handle_msicap_md_write(struct muser_ctrlr *ctrlr, const uint16_t data)
{
	assert(ctrlr != NULL);

	ctrlr->msicap.md = data;
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MD.DATA=%#x\n", ctrlr->msicap.md);
	return 0;
}

static int
handle_msicap_ma_write(struct muser_ctrlr *ctrlr, const struct ma * const ma)
{
	assert(ctrlr != NULL);
	assert(ma != NULL);

	if (ma->res1) {
		SPDK_ERRLOG("bad write to ma\n");
		return -EINVAL;
	}
	ctrlr->msicap.ma.addr = ma->addr;
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MA.ADDR=%#x\n", ctrlr->msicap.ma.addr);
	return 0;
}

static int
handle_msicap_mua_write(struct muser_ctrlr *ctrlr, const uint32_t uaddr)
{
	assert(ctrlr != NULL);

	ctrlr->msicap.mua = uaddr;
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MUA.UADDR=%#x\n", ctrlr->msicap.mua);
	return 0;
}

static int
handle_msicap_mmask_write(struct muser_ctrlr *ctrlr, const uint32_t mask)
{
	assert(ctrlr != NULL);

	ctrlr->msicap.mmask = mask;
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MMASK.MASK=%#x\n", ctrlr->msicap.mmask);
	return 0;
}

static int
handle_msicap_mpend_write(struct muser_ctrlr *ctrlr, const uint32_t pend)
{
	assert(ctrlr != NULL);

	ctrlr->msicap.mpend = pend;
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MPEND.PEND=%#x\n", ctrlr->msicap.mpend);
	return 0;
}

static int
handle_msicap_write_2_bytes(struct muser_ctrlr *c, char * const b, loff_t o)
{
	switch (o) {
		case offsetof(struct msicap, mc):
			return handle_msicap_mc_write(c, (struct mc*)b);
		case offsetof(struct msicap, md):
			return handle_msicap_md_write(c, *(uint16_t*)b);

	}
	return -EINVAL;
}

static int
handle_msicap_write_4_bytes(struct muser_ctrlr *c, char * const b, loff_t o)
{
	switch (o) {
		case offsetof(struct msicap, ma):
			return handle_msicap_ma_write(c, (struct ma*)b);
		case offsetof(struct msicap, mua):
			return handle_msicap_mua_write(c, *(uint32_t*)b);
		case offsetof(struct msicap, mmask):
			return handle_msicap_mmask_write(c, *(uint32_t*)b);
		case offsetof(struct msicap, mpend):
			return handle_msicap_mpend_write(c, *(uint32_t*)b);
	}
	return -EINVAL;
}

static int
handle_msicap_write(struct muser_ctrlr *ctrlr, char * const buf, size_t count,
                    loff_t offset)
{
	assert(ctrlr != NULL);

	switch (count) {
		case 2:
			return handle_msicap_write_2_bytes(ctrlr, buf, offset);
		case 4:
			return handle_msicap_write_4_bytes(ctrlr, buf, offset);
	}
	return -EINVAL;
}

static ssize_t
msicap_access(void *pvt, const uint8_t id, char * const buf, size_t count,
              loff_t offset, const bool is_write)
{
	struct muser_ctrlr *ctrlr = (struct muser_ctrlr *)pvt;

	if (is_write) {
		int err = handle_msicap_write(ctrlr, buf, count, offset);
		if (err != 0) {
			return err;
		}
	} else {
		memcpy(buf, ((char*)&ctrlr->msicap) + offset, count);
	}

	return count;
}

static int
handle_pxcap_pxdc_write(struct muser_ctrlr * const c, const union pxdc * const p)
{
	assert(c != NULL);
	assert(p != NULL);

	if (p->cere != c->pxcap.pxdc.cere) {
		c->pxcap.pxdc.cere = p->cere;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "CERE %s\n",
		              p->cere ? "enable" : "disable");
	}

	if (p->nfere != c->pxcap.pxdc.nfere) {
		c->pxcap.pxdc.nfere = p->nfere;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "NFERE %s\n",
		              p->nfere ? "enable" : "disable");
	}

	if (p->fere != c->pxcap.pxdc.fere) {
		c->pxcap.pxdc.fere = p->fere;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "FERE %s\n",
		              p->fere ? "enable" : "disable");
	}

	if (p->urre != c->pxcap.pxdc.urre) {
		c->pxcap.pxdc.urre = p->urre;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "URRE %s\n",
		              p->urre ? "enable" : "disable");
	}

	if (p->ero != c->pxcap.pxdc.ero) {
		c->pxcap.pxdc.ero = p->ero;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "ERO %s\n",
		              p->ero ? "enable" : "disable");
	}

	if (p->mps != c->pxcap.pxdc.mps) {
		c->pxcap.pxdc.mps = p->mps;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MPS set to %d\n", p->mps);
	}

	if (p->ete != c->pxcap.pxdc.ete) {
		c->pxcap.pxdc.ete = p->ete;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "ETE %s\n",
		              p->ete ? "enable" : "disable");
	}

	if (p->pfe != c->pxcap.pxdc.pfe) {
		c->pxcap.pxdc.pfe = p->pfe;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "PFE %s\n",
		              p->pfe ? "enable" : "disable");
	}

	if (p->appme != c->pxcap.pxdc.appme) {
		c->pxcap.pxdc.appme = p->appme;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "APPME %s\n",
		              p->appme ? "enable" : "disable");
	}

	if (p->ens != c->pxcap.pxdc.ens) {
		c->pxcap.pxdc.ens = p->ens;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "ENS %s\n",
		              p->ens ? "enable" : "disable");
	}

	if (p->mrrs != c->pxcap.pxdc.mrrs) {
		c->pxcap.pxdc.mrrs = p->mrrs;
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "MRRS set to %d\n", p->mrrs);
	}

	if (p->iflr) {
		SPDK_DEBUGLOG(SPDK_LOG_MUSER, "initiate function level reset\n");
	}

	return 0;
}

static int
handle_pxcap_write_2_bytes(struct muser_ctrlr *c, char * const b, loff_t o)
{
	switch (o) {
		case offsetof(struct pxcap, pxdc):
			return handle_pxcap_pxdc_write(c, (union pxdc*)b);
	}
	return -EINVAL;
}

static ssize_t
handle_pxcap_write(struct muser_ctrlr *ctrlr, char * const buf, size_t count,
                   loff_t offset)
{
	int err = -EINVAL;
	switch (count) {
		case 2:
			err = handle_pxcap_write_2_bytes(ctrlr, buf, offset);
			break;
	}
	if (err != 0) {
		assert(false); /* FIXME */
		return err;
	}
	return count;
}

static ssize_t
pxcap_access(void *pvt, const uint8_t id, char * const buf, size_t count,
                    loff_t offset, const bool is_write)
{
	struct muser_ctrlr *ctrlr = (struct muser_ctrlr *)pvt;

	if (is_write) {
		return handle_pxcap_write(ctrlr, buf, count, offset);
	}

	memcpy(buf, ((char*)&ctrlr->pxcap) + offset, count);

	return count;
}



static void
nvme_reg_info_fill(lm_reg_info_t *reg_info)
{
	assert(reg_info != NULL);

	memset(reg_info, 0, sizeof(*reg_info) * LM_DEV_NUM_REGS);

	reg_info[LM_DEV_BAR0_REG_IDX].flags = LM_REG_FLAG_RW;
	reg_info[LM_DEV_BAR0_REG_IDX].size  = NVME_REG_BAR0_SIZE;
	reg_info[LM_DEV_BAR0_REG_IDX].fn  = access_bar_fn;

	reg_info[LM_DEV_BAR4_REG_IDX].flags = LM_REG_FLAG_RW;
	reg_info[LM_DEV_BAR4_REG_IDX].size  = PAGE_SIZE;

	reg_info[LM_DEV_BAR5_REG_IDX].flags = LM_REG_FLAG_RW;
	reg_info[LM_DEV_BAR5_REG_IDX].size  = PAGE_SIZE;

	reg_info[LM_DEV_CFG_REG_IDX].flags = LM_REG_FLAG_RW;
	reg_info[LM_DEV_CFG_REG_IDX].size  = NVME_REG_CFG_SIZE;
	reg_info[LM_DEV_CFG_REG_IDX].fn  = access_pci_config;
}

static void
nvme_log(void *pvt, char const *msg)
{
	fprintf(stderr, "%s", msg);
}

static void
nvme_dev_info_fill(lm_dev_info_t *dev_info, struct muser_ctrlr *muser_ctrlr,
                   bool en_msi, bool en_msix)
{
	static const lm_cap_t pm = {.id = PCI_CAP_ID_PM,
                                    .size = sizeof(struct pmcap),
	                            .fn = pmcap_access};
	static const lm_cap_t px = {.id = PCI_CAP_ID_EXP,
	                            .size = sizeof(struct pxcap),
	                            .fn = pxcap_access};

	assert(dev_info != NULL);
	assert(muser_ctrlr != NULL);

	dev_info->pvt = muser_ctrlr;

	dev_info->uuid = muser_ctrlr->uuid;

	dev_info->pci_info.id.vid = 0x4e58;     /* TODO: LE ? */
	dev_info->pci_info.id.did = 0x0001;

	/* controller uses the NVM Express programming interface */
	dev_info->pci_info.cc.pi = 0x02;

	/* non-volatile memory controller */
	dev_info->pci_info.cc.scc = 0x08;

	/* mass storage controller */
	dev_info->pci_info.cc.bcc = 0x01;

	dev_info->pci_info.irq_count[LM_DEV_INTX_IRQ_IDX] = NVME_IRQ_INTX_NUM;

	dev_info->caps[dev_info->nr_caps++] = pm;

	if (en_msi) {
		static const lm_cap_t msi = {.id = PCI_CAP_ID_MSI,
		                             .size = sizeof(struct msicap),
		                             .fn = msicap_access};
		dev_info->pci_info.irq_count[LM_DEV_MSI_IRQ_IDX] = 1 << NVME_IRQ_MSI_NUM;
		dev_info->caps[dev_info->nr_caps++] = msi;
	}

	if (en_msix) {
		static const lm_cap_t msix = {.id = PCI_CAP_ID_MSIX,
		                              .size = sizeof(struct msixcap),
		                              .fn = msixcap_access};
		dev_info->pci_info.irq_count[LM_DEV_MSIX_IRQ_IDX] = NVME_IRQ_MSIX_NUM;
		dev_info->caps[dev_info->nr_caps++] = msix;
	}

	dev_info->caps[dev_info->nr_caps++] = px;

	dev_info->extended = true;

	nvme_reg_info_fill(dev_info->pci_info.reg_info);

	dev_info->log = nvme_log;
	dev_info->log_lvl = LM_DBG;
}

static void *
drive(void *arg)
{
	lm_ctx_t *lm_ctx = arg;

	lm_ctx_drive(lm_ctx);

	return NULL;
}

static void
init_pci_config_space(lm_pci_config_space_t *p)
{
	struct nvme_pcie_mlbar *mlbar;
	struct nvme_pcie_bar2 *nvme_bar2;

	/* MLBAR */
	mlbar = (struct nvme_pcie_mlbar *)&p->hdr.bars[0];
	memset(mlbar, 0, sizeof(*mlbar));

	/* MUBAR */
	p->hdr.bars[1].raw = 0x0;

	/*
	 * BAR2, index/data pair register base address or vendor specific (optional)
	 */
	nvme_bar2 = (struct nvme_pcie_bar2 *)&p->hdr.bars[2].raw;
	memset(nvme_bar2, 0, sizeof(*nvme_bar2));
	nvme_bar2->rte = 0x1;

	/* vendor specific, let's set them to zero for now */
	p->hdr.bars[3].raw = 0x0;
	p->hdr.bars[4].raw = 0x0;
	p->hdr.bars[5].raw = 0x0;

	/* enable INTx */
	p->hdr.intr.ipin = 0x1;
}

static int
muser_snprintf_subnqn(struct muser_ctrlr *ctrlr, subnqn_t *subnqn)
{
	int ret;

	assert(ctrlr != NULL);
	assert(subnqn != NULL);

	ret = snprintf((uint8_t*)subnqn, sizeof(*subnqn),
	               "nqn.2019-07.io.spdk.muser:%s", ctrlr->uuid);
	return (size_t)ret >= sizeof(*subnqn) ? -1 : 0;
}

static int
muser_listen(struct spdk_nvmf_transport *transport,
	     const struct spdk_nvme_transport_id *trid)
{
	struct muser_transport *muser_transport;
	struct muser_ctrlr *muser_ctrlr;
	lm_dev_info_t dev_info = { 0 };
	int err;
	const bool en_msi = false, en_msix = true;
	subnqn_t subnqn;

	muser_transport = SPDK_CONTAINEROF(transport, struct muser_transport,
					   transport);

	err = mdev_create(trid->traddr);
	if (err == -1) {
		return -1;
	}

	muser_ctrlr = calloc(1, sizeof(*muser_ctrlr));
	if (muser_ctrlr == NULL) {
		SPDK_ERRLOG("Error allocating ctrlr: %m\n");
		goto err;
	}
	muser_ctrlr->cntlid = 0xffff;
	assert(muser_transport->group != NULL);
	muser_ctrlr->muser_group = muser_transport->group;
	muser_ctrlr->muser_group->ctrlr = muser_ctrlr;
	memcpy(muser_ctrlr->uuid, trid->traddr, sizeof(muser_ctrlr->uuid));
	memcpy(&muser_ctrlr->trid, trid, sizeof(muser_ctrlr->trid));

	muser_ctrlr->prop_req.muser_req.req.rsp = &muser_ctrlr->prop_req.rsp;
	muser_ctrlr->prop_req.muser_req.req.cmd = &muser_ctrlr->prop_req.cmd;

	err = sem_init(&muser_ctrlr->sem, 0, 0);
	assert(err == 0); /* FIXME */

	err = muser_snprintf_subnqn(muser_ctrlr, &subnqn);
	assert(err == 0); /* FIXME */
	muser_ctrlr->subsys = spdk_nvmf_tgt_find_subsystem(transport->tgt,
	                                                   (char*)&subnqn);
	assert(muser_ctrlr->subsys != NULL); /* FIXME */

	/*
	 * Admin QP setup: in order to read NVMe registers from SPDK we must
	 * send NVMe requests to it, and SPDK expects them to be associated with
	 * a QP. Therefore we have to create the admin QP very early.
	 */
	/* FIXME queuedepth must come from NVMf */
	err = add_qp(muser_ctrlr, transport, MUSER_DEFAULT_AQ_DEPTH, 0, NULL);
	if (err) {
		goto err_free_dev;
	}

	/*
	 * TODO move the rest in separate function as they're specific to
	 * MUSER/PCI.
	 */

	/* LM setup */
	nvme_dev_info_fill(&dev_info, muser_ctrlr, en_msi, en_msix);

	/* PM */
	muser_ctrlr->pmcap.pmcs.nsfrst = 0x1;

	/* MSI */
	if (en_msi) {	
		muser_ctrlr->msicap.mc.mmc = 0x1;
		muser_ctrlr->msicap.mc.c64 = 0x1;
	}

	/* MSI-X */
	if (en_msix) {
		/*
		 * TODO for now we put table BIR and PBA BIR in BAR4 because
		 * it's just easier, otherwise in order to put it in BAR0 we'd
		 * have to figure out where exactly doorbells end.
		 */
		muser_ctrlr->msixcap.mxc.ts = 0x3;
		muser_ctrlr->msixcap.mtab.tbir = 0x4;
		muser_ctrlr->msixcap.mtab.to = 0x0;
		muser_ctrlr->msixcap.mpba.pbir = 0x5;
		muser_ctrlr->msixcap.mpba.pbao = 0x0;
	}

	/* EXP */
	muser_ctrlr->pxcap.pxcaps.ver = 0x2;
	muser_ctrlr->pxcap.pxdcap.per = 0x1;
	muser_ctrlr->pxcap.pxdcap.flrc = 0x1;
	muser_ctrlr->pxcap.pxdcap2.ctds = 0x1;
	/* FIXME check PXCAPS.DPT */

	muser_ctrlr->lm_ctx = lm_ctx_create(&dev_info);
	if (muser_ctrlr->lm_ctx == NULL) {
		/* TODO: lm_create doesn't set errno */
		SPDK_ERRLOG("Error creating libmuser ctx: %m\n");
		goto err_destroy_qp;
	}

	muser_ctrlr->pci_config_space = lm_get_pci_config_space(muser_ctrlr->lm_ctx);
	init_pci_config_space(muser_ctrlr->pci_config_space);

	err = pthread_create(&muser_ctrlr->lm_thr, NULL,
			     drive, muser_ctrlr->lm_ctx);
	if (err != 0) {
		/* TODO: pthread_create doesn't set errno */
		SPDK_ERRLOG("Error creating lm_drive thread: %m\n");
		goto err_destroy;
	}

	TAILQ_INSERT_TAIL(&muser_transport->ctrlrs, muser_ctrlr, link);

	return 0;

err_destroy:
	lm_ctx_destroy(muser_ctrlr->lm_ctx);
err_destroy_qp:
	destroy_qp(muser_ctrlr, 0);
err_free_dev:
	free(muser_ctrlr);
err:
	mdev_remove(trid->traddr);

	return -1;
}

static int
muser_stop_listen(struct spdk_nvmf_transport *transport,
		  const struct spdk_nvme_transport_id *trid)
{

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "stop listen\n");
	return -1;
}

/* FIXME when does this get executed? */
static void
muser_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn,
             void *cb_arg)
{
	int err;
	struct muser_transport *muser_transport;
	struct muser_qpair *qp, *tmp;

	muser_transport = SPDK_CONTAINEROF(transport, struct muser_transport,
					   transport);

	err = pthread_mutex_lock(&muser_transport->lock);
	if (err) {
		SPDK_ERRLOG("failed to lock poll group lock: %m\n");
		return;
	}

	TAILQ_FOREACH_SAFE(qp, &muser_transport->new_qps, link, tmp) {
		TAILQ_REMOVE(&muser_transport->new_qps, qp, link);
		cb_fn(&qp->qpair, NULL);
	}

	err = pthread_mutex_unlock(&muser_transport->lock);
	if (err) {
		SPDK_ERRLOG("failed to lock poll group lock: %m\n");
		return;
	}
}

static void
muser_discover(struct spdk_nvmf_transport *transport,
	       struct spdk_nvme_transport_id *trid,
	       struct spdk_nvmf_discovery_log_page_entry *entry)
{ }

static struct spdk_nvmf_transport_poll_group *
muser_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct muser_poll_group *muser_group;
	struct muser_transport *muser_transport;

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "create poll group\n");

	muser_group = calloc(1, sizeof(*muser_group));
	if (muser_group == NULL) {
		SPDK_ERRLOG("Error allocating poll group: %m");
		return NULL;
	}

	TAILQ_INIT(&muser_group->qps);

	muser_transport = SPDK_CONTAINEROF(transport, struct muser_transport,
					   transport);
	muser_transport->group = muser_group;

	return &muser_group->group;
}

static void
muser_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct muser_poll_group *muser_group;

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "destroy poll group\n");

	muser_group = SPDK_CONTAINEROF(group, struct muser_poll_group, group);

	free(muser_group);
}

static void
handle_connect_rsp(struct muser_qpair *qpair, struct muser_req *req);

/*
 * FIXME how does this function know when to execute? Clearly it does not
 * execute periodically. Is this cb_fn called by muser_accept?
 */
static int
muser_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
		     struct spdk_nvmf_qpair *qpair)
{
	struct muser_poll_group *muser_group;
	struct muser_qpair *muser_qpair;
	struct muser_req *muser_req;
	struct muser_ctrlr *muser_ctrlr;
	struct spdk_nvmf_request *req;
	struct spdk_nvmf_fabric_connect_data *data;
	int err;

	muser_group = SPDK_CONTAINEROF(group, struct muser_poll_group, group);
	muser_qpair = SPDK_CONTAINEROF(qpair, struct muser_qpair, qpair);
	muser_ctrlr = muser_qpair->ctrlr;

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "add QP%d=%p(%p) to poll_group=%p\n",
	              muser_qpair->qpair.qid, muser_qpair, qpair, muser_group);

	/* FIXME get_nvmf_req? */
	muser_req = TAILQ_FIRST(&muser_qpair->reqs);
	TAILQ_REMOVE(&muser_qpair->reqs, muser_req, link);

	req = &muser_req->req;
	req->cmd->connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->cmd->connect_cmd.cid = spdk_nvmf_qpair_is_admin_queue(&muser_qpair->qpair) ? 0 : muser_qpair->cmd->cid;
	req->cmd->connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	req->cmd->connect_cmd.recfmt = 0;
	req->cmd->connect_cmd.sqsize = muser_qpair->qsize - 1;
	req->cmd->connect_cmd.qid = qpair->qid;

	req->length = sizeof(struct spdk_nvmf_fabric_connect_data);
	req->data = calloc(1, req->length);
	assert(req->data != NULL); /* FIXME */

	data = (struct spdk_nvmf_fabric_connect_data *)req->data;
	/* data->hostid = { 0 } */

	data->cntlid = !spdk_nvmf_qpair_is_admin_queue(&muser_qpair->qpair) ? muser_ctrlr->cntlid : 0xffff;
	err = muser_snprintf_subnqn(muser_ctrlr, &data->subnqn);
	assert(err == 0); /* FIXME */
	/* data->hostnqn = { 0 } */

	/*
	 * FIXME If spdk_nvmf_request_exec is guaranteed to synchronously add
	 * the QP then there's no reason to use completion callbacks.
	 */
	muser_req->end_fn = handle_connect_rsp;

	SPDK_NOTICELOG("sending connect fabrics command for QID=%#x cntlid=%#x\n",
		       qpair->qid, data->cntlid);

	spdk_nvmf_request_exec(req);

	return 0;
}

static int
muser_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			struct spdk_nvmf_qpair *qpair)
{
	struct muser_qpair *muser_qpair;

	/* FIXME maybe this is where we should delete the I/O queue? */
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "remove NVMf QP%d=%p from NVMf poll_group=%p\n",
	              qpair->qid, qpair, group);

	muser_qpair = SPDK_CONTAINEROF(qpair, struct muser_qpair, qpair);
	TAILQ_REMOVE(&muser_qpair->ctrlr->muser_group->qps, muser_qpair, link);
	return 0;
}

static void
handle_admin_q_connect_rsp(struct spdk_nvmf_request *req,
                           struct muser_qpair *muser_qpair)
{
	int err;

	assert(req != NULL);
	assert(muser_qpair != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "fabric connect command completed\n");
	if (req->rsp->connect_rsp.status_code_specific.success.cntlid != 0) {
		muser_qpair->ctrlr->cntlid = req->rsp->connect_rsp.status_code_specific.success.cntlid;
	}
	err = sem_post(&muser_qpair->ctrlr->sem);
	assert(err == 0); /* FIXME */
}

/*
 * Only for CQ, which preceeds SQ creation. SQ is immediately completed in the
 * submit path.
 */
static void
handle_connect_rsp(struct muser_qpair *qpair, struct muser_req *req)
{
	int err = 0;

	assert(qpair != NULL);
	assert(req != NULL);

	/* FIXME */
	assert(!spdk_nvme_cpl_is_error(&req->req.rsp->nvme_cpl));

	if (req->req.cmd->connect_cmd.qid == 0) {
		handle_admin_q_connect_rsp(&req->req, qpair);
	}

	TAILQ_INSERT_TAIL(&qpair->ctrlr->muser_group->qps, qpair, link);

	if (req->req.cmd->connect_cmd.qid != 0) {
		/* FIXME last arg must be error status from NVMf */
		err = post_completion(qpair->ctrlr, &req->req.cmd->nvme_cmd,
	        	                   &qpair->ctrlr->qp[0]->cq, NULL, 0);
	}

	assert(err == 0); /* FIXME */
	free(req->req.data);
	req->req.data = NULL;
}

static int
map_admin_queues(struct muser_ctrlr *ctrlr)
{
	int err;

	assert(ctrlr != NULL);

	err = acq_map(ctrlr);
	if (err != 0) {
		SPDK_ERRLOG("failed to map CQ0: %d\n", err);
		return err;
	}
	err = asq_map(ctrlr);
	if (err != 0) {
		SPDK_ERRLOG("failed to map SQ0: %d\n", err);
		return err;
	}
	return 0;
}

static bool
handle_cc_write_end(struct muser_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register *cc;
	int err;

	assert(ctrlr != NULL);

	spdk_rmb();

	cc = (union spdk_nvme_cc_register*)ctrlr->prop_req.buf;

	/* spdk_nvmf_subsystem_stop must be executed from SPDK thread context */
	if (cc->bits.en == 0 && cc->bits.shn == 0 &&
	    ctrlr->qp[0]->qpair.ctrlr->vcprop.csts.bits.shst == SPDK_NVME_SHST_NORMAL) {
		SPDK_NOTICELOG("pausing NVMf subsystem\n");
		ctrlr->prop_req.dir = MUSER_NVMF_INVALID;
		err = spdk_nvmf_subsystem_stop(ctrlr->subsys,
	                        muser_nvmf_subsystem_paused,
	                        &ctrlr->prop_req.wait);
		assert(err == 0); /* FIXME */
		return false;
	} else if (cc->bits.en == 1 && cc->bits.shn == 0) {
		err = map_admin_queues(ctrlr);
		assert(err == 0); /* FIXME */
	}
	return true;
}

/*
 * Returns whether the semaphore should be fired.
 */
static bool
handle_prop_set_rsp(struct muser_ctrlr *ctrlr)
{
	assert(ctrlr != NULL);

	if (ctrlr->prop_req.pos == CC) {
		return handle_cc_write_end(ctrlr);
	}
	return true;
}

static void
handle_prop_get_rsp(struct muser_ctrlr *ctrlr, struct muser_req *req)
{
	assert(ctrlr != NULL);
	assert(req != NULL);

	memcpy(ctrlr->prop_req.buf,
	       &req->req.rsp->prop_get_rsp.value.u64,
	       ctrlr->prop_req.count);
}

static void
handle_prop_rsp(struct muser_qpair *qpair, struct muser_req *req)
{
	int err;
	bool fire = true;

	assert(qpair != NULL);
	assert(req != NULL);

	if (qpair->ctrlr->prop_req.dir == MUSER_NVMF_READ) {
		handle_prop_get_rsp(qpair->ctrlr, req);
	} else {
		assert(qpair->ctrlr->prop_req.dir == MUSER_NVMF_WRITE);
		fire = handle_prop_set_rsp(qpair->ctrlr);
	}

	if (fire) {
		/*
		 * FIXME this assume that spdk_nvmf_request_exec will call this
		 * callback before it actually returns. This is important
		 * because if we don't clear it then muser_poll_group_poll will
		 * pick up the same request again. The reason we don't clear it
		 * if fire is false is because the semaphore will be posted by
		 * a callback so it has to cleared there, right before the
		 * callback is scheduled. Check whether it's guaranteed that
		 * spdk_nvmf_request_exec is synchronous.
		 */
		qpair->ctrlr->prop_req.dir = MUSER_NVMF_INVALID;

		err = sem_post(&qpair->ctrlr->prop_req.wait);
		assert(err == 0); /* FIXME */
	}
}

static void
muser_req_done(struct spdk_nvmf_request *req)
{
	struct muser_qpair *qpair;
	struct muser_req *muser_req;

	assert(req != NULL);

	muser_req = SPDK_CONTAINEROF(req, struct muser_req, req);
	qpair = SPDK_CONTAINEROF(muser_req->req.qpair, struct muser_qpair, qpair);

	if (muser_req->end_fn != NULL) {
		muser_req->end_fn(qpair, muser_req);
	}

	TAILQ_INSERT_TAIL(&qpair->reqs, muser_req, link);
}

static int
muser_req_free(struct spdk_nvmf_request *req)
{
	/*
	 * FIXME why do we call muser_req_done both from muser_req_complete and
	 * from muser_req_free? Aren't they both always called? (first complete
	 * and then done?)
	 */
	muser_req_done(req);
	return 0;
}

static int
muser_req_complete(struct spdk_nvmf_request *req)
{
	if (req->cmd->connect_cmd.opcode != SPDK_NVME_OPC_FABRIC &&
	    req->cmd->connect_cmd.fctype != SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
		/* TODO: do cqe business */
	}

	muser_req_done(req);

	return 0;
}

static void
muser_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	struct muser_qpair *muser_qpair;

	assert(qpair != NULL);

	/* FIXME when is this called? */
	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "close QP%d\n", qpair->qid);

	muser_qpair = SPDK_CONTAINEROF(qpair, struct muser_qpair, qpair);
	destroy_qp(muser_qpair->ctrlr, qpair->qid);
}

static struct spdk_nvmf_request*
get_nvmf_req(struct muser_qpair *qpair)
{
	struct muser_req *req;

	assert(qpair != NULL);
	assert(!TAILQ_EMPTY(&qpair->reqs));

	req = TAILQ_FIRST(&qpair->reqs);
	TAILQ_REMOVE(&qpair->reqs, req, link);
	return &req->req;
}

static uint16_t
nlb(struct spdk_nvme_cmd *cmd)
{
	return 0x0000ffff & cmd->cdw12;
}

static int
handle_cmd_io_req(struct muser_ctrlr * ctrlr, struct spdk_nvmf_request *req)
{
	int err;
	bool remap = true;

	assert(ctrlr != NULL);
	assert(req != NULL);

	switch (req->cmd->nvme_cmd.opc) {
		case SPDK_NVME_OPC_FLUSH:
			req->xfer = SPDK_NVME_DATA_NONE;
			remap = false;
			break;
		case SPDK_NVME_OPC_READ:
			req->xfer = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
			break;
		case SPDK_NVME_OPC_WRITE:
			req->xfer = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
			break;
		default:
			SPDK_ERRLOG("SQ%d invalid I/O request type 0x%x\n",
			            req->qpair->qid, req->cmd->nvme_cmd.opc);
			return -EINVAL;
	}

	req->data = NULL;
	if (remap) {
		assert(is_prp(&req->cmd->nvme_cmd));
		req->length = (nlb(&req->cmd->nvme_cmd) + 1) << 9;
		/* FIXME prp address is still GPA, do we need to fix it? */
		err = muser_map_prps(ctrlr, &req->cmd->nvme_cmd, req->iov,
		                     req->length);
		if (err < 0) {
			SPDK_ERRLOG("failed to map PRP: %d\n", err);
			/* FIXME need to explicitly fail request */
			return err;
		}
		req->iovcnt = err;
	}
	return 0;
}

/* TODO find better name */
static int
handle_cmd_req(struct muser_ctrlr * ctrlr, struct spdk_nvme_cmd * cmd,
               struct spdk_nvmf_request * req)
{
	int err;
	struct muser_req *muser_req;

	assert(ctrlr != NULL);
	assert(cmd != NULL);
	assert(req != NULL);

	req->cmd->nvme_cmd = *cmd;
	if (spdk_nvmf_qpair_is_admin_queue(req->qpair)) {
		/* FIXME in which case is muser_qpair->cmd == NULL? */
		req->xfer = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
		req->length = 1 << 12;
		req->data = (void *)(req->cmd->nvme_cmd.dptr.prp.prp1 << ctrlr->cc.bits.mps);
	} else {
		err = handle_cmd_io_req(ctrlr, req);
		assert(err == 0); /* FIXME */
	}

	muser_req = SPDK_CONTAINEROF(req, struct muser_req, req);
	muser_req->end_fn = handle_cmd_rsp;

	spdk_nvmf_request_exec(req);

	return 0;
}

static void
muser_nvmf_subsystem_resumed(struct spdk_nvmf_subsystem *subsys, void *cb_arg,
                            int status)
{
	struct muser_ctrlr *ctrlr = (struct muser_ctrlr*)cb_arg;
	int err;
	struct spdk_nvmf_transport *transport;

	assert(status == 0); /* FIXME */
	assert(ctrlr != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "NVMf subsystem resumed\n");

	transport = spdk_nvmf_tgt_get_transport(subsys->tgt,
	                                        SPDK_NVME_TRANSPORT_MUSER);

	assert(transport != NULL); /* FIXME */

	err = add_qp(ctrlr, transport, MUSER_DEFAULT_AQ_DEPTH, 0, NULL);
	assert(err == 0); /* FIXME */
}

static int
muser_do_spdk_nvmf_subsystem_resume(struct muser_ctrlr *ctrlr)
{
	assert(ctrlr != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_MUSER, "resuming NVMf subsystem\n");

	return spdk_nvmf_subsystem_start(ctrlr->subsys,
	                                muser_nvmf_subsystem_resumed,
	                                ctrlr);
}

static int
handle_prop_req(struct muser_ctrlr *ctrlr)
{
	struct spdk_nvmf_request *req;
	struct muser_req *muser_req;

	assert(ctrlr != NULL);

	req = get_nvmf_req(ctrlr->qp[0]);
	assert(req != NULL); /* FIXME */
	muser_req = SPDK_CONTAINEROF(req, struct muser_req, req);

	muser_req->end_fn = handle_prop_rsp;

	req->cmd->prop_set_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->cmd->prop_set_cmd.cid = 0;
	if (ctrlr->prop_req.dir == MUSER_NVMF_WRITE) {
		req->cmd->prop_set_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
		req->cmd->prop_set_cmd.value.u32.high = 0;
		req->cmd->prop_set_cmd.value.u32.low = *(uint32_t *)ctrlr->prop_req.buf;
	} else {
		req->cmd->prop_set_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	}
	req->cmd->prop_set_cmd.attrib.size = (ctrlr->prop_req.count / 4) - 1;
	req->cmd->prop_set_cmd.ofst = ctrlr->prop_req.pos;
	req->length = 0;
	req->data = NULL;

	spdk_nvmf_request_exec(req);

	return 0;
}

static void
poll_qpair(struct muser_poll_group *group, struct muser_qpair *qpair)
{
	struct muser_ctrlr *ctrlr;
	uint32_t new_tail;

	assert(qpair != NULL);

	ctrlr = qpair->ctrlr;

	new_tail = *tdbl(ctrlr, &qpair->sq);
	if (sq_head(qpair) != new_tail) {
		handle_sq_tdbl_write(ctrlr, new_tail, qpair);
	}
}

static void
check_ctrlr(struct muser_ctrlr *ctrlr)
{
	int err = 0;

	if (ctrlr == NULL) {
		return;
	}

	/*
	 * FIXME apart from polling the doorbells, ther are other
	 * operations we need to execute for the other thread (e.g.
	 * write NVMe registers). Maybe it's best to introduce a queue?
	 */

	/*
	 * FIXME not sure what is the relationship between subsys and
	 * ctrlr.
	 */
	if (ctrlr->start) {

		/*
		 * This has to be cleared here, before the caller is
		 * woken up or the muser_poll_group_poll has a chance to
		 * run again (and find ctrlr->start set to true...).
		 */
		ctrlr->start = false;

		err = muser_do_spdk_nvmf_subsystem_resume(ctrlr);
	}

	if (ctrlr->del_admin_qp) {
		ctrlr->del_admin_qp = false;
		destroy_qp(ctrlr, 0);
		err = sem_post(&ctrlr->sem);
	}

	if (ctrlr->prop_req.dir != MUSER_NVMF_INVALID) {
		err = handle_prop_req(ctrlr);
	}

	assert(err == 0); /* FIXME */
}

/*
 * Called unconditionally, periodically, very frequently from SPDK to ask
 * whether there's work to be done.  This functions consumes requests generated
 * from read/write_bar0 by setting ctrlr->prop_req.dir.  read_bar0, and
 * occasionally write_bar0 -- though this may change, synchronously wait. This
 * function also consumes requests by looking at the doorbells.
 */
static int
muser_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct muser_poll_group *muser_group;
	struct muser_qpair *muser_qpair, *tmp;

	assert(group != NULL);

	spdk_rmb();

	muser_group = SPDK_CONTAINEROF(group, struct muser_poll_group, group);

	check_ctrlr(muser_group->ctrlr);

	TAILQ_FOREACH_SAFE(muser_qpair, &muser_group->qps, link, tmp) {

		/*
		 * TODO In init_qp the last thing we do is to point
		 * ctrlr->qp[qid] to the newly allocated qpair, which isn't
		 * fully initialized yet, and then request NVMf to add it. A
		 * better way to check whether the queue has been initialized
		 * is not to add it to ctrlr->qp[qid], so we'd only have to
		 * check whether ctrlr->qp[qid] is NULL.
		 */
		if (muser_qpair->sq.size == 0) {
			continue;
		}

		/*
		 * TODO queue is being deleted, don't check. Maybe we earlier
		 * check regarding the queue size and this check could be
		 * consolidated into a single flag, e.g. 'active'?
		 */
		if (muser_qpair->del) {
			continue;
		}
		poll_qpair(muser_group, muser_qpair);

	}

	return 0;
}

static int
muser_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			   struct spdk_nvme_transport_id *trid)
{
	struct muser_qpair *muser_qpair;
	struct muser_ctrlr *muser_ctrlr;

	muser_qpair = SPDK_CONTAINEROF(qpair, struct muser_qpair, qpair);
	muser_ctrlr = muser_qpair->ctrlr;

	memcpy(trid, &muser_ctrlr->trid, sizeof(*trid));
	return 0;
}

static int
muser_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			  struct spdk_nvme_transport_id *trid)
{
	return 0;
}

static int
muser_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			    struct spdk_nvme_transport_id *trid)
{
	struct muser_qpair *muser_qpair;
	struct muser_ctrlr *muser_ctrlr;

	muser_qpair = SPDK_CONTAINEROF(qpair, struct muser_qpair, qpair);
	muser_ctrlr = muser_qpair->ctrlr;

	memcpy(trid, &muser_ctrlr->trid, sizeof(*trid));
	return 0;
}

static int
muser_qpair_set_sq_size(struct spdk_nvmf_qpair *qpair)
{
	return 0;
}

static void
muser_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		MUSER_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	MUSER_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	MUSER_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		MUSER_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		MUSER_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =		MUSER_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	MUSER_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		MUSER_DEFAULT_BUFFER_CACHE_SIZE;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_muser = {
	.type = SPDK_NVME_TRANSPORT_MUSER,
	.opts_init = muser_opts_init,
	.create = muser_create,
	.destroy = muser_destroy,

	.listen = muser_listen,
	.stop_listen = muser_stop_listen,
	.accept = muser_accept,

	.listener_discover = muser_discover,

	.poll_group_create = muser_poll_group_create,
	.poll_group_destroy = muser_poll_group_destroy,
	.poll_group_add = muser_poll_group_add,
	.poll_group_remove = muser_poll_group_remove,
	.poll_group_poll = muser_poll_group_poll,

	.req_free = muser_req_free,
	.req_complete = muser_req_complete,

	.qpair_fini = muser_close_qpair,
	.qpair_get_local_trid = muser_qpair_get_local_trid,
	.qpair_get_peer_trid = muser_qpair_get_peer_trid,
	.qpair_get_listen_trid = muser_qpair_get_listen_trid,
	.qpair_set_sqsize = muser_qpair_set_sq_size,
};

SPDK_LOG_REGISTER_COMPONENT("nvmf_muser", SPDK_LOG_NVMF_MUSER)
