/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#define SPDK_TLS_PSK_MAX_LEN		200

struct spdk_nvmf_tgt;
struct spdk_nvmf_subsystem;
struct spdk_nvmf_ctrlr;
struct spdk_nvmf_qpair;
struct spdk_nvmf_request;
struct spdk_bdev;
struct spdk_nvmf_request;
struct spdk_nvmf_host;
struct spdk_nvmf_subsystem_listener;
struct spdk_nvmf_poll_group;
struct spdk_json_write_ctx;
struct spdk_json_val;
struct spdk_nvmf_transport;

/**
 * Specify filter rules which are applied during discovery log generation.
 */
enum spdk_nvmf_tgt_discovery_filter {
	/** Log all listeners in discovery log page */
	SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY = 0,
	/** Only log listeners with the same transport type on which the DISCOVERY command was received */
	SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE = 1u << 0u,
	/** Only log listeners with the same transport address on which the DISCOVERY command was received */
	SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS = 1u << 1u,
	/** Only log listeners with the same transport svcid on which the DISCOVERY command was received */
	SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID = 1u << 2u
};

struct spdk_nvmf_target_opts {
	size_t		size;
	char		name[NVMF_TGT_NAME_MAX_LENGTH];
	uint32_t	max_subsystems;
	uint16_t	crdt[3];
	uint32_t	discovery_filter;
	uint32_t	dhchap_digests;
	uint32_t	dhchap_dhgroups;
};

struct spdk_nvmf_transport_opts {
	uint16_t	max_queue_depth;
	uint16_t	max_qpairs_per_ctrlr;
	uint32_t	in_capsule_data_size;
	/* used to calculate mdts */
	uint32_t	max_io_size;
	uint32_t	io_unit_size;
	uint32_t	max_aq_depth;
	uint32_t	num_shared_buffers;
	uint32_t	buf_cache_size;
	bool		dif_insert_or_strip;

	/* Hole at bytes 29-31. */
	uint8_t		reserved29[3];

	uint32_t	abort_timeout_sec;
	/* ms */
	uint32_t	association_timeout;

	/* Transport specific json values.
	 *
	 * If transport specific values provided then json object is valid only at the time
	 * transport is being created. It is transport layer responsibility to maintain
	 * the copy of it or its decoding if required.
	 */
	const struct spdk_json_val *transport_specific;

	/**
	 * The size of spdk_nvmf_transport_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	uint32_t acceptor_poll_rate;
	/* Use zero-copy operations if the underlying bdev supports them */
	bool zcopy;

	/* Hole at bytes 61-63. */
	uint8_t reserved61[3];
	/* ACK timeout in milliseconds */
	uint32_t ack_timeout;
	/* Size of RDMA data WR pool */
	uint32_t data_wr_pool_size;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_transport_opts) == 72, "Incorrect size");

struct spdk_nvmf_listen_opts {
	/**
	 * The size of spdk_nvmf_listen_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	/* Transport specific json values.
	 *
	 * If transport specific values provided then json object is valid only at the time
	 * listener is being added. It is transport layer responsibility to maintain
	 * the copy of it or its decoding if required.
	 */
	const struct spdk_json_val *transport_specific;

	/**
	 * Indicates that all newly established connections shall immediately
	 * establish a secure channel, prior to any authentication.
	 */
	bool secure_channel;

	/* Hole at bytes 17-19. */
	uint8_t reserved1[3];

	/**
	 * Asymmetric Namespace Access state
	 * Optional parameter, which defines ANA_STATE that will be set for
	 * all ANA groups in this listener, when the listener is added to the subsystem.
	 * If not specified, SPDK_NVME_ANA_OPTIMIZED_STATE will be set by default.
	 */
	enum spdk_nvme_ana_state ana_state;

} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listen_opts) == 24, "Incorrect size");

/**
 * Initialize listen options
 *
 * \param opts Listener options.
 * \param opts_size Must be set to sizeof(struct spdk_nvmf_listen_opts).
 */
void spdk_nvmf_listen_opts_init(struct spdk_nvmf_listen_opts *opts, size_t opts_size);

struct spdk_nvmf_poll_group_stat {
	/* cumulative admin qpair count */
	uint32_t admin_qpairs;
	/* cumulative io qpair count */
	uint32_t io_qpairs;
	/* current admin qpair count */
	uint32_t current_admin_qpairs;
	/* current io qpair count */
	uint32_t current_io_qpairs;
	uint64_t pending_bdev_io;
	/* NVMe IO commands completed (excludes admin commands) */
	uint64_t completed_nvme_io;
};

