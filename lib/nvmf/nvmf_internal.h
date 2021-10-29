/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#ifndef __NVMF_INTERNAL_H__
#define __NVMF_INTERNAL_H__

#include "spdk/stdinc.h"

#include "spdk/likely.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/nvmf_transport.h"
#include "spdk/nvmf_spec.h"
#include "spdk/assert.h"
#include "spdk/bdev.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/thread.h"

#define NVMF_MAX_ASYNC_EVENTS	(4)

/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
#define NVMF_MIN_CNTLID 1
#define NVMF_MAX_CNTLID 0xFFEF

enum spdk_nvmf_subsystem_state {
	SPDK_NVMF_SUBSYSTEM_INACTIVE = 0,
	SPDK_NVMF_SUBSYSTEM_ACTIVATING,
	SPDK_NVMF_SUBSYSTEM_ACTIVE,
	SPDK_NVMF_SUBSYSTEM_PAUSING,
	SPDK_NVMF_SUBSYSTEM_PAUSED,
	SPDK_NVMF_SUBSYSTEM_RESUMING,
	SPDK_NVMF_SUBSYSTEM_DEACTIVATING,
	SPDK_NVMF_SUBSYSTEM_NUM_STATES,
};

struct spdk_nvmf_tgt {
	char					name[NVMF_TGT_NAME_MAX_LENGTH];

	pthread_mutex_t				mutex;

	uint64_t				discovery_genctr;

	uint32_t				max_subsystems;

	enum spdk_nvmf_tgt_discovery_filter	discovery_filter;

	/* Array of subsystem pointers of size max_subsystems indexed by sid */
	struct spdk_nvmf_subsystem		**subsystems;

	TAILQ_HEAD(, spdk_nvmf_transport)	transports;
	TAILQ_HEAD(, spdk_nvmf_poll_group)	poll_groups;

	/* Used for round-robin assignment of connections to poll groups */
	struct spdk_nvmf_poll_group		*next_poll_group;

	spdk_nvmf_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;

	uint16_t				crdt[3];

	TAILQ_ENTRY(spdk_nvmf_tgt)		link;
};

struct spdk_nvmf_host {
	char				nqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	TAILQ_ENTRY(spdk_nvmf_host)	link;
};

struct spdk_nvmf_subsystem_listener {
	struct spdk_nvmf_subsystem			*subsystem;
	spdk_nvmf_tgt_subsystem_listen_done_fn		cb_fn;
	void						*cb_arg;
	struct spdk_nvme_transport_id			*trid;
	struct spdk_nvmf_transport			*transport;
	enum spdk_nvme_ana_state			*ana_state;
	uint64_t					ana_state_change_count;
	uint16_t					id;
	TAILQ_ENTRY(spdk_nvmf_subsystem_listener)	link;
};

/* Maximum number of registrants supported per namespace */
#define SPDK_NVMF_MAX_NUM_REGISTRANTS		16

struct spdk_nvmf_registrant_info {
	uint64_t		rkey;
	char			host_uuid[SPDK_UUID_STRING_LEN];
};

struct spdk_nvmf_reservation_info {
	bool					ptpl_activated;
	enum spdk_nvme_reservation_type		rtype;
	uint64_t				crkey;
	char					bdev_uuid[SPDK_UUID_STRING_LEN];
	char					holder_uuid[SPDK_UUID_STRING_LEN];
	uint32_t				num_regs;
	struct spdk_nvmf_registrant_info	registrants[SPDK_NVMF_MAX_NUM_REGISTRANTS];
};

struct spdk_nvmf_subsystem_pg_ns_info {
	struct spdk_io_channel		*channel;
	struct spdk_uuid		uuid;
	/* current reservation key, no reservation if the value is 0 */
	uint64_t			crkey;
	/* reservation type */
	enum spdk_nvme_reservation_type	rtype;
	/* Host ID which holds the reservation */
	struct spdk_uuid		holder_id;
	/* Host ID for the registrants with the namespace */
	struct spdk_uuid		reg_hostid[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint64_t			num_blocks;

	/* I/O outstanding to this namespace */
	uint64_t			io_outstanding;
	enum spdk_nvmf_subsystem_state	state;
};

typedef void(*spdk_nvmf_poll_group_mod_done)(void *cb_arg, int status);

struct spdk_nvmf_subsystem_poll_group {
	/* Array of namespace information for each namespace indexed by nsid - 1 */
	struct spdk_nvmf_subsystem_pg_ns_info	*ns_info;
	uint32_t				num_ns;

