/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

#ifndef __NVMF_FC_H__
#define __NVMF_FC_H__

#include "spdk/nvmf.h"
#include "spdk/assert.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_fc_spec.h"
#include "spdk/event.h"
#include "spdk/io_channel.h"
#include "nvmf_internal.h"

#define SPDK_NVMF_FC_TR_ADDR_LEN 64

/*
 * FC HW port states.
 */
enum spdk_fc_port_state {
	SPDK_FC_PORT_OFFLINE = 0,
	SPDK_FC_PORT_ONLINE = 1,
	SPDK_FC_PORT_QUIESCED = 2,
};

enum spdk_fc_hwqp_state {
	SPDK_FC_HWQP_OFFLINE = 0,
	SPDK_FC_HWQP_ONLINE = 1,
};

/*
 * NVMF BCM FC Object state
 * Add all the generic states of the object here.
 * Specific object states can be added separately
 */
enum spdk_nvmf_fc_object_state {
	SPDK_NVMF_FC_OBJECT_CREATED = 0,
	SPDK_NVMF_FC_OBJECT_TO_BE_DELETED = 1,
	SPDK_NVMF_FC_OBJECT_ZOMBIE = 2,      /* Partial Create or Delete  */
};

/*
 * FC request state
 */
enum spdk_nvmf_fc_request_state {
	SPDK_NVMF_FC_REQ_INIT = 0,
	SPDK_NVMF_FC_REQ_READ_BDEV,
	SPDK_NVMF_FC_REQ_READ_XFER,
	SPDK_NVMF_FC_REQ_READ_RSP,
	SPDK_NVMF_FC_REQ_WRITE_BUFFS,
	SPDK_NVMF_FC_REQ_WRITE_XFER,
	SPDK_NVMF_FC_REQ_WRITE_BDEV,
	SPDK_NVMF_FC_REQ_WRITE_RSP,
	SPDK_NVMF_FC_REQ_NONE_BDEV,
	SPDK_NVMF_FC_REQ_NONE_RSP,
	SPDK_NVMF_FC_REQ_SUCCESS,
	SPDK_NVMF_FC_REQ_FAILED,
	SPDK_NVMF_FC_REQ_ABORTED,
	SPDK_NVMF_FC_REQ_PENDING,
	SPDK_NVMF_FC_REQ_MAX_STATE,
};

/*
 * FC HWQP pointer
 */
typedef void *spdk_nvmf_fc_lld_hwqp_t;

/*
 * FC World Wide Name
 */
struct spdk_nvmf_fc_wwn {
	union {
		uint64_t wwn; /* World Wide Names consist of eight bytes */
		uint8_t octets[sizeof(uint64_t)];
	} u;
};

/*
 * Generic DMA buffer descriptor
 */
struct spdk_nvmf_fc_buffer_desc {
	void *virt;
	uint64_t phys;
	size_t len;

	/* Internal */
	uint32_t buf_index;
};

/*
 * ABTS hadling context
 */
struct spdk_nvmf_fc_abts_ctx {
	bool handled;
	uint16_t hwqps_responded;
	uint16_t rpi;
	uint16_t oxid;
	uint16_t rxid;
	struct spdk_nvmf_fc_nport *nport;
	uint16_t nport_hdl;
	uint8_t port_hdl;
	void *abts_poller_args;
	void *sync_poller_args;
	int num_hwqps;
	bool queue_synced;
	uint64_t u_id;
	struct spdk_nvmf_fc_hwqp *ls_hwqp;
	uint16_t fcp_rq_id;
};

/*
 * NVME FC transport errors
 */
struct spdk_nvmf_fc_errors {
	uint32_t no_xri;
	uint32_t nport_invalid;
	uint32_t unknown_frame;
	uint32_t wqe_cmplt_err;
	uint32_t wqe_write_err;
	uint32_t rq_status_err;
	uint32_t rq_buf_len_err;
	uint32_t rq_id_err;
	uint32_t rq_index_err;
	uint32_t invalid_cq_type;
	uint32_t invalid_cq_id;
	uint32_t fc_req_buf_err;
	uint32_t aq_buf_alloc_err;
	uint32_t write_buf_alloc_err;
	uint32_t read_buf_alloc_err;
	uint32_t unexpected_err;
	uint32_t nvme_cmd_iu_err;
	uint32_t nvme_cmd_xfer_err;
	uint32_t queue_entry_invalid;
	uint32_t invalid_conn_err;
	uint32_t fcp_rsp_failure;
	uint32_t write_failed;
	uint32_t read_failed;
	uint32_t rport_invalid;
	uint32_t num_aborted;
	uint32_t num_abts_sent;
};

