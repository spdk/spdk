/*-
 *   BSD LICENSE
 *   Copyright (c) Intel Corporation. All rights reserved.
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

/*
 * NVMe over vfio-user transport
 */

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

#define NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH 256
#define NVMF_VFIO_USER_DEFAULT_AQ_DEPTH 32
#define NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE ((NVMF_REQ_MAX_BUFFERS - 1) << SHIFT_4KB)
#define NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE

#define NVME_DOORBELLS_OFFSET	0x1000
#define NVMF_VFIO_USER_DOORBELLS_SIZE 0x1000

/*
 * NVMe driver reads 4096 bytes, which is the extended PCI configuration space
 * available on PCI-X 2.0 and PCI Express buses
 */
#define NVME_REG_CFG_SIZE       0x1000
#define NVME_REG_BAR0_SIZE      (NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE)
#define NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR ((NVMF_VFIO_USER_DOORBELLS_SIZE) / 8)
#define NVME_IRQ_MSIX_NUM	NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR
/* MSIX Table Size */
#define NVME_BAR4_SIZE		SPDK_ALIGN_CEIL((NVME_IRQ_MSIX_NUM * 16), 0x1000)
/* MSIX Pending Bit Array Size */
#define NVME_BAR5_SIZE		SPDK_ALIGN_CEIL((NVME_IRQ_MSIX_NUM / 8), 0x1000)

#define NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR (NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR / 4)

struct nvmf_vfio_user_req;
struct nvmf_vfio_user_qpair;

typedef int (*nvmf_vfio_user_req_cb_fn)(struct nvmf_vfio_user_req *req, void *cb_arg);

/* 1 more for PRP2 list itself */
#define NVMF_VFIO_USER_MAX_IOVECS	(NVMF_REQ_MAX_BUFFERS + 1)

enum nvmf_vfio_user_req_state {
	VFIO_USER_REQUEST_STATE_FREE = 0,
	VFIO_USER_REQUEST_STATE_EXECUTING,
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

	/* placeholder for gpa_to_vva memory map table, the IO buffer doesn't use it */
	dma_sg_t				*sg;
	struct iovec				iov[NVMF_VFIO_USER_MAX_IOVECS];
	uint8_t					iovcnt;

	TAILQ_ENTRY(nvmf_vfio_user_req)		link;
};

/*
 * A NVMe queue.
 */
struct nvme_q {
	bool is_cq;

	void *addr;

	dma_sg_t *sg;
	struct iovec iov;

	uint32_t size;
	uint64_t prp1;

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
			bool phase;
		};
	};
};

enum nvmf_vfio_user_qpair_state {
	VFIO_USER_QPAIR_UNINITIALIZED = 0,
	VFIO_USER_QPAIR_ACTIVE,
	VFIO_USER_QPAIR_SQ_DELETED,
	VFIO_USER_QPAIR_INACTIVE,
	VFIO_USER_QPAIR_ERROR,
};

struct nvmf_vfio_user_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_transport_poll_group	*group;
	struct nvmf_vfio_user_ctrlr		*ctrlr;
	struct nvmf_vfio_user_req		*reqs_internal;
	uint32_t				qsize;
	struct nvme_q				cq;
	struct nvme_q				sq;
	enum nvmf_vfio_user_qpair_state		state;

	/* Copy of Create IO SQ command */
	struct spdk_nvme_cmd			create_io_sq_cmd;

	TAILQ_HEAD(, nvmf_vfio_user_req)	reqs;
	/* Poll group entry */
	TAILQ_ENTRY(nvmf_vfio_user_qpair)	link;
	/* Connected queue pair entry */
	TAILQ_ENTRY(nvmf_vfio_user_qpair)	tailq;
};

struct nvmf_vfio_user_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	TAILQ_HEAD(, nvmf_vfio_user_qpair)	qps;
};

struct nvmf_vfio_user_ctrlr {
	struct nvmf_vfio_user_endpoint		*endpoint;
	struct nvmf_vfio_user_transport		*transport;

	/* Connected queue pairs list */
	TAILQ_HEAD(, nvmf_vfio_user_qpair)	connected_qps;

	struct spdk_thread			*thread;
	struct spdk_poller			*vfu_ctx_poller;

	uint16_t				cntlid;
	struct spdk_nvmf_ctrlr			*ctrlr;

	struct nvmf_vfio_user_qpair		*qp[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];

	TAILQ_ENTRY(nvmf_vfio_user_ctrlr)	link;

	volatile uint32_t			*doorbells;

	/* internal CSTS.CFS register for vfio-user fatal errors */
	uint32_t				cfs : 1;
};

struct nvmf_vfio_user_endpoint {
	vfu_ctx_t				*vfu_ctx;
	struct msixcap				*msix;
	vfu_pci_config_space_t			*pci_config_space;
	int					devmem_fd;
	volatile uint32_t			*doorbells;

	struct spdk_nvme_transport_id		trid;
	const struct spdk_nvmf_subsystem	*subsystem;

	struct nvmf_vfio_user_ctrlr		*ctrlr;
	pthread_mutex_t				lock;

	TAILQ_ENTRY(nvmf_vfio_user_endpoint)	link;
};

struct nvmf_vfio_user_transport_opts {
	bool					disable_mappable_bar0;
};

struct nvmf_vfio_user_transport {
	struct spdk_nvmf_transport		transport;
	struct nvmf_vfio_user_transport_opts    transport_opts;
	struct spdk_poller			*accept_poller;
	pthread_mutex_t				lock;
	TAILQ_HEAD(, nvmf_vfio_user_endpoint)	endpoints;
};

/*
 * function prototypes
 */
static volatile uint32_t *
hdbl(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvme_q *q);

static volatile uint32_t *
tdbl(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvme_q *q);

static int
nvmf_vfio_user_req_free(struct spdk_nvmf_request *req);

static struct nvmf_vfio_user_req *
get_nvmf_vfio_user_req(struct nvmf_vfio_user_qpair *qpair);

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

static inline uint16_t
io_q_id(struct nvme_q *q)
{

	struct nvmf_vfio_user_qpair *vu_qpair;

	assert(q);

	if (q->is_cq) {
		vu_qpair = SPDK_CONTAINEROF(q, struct nvmf_vfio_user_qpair, cq);
	} else {
		vu_qpair = SPDK_CONTAINEROF(q, struct nvmf_vfio_user_qpair, sq);
	}
	assert(vu_qpair);
	return vu_qpair->qpair.qid;
}

static void
fail_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	assert(ctrlr != NULL);

	if (ctrlr->cfs == 0) {
		SPDK_ERRLOG(":%s failing controller\n", ctrlr_id(ctrlr));
	}

	ctrlr->cfs = 1U;
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
	if (endpoint->doorbells) {
		munmap((void *)endpoint->doorbells, NVMF_VFIO_USER_DOORBELLS_SIZE);
	}

	if (endpoint->devmem_fd > 0) {
		close(endpoint->devmem_fd);
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

	spdk_poller_unregister(&vu_transport->accept_poller);
	(void)pthread_mutex_destroy(&vu_transport->lock);

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
};

