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

#ifndef __NVMF_INTERNAL_H__
#define __NVMF_INTERNAL_H__

#include "spdk/stdinc.h"

#include "spdk/likely.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#define SPDK_NVMF_DEFAULT_NUM_CTRLRS_PER_LCORE 1

struct spdk_nvmf_tgt {
	struct spdk_nvmf_tgt_opts		opts;

	struct spdk_thread			*master_thread;

	uint16_t				next_cntlid;
	uint64_t				discovery_genctr;
	TAILQ_HEAD(, spdk_nvmf_subsystem)	subsystems;
	struct spdk_nvmf_discovery_log_page	*discovery_log_page;
	size_t					discovery_log_page_size;
	uint32_t				current_subsystem_id;
	TAILQ_HEAD(, spdk_nvmf_transport)	transports;
};

struct spdk_nvmf_host {
	char				*nqn;
	TAILQ_ENTRY(spdk_nvmf_host)	link;
};

struct spdk_nvmf_listener {
	struct spdk_nvme_transport_id	trid;
	TAILQ_ENTRY(spdk_nvmf_listener)	link;
};

struct spdk_nvmf_transport_poll_group {
	struct spdk_nvmf_transport			*transport;
	TAILQ_ENTRY(spdk_nvmf_transport_poll_group)	link;
};

struct spdk_nvmf_poll_group {
	TAILQ_HEAD(, spdk_nvmf_transport_poll_group) tgroups;

	TAILQ_ENTRY(spdk_nvmf_poll_group)	link;
};

typedef enum _spdk_nvmf_request_exec_status {
	SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE,
	SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS,
} spdk_nvmf_request_exec_status;

union nvmf_h2c_msg {
	struct spdk_nvmf_capsule_cmd			nvmf_cmd;
	struct spdk_nvme_cmd				nvme_cmd;
	struct spdk_nvmf_fabric_prop_set_cmd		prop_set_cmd;
	struct spdk_nvmf_fabric_prop_get_cmd		prop_get_cmd;
	struct spdk_nvmf_fabric_connect_cmd		connect_cmd;
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_h2c_msg) == 64, "Incorrect size");

union nvmf_c2h_msg {
	struct spdk_nvme_cpl				nvme_cpl;
	struct spdk_nvmf_fabric_prop_get_rsp		prop_get_rsp;
	struct spdk_nvmf_fabric_connect_rsp		connect_rsp;
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_c2h_msg) == 16, "Incorrect size");

struct spdk_nvmf_request {
	struct spdk_nvmf_qpair		*qpair;
	uint32_t			length;
	enum spdk_nvme_data_transfer	xfer;
	void				*data;
	union nvmf_h2c_msg		*cmd;
	union nvmf_c2h_msg		*rsp;
};

struct spdk_nvmf_ns {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	uint32_t id;
	bool allocated;
};

enum spdk_nvmf_qpair_type {
	QPAIR_TYPE_AQ = 0,
	QPAIR_TYPE_IOQ = 1,
};

struct spdk_nvmf_qpair {
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_ctrlr			*ctrlr;
	enum spdk_nvmf_qpair_type		type;

	struct spdk_thread			*thread;

	uint16_t				qid;
	uint16_t				sq_head;
	uint16_t				sq_head_max;

	TAILQ_ENTRY(spdk_nvmf_qpair) 		link;
};

/*
 * This structure represents an NVMe-oF controller,
 * which is like a "session" in networking terms.
 */
struct spdk_nvmf_ctrlr {
	uint16_t			cntlid;
	struct spdk_nvmf_subsystem 	*subsys;

	struct {
		union spdk_nvme_cap_register	cap;
		union spdk_nvme_vs_register	vs;
		union spdk_nvme_cc_register	cc;
		union spdk_nvme_csts_register	csts;
	} vcprop; /* virtual controller properties */

	TAILQ_HEAD(, spdk_nvmf_qpair) qpairs;
	int num_qpairs;
	int max_qpairs_allowed;
	uint32_t kato;
	union {
		uint32_t raw;
		struct {
			union spdk_nvme_critical_warning_state crit_warn;
			uint8_t ns_attr_notice : 1;
			uint8_t fw_activation_notice : 1;
		} bits;
	} async_event_config;
	struct spdk_nvmf_request *aer_req;
	uint8_t hostid[16];
	struct spdk_nvmf_poll_group		*group;

