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

struct spdk_nvmf_listen_addr;

struct spdk_nvmf_transport {
	struct spdk_nvmf_tgt			*tgt;
	const struct spdk_nvmf_transport_ops	*ops;

	TAILQ_ENTRY(spdk_nvmf_transport)	link;
};

struct spdk_nvmf_transport_ops {
	/**
	 * Transport type
	 */
	enum spdk_nvme_transport_type type;

	/**
	 * Create a transport for the given target
	 */
	struct spdk_nvmf_transport *(*create)(struct spdk_nvmf_tgt *tgt);

	/**
	 * Destroy the transport
	 */
	int (*destroy)(struct spdk_nvmf_transport *transport);

	/**
	 * Check for new connections on the transport.
	 */
	void (*acceptor_poll)(struct spdk_nvmf_transport *transport);

	/**
	  * Instruct the acceptor to listen on the address provided. This
	  * may be called multiple times.
	  */
	int (*listen_addr_add)(struct spdk_nvmf_transport *transport,
			       struct spdk_nvmf_listen_addr *listen_addr);

	/**
	  * Instruct to remove listening on the address provided. This
	  * may be called multiple times.
	  */
	int (*listen_addr_remove)(struct spdk_nvmf_transport *transport,
				  struct spdk_nvmf_listen_addr *listen_addr);

	/**
	 * Fill out a discovery log entry for a specific listen address.
	 */
	void (*listen_addr_discover)(struct spdk_nvmf_transport *transport,
				     struct spdk_nvmf_listen_addr *listen_addr,
				     struct spdk_nvmf_discovery_log_page_entry *entry);

	/**
	 * Create a new ctrlr
	 */
	struct spdk_nvmf_ctrlr *(*ctrlr_init)(struct spdk_nvmf_transport *transport);

	/**
	 * Destroy a ctrlr
	 */
	void (*ctrlr_fini)(struct spdk_nvmf_ctrlr *ctrlr);

	/**
	 * Add a qpair to a ctrlr
	 */
	int (*ctrlr_add_qpair)(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvmf_qpair *qpair);

	/**
	 * Remove a qpair from a ctrlr
	 */
	int (*ctrlr_remove_qpair)(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvmf_qpair *qpair);

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
	 * Poll a connection for events.
	 */
	int (*qpair_poll)(struct spdk_nvmf_qpair *qpair);

	/*
	 * True if the qpair has no pending IO.
	 */
	bool (*qpair_is_idle)(struct spdk_nvmf_qpair *qpair);
};

struct spdk_nvmf_transport *spdk_nvmf_transport_create(struct spdk_nvmf_tgt *tgt,
		enum spdk_nvme_transport_type type);
int spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport);

void spdk_nvmf_transport_poll(struct spdk_nvmf_transport *transport);

extern const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma;

#endif /* SPDK_NVMF_TRANSPORT_H */