static int
nvmf_vfio_user_accept(void *ctx);

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

	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, vfio_user_transport_opts_decoder,
					    SPDK_COUNTOF(vfio_user_transport_opts_decoder),
					    vu_transport)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		free(vu_transport);
		return NULL;
	}

	vu_transport->accept_poller = SPDK_POLLER_REGISTER(nvmf_vfio_user_accept, &vu_transport->transport,
				      vu_transport->transport.opts.acceptor_poll_rate);
	if (!vu_transport->accept_poller) {
		free(vu_transport);
		return NULL;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_mappable_bar0=%d\n",
		      vu_transport->transport_opts.disable_mappable_bar0);

	return &vu_transport->transport;

err:
	free(vu_transport);

	return NULL;
}

static uint32_t
max_queue_size(struct nvmf_vfio_user_ctrlr const *ctrlr)
{
	assert(ctrlr != NULL);
	assert(ctrlr->qp[0] != NULL);
	assert(ctrlr->qp[0]->qpair.ctrlr != NULL);

	return ctrlr->qp[0]->qpair.ctrlr->vcprop.cap.bits.mqes + 1;
}

static void *
map_one(vfu_ctx_t *ctx, uint64_t addr, uint64_t len, dma_sg_t *sg, struct iovec *iov, int prot)
{
	int ret;

	assert(ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	ret = vfu_addr_to_sg(ctx, (void *)(uintptr_t)addr, len, sg, 1, prot);
	if (ret < 0) {
		return NULL;
	}

	ret = vfu_map_sg(ctx, sg, iov, 1, 0);
	if (ret != 0) {
		return NULL;
	}

	assert(iov->iov_base != NULL);
	return iov->iov_base;
}

static inline uint32_t
sq_head(struct nvmf_vfio_user_qpair *qpair)
{
	assert(qpair != NULL);
	return qpair->sq.head;
}

static inline void
sqhd_advance(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_qpair *qpair)
{
	assert(ctrlr != NULL);
	assert(qpair != NULL);
	qpair->sq.head = (qpair->sq.head + 1) % qpair->sq.size;
}

static int
map_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q *q, bool is_cq, bool unmap)
{
	uint64_t len;

	assert(q->size);
	assert(q->addr == NULL);

	if (is_cq) {
		len = q->size * sizeof(struct spdk_nvme_cpl);
	} else {
		len = q->size * sizeof(struct spdk_nvme_cmd);
	}

	q->addr = map_one(vu_ctrlr->endpoint->vfu_ctx, q->prp1, len, q->sg,
			  &q->iov, is_cq ? PROT_READ | PROT_WRITE : PROT_READ);
	if (q->addr == NULL) {
		return -EFAULT;
	}

	if (unmap) {
		memset(q->addr, 0, len);
	}

	return 0;
}

static int
asq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvme_q *sq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);
	assert(ctrlr->qp[0] != NULL);
	assert(ctrlr->qp[0]->sq.addr == NULL);
	/* XXX ctrlr->asq == 0 is a valid memory address */

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->qp[0]->qpair.ctrlr);
	sq = &ctrlr->qp[0]->sq;
	sq->size = regs->aqa.bits.asqs + 1;
	sq->prp1 = regs->asq;
	sq->head = 0;
	sq->cqid = 0;
	sq->is_cq = false;

	ret = map_q(ctrlr, sq, false, true);
	if (ret) {
		return ret;
	}

	*tdbl(ctrlr, sq) = 0;

	return 0;
}

static inline int
queue_index(uint16_t qid, int is_cq)
{
	return (qid * 2) + is_cq;
}

static volatile uint32_t *
tdbl(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvme_q *q)
{
	assert(ctrlr != NULL);
	assert(q != NULL);
	assert(!q->is_cq);

	return &ctrlr->doorbells[queue_index(io_q_id(q), false)];
}

static volatile uint32_t *
hdbl(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvme_q *q)
{
	assert(ctrlr != NULL);
	assert(q != NULL);
	assert(q->is_cq);

	return &ctrlr->doorbells[queue_index(io_q_id(q), true)];
}

static inline bool
cq_is_full(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvme_q *q)
{
	assert(ctrlr != NULL);
	assert(q != NULL);
	assert(q->is_cq);

	return ((q->tail + 1) % q->size) == *hdbl(ctrlr, q);
}

static inline void
cq_tail_advance(struct nvme_q *q)
{
	assert(q != NULL);
	assert(q->is_cq);

	assert(q->tail < q->size);
	q->tail++;

	if (spdk_unlikely(q->tail == q->size)) {
		q->tail = 0;
		q->phase = !q->phase;
	}
}

static int
acq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvme_q *cq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);
	assert(ctrlr->qp[0] != NULL);
	assert(ctrlr->qp[0]->cq.addr == NULL);

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->qp[0]->qpair.ctrlr);
	assert(regs != NULL);
	cq = &ctrlr->qp[0]->cq;
	cq->size = regs->aqa.bits.acqs + 1;
	cq->prp1 = regs->acq;
	cq->tail = 0;
	cq->is_cq = true;
	cq->ien = true;
	cq->phase = true;

	ret = map_q(ctrlr, cq, true, true);
	if (ret) {
		return ret;
	}
	*hdbl(ctrlr, cq) = 0;

	return 0;
}

static inline dma_sg_t *
vu_req_to_sg_t(struct nvmf_vfio_user_req *vu_req, uint32_t iovcnt)
{
	return (dma_sg_t *)((uintptr_t)vu_req->sg + iovcnt * dma_sg_size());
}

static void *
_map_one(void *prv, uint64_t addr, uint64_t len, int prot)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)prv;
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_qpair *vu_qpair;
	void *ret;

	assert(req != NULL);
	qpair = req->qpair;
	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);

	assert(vu_req->iovcnt < NVMF_VFIO_USER_MAX_IOVECS);
	ret = map_one(vu_qpair->ctrlr->endpoint->vfu_ctx, addr, len,
		      vu_req_to_sg_t(vu_req, vu_req->iovcnt),
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

static int
handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
	       struct nvmf_vfio_user_qpair *vu_qpair);

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
post_completion(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvme_q *cq,
		uint32_t cdw0, uint16_t sqid, uint16_t cid, uint16_t sc, uint16_t sct)
{
	struct spdk_nvme_cpl *cpl;
	const struct spdk_nvmf_registers *regs;
	int err;

	assert(ctrlr != NULL);

	if (spdk_unlikely(cq == NULL || cq->addr == NULL)) {
		return 0;
	}

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->qp[0]->qpair.ctrlr);
	if (regs->csts.bits.shst != SPDK_NVME_SHST_NORMAL) {
		SPDK_DEBUGLOG(nvmf_vfio,
			      "%s: ignore completion SQ%d cid=%d status=%#x\n",
			      ctrlr_id(ctrlr), sqid, cid, sc);
		return 0;
	}

	if (cq_is_full(ctrlr, cq)) {
		SPDK_ERRLOG("%s: CQ%d full (tail=%d, head=%d)\n",
			    ctrlr_id(ctrlr), io_q_id(cq), cq->tail, *hdbl(ctrlr, cq));
		return -1;
	}

	cpl = ((struct spdk_nvme_cpl *)cq->addr) + cq->tail;

	assert(ctrlr->qp[sqid] != NULL);
	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: request complete SQ%d cid=%d status=%#x SQ head=%#x CQ tail=%#x\n",
		      ctrlr_id(ctrlr), sqid, cid, sc, sq_head(ctrlr->qp[sqid]),
		      cq->tail);

	cpl->sqhd = sq_head(ctrlr->qp[sqid]);
	cpl->sqid = sqid;
	cpl->cid = cid;
	cpl->cdw0 = cdw0;
	cpl->status.dnr = 0x0;
	cpl->status.m = 0x0;
	cpl->status.sct = sct;
	cpl->status.p = cq->phase;
	cpl->status.sc = sc;

	cq_tail_advance(cq);

	/*
	 * this function now executes at SPDK thread context, we
	 * might be triggering interrupts from vfio-user thread context so
	 * check for race conditions.
	 */
	if (ctrlr_interrupt_enabled(ctrlr) && cq->ien) {
		err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
			return err;
		}
	}

	return 0;
}