/*
 *  Send Single Request/Response Sequence.
 */
struct spdk_nvmf_fc_send_srsr {
	struct spdk_nvmf_fc_buffer_desc rqst;
	struct spdk_nvmf_fc_buffer_desc rsp;
	struct spdk_nvmf_fc_buffer_desc sgl; /* Note: Len = (2 * bcm_sge_t) */
	uint16_t rpi;
};

/*
 * Struct representing a nport
 */
struct spdk_nvmf_fc_nport {

	uint16_t nport_hdl;
	uint8_t port_hdl;
	uint32_t d_id;
	enum spdk_nvmf_fc_object_state nport_state;
	struct spdk_nvmf_fc_wwn fc_nodename;
	struct spdk_nvmf_fc_wwn fc_portname;

	/* list of remote ports (i.e. initiators) connected to nport */
	TAILQ_HEAD(, spdk_nvmf_fc_remote_port_info) rem_port_list;
	uint32_t rport_count;

	void *vendor_data;	/* available for vendor use */

	/* list of associations to nport */
	TAILQ_HEAD(, spdk_nvmf_fc_association) fc_associations;
	uint32_t assoc_count;
	struct spdk_nvmf_fc_port *fc_port;
	TAILQ_ENTRY(spdk_nvmf_fc_nport) link; /* list of nports on a hw port. */
};

/*
 * NVMF FC Connection
 */
struct spdk_nvmf_fc_conn {
	struct spdk_nvmf_qpair qpair;

	uint64_t conn_id;
	struct spdk_nvmf_fc_hwqp *hwqp;
	uint16_t esrp_ratio;
	uint16_t rsp_count;
	uint32_t rsn;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t max_queue_depth;
	uint16_t max_rw_depth;
	/* The current number of I/O outstanding on this connection. This number
	 * includes all I/O from the time the capsule is first received until it is
	 * completed.
	 */
	uint16_t cur_queue_depth;

	/* number of read/write requests that are outstanding */
	uint16_t cur_fc_rw_depth;

	/* requests that are waiting to obtain xri/buffer */
	TAILQ_HEAD(, spdk_nvmf_fc_request) pending_queue;

	struct spdk_nvmf_fc_association *fc_assoc;

	/* additional FC info here - TBD */
	uint16_t rpi;

	/* for association's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_link;

	/* for assocations's available connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_avail_link;

	/* for hwqp's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) link;
};

/*
 * Structure for maintaining the XRI's
 */
struct spdk_nvmf_fc_xri {
	uint32_t xri;   /* The actual xri value */
	/* Internal */
	TAILQ_ENTRY(spdk_nvmf_fc_xri) link;
	bool is_active;
};

struct spdk_nvmf_fc_poll_group;

/*
 *  HWQP poller structure passed from Master thread
 */
struct spdk_nvmf_fc_hwqp {
	uint32_t lcore_id;   /* core hwqp is running on (for tracing purposes only) */
	struct spdk_thread *thread;  /* thread hwqp is running on */
	uint32_t hwqp_id;    /* A unique id (per physical port) for a hwqp */
	uint32_t rq_size;    /* receive queue size */
	spdk_nvmf_fc_lld_hwqp_t queues;          /* vendor HW queue set */
	struct spdk_nvmf_fc_port *fc_port; /* HW port structure for these queues */
	struct spdk_nvmf_fc_poll_group *poll_group;

	void *context;			/* Vendor Context */

	TAILQ_HEAD(, spdk_nvmf_fc_conn) connection_list;
	uint32_t num_conns; /* number of connections to queue */
	uint16_t cid_cnt;   /* used to generate unique conn. id for RQ */
	uint32_t free_q_slots; /* free q slots available for connections  */
	enum spdk_fc_hwqp_state state;  /* Poller state (e.g. online, offline) */

	/* Internal */
	struct spdk_mempool *fc_request_pool;
	TAILQ_HEAD(, spdk_nvmf_fc_request) in_use_reqs;

	TAILQ_HEAD(, spdk_nvmf_fc_xri) pending_xri_list;

	struct spdk_nvmf_fc_errors counters;
	uint32_t send_frame_xri;
	uint8_t send_frame_seqid;

