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

#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/assert.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_fc_spec.h"
#include "spdk/event.h"
#include "spdk/io_channel.h"
#include "nvmf_internal.h"
#include "spdk/fc_adm_api.h"

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
 * NVMF FC Object state
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
	SPDK_NVMF_FC_REQ_BDEV_ABORTED,
	SPDK_NVMF_FC_REQ_PENDING,
	SPDK_NVMF_FC_REQ_MAX_STATE,
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
	uint32_t no_xchg;
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
	uint32_t buf_alloc_err;
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
struct spdk_nvmf_fc_srsr_bufs {
	void *rqst;
	size_t rqst_len;
	void *rsp;
	size_t rsp_len;
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
	struct spdk_nvme_transport_id trid;

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

	/* requests that are waiting to obtain xchg/buffer */
	TAILQ_HEAD(, spdk_nvmf_fc_request) pending_queue;

	struct spdk_nvmf_fc_association *fc_assoc;

	uint16_t rpi;

	/* for association's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_link;

	/* for assocations's available connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_avail_link;

	/* for hwqp's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) link;
};

/*
 * Structure for maintaining the FC exchanges
 */
struct spdk_nvmf_fc_xchg {
	uint32_t xchg_id;   /* The actual xchg identifier */

	/* Internal */
	TAILQ_ENTRY(spdk_nvmf_fc_xchg) link;
	bool active;
	bool aborted;
	bool send_abts; /* Valid if is_aborted is set. */
};

/*
 *  FC poll group structure
 */
struct spdk_nvmf_fc_poll_group {
	struct spdk_nvmf_transport_poll_group tp_poll_group;
	struct spdk_nvmf_poll_group *poll_group;
	struct spdk_nvmf_tgt *nvmf_tgt;
	struct spdk_nvmf_fc_transport *fc_transport;
	uint32_t hwqp_count; /* number of hwqp's assigned to this pg */
	TAILQ_HEAD(, spdk_nvmf_fc_hwqp) hwqp_list;

	TAILQ_ENTRY(spdk_nvmf_fc_poll_group) link;
};

/*
 *  HWQP poller structure passed from Master thread
 */
struct spdk_nvmf_fc_hwqp {
	bool nvme_aq;        /* hwqp is being used for nvme admin queue */
	enum spdk_fc_hwqp_state state;  /* queue state (for poller) */
	uint32_t lcore_id;   /* core hwqp is running on (for tracing purposes only) */
	struct spdk_thread *thread;  /* thread hwqp is running on */
	uint32_t hwqp_id;    /* A unique id (per physical port) for a hwqp */
	uint32_t rq_size;    /* receive queue size */
	spdk_nvmf_fc_lld_hwqp_t queues;    /* vendor HW queue set */
	struct spdk_nvmf_fc_port *fc_port; /* HW port structure for these queues */
	struct spdk_nvmf_fc_poll_group *fc_poll_group;

	/* qpair (fc_connection) list */
	TAILQ_HEAD(, spdk_nvmf_fc_conn) connection_list;
	uint32_t num_conns; /* number of connections to queue */

	struct spdk_mempool *fc_request_pool;
	TAILQ_HEAD(, spdk_nvmf_fc_request) in_use_reqs;

	struct spdk_nvmf_fc_errors counters;

	/* Pending LS request waiting for FC resource */
	TAILQ_HEAD(, spdk_nvmf_fc_ls_rqst) ls_pending_queue;

	/* Sync req list */
	TAILQ_HEAD(, spdk_nvmf_fc_poller_api_queue_sync_args) sync_cbs;

	TAILQ_ENTRY(spdk_nvmf_fc_hwqp) link;

	void *context;			/* Vendor specific context data */
};

/*
 * FC HW port.
 */
struct spdk_nvmf_fc_port {
	uint8_t port_hdl;
	enum spdk_fc_port_state hw_port_status;
	uint16_t fcp_rq_id;
	struct spdk_nvmf_fc_hwqp ls_queue;
	uint32_t num_io_queues;
	struct spdk_nvmf_fc_hwqp *io_queues;
	/*
	 * List of nports on this HW port.
	 */
	TAILQ_HEAD(, spdk_nvmf_fc_nport)nport_list;
	int	num_nports;
	TAILQ_ENTRY(spdk_nvmf_fc_port) link;

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
	struct spdk_nvmf_fc_xchg *xchg;
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
	void *buffers[SPDK_NVMF_MAX_SGL_ENTRIES];
	bool data_from_pool;
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