static bool
io_q_exists(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t qid, const bool is_cq)
{
	assert(vu_ctrlr != NULL);

	if (qid == 0 || qid >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		return false;
	}

	if (vu_ctrlr->qp[qid] == NULL) {
		return false;
	}

	if (!is_cq) {
		if (vu_ctrlr->qp[qid]->state == VFIO_USER_QPAIR_SQ_DELETED ||
		    vu_ctrlr->qp[qid]->state == VFIO_USER_QPAIR_UNINITIALIZED) {
			return false;
		}
	}

	return true;
}

static void
unmap_qp(struct nvmf_vfio_user_qpair *qp)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;

	if (qp->ctrlr == NULL) {
		return;
	}
	ctrlr = qp->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: unmap QP%d\n",
		      ctrlr_id(ctrlr), qp->qpair.qid);

	if (qp->sq.addr != NULL) {
		vfu_unmap_sg(ctrlr->endpoint->vfu_ctx, qp->sq.sg, &qp->sq.iov, 1);
		qp->sq.addr = NULL;
	}

	if (qp->cq.addr != NULL) {
		vfu_unmap_sg(ctrlr->endpoint->vfu_ctx, qp->cq.sg, &qp->cq.iov, 1);
		qp->cq.addr = NULL;
	}
}

static int
remap_qp(struct nvmf_vfio_user_qpair *vu_qpair)
{
	struct nvme_q *sq, *cq;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	int ret;

	vu_ctrlr = vu_qpair->ctrlr;
	sq = &vu_qpair->sq;
	cq = &vu_qpair->cq;

	if (sq->size) {
		ret = map_q(vu_ctrlr, sq, false, false);
		if (ret) {
			SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap SQID %d %#lx-%#lx\n",
				      io_q_id(sq), sq->prp1, sq->prp1 + sq->size * sizeof(struct spdk_nvme_cmd));
			return -EFAULT;
		}
	}

	if (cq->size) {
		ret = map_q(vu_ctrlr, cq, true, false);
		if (ret) {
			SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap CQID %d %#lx-%#lx\n",
				      io_q_id(cq), cq->prp1, cq->prp1 + cq->size * sizeof(struct spdk_nvme_cpl));
			return -EFAULT;
		}

	}

	return 0;
}

static void
free_qp(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid)
{
	struct nvmf_vfio_user_qpair *qpair;
	struct nvmf_vfio_user_req *vu_req;
	uint32_t i;

	if (ctrlr == NULL) {
		return;
	}

	qpair = ctrlr->qp[qid];
	if (qpair == NULL) {
		return;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: destroy QP%d=%p\n", ctrlr_id(ctrlr),
		      qid, qpair);

	unmap_qp(qpair);

	for (i = 0; i < qpair->qsize; i++) {
		vu_req = &qpair->reqs_internal[i];
		free(vu_req->sg);
	}
	free(qpair->reqs_internal);

	free(qpair->sq.sg);
	free(qpair->cq.sg);
	free(qpair);

	ctrlr->qp[qid] = NULL;
}

/* This function can only fail because of memory allocation errors. */
static int
init_qp(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_transport *transport,
	const uint32_t qsize, const uint16_t id)
{
	uint32_t i;
	struct nvmf_vfio_user_qpair *qpair;
	struct nvmf_vfio_user_req *vu_req, *tmp;
	struct spdk_nvmf_request *req;

	assert(ctrlr != NULL);
	assert(transport != NULL);

	qpair = calloc(1, sizeof(*qpair));
	if (qpair == NULL) {
		return -ENOMEM;
	}
	qpair->sq.sg = calloc(1, dma_sg_size());
	if (qpair->sq.sg == NULL) {
		free(qpair);
		return -ENOMEM;
	}
	qpair->cq.sg = calloc(1, dma_sg_size());
	if (qpair->cq.sg == NULL) {
		free(qpair->sq.sg);
		free(qpair);
		return -ENOMEM;
	}

	qpair->qpair.qid = id;
	qpair->qpair.transport = transport;
	qpair->ctrlr = ctrlr;
	qpair->qsize = qsize;

	TAILQ_INIT(&qpair->reqs);

	qpair->reqs_internal = calloc(qsize, sizeof(struct nvmf_vfio_user_req));
	if (qpair->reqs_internal == NULL) {
		SPDK_ERRLOG("%s: error allocating reqs: %m\n", ctrlr_id(ctrlr));
		goto reqs_err;
	}

	for (i = 0; i < qsize; i++) {
		vu_req = &qpair->reqs_internal[i];
		vu_req->sg = calloc(NVMF_VFIO_USER_MAX_IOVECS, dma_sg_size());
		if (vu_req->sg == NULL) {
			goto sg_err;
		}

		req = &vu_req->req;
		req->qpair = &qpair->qpair;
		req->rsp = (union nvmf_c2h_msg *)&vu_req->rsp;
		req->cmd = (union nvmf_h2c_msg *)&vu_req->cmd;

		TAILQ_INSERT_TAIL(&qpair->reqs, vu_req, link);
	}

	ctrlr->qp[id] = qpair;
	return 0;

sg_err:
	TAILQ_FOREACH_SAFE(vu_req, &qpair->reqs, link, tmp) {
		free(vu_req->sg);
	}
	free(qpair->reqs_internal);

reqs_err:
	free(qpair->sq.sg);
	free(qpair->cq.sg);
	free(qpair);
	return -ENOMEM;
}

/*
 * Creates a completion or submission I/O queue. Returns 0 on success, -errno
 * on error.
 */