	/* Pending LS request waiting for XRI. */
	TAILQ_HEAD(, spdk_nvmf_fc_ls_rqst) ls_pending_queue;

	/* Sync req list */
	TAILQ_HEAD(, spdk_nvmf_fc_poller_api_queue_sync_args) sync_cbs;

	TAILQ_ENTRY(spdk_nvmf_fc_hwqp) link;
};

struct spdk_nvmf_fc_ls_rsrc_pool {
	void *assocs_mptr;
	uint32_t assocs_count;
	TAILQ_HEAD(, spdk_nvmf_fc_association) assoc_free_list;

	void *conns_mptr;
	uint32_t conns_count;
	TAILQ_HEAD(, spdk_nvmf_fc_conn) fc_conn_free_list;
};

/*
 * FC HW port.
 */
struct spdk_nvmf_fc_port {
	uint8_t port_hdl;
	enum spdk_fc_port_state hw_port_status;
	uint32_t xri_base;
	uint32_t xri_count;
	uint16_t fcp_rq_id;
	struct spdk_ring *xri_ring;
	struct spdk_nvmf_fc_hwqp ls_queue;
	uint32_t num_io_queues;
	struct spdk_nvmf_fc_hwqp *io_queues;
	/*
	 * List of nports on this HW port.
	 */
	TAILQ_HEAD(, spdk_nvmf_fc_nport)nport_list;
	int	num_nports;
	TAILQ_ENTRY(spdk_nvmf_fc_port) link;

	struct spdk_nvmf_fc_ls_rsrc_pool ls_rsrc_pool;
	struct spdk_mempool *io_rsrc_pool; /* Pools to store bdev_io's for this port */
	void *port_ctx;
};

/*
 * NVMF FC Request
 */
struct spdk_nvmf_fc_request {
	struct spdk_nvmf_request req;
	struct spdk_nvmf_fc_ersp_iu ersp;
	uint32_t poller_lcore; /* for tracing purposes only */
	struct spdk_thread *poller_thread;
	uint16_t buf_index;
	struct spdk_nvmf_fc_xri *xri;
	uint16_t oxid;
	uint16_t rpi;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_hwqp *hwqp;
	int state;
	uint32_t transfered_len;
	bool is_aborted;
	uint32_t magic;
	uint32_t s_id;
	uint32_t d_id;
	TAILQ_ENTRY(spdk_nvmf_fc_request) link;
	TAILQ_ENTRY(spdk_nvmf_fc_request) pending_link;
	TAILQ_HEAD(, spdk_nvmf_fc_caller_ctx) abort_cbs;
};

SPDK_STATIC_ASSERT(!offsetof(struct spdk_nvmf_fc_request, req),
		   "FC request and NVMF request address don't match.");

/*
 * NVMF FC Association
 */
struct spdk_nvmf_fc_association {
	uint64_t assoc_id;
	uint32_t s_id;
	struct spdk_nvmf_fc_nport *tgtport;
	struct spdk_nvmf_fc_remote_port_info *rport;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_host *host;
	enum spdk_nvmf_fc_object_state assoc_state;

	char host_id[FCNVME_ASSOC_HOSTID_LEN];
	char host_nqn[FCNVME_ASSOC_HOSTNQN_LEN];
	char sub_nqn[FCNVME_ASSOC_HOSTNQN_LEN];

	struct spdk_nvmf_fc_conn *aq_conn; /* connection for admin queue */

	uint16_t conn_count;
	TAILQ_HEAD(, spdk_nvmf_fc_conn) fc_conns;

	void *conns_buf;
	TAILQ_HEAD(, spdk_nvmf_fc_conn) avail_fc_conns;

	TAILQ_ENTRY(spdk_nvmf_fc_association) link;

	/* for port's association free list */
	TAILQ_ENTRY(spdk_nvmf_fc_association) port_free_assoc_list_link;

	void *ls_del_op_ctx; /* delete assoc. callback list */

