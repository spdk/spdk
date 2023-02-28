/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2021 Mellanox Technologies LTD. All rights reserved.
 */

/** \file
 * NVMe-oF Target transport plugin API
 */

#ifndef SPDK_NVMF_TRANSPORT_H_
#define SPDK_NVMF_TRANSPORT_H_

#include "spdk/bdev.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/nvmf_spec.h"
#include "spdk/memory.h"

#define SPDK_NVMF_MAX_SGL_ENTRIES	16

/* The maximum number of buffers per request */
#define NVMF_REQ_MAX_BUFFERS	(SPDK_NVMF_MAX_SGL_ENTRIES * 2 + 1)

/* Maximum pending AERs that can be migrated */
#define SPDK_NVMF_MIGR_MAX_PENDING_AERS 256

#define SPDK_NVMF_MAX_ASYNC_EVENTS 4

/* AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define NVMF_DATA_BUFFER_ALIGNMENT	VALUE_4KB
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1LL)

#define SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US 10000

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
	uint32_t				elba_length;
	uint32_t				orig_length;
};

struct spdk_nvmf_stripped_data {
	uint32_t			iovcnt;
	struct iovec			iov[NVMF_REQ_MAX_BUFFERS];
	void				*buffers[NVMF_REQ_MAX_BUFFERS];
};

enum spdk_nvmf_zcopy_phase {
	NVMF_ZCOPY_PHASE_NONE,        /* Request is not using ZCOPY */
	NVMF_ZCOPY_PHASE_INIT,        /* Requesting Buffers */
	NVMF_ZCOPY_PHASE_EXECUTE,     /* Got buffers processing commands */
	NVMF_ZCOPY_PHASE_END_PENDING, /* Releasing buffers */
	NVMF_ZCOPY_PHASE_COMPLETE,    /* Buffers Released */
	NVMF_ZCOPY_PHASE_INIT_FAILED  /* Failed to get the buffers */
};

struct spdk_nvmf_request {
	struct spdk_nvmf_qpair		*qpair;
	uint32_t			length;
	uint8_t				xfer; /* type enum spdk_nvme_data_transfer */
	bool				data_from_pool;
	bool				dif_enabled;
	void				*data;
	union nvmf_h2c_msg		*cmd;
	union nvmf_c2h_msg		*rsp;
	STAILQ_ENTRY(spdk_nvmf_request)	buf_link;
	uint64_t			timeout_tsc;

	uint32_t			iovcnt;
	struct iovec			iov[NVMF_REQ_MAX_BUFFERS];
	void				*buffers[NVMF_REQ_MAX_BUFFERS];
	struct spdk_nvmf_stripped_data  *stripped_data;

	struct spdk_nvmf_dif_info	dif;

	struct spdk_bdev_io_wait_entry	bdev_io_wait;
	spdk_nvmf_nvme_passthru_cmd_cb	cmd_cb_fn;
	struct spdk_nvmf_request	*first_fused_req;
	struct spdk_nvmf_request	*req_to_abort;
	struct spdk_poller		*poller;
	struct spdk_bdev_io		*zcopy_bdev_io; /* Contains the bdev_io when using ZCOPY */
	enum spdk_nvmf_zcopy_phase	zcopy_phase;

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
	bool					connect_received;
	bool					disconnect_started;

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

	/* Protected by mutex. Counts qpairs that have connected at a
	 * transport level, but are not associated with a subsystem
	 * or controller yet (because the CONNECT capsule hasn't
	 * been received). */
	uint32_t					current_unassociated_qpairs;

	/* All of the queue pairs that belong to this poll group */
	TAILQ_HEAD(, spdk_nvmf_qpair)			qpairs;

	/* Statistics */
	struct spdk_nvmf_poll_group_stat		stat;

	spdk_nvmf_poll_group_destroy_done_fn		destroy_cb_fn;
	void						*destroy_cb_arg;

	TAILQ_ENTRY(spdk_nvmf_poll_group)		link;