/**
 * Function to be called once asynchronous listen add and remove
 * operations are completed. See spdk_nvmf_subsystem_add_listener()
 * and spdk_nvmf_transport_stop_listen_async().
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_nvmf_tgt_subsystem_listen_done_fn)(void *ctx, int status);

struct spdk_nvmf_referral_opts {
	/** Size of this structure */
	size_t size;
	/** Transport ID of the referral */
	struct spdk_nvme_transport_id trid;
	/** The referral describes a referral to a subsystem which requires a secure channel */
	bool secure_channel;
};

/**
 * Add a discovery service referral to an NVMe-oF target
 *
 * \param tgt The target to which the referral will be added
 * \param opts Options describing the referral referral.
 *
 * \return 0 on success or a negated errno on failure
 */
int spdk_nvmf_tgt_add_referral(struct spdk_nvmf_tgt *tgt,
			       const struct spdk_nvmf_referral_opts *opts);

/**
 * Remove a discovery service referral from an NVMeoF target
 *
 * \param tgt The target from which the referral will be removed
 * \param opts Options describing the referral referral.
 *
 * \return 0 on success or a negated errno on failure.
 */
int spdk_nvmf_tgt_remove_referral(struct spdk_nvmf_tgt *tgt,
				  const struct spdk_nvmf_referral_opts *opts);


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
 * Begin accepting new connections at the address provided.
 *
 * The connections will be matched with a subsystem, which may or may not allow
 * the connection based on a subsystem-specific list of allowed hosts. See
 * spdk_nvmf_subsystem_add_host() and spdk_nvmf_subsystem_add_listener()
 *
 * \param tgt The target associated with this listen address.
 * \param trid The address to listen at.
 * \param opts Listener options.
 *
 * \return 0 on success or a negated errno on failure.
 */
int spdk_nvmf_tgt_listen_ext(struct spdk_nvmf_tgt *tgt, const struct spdk_nvme_transport_id *trid,
			     struct spdk_nvmf_listen_opts *opts);

/**
 * Stop accepting new connections at the provided address.
 *
 * This is a counterpart to spdk_nvmf_tgt_listen_ext().
 *
 * \param tgt The target associated with the listen address.
 * \param trid The address to stop listening at.
 *
 * \return int. 0 on success or a negated errno on failure.
 */
int spdk_nvmf_tgt_stop_listen(struct spdk_nvmf_tgt *tgt,
			      struct spdk_nvme_transport_id *trid);

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

typedef void(*spdk_nvmf_poll_group_destroy_done_fn)(void *cb_arg, int status);

/**
 * Destroy a poll group.
 *
 * \param group The poll group to destroy.
 * \param cb_fn A callback that will be called once the poll group is destroyed.
 * \param cb_arg A context argument passed to cb_fn.
 */
void spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group,
				  spdk_nvmf_poll_group_destroy_done_fn cb_fn,
				  void *cb_arg);

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

typedef void (*nvmf_qpair_disconnect_cb)(void *ctx);

/**
 * Disconnect an NVMe-oF qpair
 *
 * \param qpair The NVMe-oF qpair to disconnect.
 *
 * \return 0 upon success.
 * \return -ENOMEM if the function specific context could not be allocated.
 * \return -EINPROGRESS if the qpair is already in the process of disconnect.
 */
int spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair);

