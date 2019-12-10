/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVMF_TGT_NAME_MAX_LENGTH	256

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
struct spdk_json_write_ctx;
struct spdk_nvmf_transport;

struct spdk_nvmf_target_opts {
	char		name[NVMF_TGT_NAME_MAX_LENGTH];
	uint32_t	max_subsystems;
};

struct spdk_nvmf_transport_opts {
	uint16_t	max_queue_depth;
	uint16_t	max_qpairs_per_ctrlr;
	uint32_t	in_capsule_data_size;
	uint32_t	max_io_size;
	uint32_t	io_unit_size;
	uint32_t	max_aq_depth;
	uint32_t	num_shared_buffers;
	uint32_t	buf_cache_size;
	uint32_t	max_srq_depth;
	bool		no_srq;
	bool		c2h_success;
	bool		dif_insert_or_strip;
	uint32_t	sock_priority;
};

struct spdk_nvmf_poll_group_stat {
	uint32_t admin_qpairs;
	uint32_t io_qpairs;
	uint64_t pending_bdev_io;
};

struct spdk_nvmf_rdma_device_stat {
	const char *name;
	uint64_t polls;
	uint64_t completions;
	uint64_t requests;
	uint64_t request_latency;
	uint64_t pending_free_request;
	uint64_t pending_rdma_read;
	uint64_t pending_rdma_write;
};

struct spdk_nvmf_transport_poll_group_stat {
	spdk_nvme_transport_type_t trtype;
	union {
		struct {
			uint64_t pending_data_buffer;
			uint64_t num_devices;
			struct spdk_nvmf_rdma_device_stat *devices;
		} rdma;
	};
};

/**
 * Function to be called for each newly discovered qpair.
 *
 * \param qpair The newly discovered qpair.
 * \param cb_arg A context argument passed to this function.
 */
typedef void (*new_qpair_fn)(struct spdk_nvmf_qpair *qpair, void *cb_arg);

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
	 * set the submission queue size of the queue pair
	 */
	int (*qpair_set_sqsize)(struct spdk_nvmf_qpair *qpair);

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
 * Construct an NVMe-oF target.
 *
 * \param opts a pointer to an spdk_nvmf_target_opts structure.
 *
 * \return a pointer to a NVMe-oF target on success, or NULL on failure.
 */
struct spdk_nvmf_tgt *spdk_nvmf_tgt_create(struct spdk_nvmf_target_opts *opts);

typedef void (spdk_nvmf_tgt_destroy_done_fn)(void *ctx, int status);

/**
 * Destroy an NVMe-oF target.
 *
 * \param tgt The target to destroy. This releases all resources.
 * \param cb_fn A callback that will be called once the target is destroyed
 * \param cb_arg A context argument passed to cb_fn.
 */
void spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
			   spdk_nvmf_tgt_destroy_done_fn cb_fn,
			   void *cb_arg);

/**
 * Get the name of an NVMe-oF target.
 *
 * \param tgt The target from which to get the name.
 *
 * \return The name of the target as a null terminated string.
 */
const char *spdk_nvmf_tgt_get_name(struct spdk_nvmf_tgt *tgt);

/**
 * Get a pointer to an NVMe-oF target.
 *
 * In order to support some legacy applications and RPC methods that may rely on the
 * concept that there is only one target, the name parameter can be passed as NULL.
 * If there is only one available target, that target will be returned.
 * Otherwise, name is a required parameter.
 *
 * \param name The name provided when the target was created.
 *
 * \return The target with the given name, or NULL if no match was found.
 */
struct spdk_nvmf_tgt *spdk_nvmf_get_tgt(const char *name);

/**
 * Get the pointer to the first NVMe-oF target.
 *
 * Combined with spdk_nvmf_get_next_tgt to iterate over all available targets.
 *
 * \return The first NVMe-oF target.
 */
struct spdk_nvmf_tgt *spdk_nvmf_get_first_tgt(void);

/**
 * Get the pointer to the first NVMe-oF target.
 *
 * Combined with spdk_nvmf_get_first_tgt to iterate over all available targets.
 *
 * \param prev A pointer to the last NVMe-oF target.
 *
 * \return The first NVMe-oF target.
 */
struct spdk_nvmf_tgt *spdk_nvmf_get_next_tgt(struct spdk_nvmf_tgt *prev);