	/* disconnect cmd buffers(sent to initiator) */
	struct spdk_nvmf_fc_srsr_bufs *snd_disconn_bufs;
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
	SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
	SPDK_NVMF_FC_POLLER_API_ADAPTER_EVENT,
	SPDK_NVMF_FC_POLLER_API_AEN,
	SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC,
	SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE,
	SPDK_NVMF_FC_ADD_HWQP_TO_POLLER,
	SPDK_NVMF_FC_REMOVE_HWQP_FROM_POLLER,
};

/*
 * Poller API callback function proto
 */
typedef void (*spdk_nvmf_fc_poller_api_cb)(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret);

/*
 * Poller API callback data
 */
struct spdk_nvmf_fc_poller_api_cb_info {
	struct spdk_thread *cb_thread;
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
	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
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
	struct spdk_nvmf_fc_xchg *xchg;
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
 * NVMF FC Exchange Info (for debug)
 */
struct spdk_nvmf_fc_xchg_info {
	uint32_t xchg_base;
	uint32_t xchg_total_count;
	uint32_t xchg_avail_count;
	uint32_t send_frame_xchg_id;
	uint8_t send_frame_seqid;
};

/*
 * Low level FC driver function table (functions provided by vendor FC device driver)
 */
struct spdk_nvmf_fc_ll_drvr_ops {

	/* initialize the low level driver */
	int (*lld_init)(void);

	/* low level driver start(up) */
	void (*lld_start)(void);

	/* low level driver finish */
	void (*lld_fini)(void);

	/* initialize hw queues */
	int (*init_q)(struct spdk_nvmf_fc_hwqp *hwqp);

	void (*reinit_q)(spdk_nvmf_fc_lld_hwqp_t queues_prev,
			 spdk_nvmf_fc_lld_hwqp_t queues_curr);

	/* initialize hw queue buffers */
	int (*init_q_buffers)(struct spdk_nvmf_fc_hwqp *hwqp);

	/* set hwq online state */
	int (*set_q_online_state)(struct spdk_nvmf_fc_hwqp *hwqp, bool online);

	/* get fc exchange */
	struct spdk_nvmf_fc_xchg *(*get_xchg)(struct spdk_nvmf_fc_hwqp *hwqp);

	/* put fc exchange */
	int (*put_xchg)(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xchg);

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

	/* abort exchange in hba */
	int (*issue_abort)(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xchg,
			   spdk_nvmf_fc_caller_cb cb, void *cb_args);

	/* transmit abts response */
	int (*xmt_bls_rsp)(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t ox_id, uint16_t rx_id, uint16_t rpi,
			   bool rjt, uint8_t rjt_exp, spdk_nvmf_fc_caller_cb cb, void *cb_args);

	/* allocate srsr buffers to send */
	struct spdk_nvmf_fc_srsr_bufs *(*alloc_srsr_bufs)(size_t rqst_len, size_t rsp_len);

	/* free srsr buffers */
	void (*free_srsr_bufs)(struct spdk_nvmf_fc_srsr_bufs *disconnect_bufs);

	/* transmit single request - single response */
	int (*xmt_srsr_req)(struct spdk_nvmf_fc_hwqp *hwqp,
			    struct spdk_nvmf_fc_srsr_bufs *srsr_bufs,
			    spdk_nvmf_fc_caller_cb cb, void *cb_args);

	/* determine if queue sync is available (abts processing) */
	bool (*q_sync_available)(void);

	/* issue queue sync (abts processing) */
	int (*issue_q_sync)(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t u_id, uint16_t skip_rq);

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

	/* get the exchange info for the hwwp */
	void (*get_xchg_info)(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg_info *info);

	/* get thread reserved by LLD for other work (e.g. SCSI) */
	struct spdk_thread *(*get_rsvd_thread)(void);
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

static inline void
spdk_nvmf_fc_create_trid(struct spdk_nvme_transport_id *trid, uint64_t n_wwn, uint64_t p_wwn)
{
	trid->trtype = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC;
	trid->adrfam = SPDK_NVMF_ADRFAM_FC;
	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "none");
	snprintf(trid->traddr, sizeof(trid->traddr), "nn-0x%lx:pn-0x%lx", n_wwn, p_wwn);
}

void spdk_nvmf_fc_ls_init(struct spdk_nvmf_fc_port *fc_port);

void spdk_nvmf_fc_ls_fini(struct spdk_nvmf_fc_port *fc_port);

void spdk_nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_ls_rqst *ls_rqst);

int spdk_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst);