	/* Number of ADMIN and FABRICS requests outstanding */
	uint64_t				mgmt_io_outstanding;
	spdk_nvmf_poll_group_mod_done		cb_fn;
	void					*cb_arg;

	enum spdk_nvmf_subsystem_state		state;

	TAILQ_HEAD(, spdk_nvmf_request)		queued;
};

struct spdk_nvmf_registrant {
	TAILQ_ENTRY(spdk_nvmf_registrant) link;
	struct spdk_uuid hostid;
	/* Registration key */
	uint64_t rkey;
};

struct spdk_nvmf_ns {
	uint32_t nsid;
	uint32_t anagrpid;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_nvmf_ns_opts opts;
	/* reservation notification mask */
	uint32_t mask;
	/* generation code */
	uint32_t gen;
	/* registrants head */
	TAILQ_HEAD(, spdk_nvmf_registrant) registrants;
	/* current reservation key */
	uint64_t crkey;
	/* reservation type */
	enum spdk_nvme_reservation_type rtype;
	/* current reservation holder, only valid if reservation type can only have one holder */
	struct spdk_nvmf_registrant *holder;
	/* Persist Through Power Loss file which contains the persistent reservation */
	char *ptpl_file;
	/* Persist Through Power Loss feature is enabled */
	bool ptpl_activated;
	/* ZCOPY supported on bdev device */
	bool zcopy;
};

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

/*
 * NVMf reservation notification log page.
 */
struct spdk_nvmf_reservation_log {
	struct spdk_nvme_reservation_notification_log	log;
	TAILQ_ENTRY(spdk_nvmf_reservation_log)		link;
	struct spdk_nvmf_ctrlr				*ctrlr;
};

/*
 * NVMf async event completion.
 */
struct spdk_nvmf_async_event_completion {
	union spdk_nvme_async_event_completion		event;
	STAILQ_ENTRY(spdk_nvmf_async_event_completion)	link;
};

/*
 * This structure represents an NVMe-oF controller,
 * which is like a "session" in networking terms.
 */
struct spdk_nvmf_ctrlr {
	uint16_t			cntlid;
	char				hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	struct spdk_nvmf_subsystem	*subsys;

	struct spdk_nvmf_ctrlr_data	cdata;

	struct spdk_nvmf_registers	vcprop;

	struct spdk_nvmf_ctrlr_feat feat;

	struct spdk_nvmf_qpair	*admin_qpair;
	struct spdk_thread	*thread;
	struct spdk_bit_array	*qpair_mask;

	const struct spdk_nvmf_subsystem_listener	*listener;

	struct spdk_nvmf_request *aer_req[NVMF_MAX_ASYNC_EVENTS];
	STAILQ_HEAD(, spdk_nvmf_async_event_completion) async_events;
	uint64_t notice_aen_mask;
	uint8_t nr_aer_reqs;
	struct spdk_uuid  hostid;

	uint32_t association_timeout; /* in milliseconds */
	uint16_t changed_ns_list_count;
	struct spdk_nvme_ns_list changed_ns_list;
	uint64_t log_page_count;
	uint8_t num_avail_log_pages;
	TAILQ_HEAD(log_page_head, spdk_nvmf_reservation_log) log_head;

	/* Time to trigger keep-alive--poller_time = now_tick + period */
	uint64_t			last_keep_alive_tick;
	struct spdk_poller		*keep_alive_poller;

	struct spdk_poller		*association_timer;

	struct spdk_poller		*cc_timer;
	uint64_t			cc_timeout_tsc;
	struct spdk_poller		*cc_timeout_timer;

	bool				dif_insert_or_strip;
	bool				in_destruct;
	bool				disconnect_in_progress;
	/* valid only when disconnect_in_progress is true */
	bool				disconnect_is_shn;
	bool				acre_enabled;
	bool				dynamic_ctrlr;

	TAILQ_ENTRY(spdk_nvmf_ctrlr)	link;
};

/* Maximum pending AERs that can be migrated */
#define NVMF_MIGR_MAX_PENDING_AERS 256

/* spdk_nvmf_ctrlr private migration data structure used to save/restore a controller */
struct nvmf_ctrlr_migr_data {
	uint32_t				opts_size;

	uint16_t				cntlid;
	uint8_t					reserved1[2];

	struct spdk_nvmf_ctrlr_feat		feat;
	uint32_t				reserved2[2];