/**
 * Write NVMe-oF target configuration into provided JSON context.
 * \param w JSON write context
 * \param tgt The NVMe-oF target
 */
void spdk_nvmf_tgt_write_config_json(struct spdk_json_write_ctx *w, struct spdk_nvmf_tgt *tgt);

/**
 * Function to be called once the target is listening.
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_nvmf_tgt_listen_done_fn)(void *ctx, int status);

/**
 * Begin accepting new connections at the address provided.
 *
 * The connections will be matched with a subsystem, which may or may not allow
 * the connection based on a subsystem-specific whitelist. See
 * spdk_nvmf_subsystem_add_host() and spdk_nvmf_subsystem_add_listener()
 *
 * \param tgt The target associated with this listen address.
 * \param trid The address to listen at.
 * \param cb_fn A callback that will be called once the target is listening
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return void. The callback status argument will be 0 on success
 *	   or a negated errno on failure.
 */
void spdk_nvmf_tgt_listen(struct spdk_nvmf_tgt *tgt,
			  struct spdk_nvme_transport_id *trid,
			  spdk_nvmf_tgt_listen_done_fn cb_fn,
			  void *cb_arg);

/**
 * Poll the target for incoming connections.
 *
 * The new_qpair_fn cb_fn will be called for each newly discovered
 * qpair. The user is expected to add that qpair to a poll group
 * to establish the connection.
 *
 * \param tgt The target associated with the listen address.
 * \param cb_fn Called for each newly discovered qpair.
 * \param cb_arg A context argument passed to cb_fn.
 */
void spdk_nvmf_tgt_accept(struct spdk_nvmf_tgt *tgt, new_qpair_fn cb_fn, void *cb_arg);

/**
 * Create a poll group.
 *
 * \param tgt The target to create a poll group.
 *
 * \return a poll group on success, or NULL on failure.
 */
struct spdk_nvmf_poll_group *spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt);

/**
 * Get optimal nvmf poll group for the qpair.
 *
 * \param qpair Requested qpair
 *
 * \return a poll group on success, or NULL on failure.
 */
struct spdk_nvmf_poll_group *spdk_nvmf_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair);

/**
 * Destroy a poll group.
 *
 * \param group The poll group to destroy.
 */
void spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group);

/**
 * Add the given qpair to the poll group.
 *
 * \param group The group to add qpair to.
 * \param qpair The qpair to add.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			     struct spdk_nvmf_qpair *qpair);

/**
 * Get current poll group statistics.
 *
 * \param tgt The NVMf target.
 * \param stat Pointer to allocated statistics structure to fill with values.
 *
 * \return 0 upon success.
 * \return -EINVAL if either group or stat is NULL.
 */
int spdk_nvmf_poll_group_get_stat(struct spdk_nvmf_tgt *tgt,
				  struct spdk_nvmf_poll_group_stat *stat);

typedef void (*nvmf_qpair_disconnect_cb)(void *ctx);

/**
 * Disconnect an NVMe-oF qpair
 *
 * \param qpair The NVMe-oF qpair to disconnect.
 * \param cb_fn The function to call upon completion of the disconnect.
 * \param ctx The context to pass to the callback function.
 *
 * \return 0 upon success.
 * \return -ENOMEM if the function specific context could not be allocated.
 */
int spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn,
			       void *ctx);

/**
 * Get the peer's transport ID for this queue pair.
 *
 * \param qpair The NVMe-oF qpair
 * \param trid Output parameter that will contain the transport id.
 *
 * \return 0 for success.
 * \return -EINVAL if the qpair is not connected.
 */
int spdk_nvmf_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				  struct spdk_nvme_transport_id *trid);

/**
 * Get the local transport ID for this queue pair.
 *
 * \param qpair The NVMe-oF qpair
 * \param trid Output parameter that will contain the transport id.
 *
 * \return 0 for success.
 * \return -EINVAL if the qpair is not connected.
 */
int spdk_nvmf_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid);

/**
 * Get the associated listener transport ID for this queue pair.
 *
 * \param qpair The NVMe-oF qpair
 * \param trid Output parameter that will contain the transport id.
 *
 * \return 0 for success.
 * \return -EINVAL if the qpair is not connected.
 */
int spdk_nvmf_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid);

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
 * \return a pointer to a NVMe-oF subsystem on success, or NULL on failure.
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