	/* req/resp buffers used to send disconnect to initiator */
	struct spdk_nvmf_fc_send_srsr snd_disconn_bufs;
};

/*
 * FC Remote Port
 */
struct spdk_nvmf_fc_remote_port_info {
	uint32_t s_id;
	uint32_t rpi;
	uint32_t assoc_count;
	struct spdk_nvmf_fc_wwn fc_nodename;
	struct spdk_nvmf_fc_wwn fc_portname;
	enum spdk_nvmf_fc_object_state rport_state;
	TAILQ_ENTRY(spdk_nvmf_fc_remote_port_info) link;
};

/*
 * Poller API error codes
 */
enum spdk_nvmf_fc_poller_api_ret {
	SPDK_NVMF_FC_POLLER_API_SUCCESS = 0,
	SPDK_NVMF_FC_POLLER_API_ERROR,
	SPDK_NVMF_FC_POLLER_API_INVALID_ARG,
	SPDK_NVMF_FC_POLLER_API_NO_CONN_ID,
	SPDK_NVMF_FC_POLLER_API_DUP_CONN_ID,
	SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND,
};

/*
 * Poller API definitions
 */
enum spdk_nvmf_fc_poller_api {
	SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION,
	SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION,
	SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE,
	SPDK_NVMF_FC_POLLER_API_ACTIVATE_QUEUE,
	SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
	SPDK_NVMF_FC_POLLER_API_ADAPTER_EVENT,
	SPDK_NVMF_FC_POLLER_API_AEN,
	SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC,
	SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE,
};

/*
 * Poller API callback function proto
 */
typedef void (*spdk_nvmf_fc_poller_api_cb)(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret);

/*
 * Poller API callback data
 */
struct spdk_nvmf_fc_poller_api_cb_info {
	spdk_nvmf_fc_poller_api_cb cb_func;
	void *cb_data;
	enum spdk_nvmf_fc_poller_api_ret ret;
};

/*
 * Poller API structures
 */
struct spdk_nvmf_fc_poller_api_add_connection_args {
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_fc_poller_api_del_connection_args {
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	bool send_abts;
	/* internal */
	int fc_request_cnt;
};

struct spdk_nvmf_fc_poller_api_quiesce_queue_args {
	void   *ctx;
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_fc_poller_api_activate_queue_args {
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_fc_poller_api_abts_recvd_args {
	struct spdk_nvmf_fc_abts_ctx *ctx;
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_fc_poller_api_queue_sync_done_args {
	struct spdk_nvmf_fc_hwqp *hwqp;
	uint64_t tag;
};

/*
 * NVMF LS request structure
 */
struct spdk_nvmf_fc_ls_rqst {
	struct spdk_nvmf_fc_buffer_desc rqstbuf;
	struct spdk_nvmf_fc_buffer_desc rspbuf;
	uint32_t rqst_len;
	uint32_t rsp_len;
	uint32_t rpi;
	struct spdk_nvmf_fc_xri *xri;
	uint16_t oxid;
	void *private_data; /* for LLD only (LS does not touch) */
	TAILQ_ENTRY(spdk_nvmf_fc_ls_rqst) ls_pending_link;
	uint32_t s_id;
	uint32_t d_id;
	struct spdk_nvmf_fc_nport *nport;
	struct spdk_nvmf_fc_remote_port_info *rport;
	struct spdk_nvmf_tgt *nvmf_tgt;
};

/*
 * RQ Buffer LS Overlay Structure
 */
#define FCNVME_LS_RSVD_SIZE (FCNVME_MAX_LS_BUFFER_SIZE - \
	(sizeof(struct spdk_nvmf_fc_ls_rqst) + FCNVME_MAX_LS_REQ_SIZE + FCNVME_MAX_LS_RSP_SIZE))

struct __attribute__((__packed__)) spdk_nvmf_fc_rq_buf_ls_request {
	uint8_t rqst[FCNVME_MAX_LS_REQ_SIZE];
	uint8_t resp[FCNVME_MAX_LS_RSP_SIZE];
	struct spdk_nvmf_fc_ls_rqst ls_rqst;
	uint8_t rsvd[FCNVME_LS_RSVD_SIZE];
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_rq_buf_ls_request) ==
		   FCNVME_MAX_LS_BUFFER_SIZE, "LS RQ Buffer overflow");


struct spdk_nvmf_fc_poller_api_queue_sync_args {
	uint64_t u_id;
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;