/**
 * Get the peer's transport ID for this queue pair.
 *
 * This function will first zero the trid structure, and then fill
 * in the relevant trid fields to identify the listener. The relevant
 * fields will depend on the transport, but the subnqn will never
 * be a relevant field for purposes of this function.
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
 * This function will first zero the trid structure, and then fill
 * in the relevant trid fields to identify the listener. The relevant
 * fields will depend on the transport, but the subnqn will never
 * be a relevant field for purposes of this function.
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
 * This function will first zero the trid structure, and then fill
 * in the relevant trid fields to identify the listener. The relevant
 * fields will depend on the transport, but the subnqn will never
 * be a relevant field for purposes of this function.
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
 * \param num_ns The maximum number of namespaces this subsystem may contain.
 *
 * \return a pointer to a NVMe-oF subsystem on success, or NULL on failure.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_create(struct spdk_nvmf_tgt *tgt,
		const char *nqn,
		enum spdk_nvmf_subtype type,
		uint32_t num_ns);

typedef void (*nvmf_subsystem_destroy_cb)(void *cb_arg);

/**
 * Destroy an NVMe-oF subsystem. A subsystem may only be destroyed when in
 * the Inactive state. See spdk_nvmf_subsystem_stop(). A subsystem may be
 * destroyed asynchronously, in that case \b cpl_cb will be called
 *
 * \param subsystem The NVMe-oF subsystem to destroy.
 * \param cpl_cb Optional callback to be called if the subsystem is destroyed asynchronously, only called if
 * return value is -EINPROGRESS
 * \param cpl_cb_arg Optional user context to be passed to \b cpl_cb
 *
 * \retval 0 if subsystem is destroyed, \b cpl_cb is not called is that case
 * \retval -EINVAl if \b subsystem is a NULL pointer
 * \retval -EAGAIN if \b subsystem is not in INACTIVE state
 * \retval -EALREADY if subsystem destruction is already started
 * \retval -EINPROGRESS if subsystem is destroyed asynchronously, cpl_cb will be called in that case
 */
int
spdk_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem, nvmf_subsystem_destroy_cb cpl_cb,
			    void *cpl_cb_arg);

/**
 * Function to be called once the subsystem has changed state.
 *
 * \param subsystem NVMe-oF subsystem that has changed state.
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
 * In a paused state, all admin queues are frozen across the whole subsystem. If
 * a namespace ID is provided, all commands to that namespace are quiesced and incoming
 * commands for that namespace are queued until the subsystem is resumed.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param nsid The namespace to pause. If 0, pause no namespaces.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
 */
int spdk_nvmf_subsystem_pause(struct spdk_nvmf_subsystem *subsystem,
			      uint32_t nsid,
			      spdk_nvmf_subsystem_state_change_done cb_fn,
			      void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Paused to Active state.
 *
 * This resumes the entire subsystem, including any paused namespaces.
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
 * Make the specified namespace visible to the specified host.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be made visible.
 * \param hostnqn The NQN for the host.
 * \param flags Must be zero (reserved for future use).
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_ns_add_host(struct spdk_nvmf_subsystem *subsystem,
			  uint32_t nsid,
			  const char *hostnqn,
			  uint32_t flags);

/**
 * Make the specified namespace not visible to the specified host.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be made not visible.
 * \param hostnqn The NQN for the host.
 * \param flags Must be zero (reserved for future use).
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_ns_remove_host(struct spdk_nvmf_subsystem *subsystem,
			     uint32_t nsid,
			     const char *hostnqn,
			     uint32_t flags);

/**
 * Allow the given host NQN to connect to the given subsystem.
 *
 * \param subsystem Subsystem to add host to.
 * \param hostnqn The NQN for the host.
 * \param params Transport specific parameters.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn, const struct spdk_json_val *params);

struct spdk_nvmf_host_opts {
	/** Size of this structure */
	size_t				size;
	/** Transport specific parameters */
	const struct spdk_json_val	*params;
	/** DH-HMAC-CHAP key */
	struct spdk_key			*dhchap_key;
	/** DH-HMAC-CHAP controller key */
	struct spdk_key			*dhchap_ctrlr_key;
};

/**
 * Allow the given host to connect to the given subsystem.
 *
 * \param subsystem Subsystem to add host to.
 * \param hostnqn Host's NQN.
 * \param opts Host's options.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_add_host_ext(struct spdk_nvmf_subsystem *subsystem,
				     const char *hostnqn, struct spdk_nvmf_host_opts *opts);

/**
 * Remove the given host NQN from the list of allowed hosts.
 *
 * This call only removes the host from the allowed list of hosts.
 * If a host with the given NQN is already connected it will not be disconnected,
 * but it will not be able to create new connections.
 *
 * \param subsystem Subsystem to remove host from.
 * \param hostnqn The NQN for the host.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/**
 * Disconnect all connections originating from the provided hostnqn
 *
 * To disconnect and block all new connections from a host, first call
 * spdk_nvmf_subsystem_remove_host() to remove it from the list of allowed hosts, then
 * call spdk_nvmf_subsystem_disconnect_host() to close any remaining connections.
 *
 * \param subsystem Subsystem to operate on
 * \param hostnqn The NQN for the host
 * \param cb_fn The function to call on completion.
 * \param cb_arg The argument to pass to the cb_fn.
 *
 * \return int. 0 when the asynchronous process starts successfully or a negated errno on failure.
 */