/**
 * Function to be called once the subsystem has changed state.
 *
 * \param subsytem NVMe-oF subsystem that has changed state.
 * \param cb_arg Argument passed to callback function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_nvmf_subsystem_state_change_done)(struct spdk_nvmf_subsystem *subsystem,
		void *cb_arg, int status);

/**
 * Transition an NVMe-oF subsystem from Inactive to Active state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
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
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
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
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
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
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
 */
int spdk_nvmf_subsystem_resume(struct spdk_nvmf_subsystem *subsystem,
			       spdk_nvmf_subsystem_state_change_done cb_fn,
			       void *cb_arg);

/**
 * Search the target for a subsystem with the given NQN.
 *
 * \param tgt The NVMe-oF target to search from.
 * \param subnqn NQN of the subsystem.
 *
 * \return a pointer to the NVMe-oF subsystem on success, or NULL on failure.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt,
		const char *subnqn);

/**
 * Begin iterating over all known subsystems. If no subsystems are present, return NULL.
 *
 * \param tgt The NVMe-oF target to iterate.
 *
 * \return a pointer to the first NVMe-oF subsystem on success, or NULL on failure.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt);

/**
 * Continue iterating over all known subsystems. If no additional subsystems, return NULL.
 *
 * \param subsystem Previous subsystem returned from \ref spdk_nvmf_subsystem_get_first or
 *                  \ref spdk_nvmf_subsystem_get_next.
 *
 * \return a pointer to the next NVMe-oF subsystem on success, or NULL on failure.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_get_next(struct spdk_nvmf_subsystem *subsystem);

/**
 * Allow the given host NQN to connect to the given subsystem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add host to.
 * \param hostnqn The NQN for the host.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn);

/**
 * Remove the given host NQN from the allowed hosts whitelist.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to remove host from.
 * \param hostnqn The NQN for the host.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/**
 * Set whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to modify.
 * \param allow_any_host true to allow any host to connect to this subsystem,
 * or false to enforce the whitelist configured with spdk_nvmf_subsystem_add_host().
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem,
		bool allow_any_host);

/**
 * Check whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to query.
 *
 * \return true if any host is allowed to connect to this subsystem, or false if
 * connecting hosts must be in the whitelist configured with spdk_nvmf_subsystem_add_host().
 */
bool spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Check if the given host is allowed to connect to the subsystem.
 *
 * \param subsystem The subsystem to query.
 * \param hostnqn The NQN of the host.
 *
 * \return true if allowed, false if not.
 */
bool spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/**
 * Get the first allowed host in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allowed host in this subsystem, or NULL if none allowed.
 */
struct spdk_nvmf_host *spdk_nvmf_subsystem_get_first_host(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the next allowed host in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_host Previous host returned from this function.
 *
 * \return next allowed host in this subsystem, or NULL if prev_host was the last host.
 */
struct spdk_nvmf_host *spdk_nvmf_subsystem_get_next_host(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_host *prev_host);

/**
 * Get a host's NQN.
 *
 * \param host Host to query.
 *
 * \return NQN of host.
 */
const char *spdk_nvmf_host_get_nqn(struct spdk_nvmf_host *host);

/**
 * Accept new connections on the address provided.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add listener to.
 * \param trid The address to accept connections from.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvme_transport_id *trid);

/**
 * Stop accepting new connections on the address provided
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to remove listener from.
 * \param trid The address to no longer accept connections from.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
					const struct spdk_nvme_transport_id *trid);

/**
 * Check if connections originated from the given address are allowed to connect
 * to the subsystem.
 *
 * \param subsystem The subsystem to query.
 * \param trid The listen address.
 *
 * \return true if allowed, or false if not.
 */
bool spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvme_transport_id *trid);

/**
 * Get the first allowed listen address in the subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allowed listen address in this subsystem, or NULL if none allowed.
 */
