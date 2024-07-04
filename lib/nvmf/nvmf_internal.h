/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __NVMF_INTERNAL_H__
#define __NVMF_INTERNAL_H__

#include "spdk/stdinc.h"

#include "spdk/keyring.h"
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
#include "spdk/tree.h"
#include "spdk/bit_array.h"

/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
#define NVMF_MIN_CNTLID 1
#define NVMF_MAX_CNTLID 0xFFEF

enum spdk_nvmf_tgt_state {
	NVMF_TGT_IDLE = 0,
	NVMF_TGT_RUNNING,
	NVMF_TGT_PAUSING,
	NVMF_TGT_PAUSED,
	NVMF_TGT_RESUMING,
};

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

RB_HEAD(subsystem_tree, spdk_nvmf_subsystem);

struct spdk_nvmf_tgt {
	char					name[NVMF_TGT_NAME_MAX_LENGTH];

	pthread_mutex_t				mutex;

	uint64_t				discovery_genctr;

	uint32_t				max_subsystems;

	uint32_t				discovery_filter;

	enum spdk_nvmf_tgt_state                state;

	struct spdk_bit_array			*subsystem_ids;

	struct subsystem_tree			subsystems;

	TAILQ_HEAD(, spdk_nvmf_transport)	transports;
	TAILQ_HEAD(, spdk_nvmf_poll_group)	poll_groups;
	TAILQ_HEAD(, spdk_nvmf_referral)	referrals;

	/* Used for round-robin assignment of connections to poll groups */
	struct spdk_nvmf_poll_group		*next_poll_group;

	spdk_nvmf_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;

	uint16_t				crdt[3];
	uint16_t				num_poll_groups;

	/* Allowed DH-HMAC-CHAP digests/dhgroups */
	uint32_t				dhchap_digests;
	uint32_t				dhchap_dhgroups;

	TAILQ_ENTRY(spdk_nvmf_tgt)		link;
};

struct spdk_nvmf_host {
	char				nqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	struct spdk_key			*dhchap_key;
	struct spdk_key			*dhchap_ctrlr_key;
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
	struct spdk_nvmf_listener_opts			opts;
	TAILQ_ENTRY(spdk_nvmf_subsystem_listener)	link;
};

struct spdk_nvmf_referral {
	/* Discovery Log Page Entry for this referral */
	struct spdk_nvmf_discovery_log_page_entry entry;
	/* Transport ID */
	struct spdk_nvme_transport_id trid;
	TAILQ_ENTRY(spdk_nvmf_referral) link;
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
	enum spdk_nvmf_subsystem_state		state;

	/* Number of ADMIN and FABRICS requests outstanding */
	uint64_t				mgmt_io_outstanding;
	spdk_nvmf_poll_group_mod_done		cb_fn;
	void					*cb_arg;

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
	/* Command Set Identifier */
	enum spdk_nvme_csi csi;
	/* Make namespace visible to controllers of these hosts */
	TAILQ_HEAD(, spdk_nvmf_host) hosts;
	/* Namespace is always visible to all controllers */
	bool always_visible;
	/* Namespace id of the underlying device, used for passthrough commands */
	uint32_t passthrough_nsid;
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
	struct spdk_bit_array		*visible_ns;

	struct spdk_nvmf_ctrlr_data	cdata;

	struct spdk_nvmf_registers	vcprop;

	struct spdk_nvmf_ctrlr_feat feat;

	struct spdk_nvmf_qpair	*admin_qpair;
	struct spdk_thread	*thread;
	struct spdk_bit_array	*qpair_mask;

	const struct spdk_nvmf_subsystem_listener	*listener;

	struct spdk_nvmf_request *aer_req[SPDK_NVMF_MAX_ASYNC_EVENTS];
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
	/* LBA Format Extension Enabled (LBAFEE) */
	bool				lbafee_enabled;

	TAILQ_ENTRY(spdk_nvmf_ctrlr)	link;
};