static int
handle_create_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	uint16_t qid, cqid;
	uint32_t qsize;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	int err = 0;
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvme_q *io_q;
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	qid = cmd->cdw10_bits.create_io_q.qid;
	if (qid == 0 || qid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid QID=%d, max=%d\n", ctrlr_id(ctrlr),
			    qid, vu_transport->transport.opts.max_qpairs_per_ctrlr);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (io_q_exists(ctrlr, qid, is_cq)) {
		SPDK_ERRLOG("%s: %cQ%d already exists\n", ctrlr_id(ctrlr),
			    is_cq ? 'C' : 'S', qid);
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

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: create I/O %cQ%d: QSIZE=%#x\n", ctrlr_id(ctrlr),
		      is_cq ? 'C' : 'S', qid, qsize);

	if (is_cq) {
		if (cmd->cdw11_bits.create_io_cq.pc != 0x1) {
			SPDK_ERRLOG("%s: non-PC CQ not supporred\n", ctrlr_id(ctrlr));
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}
		if (cmd->cdw11_bits.create_io_cq.iv > NVME_IRQ_MSIX_NUM - 1) {
			SPDK_ERRLOG("%s: IV is too big\n", ctrlr_id(ctrlr));
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR;
			goto out;
		}

		err = init_qp(ctrlr, ctrlr->qp[0]->qpair.transport, qsize, qid);
		if (err != 0) {
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}

		io_q = &ctrlr->qp[qid]->cq;
		io_q->ien = cmd->cdw11_bits.create_io_cq.ien;
		io_q->iv = cmd->cdw11_bits.create_io_cq.iv;
		io_q->phase = true;
	} else {
		cqid = cmd->cdw11_bits.create_io_sq.cqid;
		if (cqid == 0 || cqid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
			SPDK_ERRLOG("%s: invalid CQID %u\n", ctrlr_id(ctrlr), cqid);
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
			goto out;

		}
		/* CQ must be created before SQ */
		if (!io_q_exists(ctrlr, cqid, true)) {
			SPDK_ERRLOG("%s: CQ%u does not exist\n", ctrlr_id(ctrlr), cqid);
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_COMPLETION_QUEUE_INVALID;
			goto out;
		}

		if (cmd->cdw11_bits.create_io_sq.pc != 0x1) {
			SPDK_ERRLOG("%s: non-PC SQ not supported\n", ctrlr_id(ctrlr));
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}
		/* TODO: support shared IO CQ */
		if (qid != cqid) {
			SPDK_ERRLOG("%s: doesn't support shared CQ now\n", ctrlr_id(ctrlr));
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		}

		io_q = &ctrlr->qp[qid]->sq;
		io_q->cqid = cqid;
		SPDK_DEBUGLOG(nvmf_vfio, "%s: SQ%d CQID=%d\n", ctrlr_id(ctrlr),
			      qid, io_q->cqid);
	}

	io_q->is_cq = is_cq;
	io_q->size = qsize;
	io_q->prp1 = cmd->dptr.prp.prp1;

	err = map_q(ctrlr, io_q, is_cq, true);
	if (err) {
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		goto out;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped %cQ%d IOVA=%#lx vaddr=%#llx\n",
		      ctrlr_id(ctrlr), is_cq ? 'C' : 'S',
		      qid, cmd->dptr.prp.prp1, (unsigned long long)io_q->addr);

	if (is_cq) {
		*hdbl(ctrlr, io_q) = 0;
	} else {
		vu_qpair = ctrlr->qp[qid];
		*tdbl(ctrlr, io_q) = 0;
		vu_qpair->sq.head = 0;

		if (vu_qpair->state == VFIO_USER_QPAIR_SQ_DELETED) {
			vu_qpair->state = VFIO_USER_QPAIR_ACTIVE;
		} else {
			/*
			 * Create our new I/O qpair. This asynchronously invokes, on a
			 * suitable poll group, the nvmf_vfio_user_poll_group_add()
			 * callback, which will call spdk_nvmf_request_exec_fabrics()
			 * with a generated fabrics connect command. This command is
			 * then eventually completed via handle_queue_connect_rsp().
			 */
			vu_qpair->create_io_sq_cmd = *cmd;
			spdk_nvmf_tgt_new_qpair(ctrlr->transport->transport.tgt,
						&vu_qpair->qpair);
			return 0;
		}
	}

out:
	return post_completion(ctrlr, &ctrlr->qp[0]->cq, 0, 0, cmd->cid, sc, sct);
}

/* For ADMIN I/O DELETE COMPLETION QUEUE the NVMf library will disconnect and free
 * queue pair, so save the command in a context.
 */
struct vfio_user_delete_cq_ctx {
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct spdk_nvme_cmd delete_io_cq_cmd;
};