struct spdk_nvmf_listener *spdk_nvmf_subsystem_get_first_listener(
	struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the next allowed listen address in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_listener Previous listen address for this subsystem.
 *
 * \return next allowed listen address in this subsystem, or NULL if prev_listener
 * was the last address.
 */
struct spdk_nvmf_listener *spdk_nvmf_subsystem_get_next_listener(
	struct spdk_nvmf_subsystem *subsystem,
	struct spdk_nvmf_listener *prev_listener);

/**
 * Get a listen address' transport ID
 *
 * \param listener This listener.
 *
 * \return the transport ID for this listener.
 */
const struct spdk_nvme_transport_id *spdk_nvmf_listener_get_trid(
	struct spdk_nvmf_listener *listener);

/**
 * Set whether a subsystem should allow any listen address or only addresses in the allowed list.
 *
 * \param subsystem Subsystem to allow dynamic listener assignment.
 * \param allow_any_listener true to allow dynamic listener assignment for
 * this subsystem, or false to enforce the whitelist configured during
 * subsystem setup.
 */
void spdk_nvmf_subsystem_allow_any_listener(
	struct spdk_nvmf_subsystem *subsystem,
	bool allow_any_listener);

/**
 * Check whether a subsystem allows any listen address or only addresses in the allowed list.
 *
 * \param subsystem Subsystem to query.
 *
 * \return true if this subsystem allows dynamic management of listen address list,
 *  or false if only allows addresses in the whitelist configured during subsystem setup.
 */
bool spdk_nvmf_subsytem_any_listener_allowed(
	struct spdk_nvmf_subsystem *subsystem);

/** NVMe-oF target namespace creation options */
struct spdk_nvmf_ns_opts {
	/**
	 * Namespace ID
	 *
	 * Set to 0 to automatically assign a free NSID.
	 */
	uint32_t nsid;

	/**
	 * Namespace Globally Unique Identifier
	 *
	 * Fill with 0s if not specified.
	 */
	uint8_t nguid[16];

	/**
	 * IEEE Extended Unique Identifier
	 *
	 * Fill with 0s if not specified.
	 */
	uint8_t eui64[8];

	/**
	 * Namespace UUID
	 *
	 * Fill with 0s if not specified.
	 */
	struct spdk_uuid uuid;
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
 * \param ptpl_file Persist through power loss file path.
 *
 * \return newly added NSID on success, or 0 on failure.
 */
uint32_t spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
				    const struct spdk_nvmf_ns_opts *opts, size_t opts_size,
				    const char *ptpl_file);

/**
 * Remove a namespace from a subsytem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be removed.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

/**
 * Get the first allocated namespace in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allocated namespace in this subsystem, or NULL if this subsystem
 * has no namespaces.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the next allocated namespace in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_ns Previous ns returned from this function.
 *
 * \return next allocated namespace in this subsystem, or NULL if prev_ns was the
 * last namespace.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_ns *prev_ns);

/**
 * Get a namespace in a subsystem by NSID.
 *
 * \param subsystem Subsystem to search.
 * \param nsid Namespace ID to find.
 *
 * \return namespace matching nsid, or NULL if nsid was not found.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid);

/**
 * Get the maximum number of namespaces allowed in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return Maximum number of namespaces allowed in the subsystem, or 0 for unlimited.
 */
uint32_t spdk_nvmf_subsystem_get_max_namespaces(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get a namespace's NSID.
 *
 * \param ns Namespace to query.
 *
 * \return NSID of ns.
 */
uint32_t spdk_nvmf_ns_get_id(const struct spdk_nvmf_ns *ns);

/**
 * Get a namespace's associated bdev.
 *
 * \param ns Namespace to query.
 *
 * \return backing bdev of ns.
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

/**
 * Get the serial number of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return serial number of the specified subsystem.
 */
const char *spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem);


/**
 * Set the serial number for the specified subsystem.
 *
 * \param subsystem Subsystem to set for.
 * \param sn serial number to set.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn);

/**
 * Get the model number of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return model number of the specified subsystem.
 */
const char *spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem);


/**
 * Set the model number for the specified subsystem.
 *
 * \param subsystem Subsystem to set for.
 * \param mn model number to set.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_subsystem_set_mn(struct spdk_nvmf_subsystem *subsystem, const char *mn);

/**
 * Get the NQN of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return NQN of the specified subsystem.
 */
const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the type of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return the type of the specified subsystem.
 */
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);

/**
 * Initialize transport options
 *
 * \param type The transport type to create
 * \param opts The transport options (e.g. max_io_size)
 *
 * \return bool. true if successful, false if transport type
 *	   not found.
 */
bool
spdk_nvmf_transport_opts_init(enum spdk_nvme_transport_type type,
			      struct spdk_nvmf_transport_opts *opts);