	/* Used internally by poller */
	TAILQ_ENTRY(spdk_nvmf_fc_poller_api_queue_sync_args) link;
};

/*
 * dump info
 */
struct spdk_nvmf_fc_queue_dump_info {
	char *buffer;
	int   offset;
};
#define SPDK_FC_HW_DUMP_BUF_SIZE (10 * 4096)

static inline void
spdk_nvmf_fc_dump_buf_print(struct spdk_nvmf_fc_queue_dump_info *dump_info, char *fmt, ...)
{
	uint64_t buffer_size = SPDK_FC_HW_DUMP_BUF_SIZE;
	int32_t avail = (int32_t)(buffer_size - dump_info->offset);

	if (avail > 0) {
		va_list ap;
		int32_t written;

		va_start(ap, fmt);
		written = vsnprintf(dump_info->buffer + dump_info->offset, avail, fmt, ap);
		if (written >= avail) {
			dump_info->offset += avail;
		} else {
			dump_info->offset += written;
		}
		va_end(ap);
	}
}

/*
 * NVMF FC caller callback definitions
 */
typedef void (*spdk_nvmf_fc_caller_cb)(void *hwqp, int32_t status, void *args);

struct spdk_nvmf_fc_caller_ctx {
	void *ctx;
	spdk_nvmf_fc_caller_cb cb;
	void *cb_args;
	TAILQ_ENTRY(spdk_nvmf_fc_caller_ctx) link;
};

/*
 * Low level FC driver function table (functions provided by vendor FC device driver)
 */
struct spdk_nvmf_fc_ll_drvr_ops {

	/* initialize the low level driver */
	int (*lld_init)(void);

	/* low level driver finish */
	void (*lld_fini)(void);

	/* initialize hw queues */
	int (*init_q)(struct spdk_nvmf_fc_hwqp *hwqp);

	void (*reinit_q)(spdk_nvmf_fc_lld_hwqp_t queues_prev,
			 spdk_nvmf_fc_lld_hwqp_t queues_curr);

	/* initialize hw queue buffers */
	int (*init_q_buffers)(struct spdk_nvmf_fc_hwqp *hwqp);

	/* poll the hw queues for requests */
	uint32_t (*poll_queue)(struct spdk_nvmf_fc_hwqp *hwqp);

	/* receive data (for data-in requests) */
	int (*recv_data)(struct spdk_nvmf_fc_request *fc_req);

	/* send data (for data-out requests) */
	int (*send_data)(struct spdk_nvmf_fc_request *fc_req);

	/* release hw queust buffer */
	void (*q_buffer_release)(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t buff_idx);

	/* transmist nvme response */
	int (*xmt_rsp)(struct spdk_nvmf_fc_request *fc_req, uint8_t *ersp_buf, uint32_t ersp_len);

	/* transmist LS response */
	int (*xmt_ls_rsp)(struct spdk_nvmf_fc_nport *tgtport, struct spdk_nvmf_fc_ls_rqst *ls_rqst);

	/* issue abts */
	int (*issue_abort)(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xri *xri,
			   bool send_abts, spdk_nvmf_fc_caller_cb cb, void *cb_args);

	/* transmit abts response */
	int (*xmt_bls_rsp)(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t ox_id, uint16_t rx_id, uint16_t rpi,
			   bool rjt, uint8_t rjt_exp, spdk_nvmf_fc_caller_cb cb, void *cb_args);

	/* transmit single request - single response */
	int (*xmt_srsr_req)(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_send_srsr *srsr,
			    spdk_nvmf_fc_caller_cb cb, void *cb_args);

	/* issue queue marker (abts processing) */
	int (*issue_q_marker)(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t u_id, uint16_t skip_rq);

	/* assign a new connection to a hwqp (return connection ID) */
	struct spdk_nvmf_fc_hwqp *(*assign_conn_to_hwqp)(
		struct spdk_nvmf_fc_hwqp *queues, uint32_t num_queues,
		uint64_t *conn_id, uint32_t sq_size, bool for_aq);

	/* get the hwqp from the given connection id */
	struct spdk_nvmf_fc_hwqp *(*get_hwqp_from_conn_id)(struct spdk_nvmf_fc_hwqp *hwqp,
			uint32_t num_queues, uint64_t conn_id);

	/* release connection ID (done with using it) */
	void (*release_conn)(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t conn_id, uint32_t sq_size);