static void
vfio_user_qpair_delete_cb(void *cb_arg)
{
	struct vfio_user_delete_cq_ctx *ctx = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx->vu_ctrlr;

	post_completion(vu_ctrlr, &vu_ctrlr->qp[0]->cq, 0, 0, ctx->delete_io_cq_cmd.cid,
			SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
	free(ctx);
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
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct vfio_user_delete_cq_ctx *ctx;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete I/O %cQ: QID=%d\n",
		      ctrlr_id(ctrlr), is_cq ? 'C' : 'S',
		      cmd->cdw10_bits.delete_io_q.qid);

	if (!io_q_exists(ctrlr, cmd->cdw10_bits.delete_io_q.qid, is_cq)) {
		SPDK_ERRLOG("%s: I/O %cQ%d does not exist\n", ctrlr_id(ctrlr),
			    is_cq ? 'C' : 'S', cmd->cdw10_bits.delete_io_q.qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	vu_qpair = ctrlr->qp[cmd->cdw10_bits.delete_io_q.qid];
	if (is_cq) {
		if (vu_qpair->state == VFIO_USER_QPAIR_UNINITIALIZED) {
			free_qp(ctrlr, cmd->cdw10_bits.delete_io_q.qid);
			goto out;
		}

		/* SQ must have been deleted first */
		if (vu_qpair->state != VFIO_USER_QPAIR_SQ_DELETED) {
			SPDK_ERRLOG("%s: the associated SQ must be deleted first\n", ctrlr_id(ctrlr));
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_DELETION;
			goto out;
		}
		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			sct = SPDK_NVME_SCT_GENERIC;
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		ctx->vu_ctrlr = ctrlr;
		ctx->delete_io_cq_cmd = *cmd;
		spdk_nvmf_qpair_disconnect(&vu_qpair->qpair, vfio_user_qpair_delete_cb, ctx);
		return 0;
	} else {
		if (vu_qpair->state == VFIO_USER_QPAIR_SQ_DELETED) {
			SPDK_DEBUGLOG(nvmf_vfio, "%s: SQ%u is already deleted\n", ctrlr_id(ctrlr),
				      cmd->cdw10_bits.delete_io_q.qid);
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
			goto out;
		}

		/*
		 * This doesn't actually delete the SQ, We're merely telling the poll_group_poll
		 * function to skip checking this SQ.  The queue pair will be disconnected in Delete
		 * IO CQ command.
		 */
		vu_qpair->state = VFIO_USER_QPAIR_SQ_DELETED;
		vfu_unmap_sg(ctrlr->endpoint->vfu_ctx, vu_qpair->sq.sg, &vu_qpair->sq.iov, 1);
		vu_qpair->sq.addr = NULL;
	}

out:
	return post_completion(ctrlr, &ctrlr->qp[0]->cq, 0, 0, cmd->cid, sc, sct);
}

/*
 * Returns 0 on success and -errno on error.
 */
static int
consume_admin_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	assert(ctrlr != NULL);
	assert(cmd != NULL);

	if (cmd->fuse != 0) {
		/* Fused admin commands are not supported. */
		return post_completion(ctrlr, &ctrlr->qp[0]->cq, 0, 0, cmd->cid,
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
	default:
		return handle_cmd_req(ctrlr, cmd, ctrlr->qp[0]);
	}
}

static int
handle_cmd_rsp(struct nvmf_vfio_user_req *vu_req, void *cb_arg)
{
	struct nvmf_vfio_user_qpair *vu_qpair = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = vu_qpair->ctrlr;
	uint16_t sqid, cqid;

	assert(vu_qpair != NULL);
	assert(vu_req != NULL);
	assert(vu_ctrlr != NULL);

	if (spdk_likely(vu_req->iovcnt)) {
		vfu_unmap_sg(vu_ctrlr->endpoint->vfu_ctx, vu_req->sg, vu_req->iov, vu_req->iovcnt);
	}
	sqid = vu_qpair->qpair.qid;
	cqid = vu_ctrlr->qp[sqid]->sq.cqid;

	return post_completion(vu_ctrlr, &vu_ctrlr->qp[cqid]->cq,
			       vu_req->req.rsp->nvme_cpl.cdw0,
			       sqid,
			       vu_req->req.cmd->nvme_cmd.cid,
			       vu_req->req.rsp->nvme_cpl.status.sc,
			       vu_req->req.rsp->nvme_cpl.status.sct);
}

static int
consume_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_qpair *qpair,
	    struct spdk_nvme_cmd *cmd)
{
	assert(qpair != NULL);
	if (nvmf_qpair_is_admin_queue(&qpair->qpair)) {
		return consume_admin_cmd(ctrlr, cmd);
	}

	return handle_cmd_req(ctrlr, cmd, qpair);
}

/* Returns the number of commands processed, or a negative value on error. */
static int
handle_sq_tdbl_write(struct nvmf_vfio_user_ctrlr *ctrlr, const uint32_t new_tail,
		     struct nvmf_vfio_user_qpair *qpair)
{
	struct spdk_nvme_cmd *queue;
	int count = 0;

	assert(ctrlr != NULL);
	assert(qpair != NULL);

	queue = qpair->sq.addr;
	while (sq_head(qpair) != new_tail) {
		int err;
		struct spdk_nvme_cmd *cmd = &queue[sq_head(qpair)];

		count++;

		/*
		 * SQHD must contain the new head pointer, so we must increase
		 * it before we generate a completion.
		 */
		sqhd_advance(ctrlr, qpair);

		err = consume_cmd(ctrlr, qpair, cmd);
		if (err != 0) {
			return err;
		}
	}

	return count;
}

static int
enable_admin_queue(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	int err;

	assert(ctrlr != NULL);

	err = acq_setup(ctrlr);
	if (err != 0) {
		return err;
	}

	err = asq_setup(ctrlr);
	if (err != 0) {
		return err;
	}

	return 0;
}

static void
disable_admin_queue(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	assert(ctrlr->qp[0] != NULL);

	unmap_qp(ctrlr->qp[0]);
}

static void
memory_region_add_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct nvmf_vfio_user_qpair *qpair;
	int ret;

	/*
	 * We're not interested in any DMA regions that aren't mappable (we don't
	 * support clients that don't share their memory).
	 */
	if (!info->vaddr) {
		return;
	}

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %#lx-%#lx\n", info->vaddr,
			      (uintptr_t)info->mapping.iov_base,
			      (uintptr_t)info->mapping.iov_base + info->mapping.iov_len);
		return;
	}

	assert(endpoint != NULL);
	if (endpoint->ctrlr == NULL) {
		return;
	}
	ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: map IOVA %#lx-%#lx\n", ctrlr_id(ctrlr),
		      (uintptr_t)info->mapping.iov_base,
		      (uintptr_t)info->mapping.iov_base + info->mapping.iov_len);

	/* VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE are enabled when registering to VFIO, here we also
	 * check the protection bits before registering.
	 */
	if (info->prot == (PROT_WRITE | PROT_READ)) {
		ret = spdk_mem_register(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region register %#lx-%#lx failed, ret=%d\n",
				    (uint64_t)(uintptr_t)info->mapping.iov_base,
				    (uint64_t)(uintptr_t)info->mapping.iov_base + info->mapping.iov_len,
				    ret);
		}
	}

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_FOREACH(qpair, &ctrlr->connected_qps, tailq) {
		if (qpair->state != VFIO_USER_QPAIR_INACTIVE) {
			continue;
		}

		ret = remap_qp(qpair);
		if (ret) {
			continue;
		}
		qpair->state = VFIO_USER_QPAIR_ACTIVE;
		SPDK_DEBUGLOG(nvmf_vfio, "Remap QP %u successfully\n", qpair->qpair.qid);
	}
	pthread_mutex_unlock(&endpoint->lock);
}

static int
memory_region_remove_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct nvmf_vfio_user_qpair *qpair;
	void *map_start, *map_end;
	int ret = 0;

	if (!info->vaddr) {
		return 0;
	}

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %#lx-%#lx\n", info->vaddr,
			      (uintptr_t)info->mapping.iov_base,
			      (uintptr_t)info->mapping.iov_base + info->mapping.iov_len);
		return 0;
	}

	assert(endpoint != NULL);
	if (endpoint->ctrlr == NULL) {
		return 0;
	}
	ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: unmap IOVA %#lx-%#lx\n", ctrlr_id(ctrlr),
		      (uintptr_t)info->mapping.iov_base,
		      (uintptr_t)info->mapping.iov_base + info->mapping.iov_len);

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_FOREACH(qpair, &ctrlr->connected_qps, tailq) {
		if ((qpair->cq.addr >= map_start && qpair->cq.addr <= map_end) ||
		    (qpair->sq.addr >= map_start && qpair->sq.addr <= map_end)) {
			/* TODO: Ideally we should disconnect this queue pair
			 * before returning to caller.
			 */
			unmap_qp(qpair);
			qpair->state = VFIO_USER_QPAIR_INACTIVE;
		}
	}
	pthread_mutex_unlock(&endpoint->lock);

	if (info->prot == (PROT_WRITE | PROT_READ)) {
		ret = spdk_mem_unregister(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region unregister %#lx-%#lx failed, ret=%d\n",
				    (uint64_t)(uintptr_t)info->mapping.iov_base,
				    (uint64_t)(uintptr_t)info->mapping.iov_base + info->mapping.iov_len,
				    ret);
		}
	}

	return 0;
}