/**
 * Create a protocol transport
 *
 * \param type The transport type to create
 * \param opts The transport options (e.g. max_io_size)
 *
 * \return new transport or NULL if create fails
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_create(enum spdk_nvme_transport_type type,
		struct spdk_nvmf_transport_opts *opts);

/**
 * Destroy a protocol transport
 *
 * \param transport The transport to destory
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport);

/**
 * Get an existing transport from the target
 *
 * \param tgt The NVMe-oF target
 * \param type The transport type to get
 *
 * \return the transport or NULL if not found
 */
struct spdk_nvmf_transport *spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt,
		enum spdk_nvme_transport_type type);

/**
 * Get the first transport registered with the given target
 *
 * \param tgt The NVMe-oF target
 *
 * \return The first transport registered on the target
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_get_first(struct spdk_nvmf_tgt *tgt);

/**
 * Get the next transport in a target's list.
 *
 * \param transport A handle to a transport object
 *
 * \return The next transport associated with the NVMe-oF target
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_get_next(struct spdk_nvmf_transport *transport);

/**
 * Get the opts for a given transport.
 *
 * \param transport The transport to query
 *
 * \return The opts associated with the given transport
 */
const struct spdk_nvmf_transport_opts *spdk_nvmf_get_transport_opts(struct spdk_nvmf_transport
		*transport);

/**
 * Get the transport type for a given transport.
 *
 * \param transport The transport to query
 *
 * \return the transport type for the given transport
 */
spdk_nvme_transport_type_t spdk_nvmf_get_transport_type(struct spdk_nvmf_transport *transport);

/**
 * Function to be called once transport add is complete
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_nvmf_tgt_add_transport_done_fn)(void *cb_arg, int status);

/**
 * Add a transport to a target
 *
 * \param tgt The NVMe-oF target
 * \param transport The transport to add
 * \param cb_fn A callback that will be called once the transport is created
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return void. The callback status argument will be 0 on success
 *	   or a negated errno on failure.
 */
void spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
				 struct spdk_nvmf_transport *transport,
				 spdk_nvmf_tgt_add_transport_done_fn cb_fn,
				 void *cb_arg);

/**
 *
 * Add listener to transport and begin accepting new connections.
 *
 * \param transport The transport to add listener to
 * \param trid Address to listen at
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */

int spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			       const struct spdk_nvme_transport_id *trid);

/**
 * Write NVMe-oF target's transport configurations into provided JSON context.
 * \param w JSON write context
 * \param tgt The NVMe-oF target
 */
void
spdk_nvmf_tgt_transport_write_config_json(struct spdk_json_write_ctx *w, struct spdk_nvmf_tgt *tgt);


/**
 * \brief Get current transport poll group statistics.
 *
 * This function allocates memory for statistics and returns it
 * in \p stat parameter. Caller must free this memory with
 * spdk_nvmf_transport_poll_group_free_stat() when it is not needed
 * anymore.
 *
 * \param tgt The NVMf target.
 * \param transport The NVMf transport.
 * \param stat Output parameter that will contain pointer to allocated statistics structure.
 *
 * \return 0 upon success.
 * \return -ENOTSUP if transport does not support statistics.
 * \return -EINVAL if any of parameters is NULL.
 * \return -ENOENT if transport poll group is not found.
 * \return -ENOMEM if memory allocation failed.
 */
int
spdk_nvmf_transport_poll_group_get_stat(struct spdk_nvmf_tgt *tgt,
					struct spdk_nvmf_transport *transport,
					struct spdk_nvmf_transport_poll_group_stat **stat);

/**
 * Free statistics memory previously allocated with spdk_nvmf_transport_poll_group_get_stat().
 *
 * \param transport The NVMf transport.
 * \param stat Pointer to transport poll group statistics structure.
 */
void
spdk_nvmf_transport_poll_group_free_stat(struct spdk_nvmf_transport *transport,
		struct spdk_nvmf_transport_poll_group_stat *stat);

/**
 * \brief Set the global hooks for the RDMA transport, if necessary.
 *
 * This call is optional and must be performed prior to probing for
 * any devices. By default, the RDMA transport will use the ibverbs
 * library to create protection domains and register memory. This
 * is a mechanism to subvert that and use an existing registration.
 *
 * This function may only be called one time per process.
 *
 * \param hooks for initializing global hooks
 */
void spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks);

#ifdef __cplusplus
}
#endif

#endif