	pthread_mutex_t					mutex;
};

struct spdk_nvmf_listener {
	struct spdk_nvme_transport_id	trid;
	uint32_t			ref;

	TAILQ_ENTRY(spdk_nvmf_listener)	link;
};

/**
 * A subset of struct spdk_nvme_ctrlr_data that are emulated by a fabrics device.
 */
struct spdk_nvmf_ctrlr_data {
	uint8_t aerl;
	uint16_t kas;
	/** pci vendor id */
	uint16_t vid;
	/** pci subsystem vendor id */
	uint16_t ssvid;
	/** ieee oui identifier */
	uint8_t ieee[3];
	struct spdk_nvme_cdata_oacs oacs;
	struct spdk_nvme_cdata_oncs oncs;
	struct spdk_nvme_cdata_fuses fuses;
	struct spdk_nvme_cdata_sgls sgls;
	struct spdk_nvme_cdata_nvmf_specific nvmf_specific;
};

struct spdk_nvmf_transport {
	struct spdk_nvmf_tgt			*tgt;
	const struct spdk_nvmf_transport_ops	*ops;
	struct spdk_nvmf_transport_opts		opts;

	/* A mempool for transport related data transfers */
	struct spdk_mempool			*data_buf_pool;

	TAILQ_HEAD(, spdk_nvmf_listener)	listeners;
	TAILQ_ENTRY(spdk_nvmf_transport)	link;

	pthread_mutex_t				mutex;
};

typedef void (*spdk_nvmf_transport_qpair_fini_cb)(void *cb_arg);

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
	 * Create a transport for the given transport opts. Either synchronous
	 * or asynchronous version shall be implemented.
	 */
	struct spdk_nvmf_transport *(*create)(struct spdk_nvmf_transport_opts *opts);
	int (*create_async)(struct spdk_nvmf_transport_opts *opts, spdk_nvmf_transport_create_done_cb cb_fn,
			    void *cb_arg);

	/**
	 * Dump transport-specific opts into JSON
	 */
	void (*dump_opts)(struct spdk_nvmf_transport *transport,
			  struct spdk_json_write_ctx *w);

	/**
	 * Destroy the transport
	 */
	int (*destroy)(struct spdk_nvmf_transport *transport,
		       spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

	/**
	  * Instruct the transport to accept new connections at the address
	  * provided. This may be called multiple times.
	  */
	int (*listen)(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvmf_listen_opts *opts);

	/**
	 * Dump transport-specific listen opts into JSON
	 */
	void (*listen_dump_opts)(struct spdk_nvmf_transport *transport,
				 const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w);

	/**
	  * Stop accepting new connections at the given address.
	  */
	void (*stop_listen)(struct spdk_nvmf_transport *transport,
			    const struct spdk_nvme_transport_id *trid);

	/**
	 * It is a notification that a listener is being associated with the subsystem.
	 * Most transports will not need to take any action here, as the enforcement
	 * of the association is done in the generic code.
	 *
	 * Returns a negated errno code to block the association. 0 to allow.
	 */
	int (*listen_associate)(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem,
				const struct spdk_nvme_transport_id *trid);

	/**
	 * It is a notification that a namespace is being added to the subsystem.
	 * Most transports will not need to take any action here.
	 *
	 * Returns a negated errno code to block the attachment. 0 to allow.
	 */
	int (*subsystem_add_ns)(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *ns);

	/**
	 * It is a notification that a namespace has been removed from the subsystem.
	 * Most transports will not need to take any action here.
	 */
	void (*subsystem_remove_ns)(struct spdk_nvmf_transport *transport,
				    const struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

	/**
	 * Initialize subset of identify controller data.
	 */
	void (*cdata_init)(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
			   struct spdk_nvmf_ctrlr_data *cdata);

	/**
	 * Fill out a discovery log entry for a specific listen address.
	 */
	void (*listener_discover)(struct spdk_nvmf_transport *transport,
				  struct spdk_nvme_transport_id *trid,
				  struct spdk_nvmf_discovery_log_page_entry *entry);

	/**
	 * Create a new poll group
	 */
	struct spdk_nvmf_transport_poll_group *(*poll_group_create)(struct spdk_nvmf_transport *transport,
			struct spdk_nvmf_poll_group *group);

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
	void (*qpair_fini)(struct spdk_nvmf_qpair *qpair,
			   spdk_nvmf_transport_qpair_fini_cb cb_fn,
			   void *cb_args);

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
	 * Abort the request which the abort request specifies.
	 * This function can complete synchronously or asynchronously, but
	 * is expected to call spdk_nvmf_request_complete() in the end
	 * for both cases.
	 */
	void (*qpair_abort_request)(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvmf_request *req);

	/*
	 * Dump transport poll group statistics into JSON.
	 */
	void (*poll_group_dump_stat)(struct spdk_nvmf_transport_poll_group *group,
				     struct spdk_json_write_ctx *w);
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

int spdk_nvmf_ctrlr_connect(struct spdk_nvmf_request *req);

/**
 * Function to be called for each newly discovered qpair.
 *
 * \param tgt The nvmf target
 * \param qpair The newly discovered qpair.
 */
void spdk_nvmf_tgt_new_qpair(struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair);

/**
 * A subset of struct spdk_nvme_registers that are emulated by a fabrics device.
 */
struct spdk_nvmf_registers {
	union spdk_nvme_cap_register	cap;
	union spdk_nvme_vs_register	vs;
	union spdk_nvme_cc_register	cc;
	union spdk_nvme_csts_register	csts;
	union spdk_nvme_aqa_register	aqa;
	uint64_t			asq;
	uint64_t			acq;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_registers) == 40, "Incorrect size");

const struct spdk_nvmf_registers *spdk_nvmf_ctrlr_get_regs(struct spdk_nvmf_ctrlr *ctrlr);

void spdk_nvmf_request_free_buffers(struct spdk_nvmf_request *req,
				    struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_nvmf_transport *transport);
int spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
				  struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_transport *transport,
				  uint32_t length);

