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

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_nvmf_tgt;
struct spdk_nvmf_subsystem;
struct spdk_nvmf_ctrlr;
struct spdk_nvmf_qpair;
struct spdk_nvmf_request;
struct spdk_bdev;
struct spdk_nvmf_request;
struct spdk_nvmf_host;
struct spdk_nvmf_listener;
struct spdk_nvmf_poll_group;

struct spdk_nvmf_tgt_opts {
	uint16_t max_queue_depth;
	uint16_t max_qpairs_per_ctrlr;
	uint32_t in_capsule_data_size;
	uint32_t max_io_size;
};

void spdk_nvmf_tgt_opts_init(struct spdk_nvmf_tgt_opts *opts);

/**
 * Construct an NVMe-oF target
 *
 * \param opts Options
 * \return An spdk_nvmf_tgt on success, NULL on failure.
 */
struct spdk_nvmf_tgt *spdk_nvmf_tgt_create(struct spdk_nvmf_tgt_opts *opts);

/**
 * Destroy an NVMe-oF target
 *
 * \param tgt The target to destroy. This releases all resources.
 */
void spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt);

/**
 * Begin accepting new connections at the address provided.
 *
 * The connections will be matched with a subsystem, which may or may not allow
 * the connection based on a subsystem-specific whitelist. See
 * spdk_nvmf_subsystem_add_host() and spdk_nvmf_subsystem_add_listener()
 *
 * \param tgt The target associated with this listen address
 * \param trid The address to listen at
 *
 * \return 0 on success. Negated errno on failure.
 */
int spdk_nvmf_tgt_listen(struct spdk_nvmf_tgt *tgt,
			 struct spdk_nvme_transport_id *trid);

typedef void (*new_qpair_fn)(struct spdk_nvmf_qpair *qpair);

/**
 * Poll the target for incoming connections.
 *
 * The new_qpair_fn cb_fn will be called for each newly discovered
 * qpair. The user is expected to add that qpair to a poll group
 * to establish the connection.
 */
void spdk_nvmf_tgt_accept(struct spdk_nvmf_tgt *tgt, new_qpair_fn cb_fn);

/**
 * Create a poll group.
 */
struct spdk_nvmf_poll_group *spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt);

/**
 * Destroy a poll group.
 */
void spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group);

/**
 * Add the given qpair to the poll group.
 */
int spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			     struct spdk_nvmf_qpair *qpair);

/**
 * Remove the given qpair from the poll group.
 */
int spdk_nvmf_poll_group_remove(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_qpair *qpair);

/**
 * Create an NVMe-oF subsystem.
 *
 * Subsystems are in one of three states: Inactive, Active, Paused. This
 * state affects which operations may be performed on the subsystem. Upon
 * creation, the subsystem will be in the Inactive state and may be activated
 * by calling spdk_nvmf_subsystem_start(). No I/O will be processed in the Inactive
 * or Paused states, but changes to the state of the subsystem may be made.
 *
 * \param tgt The NVMe-oF target that will own this subsystem.
 * \param nqn The NVMe qualified name of this subsystem.
 * \param type Whether this subsystem is an I/O subsystem or a Discovery subsystem.
 * \param num_ns The number of namespaces this subsystem contains.
 *
 * \return An spdk_nvmf_subsystem or NULL on error.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_create(struct spdk_nvmf_tgt *tgt,
		const char *nqn,
		enum spdk_nvmf_subtype type,
		uint32_t num_ns);

/**
 * Destroy an NVMe-oF subsystem. A subsystem may only be destroyed when in
 * the Inactive state. See spdk_nvmf_subsystem_stop().
 *
 * \param subsystem The NVMe-oF subsystem to destroy.
 */
void spdk_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem);

typedef void (*spdk_nvmf_subsystem_state_change_done)(struct spdk_nvmf_subsystem *subsystem,
		void *cb_arg, int status);

/**
 * Transition an NVMe-oF subsystem from Inactive to Active state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success. Negated errno on failure. The callback provided
 *	     will only be called on success.
 */
int spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_subsystem_state_change_done cb_fn,
			      void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Active to Inactive state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success. Negated errno on failure. The callback provided
 *	     will only be called on success.
 */
int spdk_nvmf_subsystem_stop(struct spdk_nvmf_subsystem *subsystem,
			     spdk_nvmf_subsystem_state_change_done cb_fn,
			     void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Active to Paused state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success. Negated errno on failure. The callback provided
 *	     will only be called on success.
 */
int spdk_nvmf_subsystem_pause(struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_subsystem_state_change_done cb_fn,
			      void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Paused to Active state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success. Negated errno on failure. The callback provided
 *	     will only be called on success.
 */
int spdk_nvmf_subsystem_resume(struct spdk_nvmf_subsystem *subsystem,
			       spdk_nvmf_subsystem_state_change_done cb_fn,
			       void *cb_arg);

/**
 * Search the target for a subsystem with the given NQN
 */
struct spdk_nvmf_subsystem *spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt,
		const char *subnqn);

/**
 * Begin iterating over all known subsystems. If no subsystems are present, return NULL.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt);

/**
 * Continue iterating over all known subsystems. If no additional subsystems, return NULL.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_get_next(struct spdk_nvmf_subsystem *subsystem);

/**
 * Allow the given host NQN to connect to the given subsystem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add host to
 * \param host_nqn The NQN for the host
 * \return 0 on success. Negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn);

/**
 * Set whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to modify.
 * \param allow_any_host true to allow any host to connect to this subsystem, or false to enforce
 *                       the whitelist configured with spdk_nvmf_subsystem_add_host().
 * \return 0 on success. Negated errno value on failure.
 */
int spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem,
		bool allow_any_host);

/**
 * Check whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to modify.
 * \return true if any host is allowed to connect to this subsystem, or false if connecting hosts
 *         must be in the whitelist configured with spdk_nvmf_subsystem_add_host().
 */
bool spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem);

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

/**
 * Accept new connections on the address provided
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add listener to
 * \param trid The address to accept connections from
 * \return 0 on success. Negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvme_transport_id *trid);

/**
 * Stop accepting new connections on the address provided
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to remove listener from
 * \param trid The address to no longer accept connections from
 * \return 0 on success. Negated errno value on failure.
 */
int spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
					const struct spdk_nvme_transport_id *trid);

/**
 * Check if connections originated from the given address are allowed to connect to the subsystem.
 *
 * \param subsystem The subsystem to query
 * \param trid The listen address
 * \return true if allowed, false if not.
 */
bool spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvme_transport_id *trid);

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

/** NVMe-oF target namespace creation options */
struct spdk_nvmf_ns_opts {
	/**
	 * Namespace ID
	 *
	 * Set to 0 to automatically assign a free NSID.
	 */
	uint32_t nsid;
};

/**
 * Get default namespace creation options.
 *
 * \param opts Namespace options to fill with defaults.
 * \param opts_size sizeof(struct spdk_nvmf_ns_opts)
 */
void spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size);

/**
 * Add a namespace to a subsytem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add namespace to.
 * \param bdev Block device to add as a namespace.
 * \param opts Namespace options, or NULL to use defaults.
 * \param opts_size sizeof(*opts)
 *
 * \return Newly added NSID on success or 0 on failure.
 */
uint32_t spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
				    const struct spdk_nvmf_ns_opts *opts, size_t opts_size);

/**
 * Remove a namespace from a subsytem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be removed.
 *
 * \return 0 on success or -1 on failure.
 */
int spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

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

/**
 * Get the options specified for a namespace.
 *
 * \param ns Namespace to query.
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
void spdk_nvmf_ns_get_opts(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_ns_opts *opts,
			   size_t opts_size);

const char *spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem);

int spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn);

const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem);
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);

void spdk_nvmf_handle_connect(struct spdk_nvmf_request *req);

void spdk_nvmf_ctrlr_disconnect(struct spdk_nvmf_qpair *qpair);

#ifdef __cplusplus
}
#endif

#endif