	/* dump all queue info into dump_info */
	void (*dump_all_queues)(struct spdk_nvmf_fc_hwqp *ls_queues,
				struct spdk_nvmf_fc_hwqp *io_queues,
				uint32_t num_queues,
				struct spdk_nvmf_fc_queue_dump_info *dump_info);
};

extern struct spdk_nvmf_fc_ll_drvr_ops spdk_nvmf_fc_lld_ops;

/*
 * NVMF FC inline and function prototypes
 */

static inline struct spdk_nvmf_fc_request *
spdk_nvmf_fc_get_fc_req(struct spdk_nvmf_request *req)
{
	return (struct spdk_nvmf_fc_request *)
	       ((uintptr_t)req - offsetof(struct spdk_nvmf_fc_request, req));
}

static inline bool
spdk_nvmf_fc_is_port_dead(struct spdk_nvmf_fc_hwqp *hwqp)
{
	switch (hwqp->fc_port->hw_port_status) {
	case SPDK_FC_PORT_QUIESCED:
		return true;
	default:
		return false;
	}
}

static inline bool
spdk_nvmf_fc_req_in_xfer(struct spdk_nvmf_fc_request *fc_req)
{
	switch (fc_req->state) {
	case SPDK_NVMF_FC_REQ_READ_XFER:
	case SPDK_NVMF_FC_REQ_READ_RSP:
	case SPDK_NVMF_FC_REQ_WRITE_XFER:
	case SPDK_NVMF_FC_REQ_WRITE_RSP:
	case SPDK_NVMF_FC_REQ_NONE_RSP:
		return true;
	default:
		return false;
	}
}

typedef void (*spdk_nvmf_fc_del_assoc_cb)(void *arg, uint32_t err);
int spdk_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
				    uint64_t assoc_id, bool send_abts,
				    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				    void *cb_data);

void spdk_nvmf_fc_ls_init(struct spdk_nvmf_fc_port *fc_port);

void spdk_nvmf_fc_ls_fini(struct spdk_nvmf_fc_port *fc_port);

struct spdk_nvmf_fc_port *spdk_nvmf_fc_port_list_get(uint8_t port_hdl);

int spdk_nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
				 enum spdk_nvmf_fc_object_state state);

int spdk_nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
				 enum spdk_nvmf_fc_object_state state);

bool spdk_nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
				     struct spdk_nvmf_fc_remote_port_info *rem_port);

bool spdk_nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
					struct spdk_nvmf_fc_remote_port_info *rem_port);

void spdk_nvmf_fc_init_poller_queues(struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_reinit_poller_queues(struct spdk_nvmf_fc_hwqp *hwqp,
				       void *queues_curr);

void spdk_nvmf_fc_init_poller(struct spdk_nvmf_fc_port *fc_port,
			      struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_add_hwqp_to_poller(struct spdk_nvmf_fc_hwqp *hwqp, bool admin_q);

void spdk_nvmf_fc_remove_hwqp_from_poller(struct spdk_nvmf_fc_hwqp *hwqp);

bool spdk_nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port);

bool spdk_nvmf_fc_port_is_online(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_hwqp_port_set_online(struct spdk_nvmf_fc_hwqp *hwqp);

int spdk_nvmf_fc_hwqp_port_set_offline(struct spdk_nvmf_fc_hwqp *hwqp);

int spdk_nvmf_fc_rport_set_state(struct spdk_nvmf_fc_remote_port_info *rport,
				 enum spdk_nvmf_fc_object_state state);

void spdk_nvmf_fc_port_list_add(struct spdk_nvmf_fc_port *fc_port);

struct spdk_nvmf_fc_nport *spdk_nvmf_fc_nport_get(uint8_t port_hdl, uint16_t nport_hdl);

int spdk_nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
				struct spdk_nvmf_fc_nport *nport);

uint32_t spdk_nvmf_fc_nport_get_association_count(struct spdk_nvmf_fc_nport *nport);

int spdk_nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
				   struct spdk_nvmf_fc_nport *nport);

uint32_t spdk_nvmf_fc_get_prli_service_params(void);

bool spdk_nvmf_fc_nport_is_rport_empty(struct spdk_nvmf_fc_nport *nport);

void spdk_nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport,
				    uint16_t rpi, uint16_t oxid,
				    uint16_t rxid);

void spdk_nvmf_fc_dump_all_queues(struct spdk_nvmf_fc_port *fc_port,
				  struct spdk_nvmf_fc_queue_dump_info *dump_info);

void spdk_nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_ls_rqst *ls_rqst);

int spdk_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst);

struct spdk_nvmf_fc_nport *spdk_nvmf_bcm_req_fc_nport_get(struct spdk_nvmf_request *req);