#define NVMF_MAX_LISTENERS_PER_SUBSYSTEM	16

struct nvmf_subsystem_state_change_ctx {
	struct spdk_nvmf_subsystem			*subsystem;
	uint16_t					nsid;

	enum spdk_nvmf_subsystem_state			original_state;
	enum spdk_nvmf_subsystem_state			requested_state;
	int						status;
	struct spdk_thread				*thread;

	spdk_nvmf_subsystem_state_change_done		cb_fn;
	void						*cb_arg;
	TAILQ_ENTRY(nvmf_subsystem_state_change_ctx)	link;
};

struct spdk_nvmf_subsystem {
	struct spdk_thread				*thread;

	uint32_t					id;

	enum spdk_nvmf_subsystem_state			state;
	enum spdk_nvmf_subtype				subtype;

	uint16_t					next_cntlid;
	struct {
		uint8_t					allow_any_listener : 1;
		uint8_t					ana_reporting : 1;
		uint8_t					reserved : 6;
	} flags;

	/* Protected against concurrent access by ->mutex */
	bool						allow_any_host;

	bool						destroying;
	bool						async_destroy;

	/* FDP related fields */
	bool						fdp_supported;

	/* Zoned storage related fields */
	uint64_t					max_zone_append_size_kib;

	struct spdk_nvmf_tgt				*tgt;
	RB_ENTRY(spdk_nvmf_subsystem)			link;

	/* Array of pointers to namespaces of size max_nsid indexed by nsid - 1 */
	struct spdk_nvmf_ns				**ns;
	uint32_t					max_nsid;

	uint16_t					min_cntlid;
	uint16_t					max_cntlid;

	uint64_t					max_discard_size_kib;
	uint64_t					max_write_zeroes_size_kib;

	TAILQ_HEAD(, spdk_nvmf_ctrlr)			ctrlrs;

	/* This mutex is used to protect fields that aren't touched on the I/O path (e.g. it's
	 * needed for handling things like the CONNECT command) instead of requiring the subsystem
	 * to be paused.  It makes it possible to modify those fields (e.g. add/remove hosts)
	 * without affecting outstanding I/O requests.
	 */
	pthread_mutex_t					mutex;
	/* Protected against concurrent access by ->mutex */
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
	/* Queue of a state change requests */
	TAILQ_HEAD(, nvmf_subsystem_state_change_ctx)	state_changes;
	/* In-band authentication sequence number, protected by ->mutex */
	uint32_t					auth_seqnum;
	bool						passthrough;
};

static int
subsystem_cmp(struct spdk_nvmf_subsystem *subsystem1, struct spdk_nvmf_subsystem *subsystem2)
{
	return strncmp(subsystem1->subnqn, subsystem2->subnqn, sizeof(subsystem1->subnqn));
}

RB_GENERATE_STATIC(subsystem_tree, spdk_nvmf_subsystem, link, subsystem_cmp);

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
bool nvmf_ctrlr_copy_supported(struct spdk_nvmf_ctrlr *ctrlr);
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
int nvmf_bdev_ctrlr_copy_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
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
bool nvmf_subsystem_host_auth_required(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);
enum nvmf_auth_key_type {
	NVMF_AUTH_KEY_HOST,
	NVMF_AUTH_KEY_CTRLR,
};
struct spdk_key *nvmf_subsystem_get_dhchap_key(struct spdk_nvmf_subsystem *subsys, const char *nqn,
		enum nvmf_auth_key_type type);
struct spdk_nvmf_subsystem_listener *nvmf_subsystem_find_listener(
	struct spdk_nvmf_subsystem *subsystem,
	const struct spdk_nvme_transport_id *trid);
bool nvmf_subsystem_zone_append_supported(struct spdk_nvmf_subsystem *subsystem);
struct spdk_nvmf_listener *nvmf_transport_find_listener(
	struct spdk_nvmf_transport *transport,
	const struct spdk_nvme_transport_id *trid);
void nvmf_transport_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w,
			      bool named);
