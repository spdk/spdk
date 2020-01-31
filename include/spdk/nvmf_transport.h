/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

/** \file
 * NVMe-oF Target transport plugin API
 */

#ifndef SPDK_NVMF_TRANSPORT_H_
#define SPDK_NVMF_TRANSPORT_H_

#include "spdk/bdev.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"

#define SPDK_NVMF_MAX_SGL_ENTRIES	16

/* The maximum number of buffers per request */
#define NVMF_REQ_MAX_BUFFERS	(SPDK_NVMF_MAX_SGL_ENTRIES * 2)

/* AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define SHIFT_4KB			12u
#define NVMF_DATA_BUFFER_ALIGNMENT	(1u << SHIFT_4KB)
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1LL)

typedef void (*spdk_nvmf_nvme_passthru_cmd_cb)(struct spdk_nvmf_request *req);

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

struct spdk_nvmf_dif_info {
	struct spdk_dif_ctx			dif_ctx;
	bool					dif_insert_or_strip;
	uint32_t				elba_length;
	uint32_t				orig_length;
};

struct spdk_nvmf_request {
	struct spdk_nvmf_qpair		*qpair;
	uint32_t			length;
	enum spdk_nvme_data_transfer	xfer;
	void				*data;
	union nvmf_h2c_msg		*cmd;
	union nvmf_c2h_msg		*rsp;
	void				*buffers[NVMF_REQ_MAX_BUFFERS];
	struct iovec			iov[NVMF_REQ_MAX_BUFFERS];
	uint32_t			iovcnt;
	bool				data_from_pool;
	struct spdk_bdev_io_wait_entry	bdev_io_wait;
	struct spdk_nvmf_dif_info	dif;
	spdk_nvmf_nvme_passthru_cmd_cb	cmd_cb_fn;
	struct spdk_nvmf_request	*first_fused_req;

	STAILQ_ENTRY(spdk_nvmf_request)	buf_link;
	TAILQ_ENTRY(spdk_nvmf_request)	link;
};

enum spdk_nvmf_qpair_state {
	SPDK_NVMF_QPAIR_UNINITIALIZED = 0,
	SPDK_NVMF_QPAIR_ACTIVE,
	SPDK_NVMF_QPAIR_DEACTIVATING,
	SPDK_NVMF_QPAIR_ERROR,
};

typedef void (*spdk_nvmf_state_change_done)(void *cb_arg, int status);

struct spdk_nvmf_qpair {
	enum spdk_nvmf_qpair_state		state;
	spdk_nvmf_state_change_done		state_cb;
	void					*state_cb_arg;

	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_ctrlr			*ctrlr;
	struct spdk_nvmf_poll_group		*group;

	uint16_t				qid;
	uint16_t				sq_head;
	uint16_t				sq_head_max;

	struct spdk_nvmf_request		*first_fused_req;

	TAILQ_HEAD(, spdk_nvmf_request)		outstanding;
	TAILQ_ENTRY(spdk_nvmf_qpair)		link;
};

struct spdk_nvmf_transport_pg_cache_buf {
	STAILQ_ENTRY(spdk_nvmf_transport_pg_cache_buf) link;
};

struct spdk_nvmf_transport_poll_group {
	struct spdk_nvmf_transport					*transport;
	/* Requests that are waiting to obtain a data buffer */
	STAILQ_HEAD(, spdk_nvmf_request)				pending_buf_queue;
	STAILQ_HEAD(, spdk_nvmf_transport_pg_cache_buf)			buf_cache;
	uint32_t							buf_cache_count;
	uint32_t							buf_cache_size;
	struct spdk_nvmf_poll_group					*group;
	TAILQ_ENTRY(spdk_nvmf_transport_poll_group)			link;
};

struct spdk_nvmf_poll_group {
	struct spdk_thread				*thread;
	struct spdk_poller				*poller;

	TAILQ_HEAD(, spdk_nvmf_transport_poll_group)	tgroups;

	/* Array of poll groups indexed by subsystem id (sid) */
	struct spdk_nvmf_subsystem_poll_group		*sgroups;
	uint32_t					num_sgroups;

	/* All of the queue pairs that belong to this poll group */
	TAILQ_HEAD(, spdk_nvmf_qpair)			qpairs;

	/* Statistics */
	struct spdk_nvmf_poll_group_stat		stat;
};

struct spdk_nvmf_transport {
	struct spdk_nvmf_tgt			*tgt;
	const struct spdk_nvmf_transport_ops	*ops;
	struct spdk_nvmf_transport_opts		opts;

	/* A mempool for transport related data transfers */
	struct spdk_mempool			*data_buf_pool;

	TAILQ_ENTRY(spdk_nvmf_transport)	link;
};

struct spdk_nvmf_transport_ops {
	/**
	 * Transport name
	 */
	char name[SPDK_NVMF_TRSTRING_MAX_LEN];

	/**
	 * Transport type
	 */
	enum spdk_nvme_transport_type type;

	/**
	 * Initialize transport options to default value
	 */
	void (*opts_init)(struct spdk_nvmf_transport_opts *opts);

	/**
	 * Create a transport for the given transport opts
	 */
	struct spdk_nvmf_transport *(*create)(struct spdk_nvmf_transport_opts *opts);

	/**
	 * Destroy the transport
	 */
	int (*destroy)(struct spdk_nvmf_transport *transport);