struct spdk_nvmf_fc_association *spdk_nvmf_fc_get_ctrlr_assoc(struct spdk_nvmf_ctrlr *ctrlr);

bool spdk_nvmf_fc_nport_is_association_empty(struct spdk_nvmf_fc_nport *nport);

int spdk_nvmf_fc_xmt_srsr_req(struct spdk_nvmf_fc_hwqp *hwqp,
			      struct spdk_nvmf_fc_send_srsr *srsr,
			      spdk_nvmf_fc_caller_cb cb, void *cb_args);

uint32_t spdk_nvmf_fc_get_num_nport_ctrlrs_in_subsystem(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_subsystem *subsys);

bool spdk_nvmf_fc_is_spdk_ctrlr_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_ctrlr *ctrlr);

int spdk_nvmf_fc_get_ctrlr_init_traddr(char *traddr, struct spdk_nvmf_ctrlr *ctrlr);

uint32_t spdk_nvmf_fc_get_hwqp_id(struct spdk_nvmf_request *req);

void spdk_nvmf_fc_req_abort(struct spdk_nvmf_fc_request *fc_req,
			    bool send_abts, spdk_nvmf_fc_caller_cb cb,
			    void *cb_args);

int spdk_nvmf_fc_add_port_listen(void *arg1, void *arg2);

int spdk_nvmf_fc_remove_port_listen(void *arg1, void *arg2);

void spdk_nvmf_fc_subsys_connect_cb(void *cb_ctx,
				    struct spdk_nvmf_request *req);

void spdk_nvmf_fc_subsys_disconnect_cb(void *cb_ctx,
				       struct spdk_nvmf_qpair *qpair);

uint32_t spdk_nvmf_fc_get_master_lcore(void);

struct spdk_thread *spdk_nvmf_fc_get_master_thread(void);

/*
 * These functions are used by low level FC driver
 */

static inline struct spdk_nvmf_fc_conn *
spdk_nvmf_fc_get_conn(struct spdk_nvmf_qpair *qpair)
{
	return (struct spdk_nvmf_fc_conn *)
	       ((uintptr_t)qpair - offsetof(struct spdk_nvmf_fc_conn, qpair));
}

static inline uint16_t
spdk_nvmf_fc_advance_conn_sqhead(struct spdk_nvmf_qpair *qpair)
{
	/* advance sq_head pointer - wrap if needed */
	qpair->sq_head = (qpair->sq_head == qpair->sq_head_max) ?
			 0 : (qpair->sq_head + 1);
	return qpair->sq_head;
}

static inline bool
spdk_nvmf_fc_use_send_frame(struct spdk_nvmf_request *req)
{
	/* For now use for only keepalives. */
	if (req->qpair->qid == 0 &&
	    (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_KEEP_ALIVE)) {
		return true;
	}
	return false;
}

enum spdk_nvmf_fc_poller_api_ret spdk_nvmf_fc_poller_api_func(
	struct spdk_nvmf_fc_hwqp *hwqp,
	enum spdk_nvmf_fc_poller_api api,
	void *api_args);

int spdk_nvmf_fc_process_frame(struct spdk_nvmf_fc_hwqp *hwqp, uint32_t buff_idx,
			       struct spdk_nvmf_fc_frame_hdr *frame,
			       struct spdk_nvmf_fc_buffer_desc *buffer, uint32_t plen);

void spdk_nvmf_fc_process_pending_req(struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_process_pending_ls_rqst(struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_req_set_state(struct spdk_nvmf_fc_request *fc_req,
				enum spdk_nvmf_fc_request_state state);

void spdk_nvmf_fc_free_req(struct spdk_nvmf_fc_request *fc_req);

void spdk_nvmf_fc_req_abort_complete(void *arg1);

bool spdk_nvmf_fc_send_ersp_required(struct spdk_nvmf_fc_request *fc_req,
				     uint32_t rsp_cnt, uint32_t xfer_len);

struct spdk_nvmf_fc_xri *spdk_nvmf_fc_get_xri(struct spdk_nvmf_fc_hwqp *hwqp);

int spdk_nvmf_fc_put_xri(struct spdk_nvmf_fc_hwqp *hwqp,
			 struct spdk_nvmf_fc_xri *xri);

void spdk_nvmf_fc_release_xri(struct spdk_nvmf_fc_hwqp *hwqp,
			      struct spdk_nvmf_fc_xri *xri, bool xb, bool abts);

int spdk_nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *req);
#endif