int spdk_nvmf_subsystem_disconnect_host(struct spdk_nvmf_subsystem *subsystem,
					const char *hostnqn,
					spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
					void *cb_arg);

/**
 * Set whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to modify.
 * \param allow_any_host true to allow any host to connect to this subsystem,
 * or false to enforce the list configured with spdk_nvmf_subsystem_add_host().
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
 * connecting hosts must be in the list configured with spdk_nvmf_subsystem_add_host().
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
const char *spdk_nvmf_host_get_nqn(const struct spdk_nvmf_host *host);

/**
 * Accept new connections on the address provided.
 *
 * This does not start the listener. Use spdk_nvmf_tgt_listen_ext() for that.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * No namespaces are required to be paused.
 *
 * \param subsystem Subsystem to add listener to.
 * \param trid The address to accept connections from.
 * \param cb_fn A callback that will be called once the association is complete.
 * \param cb_arg Argument passed to cb_fn.
 */
void spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvme_transport_id *trid,
				      spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				      void *cb_arg);

/* Additional options for listener creation. */
struct spdk_nvmf_listener_opts {
	/**
	 * The size of spdk_nvmf_listener_opts according to the caller of this library is used for
	 * ABI compatibility. The library uses this field to know how many fields in this structure
	 * are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	/* Secure channel parameter used in TCP TLS. */
	bool secure_channel;

	/* Hole at bytes 9-11. */
	uint8_t reserved1[3];

	/* Asymmetric namespace access state */
	enum spdk_nvme_ana_state ana_state;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listener_opts) == 16, "Incorrect size");

/**
 * Initialize options structure for listener creation.
 *
 * \param opts Options structure to initialize.
 * \param size Size of the structure.
 */
void spdk_nvmf_subsystem_listener_opts_init(struct spdk_nvmf_listener_opts *opts, size_t size);

/**
 * Accept new connections on the address provided.
 *
 * This does not start the listener. Use spdk_nvmf_tgt_listen_ext() for that.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * No namespaces are required to be paused.
 *
 * \param subsystem Subsystem to add listener to.
 * \param trid The address to accept connections from.
 * \param cb_fn A callback that will be called once the association is complete.
 * \param cb_arg Argument passed to cb_fn.
 * \param opts NULL or options requested for listener creation.
 */
void spdk_nvmf_subsystem_add_listener_ext(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvme_transport_id *trid,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
		void *cb_arg, struct spdk_nvmf_listener_opts *opts);

/**
 * Remove the listener from subsystem.
 *
 * New connections to the address won't be propagated to the subsystem.
 * However to stop listening at target level one must use the
 * spdk_nvmf_tgt_stop_listen().
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * No namespaces are required to be paused.
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
		const struct spdk_nvme_transport_id *trid);

/**
 * Get the first allowed listen address in the subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allowed listen address in this subsystem, or NULL if none allowed.
 */