	TAILQ_ENTRY(spdk_nvmf_ctrlr) 		link;
};

struct spdk_nvmf_subsystem {
	uint32_t id;
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	enum spdk_nvmf_subtype subtype;
	bool is_removed;

	struct spdk_nvmf_tgt			*tgt;

	char sn[SPDK_NVME_CTRLR_SN_LEN + 1];

	/* Array of namespaces of size max_nsid indexed by nsid - 1 */
	struct spdk_nvmf_ns			*ns;
	uint32_t 				max_nsid;
	uint32_t				num_allocated_nsid;

	TAILQ_HEAD(, spdk_nvmf_ctrlr)		ctrlrs;

	TAILQ_HEAD(, spdk_nvmf_host)		hosts;

	TAILQ_HEAD(, spdk_nvmf_listener)	listeners;

	TAILQ_ENTRY(spdk_nvmf_subsystem)	entries;
};

uint16_t spdk_nvmf_tgt_gen_cntlid(struct spdk_nvmf_tgt *tgt);
struct spdk_nvmf_transport *spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt,
		enum spdk_nvme_transport_type);

struct spdk_nvmf_poll_group *spdk_nvmf_poll_group_create(
	struct spdk_nvmf_tgt *tgt);
void spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group);
int spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			     struct spdk_nvmf_qpair *qpair);
int spdk_nvmf_poll_group_remove(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_qpair *qpair);
int spdk_nvmf_poll_group_poll(struct spdk_nvmf_poll_group *group);

int spdk_nvmf_request_exec(struct spdk_nvmf_request *req);
int spdk_nvmf_request_complete(struct spdk_nvmf_request *req);
int spdk_nvmf_request_abort(struct spdk_nvmf_request *req);

void spdk_nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt,
				      void *buffer, uint64_t offset,
				      uint32_t length);

void spdk_nvmf_property_get(struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_fabric_prop_get_cmd *cmd,
			    struct spdk_nvmf_fabric_prop_get_rsp *response);

void spdk_nvmf_property_set(struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_fabric_prop_set_cmd *cmd,
			    struct spdk_nvme_cpl *rsp);

void spdk_nvmf_ctrlr_connect(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvmf_fabric_connect_cmd *cmd,
			     struct spdk_nvmf_fabric_connect_data *data,
			     struct spdk_nvmf_fabric_connect_rsp *rsp);
struct spdk_nvmf_qpair *spdk_nvmf_ctrlr_get_qpair(struct spdk_nvmf_ctrlr *ctrlr, uint16_t qid);
int spdk_nvmf_ctrlr_poll(struct spdk_nvmf_ctrlr *ctrlr);
void spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr);
int spdk_nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req);
int spdk_nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req);
bool spdk_nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr);

int spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_bdev *bdev, struct spdk_nvme_ns_data *nsdata);

int spdk_nvmf_subsystem_bdev_attach(struct spdk_nvmf_subsystem *subsystem);
void spdk_nvmf_subsystem_bdev_detach(struct spdk_nvmf_subsystem *subsystem);

static inline struct spdk_nvmf_ns *
_spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;

	/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
	if (spdk_unlikely(nsid - 1 >= subsystem->max_nsid)) {
		return NULL;
	}

	ns = &subsystem->ns[nsid - 1];
	if (!ns->allocated) {
		return NULL;
	}

	return ns;
}

#define OBJECT_NVMF_IO				0x30

#define TRACE_GROUP_NVMF			0x3
#define TRACE_NVMF_IO_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0)
#define TRACE_RDMA_READ_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x1)
#define TRACE_RDMA_WRITE_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x2)
#define TRACE_RDMA_READ_COMPLETE      		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x3)
#define TRACE_RDMA_WRITE_COMPLETE		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x4)
#define TRACE_NVMF_LIB_READ_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x5)
#define TRACE_NVMF_LIB_WRITE_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x6)
#define TRACE_NVMF_LIB_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x7)
#define TRACE_NVMF_IO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x8)

#endif /* __NVMF_INTERNAL_H__ */
