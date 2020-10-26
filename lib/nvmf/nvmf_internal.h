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

	struct spdk_poller			*accept_poller;

	uint32_t				max_subsystems;

	/* Array of subsystem pointers of size max_subsystems indexed by sid */
	struct spdk_nvmf_subsystem		**subsystems;

	TAILQ_HEAD(, spdk_nvmf_transport)	transports;
	TAILQ_HEAD(, spdk_nvmf_poll_group)	poll_groups;

	/* Used for round-robin assignment of connections to poll groups */
	struct spdk_nvmf_poll_group		*next_poll_group;

	spdk_nvmf_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;

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
	enum spdk_nvme_ana_state			ana_state;
	uint64_t					ana_state_change_count;
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
};

typedef void(*spdk_nvmf_poll_group_mod_done)(void *cb_arg, int status);

struct spdk_nvmf_subsystem_poll_group {
	/* Array of namespace information for each namespace indexed by nsid - 1 */
	struct spdk_nvmf_subsystem_pg_ns_info	*ns_info;
	uint32_t				num_ns;

	uint64_t				io_outstanding;
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
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_nvmf_ns_opts opts;
	/* reservation notificaton mask */
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
};

struct spdk_nvmf_ctrlr_feat {
	union spdk_nvme_feat_arbitration arbitration;
	union spdk_nvme_feat_power_management power_management;
	union spdk_nvme_feat_error_recovery error_recovery;
	union spdk_nvme_feat_volatile_write_cache volatile_write_cache;
	union spdk_nvme_feat_number_of_queues number_of_queues;
	union spdk_nvme_feat_write_atomicity write_atomicity;
	union spdk_nvme_feat_async_event_configuration async_event_configuration;
	union spdk_nvme_feat_keep_alive_timer keep_alive_timer;
};

/*
 * NVMf reservation notificaton log page.
 */
struct spdk_nvmf_reservation_log {
	struct spdk_nvme_reservation_notification_log	log;
	TAILQ_ENTRY(spdk_nvmf_reservation_log)		link;
	struct spdk_nvmf_ctrlr				*ctrlr;
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
	union spdk_nvme_async_event_completion notice_event;
	union spdk_nvme_async_event_completion reservation_event;
	uint8_t nr_aer_reqs;
	struct spdk_uuid  hostid;

	uint16_t changed_ns_list_count;
	struct spdk_nvme_ns_list changed_ns_list;
	uint64_t log_page_count;
	uint8_t num_avail_log_pages;
	TAILQ_HEAD(log_page_head, spdk_nvmf_reservation_log) log_head;

	/* Time to trigger keep-alive--poller_time = now_tick + period */
	uint64_t			last_keep_alive_tick;
	struct spdk_poller		*keep_alive_poller;

	struct spdk_poller		*association_timer;

	bool				dif_insert_or_strip;
	bool				in_destruct;

	TAILQ_ENTRY(spdk_nvmf_ctrlr)	link;
};

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

	struct spdk_nvmf_tgt				*tgt;

	/* Array of pointers to namespaces of size max_nsid indexed by nsid - 1 */
	struct spdk_nvmf_ns				**ns;
	uint32_t					max_nsid;
	/* This is the maximum allowed nsid to a subsystem */
	uint32_t					max_allowed_nsid;

	TAILQ_HEAD(, spdk_nvmf_ctrlr)			ctrlrs;

	/* A mutex used to protect the hosts list and allow_any_host flag. Unlike the namespace
	 * array, this list is not used on the I/O path (it's needed for handling things like
	 * the CONNECT command), so use a mutex to protect it instead of requiring the subsystem
	 * state to be paused. This removes the requirement to pause the subsystem when hosts
	 * are added or removed dynamically. */
	pthread_mutex_t					mutex;
	TAILQ_HEAD(, spdk_nvmf_host)			hosts;
	TAILQ_HEAD(, spdk_nvmf_subsystem_listener)	listeners;

	TAILQ_ENTRY(spdk_nvmf_subsystem)		entries;

	char						sn[SPDK_NVME_CTRLR_SN_LEN + 1];
	char						mn[SPDK_NVME_CTRLR_MN_LEN + 1];
	char						subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
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
				     struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);
void nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);

void nvmf_update_discovery_log(struct spdk_nvmf_tgt *tgt, const char *hostnqn);
void nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, const char *hostnqn,
				 struct iovec *iov,
				 uint32_t iovcnt, uint64_t offset, uint32_t length);

void nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req);
int nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req);
bool nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr);
bool nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid);

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
void nvmf_subsystem_set_ana_state(struct spdk_nvmf_subsystem *subsystem,
				  const struct spdk_nvme_transport_id *trid,
				  enum spdk_nvme_ana_state ana_state,
				  spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg);

int nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr);
int nvmf_ctrlr_async_event_discovery_log_change_notice(struct spdk_nvmf_ctrlr *ctrlr);
void nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr);
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

#endif /* __NVMF_INTERNAL_H__ */