	uint32_t				num_async_events;
	uint32_t				acre_enabled;
	uint64_t				notice_aen_mask;
	union spdk_nvme_async_event_completion	async_events[NVMF_MIGR_MAX_PENDING_AERS];

	/* New fields shouldn't go after reserved3 */
	uint8_t					reserved3[3000];
};
SPDK_STATIC_ASSERT(sizeof(struct nvmf_ctrlr_migr_data) == 0x1000, "Incorrect size");

#define NVMF_MAX_LISTENERS_PER_SUBSYSTEM	16

struct spdk_nvmf_subsystem {
	struct spdk_thread				*thread;

	uint32_t					id;

	enum spdk_nvmf_subsystem_state			state;
	enum spdk_nvmf_subtype				subtype;

	uint16_t					next_cntlid;
	struct {
		uint8_t					allow_any_host : 1;
		uint8_t					allow_any_listener : 1;
		uint8_t					ana_reporting : 1;
		uint8_t					reserved : 5;
	} flags;

	/* boolean for state change synchronization */
	bool						changing_state;

	bool						destroying;
	bool						async_destroy;

	struct spdk_nvmf_tgt				*tgt;

	/* Array of pointers to namespaces of size max_nsid indexed by nsid - 1 */
	struct spdk_nvmf_ns				**ns;
	uint32_t					max_nsid;

	uint16_t					min_cntlid;
	uint16_t					max_cntlid;

	TAILQ_HEAD(, spdk_nvmf_ctrlr)			ctrlrs;

	/* A mutex used to protect the hosts list and allow_any_host flag. Unlike the namespace
	 * array, this list is not used on the I/O path (it's needed for handling things like
	 * the CONNECT command), so use a mutex to protect it instead of requiring the subsystem
	 * state to be paused. This removes the requirement to pause the subsystem when hosts
	 * are added or removed dynamically. */
	pthread_mutex_t					mutex;
	TAILQ_HEAD(, spdk_nvmf_host)			hosts;
	TAILQ_HEAD(, spdk_nvmf_subsystem_listener)	listeners;
	struct spdk_bit_array				*used_listener_ids;

	TAILQ_ENTRY(spdk_nvmf_subsystem)		entries;

	nvmf_subsystem_destroy_cb			async_destroy_cb;
	void						*async_destroy_cb_arg;

	char						sn[SPDK_NVME_CTRLR_SN_LEN + 1];
	char						mn[SPDK_NVME_CTRLR_MN_LEN + 1];
	char						subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

	/* Array of namespace count per ANA group of size max_nsid indexed anagrpid - 1
	 * It will be enough for ANA group to use the same size as namespaces.
	 */
	uint32_t					*ana_group;
};

int nvmf_poll_group_add_transport(struct spdk_nvmf_poll_group *group,
				  struct spdk_nvmf_transport *transport);
int nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				     struct spdk_nvmf_subsystem *subsystem);
int nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
				  struct spdk_nvmf_subsystem *subsystem,
				  spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);
void nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);
void nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				     struct spdk_nvmf_subsystem *subsystem,
				     uint32_t nsid,
				     spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);
void nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);

void nvmf_update_discovery_log(struct spdk_nvmf_tgt *tgt, const char *hostnqn);
void nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, const char *hostnqn, struct iovec *iov,
				 uint32_t iovcnt, uint64_t offset, uint32_t length,
				 struct spdk_nvme_transport_id *cmd_source_trid);

void nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req);
int nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req);
bool nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr);
bool nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid);
bool nvmf_ctrlr_use_zcopy(struct spdk_nvmf_request *req);

void nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
				 bool dif_insert_or_strip);
int nvmf_bdev_ctrlr_read_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
int nvmf_bdev_ctrlr_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			      struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
int nvmf_bdev_ctrlr_compare_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
int nvmf_bdev_ctrlr_compare_and_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req);
int nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
int nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			      struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
int nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
int nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);
bool nvmf_bdev_ctrlr_get_dif_ctx(struct spdk_bdev *bdev, struct spdk_nvme_cmd *cmd,
				 struct spdk_dif_ctx *dif_ctx);
bool nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev);

int nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			     struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_subsystem_remove_all_listeners(struct spdk_nvmf_subsystem *subsystem,
		bool stop);
struct spdk_nvmf_ctrlr *nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem,
		uint16_t cntlid);