static int
nvmf_vfio_user_prop_req_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_qpair *vu_qpair = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	bool disable_admin = false;
	int ret;

	assert(vu_qpair != NULL);
	assert(req != NULL);

	if (req->req.cmd->prop_get_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET) {
		assert(vu_qpair->ctrlr != NULL);
		assert(req != NULL);

		memcpy(req->req.data,
		       &req->req.rsp->prop_get_rsp.value.u64,
		       req->req.length);
	} else {
		assert(req->req.cmd->prop_set_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET);
		assert(vu_qpair->ctrlr != NULL);
		vu_ctrlr = vu_qpair->ctrlr;

		if (req->req.cmd->prop_set_cmd.ofst == offsetof(struct spdk_nvme_registers, cc)) {
			union spdk_nvme_cc_register cc, diff;

			cc.raw = req->req.cmd->prop_set_cmd.value.u64;
			diff.raw = cc.raw ^ req->cc.raw;

			if (diff.bits.en) {
				if (cc.bits.en) {
					SPDK_DEBUGLOG(nvmf_vfio, "%s: MAP Admin queue\n", ctrlr_id(vu_ctrlr));
					ret = enable_admin_queue(vu_ctrlr);
					if (ret) {
						SPDK_ERRLOG("%s: failed to map Admin queue\n", ctrlr_id(vu_ctrlr));
						return ret;
					}
					vu_qpair->state = VFIO_USER_QPAIR_ACTIVE;
				} else {
					disable_admin = true;
				}
			}

			if (diff.bits.shn) {
				if (cc.bits.shn == SPDK_NVME_SHN_NORMAL || cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
					disable_admin = true;
				}
			}

			if (disable_admin) {
				SPDK_DEBUGLOG(nvmf_vfio,
					      "%s: UNMAP Admin queue\n",
					      ctrlr_id(vu_ctrlr));
				vu_qpair->state = VFIO_USER_QPAIR_INACTIVE;
				disable_admin_queue(vu_ctrlr);
				/* For PCIe controller reset or shutdown, we will drop all AER responses */
				nvmf_ctrlr_abort_aer(vu_qpair->qpair.ctrlr);
			}
		}
	}

	return 0;
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
	assert(ctrlr != NULL);
	assert(buf != NULL);

	if (count != sizeof(uint32_t)) {
		SPDK_ERRLOG("%s: bad doorbell buffer size %ld\n",
			    ctrlr_id(ctrlr), count);
		errno = EINVAL;
		return -1;
	}

	pos -= NVME_DOORBELLS_OFFSET;

	/* pos must be dword aligned */
	if ((pos & 0x3) != 0) {
		SPDK_ERRLOG("%s: bad doorbell offset %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	/* convert byte offset to array index */
	pos >>= 2;

	if (pos >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2) {
		SPDK_ERRLOG("%s: bad doorbell index %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	if (is_write) {
		ctrlr->doorbells[pos] = *buf;
		spdk_wmb();
	} else {
		spdk_rmb();
		*buf = ctrlr->doorbells[pos];
	}
	return 0;
}

static ssize_t
access_bar0_fn(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
	       bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct nvmf_vfio_user_req *req;
	const struct spdk_nvmf_registers *regs;
	int ret;

	ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: bar0 %s ctrlr: %p, count=%zu, pos=%"PRIX64"\n",
		      endpoint_id(endpoint), is_write ? "write" : "read",
		      ctrlr, count, pos);

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

	/* Construct a Fabric Property Get/Set command and send it */
	req = get_nvmf_vfio_user_req(ctrlr->qp[0]);
	if (req == NULL) {
		errno = ENOBUFS;
		return -1;
	}
	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->qp[0]->qpair.ctrlr);
	req->cc.raw = regs->cc.raw;

	req->cb_fn = nvmf_vfio_user_prop_req_rsp;
	req->cb_arg = ctrlr->qp[0];
	req->req.cmd->prop_set_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->req.cmd->prop_set_cmd.cid = 0;
	req->req.cmd->prop_set_cmd.attrib.size = (count / 4) - 1;
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

static int
vfio_user_dev_info_fill(struct nvmf_vfio_user_transport *vu_transport,
			struct nvmf_vfio_user_endpoint *endpoint)
{
	int ret;
	ssize_t cap_offset;
	vfu_ctx_t *vfu_ctx = endpoint->vfu_ctx;

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

static void
_free_ctrlr(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;

	spdk_poller_unregister(&ctrlr->vfu_ctx_poller);
	free(ctrlr);
}

static void
free_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr, bool free_qps)
{
	int i;
	assert(ctrlr != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "free %s\n", ctrlr_id(ctrlr));

	if (free_qps) {
		for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
			free_qp(ctrlr, i);
		}
	}

	if (ctrlr->thread == spdk_get_thread()) {
		_free_ctrlr(ctrlr);
	} else {
		spdk_thread_send_msg(ctrlr->thread, _free_ctrlr, ctrlr);
	}
}

static void
nvmf_vfio_user_create_ctrlr(struct nvmf_vfio_user_transport *transport,
			    struct nvmf_vfio_user_endpoint *endpoint)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int err = 0;

	/* First, construct a vfio-user CUSTOM transport controller */
	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		err = -ENOMEM;
		goto out;
	}
	/* We can only support one connection for now */
	ctrlr->cntlid = 0x1;
	ctrlr->transport = transport;
	ctrlr->endpoint = endpoint;
	ctrlr->doorbells = endpoint->doorbells;
	TAILQ_INIT(&ctrlr->connected_qps);

	/* Then, construct an admin queue pair */
	err = init_qp(ctrlr, &transport->transport, NVMF_VFIO_USER_DEFAULT_AQ_DEPTH, 0);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}
	endpoint->ctrlr = ctrlr;

	/* Notify the generic layer about the new admin queue pair */
	spdk_nvmf_tgt_new_qpair(transport->transport.tgt, &ctrlr->qp[0]->qpair);

out:
	if (err != 0) {
		SPDK_ERRLOG("%s: failed to create vfio-user controller: %s\n",
			    endpoint_id(endpoint), strerror(-err));
	}
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

	endpoint->devmem_fd = ret;
	ret = ftruncate(endpoint->devmem_fd,
			NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE);
	if (ret != 0) {
		SPDK_ERRLOG("%s: error to ftruncate file %s: %s.\n", endpoint_id(endpoint), path,
			    spdk_strerror(errno));
		goto out;
	}

	endpoint->doorbells = mmap(NULL, NVMF_VFIO_USER_DOORBELLS_SIZE,
				   PROT_READ | PROT_WRITE, MAP_SHARED, endpoint->devmem_fd, NVME_DOORBELLS_OFFSET);
	if (endpoint->doorbells == MAP_FAILED) {
		SPDK_ERRLOG("%s: error to mmap file %s: %s.\n", endpoint_id(endpoint), path, spdk_strerror(errno));
		endpoint->doorbells = NULL;
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
	vfu_setup_log(endpoint->vfu_ctx, vfio_user_log, vfio_user_get_log_level());

	ret = vfio_user_dev_info_fill(vu_transport, endpoint);
	if (ret < 0) {
		goto out;
	}

	TAILQ_INSERT_TAIL(&vu_transport->endpoints, endpoint, link);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: doorbells %p\n", uuid, endpoint->doorbells);

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
			if (endpoint->ctrlr) {
				/* Users may kill NVMeoF target while VM
				 * is connected, free all resources.
				 */
				free_ctrlr(endpoint->ctrlr, true);
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
	cdata->vid = SPDK_PCI_VID_NUTANIX;
	cdata->ssvid = SPDK_PCI_VID_NUTANIX;
	cdata->ieee[0] = 0x8d;
	cdata->ieee[1] = 0x6b;
	cdata->ieee[2] = 0x50;
	memset(&cdata->sgls, 0, sizeof(struct spdk_nvme_cdata_sgls));
	cdata->sgls.supported = SPDK_NVME_SGLS_SUPPORTED_DWORD_ALIGNED;
	/* libvfio-user can only support 1 connection for now */
	cdata->oncs.reservations = 0;
}

static int
nvmf_vfio_user_listen_associate(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem,
				const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	TAILQ_FOREACH(endpoint, &vu_transport->endpoints, link) {
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			break;
		}
	}

	if (endpoint == NULL) {
		return -ENOENT;
	}

	endpoint->subsystem = subsystem;

	return 0;
}

/*
 * Executed periodically at a default SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US
 * frequency.
 *
 * For each transport endpoint (which at the libvfio-user level corresponds to
 * a socket), if we don't currently have a controller set up, peek to see if the
 * socket is able to accept a new connection.
 *
 * This poller also takes care of handling the creation of any pending new
 * qpairs.
 */