void nvmf_transport_listen_dump_trid(const struct spdk_nvme_transport_id *trid,
				     struct spdk_json_write_ctx *w);

int nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_ctrlr_async_event_discovery_log_change_notice(void *ctx);
void nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr);

void nvmf_ns_reservation_request(void *ctx);
void nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvmf_ns *ns,
				       enum spdk_nvme_reservation_notification_log_page_type type);

bool nvmf_ns_is_ptpl_capable(const struct spdk_nvmf_ns *ns);

static inline struct spdk_nvmf_host *
nvmf_ns_find_host(struct spdk_nvmf_ns *ns, const char *hostnqn)
{
	struct spdk_nvmf_host *host = NULL;

	TAILQ_FOREACH(host, &ns->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return host;
		}
	}

	return NULL;
}

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

void nvmf_ctrlr_set_fatal_status(struct spdk_nvmf_ctrlr *ctrlr);

static inline bool
nvmf_ctrlr_ns_is_visible(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	return spdk_bit_array_get(ctrlr->visible_ns, nsid - 1);
}

static inline struct spdk_nvmf_ns *
_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
	if (spdk_unlikely(nsid - 1 >= subsystem->max_nsid)) {
		return NULL;
	}

	return subsystem->ns[nsid - 1];
}

static inline struct spdk_nvmf_ns *
nvmf_ctrlr_get_ns(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns = _nvmf_subsystem_get_ns(subsystem, nsid);

	return ns && nvmf_ctrlr_ns_is_visible(ctrlr, nsid) ? ns : NULL;
}

static inline bool
nvmf_qpair_is_admin_queue(struct spdk_nvmf_qpair *qpair)
{
	return qpair->qid == 0;
}

void nvmf_qpair_set_state(struct spdk_nvmf_qpair *qpair, enum spdk_nvmf_qpair_state state);

int nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair);
void nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair);
void nvmf_qpair_auth_dump(struct spdk_nvmf_qpair *qpair, struct spdk_json_write_ctx *w);

int nvmf_auth_request_exec(struct spdk_nvmf_request *req);
bool nvmf_auth_is_supported(void);

static inline bool
nvmf_request_is_fabric_connect(struct spdk_nvmf_request *req)
{
	return req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC &&
	       req->cmd->nvmf_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT;
}

/*
 * Tests whether a given string represents a valid NQN.
 */
bool nvmf_nqn_is_valid(const char *nqn);

/*
 * Tests whether a given NQN describes a discovery subsystem.
 */
bool nvmf_nqn_is_discovery(const char *nqn);

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

/**
 * Publishes the mDNS PRR (Pull Registration Request) for the NVMe-oF target.
 *
 * \param tgt The NVMe-oF target
 *
 * \return 0 on success, negative errno on failure
 */
int nvmf_publish_mdns_prr(struct spdk_nvmf_tgt *tgt);

/**
 * Stops the mDNS PRR (Pull Registration Request) for the NVMe-oF target.
 *
 * \param tgt The NVMe-oF target
 */
void nvmf_tgt_stop_mdns_prr(struct spdk_nvmf_tgt *tgt);

/**
 * Updates the listener list in the mDNS PRR (Pull Registration Request) for the NVMe-oF target.
 *
 * \param tgt The NVMe-oF target
 *
 * \return 0 on success, negative errno on failure
 */
int nvmf_tgt_update_mdns_prr(struct spdk_nvmf_tgt *tgt);

static inline struct spdk_nvmf_transport_poll_group *
nvmf_get_transport_poll_group(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == transport) {
			return tgroup;
		}
	}

	return NULL;
}

/**
 * Generates a new NVMF controller id
 *
 * \param subsystem The subsystem
 *
 * \return unique controller id or 0xFFFF when all controller ids are in use
 */
uint16_t nvmf_subsystem_gen_cntlid(struct spdk_nvmf_subsystem *subsystem);

#endif /* __NVMF_INTERNAL_H__ */