bool spdk_nvmf_request_get_dif_ctx(struct spdk_nvmf_request *req, struct spdk_dif_ctx *dif_ctx);

void spdk_nvmf_request_exec(struct spdk_nvmf_request *req);
void spdk_nvmf_request_exec_fabrics(struct spdk_nvmf_request *req);
int spdk_nvmf_request_free(struct spdk_nvmf_request *req);
int spdk_nvmf_request_complete(struct spdk_nvmf_request *req);
void spdk_nvmf_request_zcopy_start(struct spdk_nvmf_request *req);
void spdk_nvmf_request_zcopy_end(struct spdk_nvmf_request *req, bool commit);

static inline bool
spdk_nvmf_request_using_zcopy(const struct spdk_nvmf_request *req)
{
	return req->zcopy_phase != NVMF_ZCOPY_PHASE_NONE;
}

/**
 * Remove the given qpair from the poll group.
 *
 * \param qpair The qpair to remove.
 */
void spdk_nvmf_poll_group_remove(struct spdk_nvmf_qpair *qpair);

/**
 * Get the NVMe-oF subsystem associated with this controller.
 *
 * \param ctrlr The NVMe-oF controller
 *
 * \return The NVMe-oF subsystem
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_ctrlr_get_subsystem(struct spdk_nvmf_ctrlr *ctrlr);

/**
 * Get the NVMe-oF controller ID.
 *
 * \param ctrlr The NVMe-oF controller
 *
 * \return The NVMe-oF controller ID
 */
uint16_t spdk_nvmf_ctrlr_get_id(struct spdk_nvmf_ctrlr *ctrlr);