static int
nvmf_vfio_user_accept(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint;
	uint32_t count = 0;
	int err;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_lock(&vu_transport->lock);

	TAILQ_FOREACH(endpoint, &vu_transport->endpoints, link) {
		if (endpoint->ctrlr != NULL) {
			continue;
		}

		err = vfu_attach_ctx(endpoint->vfu_ctx);
		if (err != 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}

			pthread_mutex_unlock(&vu_transport->lock);
			return SPDK_POLLER_BUSY;
		}

		count++;

		/* Construct a controller */
		nvmf_vfio_user_create_ctrlr(vu_transport, endpoint);
	}

	pthread_mutex_unlock(&vu_transport->lock);

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_vfio_user_discover(struct spdk_nvmf_transport *transport,
			struct spdk_nvme_transport_id *trid,
			struct spdk_nvmf_discovery_log_page_entry *entry)
{ }

static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct nvmf_vfio_user_poll_group *vu_group;

	SPDK_DEBUGLOG(nvmf_vfio, "create poll group\n");

	vu_group = calloc(1, sizeof(*vu_group));
	if (vu_group == NULL) {
		SPDK_ERRLOG("Error allocating poll group: %m");
		return NULL;
	}

	TAILQ_INIT(&vu_group->qps);

	return &vu_group->group;
}

/* called when process exits */
static void
nvmf_vfio_user_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group;

	SPDK_DEBUGLOG(nvmf_vfio, "destroy poll group\n");

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);

	free(vu_group);
}

static void
vfio_user_qpair_disconnect_cb(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	pthread_mutex_lock(&endpoint->lock);
	ctrlr = endpoint->ctrlr;
	if (!ctrlr) {
		pthread_mutex_unlock(&endpoint->lock);
		return;
	}

	if (TAILQ_EMPTY(&ctrlr->connected_qps)) {
		endpoint->ctrlr = NULL;
		free_ctrlr(ctrlr, false);
	}
	pthread_mutex_unlock(&endpoint->lock);
}

static void
_vfio_user_qpair_disconnect(void *ctx)
{
	struct nvmf_vfio_user_qpair *vu_qpair = ctx;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint;

	vu_ctrlr = vu_qpair->ctrlr;
	endpoint = vu_ctrlr->endpoint;

	spdk_nvmf_qpair_disconnect(&vu_qpair->qpair, vfio_user_qpair_disconnect_cb, endpoint);
}

static int
vfio_user_destroy_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_qpair *qpair;
	struct nvmf_vfio_user_endpoint *endpoint;

	SPDK_DEBUGLOG(nvmf_vfio, "%s stop processing\n", ctrlr_id(ctrlr));

	endpoint = ctrlr->endpoint;
	assert(endpoint != NULL);

	pthread_mutex_lock(&endpoint->lock);
	if (TAILQ_EMPTY(&ctrlr->connected_qps)) {
		endpoint->ctrlr = NULL;
		free_ctrlr(ctrlr, false);
		pthread_mutex_unlock(&endpoint->lock);
		return 0;
	}

	TAILQ_FOREACH(qpair, &ctrlr->connected_qps, tailq) {
		/* add another round thread poll to avoid recursive endpoint lock */
		spdk_thread_send_msg(ctrlr->thread, _vfio_user_qpair_disconnect, qpair);
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
			return SPDK_POLLER_BUSY;
		}

		spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

		/* initiator shutdown or reset, waiting for another re-connect */
		if (errno == ENOTCONN) {
			vfio_user_destroy_ctrlr(ctrlr);
			return SPDK_POLLER_BUSY;
		}

		fail_ctrlr(ctrlr);
	}

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
handle_queue_connect_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_qpair *vu_qpair = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint;

	assert(vu_qpair != NULL);
	assert(req != NULL);

	vu_ctrlr = vu_qpair->ctrlr;
	assert(vu_ctrlr != NULL);
	endpoint = vu_ctrlr->endpoint;
	assert(endpoint != NULL);

	if (spdk_nvme_cpl_is_error(&req->req.rsp->nvme_cpl)) {
		SPDK_ERRLOG("SC %u, SCT %u\n", req->req.rsp->nvme_cpl.status.sc, req->req.rsp->nvme_cpl.status.sct);
		endpoint->ctrlr = NULL;
		free_ctrlr(vu_ctrlr, true);
		return -1;
	}

	vu_group = SPDK_CONTAINEROF(vu_qpair->group, struct nvmf_vfio_user_poll_group, group);
	TAILQ_INSERT_TAIL(&vu_group->qps, vu_qpair, link);
	vu_qpair->state = VFIO_USER_QPAIR_ACTIVE;

	pthread_mutex_lock(&endpoint->lock);
	if (nvmf_qpair_is_admin_queue(&vu_qpair->qpair)) {
		vu_ctrlr->cntlid = vu_qpair->qpair.ctrlr->cntlid;
		vu_ctrlr->thread = spdk_get_thread();
		vu_ctrlr->ctrlr = vu_qpair->qpair.ctrlr;
		vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx, vu_ctrlr, 0);
	} else {
		/* For I/O queues this command was generated in response to an
		 * ADMIN I/O CREATE SUBMISSION QUEUE command which has not yet
		 * been completed. Complete it now.
		 */
		post_completion(vu_ctrlr, &vu_ctrlr->qp[0]->cq, 0, 0,
				vu_qpair->create_io_sq_cmd.cid, SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
	}
	TAILQ_INSERT_TAIL(&vu_ctrlr->connected_qps, vu_qpair, tailq);
	pthread_mutex_unlock(&endpoint->lock);

	free(req->req.data);
	req->req.data = NULL;

	return 0;
}

/*
 * Add the given qpair to the given poll group. New qpairs are added via
 * spdk_nvmf_tgt_new_qpair(), which picks a poll group, then calls back
 * here via nvmf_transport_poll_group_add().
 */
static int
nvmf_vfio_user_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct spdk_nvmf_request *req;
	struct spdk_nvmf_fabric_connect_data *data;
	bool admin;

	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);
	vu_qpair->group = group;
	ctrlr = vu_qpair->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: add QP%d=%p(%p) to poll_group=%p\n",
		      ctrlr_id(ctrlr), vu_qpair->qpair.qid,
		      vu_qpair, qpair, group);

	admin = nvmf_qpair_is_admin_queue(&vu_qpair->qpair);

	vu_req = get_nvmf_vfio_user_req(vu_qpair);
	if (vu_req == NULL) {
		return -1;
	}

	req = &vu_req->req;
	req->cmd->connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->cmd->connect_cmd.cid = 0;
	req->cmd->connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	req->cmd->connect_cmd.recfmt = 0;
	req->cmd->connect_cmd.sqsize = vu_qpair->qsize - 1;
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
	vu_req->cb_arg = vu_qpair;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: sending connect fabrics command for QID=%#x cntlid=%#x\n",
		      ctrlr_id(ctrlr), qpair->qid, data->cntlid);

	spdk_nvmf_request_exec_fabrics(req);
	return 0;
}