struct spdk_nvmf_subsystem_listener *nvmf_subsystem_find_listener(
	struct spdk_nvmf_subsystem *subsystem,
	const struct spdk_nvme_transport_id *trid);
struct spdk_nvmf_listener *nvmf_transport_find_listener(
	struct spdk_nvmf_transport *transport,
	const struct spdk_nvme_transport_id *trid);
void nvmf_transport_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w,
			      bool named);
void nvmf_transport_listen_dump_opts(struct spdk_nvmf_transport *transport,
				     const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w);
void nvmf_subsystem_set_ana_state(struct spdk_nvmf_subsystem *subsystem,
				  const struct spdk_nvme_transport_id *trid,
				  enum spdk_nvme_ana_state ana_state, uint32_t anagrpid,
				  spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg);
bool nvmf_subsystem_get_ana_reporting(struct spdk_nvmf_subsystem *subsystem);

/**
 * Sets the controller ID range for a subsystem.
 * Valid range is [1, 0xFFEF].
 *
 * May only be performed on subsystems in the INACTIVE state.
 *
 * \param subsystem Subsystem to modify.
 * \param min_cntlid Minimum controller ID.
 * \param max_cntlid Maximum controller ID.
 *
 * \return 0 on success, or negated errno value on failure.
 */
int nvmf_subsystem_set_cntlid_range(struct spdk_nvmf_subsystem *subsystem,
				    uint16_t min_cntlid, uint16_t max_cntlid);

int nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_async_event_discovery_log_change_notice(struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_async_event_error_event(struct spdk_nvmf_ctrlr *ctrlr,
				       union spdk_nvme_async_event_completion event);
void nvmf_ns_reservation_request(void *ctx);
void nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvmf_ns *ns,
				       enum spdk_nvme_reservation_notification_log_page_type type);

/*
 * Abort aer is sent on a per controller basis and sends a completion for the aer to the host.
 * This function should be called when attempting to recover in error paths when it is OK for
 * the host to send a subsequent AER.
 */
void nvmf_ctrlr_abort_aer(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_save_aers(struct spdk_nvmf_ctrlr *ctrlr, uint16_t *aer_cids,
			 uint16_t max_aers);

int nvmf_ctrlr_save_migr_data(struct spdk_nvmf_ctrlr *ctrlr, struct nvmf_ctrlr_migr_data *data);
int nvmf_ctrlr_restore_migr_data(struct spdk_nvmf_ctrlr *ctrlr, struct nvmf_ctrlr_migr_data *data);

/*
 * Abort zero-copy requests that already got the buffer (received zcopy_start cb), but haven't
 * started zcopy_end.  These requests are kept on the outstanding queue, but are not waiting for a
 * completion from the bdev layer, so, when a qpair is being disconnected, we need to kick them to
 * force their completion.
 */
void nvmf_qpair_abort_pending_zcopy_reqs(struct spdk_nvmf_qpair *qpair);

/*
 * Free aer simply frees the rdma resources for the aer without informing the host.
 * This function should be called when deleting a qpair when one wants to make sure
 * the qpair is completely empty before freeing the request. The reason we free the
 * AER without sending a completion is to prevent the host from sending another AER.
 */
void nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair);

int nvmf_ctrlr_abort_request(struct spdk_nvmf_request *req);

static inline struct spdk_nvmf_ns *
_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
	if (spdk_unlikely(nsid - 1 >= subsystem->max_nsid)) {
		return NULL;
	}

	return subsystem->ns[nsid - 1];
}

static inline bool
nvmf_qpair_is_admin_queue(struct spdk_nvmf_qpair *qpair)
{
	return qpair->qid == 0;
}

/**
 * Initiates a zcopy start operation
 *
 * \param bdev The \ref spdk_bdev
 * \param desc The \ref spdk_bdev_desc
 * \param ch The \ref spdk_io_channel
 * \param req The \ref spdk_nvmf_request passed to the bdev for processing
 *
 * \return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE if the command was completed immediately or
 *         SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS if the command was submitted and will be
 *         completed asynchronously.  Asynchronous completions are notified through
 *         spdk_nvmf_request_complete().
 */
int nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
				struct spdk_bdev_desc *desc,
				struct spdk_io_channel *ch,
				struct spdk_nvmf_request *req);

/**
 * Ends a zcopy operation
 *
 * \param req The NVMe-oF request
 * \param commit Flag indicating whether the buffers should be committed
 */
void nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit);

#endif /* __NVMF_INTERNAL_H__ */