struct spdk_nvmf_subsystem_listener *spdk_nvmf_subsystem_get_first_listener(
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
struct spdk_nvmf_subsystem_listener *spdk_nvmf_subsystem_get_next_listener(
	struct spdk_nvmf_subsystem *subsystem,
	struct spdk_nvmf_subsystem_listener *prev_listener);

/**
 * Get a listen address' transport ID
 *
 * \param listener This listener.
 *
 * \return the transport ID for this listener.
 */
const struct spdk_nvme_transport_id *spdk_nvmf_subsystem_listener_get_trid(
	struct spdk_nvmf_subsystem_listener *listener);

/**
 * Set whether a subsystem should allow any listen address or only addresses in the allowed list.
 *
 * \param subsystem Subsystem to allow dynamic listener assignment.
 * \param allow_any_listener true to allow dynamic listener assignment for
 * this subsystem, or false to enforce the list configured during
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
 *  or false if only allows addresses in the list configured during subsystem setup.
 */
bool spdk_nvmf_subsystem_any_listener_allowed(
	struct spdk_nvmf_subsystem *subsystem);

/**
 * Set whether a subsystem supports Asymmetric Namespace Access (ANA)
 * reporting.
 *
 * May only be performed on subsystems in the INACTIVE state.
 *
 * \param subsystem Subsystem to modify.
 * \param ana_reporting true to support or false not to support ANA reporting.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_set_ana_reporting(struct spdk_nvmf_subsystem *subsystem,
		bool ana_reporting);

/**
 * Get whether a subsystem supports Asymmetric Namespace Access (ANA)
 * reporting.
 *
 * \param subsystem Subsystem to check
 *
 * \return true if subsystem supports ANA reporting, false otherwise.
 */
bool spdk_nvmf_subsystem_get_ana_reporting(struct spdk_nvmf_subsystem *subsystem);

/**
 * Set Asymmetric Namespace Access (ANA) state for the specified ANA group id.
 *
 * May only be performed on subsystems in the INACTIVE or PAUSED state.
 *
 * \param subsystem Subsystem to operate on
 * \param trid Address for which the new state will apply
 * \param ana_state The ANA state which is to be set
 * \param anagrpid The ANA group ID to operate on
 * \param cb_fn The function to call on completion
 * \param cb_arg The argument to pass to the cb_fn
 *
 */
void spdk_nvmf_subsystem_set_ana_state(struct spdk_nvmf_subsystem *subsystem,
				       const struct spdk_nvme_transport_id *trid,
				       enum spdk_nvme_ana_state ana_state, uint32_t anagrpid,
				       spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg);

/**
 * Get Asymmetric Namespace Access (ANA) state for the specified ANA group id.
 *
 * \param subsystem Subsystem to operate on
 * \param trid Address for which the ANA is to be looked up
 * \param anagrpid The ANA group ID to check for
 * \param ana_state Output parameter that will contain the ANA state
 *
 * \return 0 on success, or negated errno value on failure.
 *
 */
int spdk_nvmf_subsystem_get_ana_state(struct spdk_nvmf_subsystem *subsystem,
				      const struct spdk_nvme_transport_id *trid,
				      uint32_t anagrpid,
				      enum spdk_nvme_ana_state *ana_state);

/**
 * Change ANA group ID of a namespace of a subsystem.
 *
 * May only be performed on subsystems in the INACTIVE or PAUSED state.
 *
 * \param subsystem Subsystem the namespace belongs to.
 * \param nsid Namespace ID to change.
 * \param anagrpid A new ANA group ID to set.
 * \param transit_anagrpid This ANA group is used for transit before going to the final
 *        ANA group. If 0, move directly to the final ANA group.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_subsystem_set_ns_ana_group(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid, uint32_t anagrpid,
		uint32_t transit_anagrpid);

/**
 * Sets the controller ID range for a subsystem.
 *
 * Valid range is [1, 0xFFEF].
 * May only be performed on subsystems in the INACTIVE state.
 *
 * \param subsystem Subsystem to modify.
 * \param min_cntlid Minimum controller ID.
 * \param max_cntlid Maximum controller ID.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int spdk_nvmf_subsystem_set_cntlid_range(struct spdk_nvmf_subsystem *subsystem,
		uint16_t min_cntlid, uint16_t max_cntlid);

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

	/* Hole at bytes 44-47. */
	uint8_t reserved44[4];

	/**
	 * The size of spdk_nvmf_ns_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this structure
	 * are valid.  And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	/**
	 * ANA group ID
	 *
	 * Set to be equal with the NSID if not specified.
	 */
	uint32_t anagrpid;

	/**
	 * Do not automatically make namespace visible to controllers
	 *
	 * False if not specified
	 */
	bool no_auto_visible;

	/* Hole at bytes 61-63. */
	uint8_t reserved61[3];

	/* Transport specific json values.
	 *
	 * If transport specific values provided then json object is valid only at the time
	 * namespace is being added. It is transport layer responsibility to maintain
	 * the copy of it or its decoding if required. When \ref spdk_nvmf_ns_get_opts used
	 * after namespace has been added object becomes invalid.
	 */
	const struct spdk_json_val *transport_specific;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ns_opts) == 72, "Incorrect size");

/**
 * Get default namespace creation options.
 *
 * \param opts Namespace options to fill with defaults.
 * \param opts_size sizeof(struct spdk_nvmf_ns_opts)
 */
void spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size);

/**
 * Add a namespace to a subsystems in the PAUSED or INACTIVE states.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add namespace to.
 * \param bdev_name Block device name to add as a namespace.
 * \param opts Namespace options, or NULL to use defaults.
 * \param opts_size sizeof(*opts)
 * \param ptpl_file Persist through power loss file path.
 *
 * \return newly added NSID on success, or 0 on failure.
 */