static int
nvmf_vfio_user_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_poll_group *vu_group;

	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: remove NVMf QP%d=%p from NVMf poll_group=%p\n",
		      ctrlr_id(vu_qpair->ctrlr), qpair->qid, qpair, group);


	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);
	TAILQ_REMOVE(&vu_group->qps, vu_qpair, link);

	return 0;
}

static void
_nvmf_vfio_user_req_free(struct nvmf_vfio_user_qpair *vu_qpair, struct nvmf_vfio_user_req *vu_req)
{
	memset(&vu_req->cmd, 0, sizeof(vu_req->cmd));
	memset(&vu_req->rsp, 0, sizeof(vu_req->rsp));
	vu_req->iovcnt = 0;
	vu_req->state = VFIO_USER_REQUEST_STATE_FREE;

	TAILQ_INSERT_TAIL(&vu_qpair->reqs, vu_req, link);
}

static int
nvmf_vfio_user_req_free(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_req *vu_req;

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	vu_qpair = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_qpair, qpair);

	_nvmf_vfio_user_req_free(vu_qpair, vu_req);

	return 0;
}

static int
nvmf_vfio_user_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_req *vu_req;

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	vu_qpair = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_qpair, qpair);

	if (vu_req->cb_fn != NULL) {
		if (vu_req->cb_fn(vu_req, vu_req->cb_arg) != 0) {
			fail_ctrlr(vu_qpair->ctrlr);
		}
	}

	_nvmf_vfio_user_req_free(vu_qpair, vu_req);

	return 0;
}

static void
nvmf_vfio_user_close_qpair(struct spdk_nvmf_qpair *qpair,
			   spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;

	assert(qpair != NULL);
	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);
	vu_ctrlr = vu_qpair->ctrlr;

	pthread_mutex_lock(&vu_ctrlr->endpoint->lock);
	TAILQ_REMOVE(&vu_ctrlr->connected_qps, vu_qpair, tailq);
	pthread_mutex_unlock(&vu_ctrlr->endpoint->lock);

	free_qp(vu_ctrlr, qpair->qid);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

/**
 * Returns a preallocated spdk_nvmf_request or NULL if there isn't one available.
 */
static struct nvmf_vfio_user_req *
get_nvmf_vfio_user_req(struct nvmf_vfio_user_qpair *qpair)
{
	struct nvmf_vfio_user_req *req;

	assert(qpair != NULL);

	if (TAILQ_EMPTY(&qpair->reqs)) {
		return NULL;
	}

	req = TAILQ_FIRST(&qpair->reqs);
	TAILQ_REMOVE(&qpair->reqs, req, link);

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
	uint32_t len = 0;
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
		len = (((cmd->cdw11_bits.get_log_page.numdu << 16) | cmd->cdw10_bits.get_log_page.numdl) + 1) * 4;
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
	       struct nvmf_vfio_user_qpair *vu_qpair)
{
	int err;
	struct nvmf_vfio_user_req *vu_req;
	struct spdk_nvmf_request *req;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	vu_req = get_nvmf_vfio_user_req(vu_qpair);
	if (spdk_unlikely(vu_req == NULL)) {
		SPDK_ERRLOG("%s: no request for NVMe command opc 0x%x\n", ctrlr_id(ctrlr), cmd->opc);
		return post_completion(ctrlr, &vu_qpair->cq, 0, 0, cmd->cid,
				       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, SPDK_NVME_SCT_GENERIC);

	}
	req = &vu_req->req;

	assert(req->qpair != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: handle qid%u, req opc=%#x cid=%d\n",
		      ctrlr_id(ctrlr), req->qpair->qid, cmd->opc, cmd->cid);

	vu_req->cb_fn = handle_cmd_rsp;
	vu_req->cb_arg = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_qpair, qpair);
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
		_nvmf_vfio_user_req_free(vu_qpair, vu_req);
		return err;
	}

	vu_req->state = VFIO_USER_REQUEST_STATE_EXECUTING;
	spdk_nvmf_request_exec(req);

	return 0;
}

/* Returns the number of commands processed, or a negative value on error. */
static int
nvmf_vfio_user_qpair_poll(struct nvmf_vfio_user_qpair *qpair)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	uint32_t new_tail;
	int count = 0;

	assert(qpair != NULL);

	ctrlr = qpair->ctrlr;

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
	spdk_ivdt_dcache(tdbl(ctrlr, &qpair->sq));

	/* Load-Acquire. */
	new_tail = *tdbl(ctrlr, &qpair->sq);

	/*
	 * Ensure that changes to the queue are visible to us.
	 * The host driver should write the queue first, do a wmb(), and then
	 * update the SQ tail doorbell (their Store-Release).
	 */
	spdk_rmb();

	new_tail = new_tail & 0xffffu;
	if (spdk_unlikely(new_tail >= qpair->sq.size)) {
		union spdk_nvme_async_event_completion event = {};

		SPDK_DEBUGLOG(nvmf_vfio, "%s: invalid SQ%u doorbell value %u\n", ctrlr_id(ctrlr), qpair->qpair.qid,
			      new_tail);
		event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_ERROR;
		event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE;
		nvmf_ctrlr_async_event_error_event(qpair->qpair.ctrlr, event);

		return 0;
	}

	if (sq_head(qpair) == new_tail) {
		return 0;
	}

	count = handle_sq_tdbl_write(ctrlr, new_tail, qpair);
	if (count < 0) {
		fail_ctrlr(ctrlr);
	}

	return count;
}

/*
 * vfio-user transport poll handler. Note that the library context is polled in
 * a separate poller (->vfu_ctx_poller), so this poller only needs to poll the
 * active qpairs.
 *
 * Returns the number of commands processed, or a negative value on error.
 */
static int
nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_qpair *vu_qpair, *tmp;
	int count = 0;

	assert(group != NULL);

	spdk_rmb();

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);

	TAILQ_FOREACH_SAFE(vu_qpair, &vu_group->qps, link, tmp) {
		int ret;

		if (spdk_unlikely(vu_qpair->state != VFIO_USER_QPAIR_ACTIVE || !vu_qpair->sq.size)) {
			continue;
		}

		ret = nvmf_vfio_user_qpair_poll(vu_qpair);

		if (ret < 0) {
			return ret;
		}

		count += ret;
	}

	return count;
}

static int
nvmf_vfio_user_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);
	ctrlr = vu_qpair->ctrlr;

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
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);
	ctrlr = vu_qpair->ctrlr;

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));
	return 0;
}

static void
nvmf_vfio_user_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_qpair *vu_qpair;
	struct nvmf_vfio_user_req *vu_req, *vu_req_to_abort = NULL;
	uint32_t i;
	uint16_t cid;

	vu_qpair = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_qpair, qpair);

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	for (i = 0; i < vu_qpair->qsize; i++) {
		vu_req = &vu_qpair->reqs_internal[i];
		if (vu_req->state == VFIO_USER_REQUEST_STATE_EXECUTING && vu_req->cmd.cid == cid) {
			vu_req_to_abort = vu_req;
			break;
		}
	}

	if (vu_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &vu_req_to_abort->req;
	nvmf_ctrlr_abort_request(req);
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
};

SPDK_NVMF_TRANSPORT_REGISTER(muser, &spdk_nvmf_transport_vfio_user);
SPDK_LOG_REGISTER_COMPONENT(nvmf_vfio)