void spdk_nvmf_fc_init_hwqp(struct spdk_nvmf_fc_port *fc_port, struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_init_poller_queues(struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_reinit_poller_queues(struct spdk_nvmf_fc_hwqp *hwqp, void *queues_curr);

struct spdk_nvmf_fc_port *spdk_nvmf_fc_port_list_get(uint8_t port_hdl);

bool spdk_nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port);

bool spdk_nvmf_fc_port_is_online(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_rport_set_state(struct spdk_nvmf_fc_remote_port_info *rport,
				 enum spdk_nvmf_fc_object_state state);

void spdk_nvmf_fc_port_list_add(struct spdk_nvmf_fc_port *fc_port);

int spdk_nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
				struct spdk_nvmf_fc_nport *nport);

int spdk_nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
				   struct spdk_nvmf_fc_nport *nport);

struct spdk_nvmf_fc_nport *spdk_nvmf_fc_nport_get(uint8_t port_hdl, uint16_t nport_hdl);

int spdk_nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
				 enum spdk_nvmf_fc_object_state state);

bool spdk_nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
				     struct spdk_nvmf_fc_remote_port_info *rem_port);

bool spdk_nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
					struct spdk_nvmf_fc_remote_port_info *rem_port);

bool spdk_nvmf_fc_nport_is_rport_empty(struct spdk_nvmf_fc_nport *nport);

struct spdk_nvmf_fc_association *spdk_nvmf_fc_get_ctrlr_assoc(struct spdk_nvmf_ctrlr *ctrlr);

int spdk_nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
				 enum spdk_nvmf_fc_object_state state);

typedef void (*spdk_nvmf_fc_del_assoc_cb)(void *arg, uint32_t err);

int spdk_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
				    uint64_t assoc_id, bool send_abts,
				    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				    void *cb_data);

bool spdk_nvmf_fc_is_spdk_ctrlr_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_ctrlr *ctrlr);

void spdk_nvmf_fc_assign_queue_to_master_thread(struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_add_hwqp_to_poller(struct spdk_nvmf_fc_hwqp *hwqp);

void spdk_nvmf_fc_remove_hwqp_from_poller(struct spdk_nvmf_fc_hwqp *hwqp);

int spdk_nvmf_fc_hwqp_set_online(struct spdk_nvmf_fc_hwqp *hwqp);

int spdk_nvmf_fc_hwqp_set_offline(struct spdk_nvmf_fc_hwqp *hwqp);

uint32_t spdk_nvmf_fc_get_prli_service_params(void);

void spdk_nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi, uint16_t oxid,
				    uint16_t rxid);

void spdk_nvmf_fc_req_abort(struct spdk_nvmf_fc_request *fc_req, bool send_abts,
			    spdk_nvmf_fc_caller_cb cb, void *cb_args);

int spdk_nvmf_fc_xmt_srsr_req(struct spdk_nvmf_fc_hwqp *hwqp,
			      struct spdk_nvmf_fc_srsr_bufs *srsr_bufs,
			      spdk_nvmf_fc_caller_cb cb, void *cb_args);

struct spdk_nvmf_tgt *spdk_nvmf_fc_get_tgt(void);

void spdk_nvmf_fc_dump_all_queues(struct spdk_nvmf_fc_port *fc_port,
				  struct spdk_nvmf_fc_queue_dump_info *dump_info);

struct spdk_thread *spdk_nvmf_fc_get_master_thread(void);

/*
 * These functions are called by low level FC driver
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

char *spdk_nvmf_fc_req_get_state_str(int state);

void spdk_nvmf_fc_free_req(struct spdk_nvmf_fc_request *fc_req);

void spdk_nvmf_fc_req_abort_complete(void *arg1);

bool spdk_nvmf_fc_send_ersp_required(struct spdk_nvmf_fc_request *fc_req,
				     uint32_t rsp_cnt, uint32_t xfer_len);

int spdk_nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *req);
#endif