	/**
	  * Instruct the transport to accept new connections at the address
	  * provided. This may be called multiple times.
	  */
	int (*listen)(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid,
		      spdk_nvmf_tgt_listen_done_fn cb_fn,
		      void *cb_arg);

	/**
	  * Stop accepting new connections at the given address.
	  */
	int (*stop_listen)(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid);

	/**
	 * Check for new connections on the transport.
	 */
	void (*accept)(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn, void *cb_arg);

	/**
	 * Fill out a discovery log entry for a specific listen address.
	 */
	void (*listener_discover)(struct spdk_nvmf_transport *transport,
				  struct spdk_nvme_transport_id *trid,
				  struct spdk_nvmf_discovery_log_page_entry *entry);

	/**
	 * Create a new poll group
	 */
	struct spdk_nvmf_transport_poll_group *(*poll_group_create)(struct spdk_nvmf_transport *transport);

	/**
	 * Get the polling group of the queue pair optimal for the specific transport
	 */
	struct spdk_nvmf_transport_poll_group *(*get_optimal_poll_group)(struct spdk_nvmf_qpair *qpair);

	/**
	 * Destroy a poll group
	 */
	void (*poll_group_destroy)(struct spdk_nvmf_transport_poll_group *group);

	/**
	 * Add a qpair to a poll group
	 */
	int (*poll_group_add)(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair);

	/**
	 * Remove a qpair from a poll group
	 */
	int (*poll_group_remove)(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair);

	/**
	 * Poll the group to process I/O
	 */
	int (*poll_group_poll)(struct spdk_nvmf_transport_poll_group *group);

	/*
	 * Free the request without sending a response
	 * to the originator. Release memory tied to this request.
	 */
	int (*req_free)(struct spdk_nvmf_request *req);

	/*
	 * Signal request completion, which sends a response
	 * to the originator.
	 */
	int (*req_complete)(struct spdk_nvmf_request *req);

	/*
	 * Deinitialize a connection.
	 */
	void (*qpair_fini)(struct spdk_nvmf_qpair *qpair);

	/*
	 * Get the peer transport ID for the queue pair.
	 */
	int (*qpair_get_peer_trid)(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid);

	/*
	 * Get the local transport ID for the queue pair.
	 */
	int (*qpair_get_local_trid)(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid);

	/*
	 * Get the listener transport ID that accepted this qpair originally.
	 */
	int (*qpair_get_listen_trid)(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid);

	/*
	 * Get transport poll group statistics
	 */
	int (*poll_group_get_stat)(struct spdk_nvmf_tgt *tgt,
				   struct spdk_nvmf_transport_poll_group_stat **stat);

	/*
	 * Free transport poll group statistics previously allocated with poll_group_get_stat()
	 */
	void (*poll_group_free_stat)(struct spdk_nvmf_transport_poll_group_stat *stat);
};

/**
 * Register the operations for a given transport type.
 *
 * This function should be invoked by referencing the macro
 * SPDK_NVMF_TRANSPORT_REGISTER macro in the transport's .c file.
 *
 * \param ops The operations associated with an NVMe-oF transport.
 */
void spdk_nvmf_transport_register(const struct spdk_nvmf_transport_ops *ops);

void spdk_nvmf_request_free_buffers(struct spdk_nvmf_request *req,
				    struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_nvmf_transport *transport);
int spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
				  struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_transport *transport,
				  uint32_t length);
int spdk_nvmf_request_get_buffers_multi(struct spdk_nvmf_request *req,
					struct spdk_nvmf_transport_poll_group *group,
					struct spdk_nvmf_transport *transport,
					uint32_t *lengths, uint32_t num_lengths);

bool spdk_nvmf_request_get_dif_ctx(struct spdk_nvmf_request *req, struct spdk_dif_ctx *dif_ctx);

void spdk_nvmf_request_exec(struct spdk_nvmf_request *req);
void spdk_nvmf_request_exec_fabrics(struct spdk_nvmf_request *req);
int spdk_nvmf_request_free(struct spdk_nvmf_request *req);
int spdk_nvmf_request_complete(struct spdk_nvmf_request *req);

static inline enum spdk_nvme_data_transfer
spdk_nvmf_req_get_xfer(struct spdk_nvmf_request *req) {
	enum spdk_nvme_data_transfer xfer;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC)
	{
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd->nvmf_cmd.fctype);
	} else
	{
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	}

	if (xfer == SPDK_NVME_DATA_NONE)
	{
		return xfer;
	}

	/* Even for commands that may transfer data, they could have specified 0 length.
	 * We want those to show up with xfer SPDK_NVME_DATA_NONE.
	 */
	switch (sgl->generic.type)
	{
	case SPDK_NVME_SGL_TYPE_DATA_BLOCK:
	case SPDK_NVME_SGL_TYPE_BIT_BUCKET:
	case SPDK_NVME_SGL_TYPE_SEGMENT:
	case SPDK_NVME_SGL_TYPE_LAST_SEGMENT:
	case SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK:
		if (sgl->unkeyed.length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	case SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK:
		if (sgl->keyed.length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	}

	return xfer;
}

/*
 * Macro used to register new transports.
 */
#define SPDK_NVMF_TRANSPORT_REGISTER(name, transport_ops) \
static void __attribute__((constructor)) spdk_nvmf_transport_register_##name(void) \
{ \
	spdk_nvmf_transport_register(transport_ops); \
}\

#endif
