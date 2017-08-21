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

/** \file
 * NVMe over Fabrics target public API
 */

#ifndef SPDK_NVMF_H
#define SPDK_NVMF_H

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/queue.h"

#define MAX_VIRTUAL_NAMESPACE 16
#define MAX_SN_LEN 20

int spdk_nvmf_tgt_init(uint16_t max_queue_depth, uint16_t max_qpair_per_ctrlr,
		       uint32_t in_capsule_data_size, uint32_t max_io_size);

int spdk_nvmf_tgt_fini(void);

int spdk_nvmf_check_pools(void);

struct spdk_nvmf_subsystem;
struct spdk_nvmf_ctrlr;
struct spdk_nvmf_qpair;
struct spdk_nvmf_request;
struct spdk_bdev;
struct spdk_nvmf_request;
struct spdk_nvmf_host;
struct spdk_nvmf_listener;

typedef void (*spdk_nvmf_subsystem_connect_fn)(void *cb_ctx, struct spdk_nvmf_request *req);
typedef void (*spdk_nvmf_subsystem_disconnect_fn)(void *cb_ctx, struct spdk_nvmf_qpair *qpair);

struct spdk_nvmf_listen_addr {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(spdk_nvmf_listen_addr)	link;
};

struct spdk_nvmf_ns {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	uint32_t id;
	bool allocated;
};

/*
 * The NVMf subsystem, as indicated in the specification, is a collection
 * of controllers.  Any individual controller has
 * access to all the NVMe device/namespaces maintained by the subsystem.
 */
struct spdk_nvmf_subsystem {
	uint32_t id;
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	enum spdk_nvmf_subtype subtype;
	bool is_removed;

	char sn[MAX_SN_LEN + 1];

	struct spdk_nvmf_ns			ns[MAX_VIRTUAL_NAMESPACE];
	uint32_t 				max_nsid;

	void					*cb_ctx;
	spdk_nvmf_subsystem_connect_fn		connect_cb;
	spdk_nvmf_subsystem_disconnect_fn	disconnect_cb;

	TAILQ_HEAD(, spdk_nvmf_ctrlr)		ctrlrs;

	TAILQ_HEAD(, spdk_nvmf_host)		hosts;

	TAILQ_HEAD(, spdk_nvmf_listener)	listeners;

	TAILQ_ENTRY(spdk_nvmf_subsystem)	entries;
};

struct spdk_nvmf_subsystem *spdk_nvmf_create_subsystem(const char *nqn,
		enum spdk_nvmf_subtype type,
		void *cb_ctx,
		spdk_nvmf_subsystem_connect_fn connect_cb,
		spdk_nvmf_subsystem_disconnect_fn disconnect_cb);

/**
 * Initialize the subsystem on the thread that will be used to poll it.
 *
 * \param subsystem Subsystem that will be polled on this core.
 */
int spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem);

void spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem);

struct spdk_nvmf_subsystem *spdk_nvmf_find_subsystem(const char *subnqn);

/**
 * Allow the given host NQN to connect to the given subsystem.
 *
 * \param subsystem Subsystem to add host to
 * \param host_nqn The NQN for the host
 * \return 0 on success. Negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn);

/**
 * Check if the given host is allowed to connect to the subsystem.
 *
 * \param subsystem The subsystem to query
 * \param hostnqn The NQN of the host
 * \return true if allowed, false if not.
 */
bool spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/**
 * Return the first allowed host in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \return First allowed host in this subsystem, or NULL if none allowed.
 */
struct spdk_nvmf_host *spdk_nvmf_subsystem_get_first_host(struct spdk_nvmf_subsystem *subsystem);

/**
 * Return the next allowed host in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_host Previous host returned from this function.
 * \return Next allowed host in this subsystem, or NULL if prev_host was the last host.
 */
struct spdk_nvmf_host *spdk_nvmf_subsystem_get_next_host(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_host *prev_host);

/**
 * Get a host's NQN
 *
 * \param host Host to query.
 * \return NQN of host.
 */
const char *spdk_nvmf_host_get_nqn(struct spdk_nvmf_host *host);

struct spdk_nvmf_listen_addr *spdk_nvmf_tgt_listen(struct spdk_nvme_transport_id *trid);

/**
 * Accept new connections on the address provided
 *
 * \param subsystem Subsystem to add listener to
 * \param listen_addr The address to listen on.
 * \return 0 on success. Negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvmf_listen_addr *listen_addr);

/**
 * Check if connections originated from the given address are allowed to connect to the subsystem.
 *
 * \param subsystem The subsystem to query
 * \param listen_addr The listen address
 * \return true if allowed, false if not.
 */
bool spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_listen_addr *listen_addr);

/**
 * Return the first allowed listen address in the subsystem.
 *
 * \param subsystem Subsystem to query.
 * \return First allowed listen address in this subsystem, or NULL if none allowed.
 */
struct spdk_nvmf_listener *spdk_nvmf_subsystem_get_first_listener(
	struct spdk_nvmf_subsystem *subsystem);

/**
 * Return the next allowed listen address in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_listener Previous listen address for this subsystem
 * \return Next allowed listen address in this subsystem, or NULL if prev_listener was the last address.
 */
struct spdk_nvmf_listener *spdk_nvmf_subsystem_get_next_listener(
	struct spdk_nvmf_subsystem *subsystem,
	struct spdk_nvmf_listener *prev_listener);

/**
 * Get a listen address' transport ID
 *
 * \param listener This listener
 * \return The transport ID for this listener
 */
const struct spdk_nvme_transport_id *spdk_nvmf_listener_get_trid(
	struct spdk_nvmf_listener *listener);

void spdk_nvmf_subsystem_poll(struct spdk_nvmf_subsystem *subsystem);

/**
 * Add a namespace to a subsytem.
 *
 * \param subsystem Subsystem to add namespace to.
 * \param bdev Block device to add as a namespace.
 * \param nsid Namespace ID to assign to the new namespace, or 0 to automatically use an available
 *             NSID.
 *
 * \return Newly added NSID on success or 0 on failure.
 */
uint32_t spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
				    uint32_t nsid);

/**
 * Return the first allocated namespace in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \return First allocated namespace in this subsystem, or NULL if this subsystem has no namespaces.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem);

/**
 * Return the next allocated namespace in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_ns Previous ns returned from this function.
 * \return Next allocated namespace in this subsystem, or NULL if prev_ns was the last namespace.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_ns *prev_ns);

/**
 * Get a namespace in a subsystem by NSID.
 *
 * \param subsystem Subsystem to search.
 * \param nsid Namespace ID to find.
 * \return Namespace matching nsid, or NULL if nsid was not found.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid);

/**
 * Get a namespace's NSID.
 *
 * \param ns Namespace to query.
 * \return NSID of ns.
 */
uint32_t spdk_nvmf_ns_get_id(const struct spdk_nvmf_ns *ns);

/**
 * Get a namespace's associated bdev.
 *
 * \param ns Namespace to query
 * \return Backing bdev of ns.
 */
struct spdk_bdev *spdk_nvmf_ns_get_bdev(struct spdk_nvmf_ns *ns);

const char *spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem);

int spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn);

const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem);
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);

void spdk_nvmf_tgt_poll(void);

void spdk_nvmf_handle_connect(struct spdk_nvmf_request *req);

void spdk_nvmf_ctrlr_disconnect(struct spdk_nvmf_qpair *qpair);

#endif