uint32_t spdk_nvmf_subsystem_add_ns_ext(struct spdk_nvmf_subsystem *subsystem,
					const char *bdev_name,
					const struct spdk_nvmf_ns_opts *opts, size_t opts_size,
					const char *ptpl_file);

/**
 * Remove a namespace from a subsystem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * Additionally, the namespace must be paused.
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
 * Get the minimum controller ID allowed in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return Minimum controller ID allowed in the subsystem.
 */
uint16_t spdk_nvmf_subsystem_get_min_cntlid(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the maximum controller ID allowed in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return Maximum controller ID allowed in the subsystem.
 */
uint16_t spdk_nvmf_subsystem_get_max_cntlid(const struct spdk_nvmf_subsystem *subsystem);

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
const char *spdk_nvmf_subsystem_get_nqn(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the type of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return the type of the specified subsystem.
 */
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get maximum namespace id of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return maximum namespace id
 */
uint32_t spdk_nvmf_subsystem_get_max_nsid(struct spdk_nvmf_subsystem *subsystem);

/**
 * Checks whether a given subsystem is a discovery subsystem
 *
 * \param subsystem Subsystem to check.
 *
 * \return true if a given subsystem is a discovery subsystem, false
 *	   if the subsystem is an nvm subsystem
 */
bool spdk_nvmf_subsystem_is_discovery(struct spdk_nvmf_subsystem *subsystem);


/**
 * Initialize transport options
 *
 * \param transport_name The transport type to create
 * \param opts The transport options (e.g. max_io_size)
 * \param opts_size Must be set to sizeof(struct spdk_nvmf_transport_opts).
 *
 * \return bool. true if successful, false if transport type
 *	   not found.
 */
bool
spdk_nvmf_transport_opts_init(const char *transport_name,
			      struct spdk_nvmf_transport_opts *opts, size_t opts_size);

/**
 * Create a protocol transport - deprecated, please use \ref spdk_nvmf_transport_create_async.
 *
 * \param transport_name The transport type to create
 * \param opts The transport options (e.g. max_io_size). It should not be NULL, and opts_size
 *        pointed in this structure should not be zero value.
 *
 * \return new transport or NULL if create fails
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_create(const char *transport_name,
		struct spdk_nvmf_transport_opts *opts);

typedef void (*spdk_nvmf_transport_create_done_cb)(void *cb_arg,
		struct spdk_nvmf_transport *transport);

/**
 * Create a protocol transport
 *
 * The callback will be executed asynchronously - i.e. spdk_nvmf_transport_create_async will always return
 * prior to `cb_fn` being called.
 *
 * \param transport_name The transport type to create
 * \param opts The transport options (e.g. max_io_size). It should not be NULL, and opts_size
 *        pointed in this structure should not be zero value.
 * \param cb_fn A callback that will be called once the transport is created
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 on success, or negative errno on failure (`cb_fn` will not be executed then).
 */
int spdk_nvmf_transport_create_async(const char *transport_name,
				     struct spdk_nvmf_transport_opts *opts,
				     spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg);

typedef void (*spdk_nvmf_transport_destroy_done_cb)(void *cb_arg);

/**
 * Destroy a protocol transport
 *
 * \param transport The transport to destroy
 * \param cb_fn A callback that will be called once the transport is destroyed
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport,
				spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

/**
 * Get an existing transport from the target
 *
 * \param tgt The NVMe-oF target
 * \param transport_name The name of the transport type to get.
 *
 * \return the transport or NULL if not found
 */
struct spdk_nvmf_transport *spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt,
		const char *transport_name);

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
 * Get the transport name for a given transport.
 *
 * \param transport The transport to query
 *
 * \return the transport name for the given transport
 */
const char *spdk_nvmf_get_transport_name(struct spdk_nvmf_transport *transport);

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
 */
void spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
				 struct spdk_nvmf_transport *transport,
				 spdk_nvmf_tgt_add_transport_done_fn cb_fn,
				 void *cb_arg);

/**
 * Function to be called once target pause is complete.
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_nvmf_tgt_pause_polling_cb_fn)(void *cb_arg, int status);

/**
 * Pause polling on the given target.
 *
 * \param tgt The target to pause
 * \param cb_fn A callback that will be called once the target is paused
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 if it completed successfully, or negative errno if it failed.
 */
