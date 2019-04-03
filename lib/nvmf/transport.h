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

#ifndef SPDK_NVMF_TRANSPORT_H
#define SPDK_NVMF_TRANSPORT_H

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/nvmf.h"

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
		      const struct spdk_nvme_transport_id *trid);

	/**
	  * Stop accepting new connections at the given address.
	  */
	int (*stop_listen)(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid);

	/**
	 * Check for new connections on the transport.
	 */
	void (*accept)(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn);

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
	 * set the submission queue size of the queue pair
	 */
	int (*qpair_set_sqsize)(struct spdk_nvmf_qpair *qpair);
};


int spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				    const struct spdk_nvme_transport_id *trid);

void spdk_nvmf_transport_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn);

void spdk_nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
		struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_discovery_log_page_entry *entry);

struct spdk_nvmf_transport_poll_group *spdk_nvmf_transport_poll_group_create(
	struct spdk_nvmf_transport *transport, struct spdk_nvmf_poll_group *poll_group);

void spdk_nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

int spdk_nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
				       struct spdk_nvmf_qpair *qpair);

int spdk_nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_qpair *qpair);

int spdk_nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

int spdk_nvmf_transport_req_free(struct spdk_nvmf_request *req);

int spdk_nvmf_transport_req_complete(struct spdk_nvmf_request *req);

void spdk_nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair);

int spdk_nvmf_transport_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid);

int spdk_nvmf_transport_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid);

int spdk_nvmf_transport_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid);
int spdk_nvmf_transport_qpair_set_sqsize(struct spdk_nvmf_qpair *qpair);

bool spdk_nvmf_transport_opts_init(enum spdk_nvme_transport_type type,
				   struct spdk_nvmf_transport_opts *opts);

extern const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma;
extern const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp;

extern const struct spdk_nvmf_transport_ops spdk_nvmf_transport_fc;

#endif /* SPDK_NVMF_TRANSPORT_H */