struct spdk_nvmf_ctrlr_feat {
	union spdk_nvme_feat_arbitration arbitration;
	union spdk_nvme_feat_power_management power_management;
	union spdk_nvme_feat_error_recovery error_recovery;
	union spdk_nvme_feat_volatile_write_cache volatile_write_cache;
	union spdk_nvme_feat_number_of_queues number_of_queues;
	union spdk_nvme_feat_interrupt_coalescing interrupt_coalescing;
	union spdk_nvme_feat_interrupt_vector_configuration interrupt_vector_configuration;
	union spdk_nvme_feat_write_atomicity write_atomicity;
	union spdk_nvme_feat_async_event_configuration async_event_configuration;
	union spdk_nvme_feat_keep_alive_timer keep_alive_timer;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr_feat) == 40, "Incorrect size");

/*
 * Migration data structure used to save & restore a NVMe-oF controller.
 *
 * The data structure is experimental.
 *
 */
struct spdk_nvmf_ctrlr_migr_data {
	/* `data_size` is valid size of `spdk_nvmf_ctrlr_migr_data` without counting `unused`.
	 * We use this field to migrate `spdk_nvmf_ctrlr_migr_data` from source VM and restore
	 * it in destination VM.
	 */
	uint32_t data_size;
	/* `regs_size` is valid size of `spdk_nvmf_registers`. */
	uint32_t regs_size;
	/* `feat_size` is valid size of `spdk_nvmf_ctrlr_feat`. */
	uint32_t feat_size;
	uint32_t reserved;

	struct spdk_nvmf_registers regs;
	uint8_t regs_reserved[216];

	struct spdk_nvmf_ctrlr_feat feat;
	uint8_t feat_reserved[216];

	uint16_t cntlid;
	uint8_t acre;
	uint8_t num_aer_cids;
	uint32_t num_async_events;

	union spdk_nvme_async_event_completion async_events[SPDK_NVMF_MIGR_MAX_PENDING_AERS];
	uint16_t aer_cids[SPDK_NVMF_MAX_ASYNC_EVENTS];
	uint64_t notice_aen_mask;

	uint8_t unused[2516];
};
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ctrlr_migr_data,
			    regs) - offsetof(struct spdk_nvmf_ctrlr_migr_data, data_size) == 16, "Incorrect header size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ctrlr_migr_data,
			    feat) - offsetof(struct spdk_nvmf_ctrlr_migr_data, regs) == 256, "Incorrect regs size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ctrlr_migr_data,
			    cntlid) - offsetof(struct spdk_nvmf_ctrlr_migr_data, feat) == 256, "Incorrect feat size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr_migr_data) == 4096, "Incorrect size");

/**
 * Save the NVMe-oF controller state and configuration.
 *
 * The API is experimental.
 *
 * It is allowed to save the data only when the nvmf subystem is in paused
 * state i.e. there are no outstanding cmds in nvmf layer (other than aer),
 * pending async event completions are getting blocked.
 *
 * To preserve thread safety this function must be executed on the same thread
 * the NVMe-OF controller was created.
 *
 * \param ctrlr The NVMe-oF controller
 * \param data The NVMe-oF controller state and configuration to be saved
 *
 * \return 0 on success or a negated errno on failure.
 */
int spdk_nvmf_ctrlr_save_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvmf_ctrlr_migr_data *data);

/**
 * Restore the NVMe-oF controller state and configuration.
 *
 * The API is experimental.
 *
 * It is allowed to restore the data only when the nvmf subystem is in paused
 * state.
 *
 * To preserve thread safety this function must be executed on the same thread
 * the NVMe-OF controller was created.
 *
 * AERs shall be restored using spdk_nvmf_request_exec after this function is executed.
 *
 * \param ctrlr The NVMe-oF controller
 * \param data The NVMe-oF controller state and configuration to be restored
 *
 * \return 0 on success or a negated errno on failure.
 */
int spdk_nvmf_ctrlr_restore_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
				      const struct spdk_nvmf_ctrlr_migr_data *data);

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
static void __attribute__((constructor)) _spdk_nvmf_transport_register_##name(void) \
{ \
	spdk_nvmf_transport_register(transport_ops); \
}

#endif