int spdk_nvmf_tgt_pause_polling(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_pause_polling_cb_fn cb_fn,
				void *cb_arg);

/**
 * Function to be called once target resume is complete.
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_nvmf_tgt_resume_polling_cb_fn)(void *cb_arg, int status);

/**
 * Resume polling on the given target.
 *
 * \param tgt The target to resume
 * \param cb_fn A callback that will be called once the target is resumed
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 if it completed successfully, or negative errno if it failed.
 */
int spdk_nvmf_tgt_resume_polling(struct spdk_nvmf_tgt *tgt,
				 spdk_nvmf_tgt_resume_polling_cb_fn cb_fn, void *cb_arg);

/**
 * Add listener to transport and begin accepting new connections.
 *
 * \param transport The transport to add listener to.
 * \param trid The address to listen at.
 * \param opts Listener options.
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */
int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid, struct spdk_nvmf_listen_opts *opts);

/**
 * Remove listener from transport and stop accepting new connections.
 *
 * \param transport The transport to remove listener from
 * \param trid Address to stop listen at
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */
int
spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid);

/**
 * Stop accepting new connections at the provided address.
 *
 * This is a counterpart to spdk_nvmf_tgt_listen_ext(). It differs
 * from spdk_nvmf_transport_stop_listen() in that it also destroys
 * qpairs that are connected to the specified listener. Because
 * this function disconnects the qpairs, it has to be asynchronous.
 *
 * The subsystem is matched using the subsystem parameter, not the
 * subnqn field in the trid.
 *
 * \param transport The transport associated with the listen address.
 * \param trid The address to stop listening at. subnqn must be an empty
 *             string.
 * \param subsystem The subsystem to match for qpairs with the specified
 *                  trid. If NULL, it will disconnect all qpairs with the
 *                  specified trid.
 * \param cb_fn The function to call on completion.
 * \param cb_arg The argument to pass to the cb_fn.
 *
 * \return int. 0 when the asynchronous process starts successfully or a negated errno on failure.
 */
int spdk_nvmf_transport_stop_listen_async(struct spdk_nvmf_transport *transport,
		const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_subsystem *subsystem,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
		void *cb_arg);

/**
 * Dump poll group statistics into JSON.
 *
 * \param group The group which statistics should be dumped.
 * \param w The JSON write context to which statistics should be dumped.
 */
void spdk_nvmf_poll_group_dump_stat(struct spdk_nvmf_poll_group *group,
				    struct spdk_json_write_ctx *w);

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

/* Maximum number of registrants supported per namespace */
#define SPDK_NVMF_MAX_NUM_REGISTRANTS		16

struct spdk_nvmf_registrant_info {
	uint64_t		rkey;
	char			host_uuid[SPDK_UUID_STRING_LEN];
};

struct spdk_nvmf_reservation_info {
	uint64_t				crkey;
	uint8_t					rtype;
	uint8_t					ptpl_activated;
	char					bdev_uuid[SPDK_UUID_STRING_LEN];
	char					holder_uuid[SPDK_UUID_STRING_LEN];
	uint8_t					reserved[3];
	uint8_t					num_regs;
	struct spdk_nvmf_registrant_info	registrants[SPDK_NVMF_MAX_NUM_REGISTRANTS];
};

struct spdk_nvmf_ns_reservation_ops {
	/* Checks if the namespace supports the Persist Through Power Loss capability. */
	bool (*is_ptpl_capable)(const struct spdk_nvmf_ns *ns);

	/* Called when namespace reservation information needs to be updated.
	 * The new reservation information is provided via the info parameter.
	 * Returns 0 on success, negated errno on failure. */
	int (*update)(const struct spdk_nvmf_ns *ns, const struct spdk_nvmf_reservation_info *info);

	/* Called when restoring the namespace reservation information.
	 * The new reservation information is returned via the info parameter.
	 * Returns 0 on success, negated errno on failure. */
	int (*load)(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info);
};

/**
 * Set custom handlers for namespace reservation operations.
 *
 * This call allows to override the default namespace reservation operations with custom handlers.
 * This function may only be called before any namespace has been added.
 *
 * @param ops The reservation ops handers
 */
void spdk_nvmf_set_custom_ns_reservation_ops(const struct spdk_nvmf_ns_reservation_ops *ops);

#ifdef __cplusplus
}
#endif

#endif
