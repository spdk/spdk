/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "event_nvmf.h"

#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_cmd.h"
#include "spdk_internal/usdt.h"

enum nvmf_tgt_state {
	NVMF_TGT_INIT_NONE = 0,
	NVMF_TGT_INIT_CREATE_TARGET,
	NVMF_TGT_INIT_CREATE_POLL_GROUPS,
	NVMF_TGT_INIT_START_SUBSYSTEMS,
	NVMF_TGT_RUNNING,
	NVMF_TGT_FINI_STOP_LISTEN,
	NVMF_TGT_FINI_STOP_SUBSYSTEMS,
	NVMF_TGT_FINI_DESTROY_SUBSYSTEMS,
	NVMF_TGT_FINI_DESTROY_POLL_GROUPS,
	NVMF_TGT_FINI_DESTROY_TARGET,
	NVMF_TGT_STOPPED,
	NVMF_TGT_ERROR,
};

struct nvmf_tgt_poll_group {
	struct spdk_nvmf_poll_group		*group;
	struct spdk_thread			*thread;
	TAILQ_ENTRY(nvmf_tgt_poll_group)	link;
};

#define NVMF_TGT_DEFAULT_DIGESTS (SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA256) | \
				  SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA384) | \
				  SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA512))

#define NVMF_TGT_DEFAULT_DHGROUPS (SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_NULL) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_2048) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_3072) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_4096) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_6144) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_8192))

struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf = {
	.opts = {
		.size = SPDK_SIZEOF(&g_spdk_nvmf_tgt_conf.opts, dhchap_dhgroups),
		.name = "nvmf_tgt",
		.max_subsystems = 0,
		.crdt = { 0, 0, 0 },
		.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY,
		.dhchap_digests = NVMF_TGT_DEFAULT_DIGESTS,
		.dhchap_dhgroups = NVMF_TGT_DEFAULT_DHGROUPS,
	},
	.admin_passthru.identify_ctrlr = false,
	.admin_passthru.identify_uuid_list = false,
	.admin_passthru.get_log_page = false,
	.admin_passthru.get_set_features = false,
	.admin_passthru.sanitize = false,
	.admin_passthru.security_send_recv = false,
	.admin_passthru.fw_update = false,
	.admin_passthru.nvme_mi = false,
	.admin_passthru.vendor_specific = false
};

struct spdk_cpuset *g_poll_groups_mask = NULL;
struct spdk_nvmf_tgt *g_spdk_nvmf_tgt = NULL;

static enum nvmf_tgt_state g_tgt_state;

static struct spdk_thread *g_tgt_init_thread = NULL;
static struct spdk_thread *g_tgt_fini_thread = NULL;

static TAILQ_HEAD(, nvmf_tgt_poll_group) g_poll_groups = TAILQ_HEAD_INITIALIZER(g_poll_groups);
static size_t g_num_poll_groups = 0;

static void nvmf_tgt_advance_state(void);

static void
nvmf_shutdown_cb(void *arg1)
{
	/* Still in initialization state, defer shutdown operation */
	if (g_tgt_state < NVMF_TGT_RUNNING) {
		spdk_thread_send_msg(spdk_get_thread(), nvmf_shutdown_cb, NULL);
		return;
	} else if (g_tgt_state != NVMF_TGT_RUNNING && g_tgt_state != NVMF_TGT_ERROR) {
		/* Already in Shutdown status, ignore the signal */
		return;
	}

	if (g_tgt_state == NVMF_TGT_ERROR) {
		/* Parse configuration error */
		g_tgt_state = NVMF_TGT_FINI_DESTROY_TARGET;
	} else {
		g_tgt_state = NVMF_TGT_FINI_STOP_LISTEN;
	}
	nvmf_tgt_advance_state();
}

static void
nvmf_subsystem_fini(void)
{
	nvmf_shutdown_cb(NULL);
}

static void
_nvmf_tgt_destroy_poll_group_done(void *ctx)
{
	assert(g_num_poll_groups > 0);

	if (--g_num_poll_groups == 0) {
		g_tgt_state = NVMF_TGT_FINI_DESTROY_TARGET;
		nvmf_tgt_advance_state();
	}
}

static void
nvmf_tgt_destroy_poll_group_done(void *cb_arg, int status)
{
	struct nvmf_tgt_poll_group *pg = cb_arg;

	free(pg);

	spdk_thread_send_msg(g_tgt_fini_thread, _nvmf_tgt_destroy_poll_group_done, NULL);

	spdk_thread_exit(spdk_get_thread());
}

static void
nvmf_tgt_destroy_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg = ctx;

	spdk_nvmf_poll_group_destroy(pg->group, nvmf_tgt_destroy_poll_group_done, pg);
}

static void
nvmf_tgt_destroy_poll_groups(void)
{
	struct nvmf_tgt_poll_group *pg, *tpg;

	g_tgt_fini_thread = spdk_get_thread();
	assert(g_tgt_fini_thread != NULL);

	TAILQ_FOREACH_SAFE(pg, &g_poll_groups, link, tpg) {
		TAILQ_REMOVE(&g_poll_groups, pg, link);
		spdk_thread_send_msg(pg->thread, nvmf_tgt_destroy_poll_group, pg);
	}
}

static uint32_t
nvmf_get_cpuset_count(void)
{
	if (g_poll_groups_mask) {
		return spdk_cpuset_count(g_poll_groups_mask);
	} else {
		return spdk_env_get_core_count();
	}
}

static void
nvmf_tgt_create_poll_group_done(void *ctx)
{
	struct nvmf_tgt_poll_group *pg = ctx;

	assert(pg);

	if (!pg->group) {
		SPDK_ERRLOG("Failed to create nvmf poll group\n");
		/* Change the state to error but wait for completions from all other threads */
		g_tgt_state = NVMF_TGT_ERROR;
	}

	TAILQ_INSERT_TAIL(&g_poll_groups, pg, link);

	assert(g_num_poll_groups < nvmf_get_cpuset_count());

	if (++g_num_poll_groups == nvmf_get_cpuset_count()) {
		if (g_tgt_state != NVMF_TGT_ERROR) {
			g_tgt_state = NVMF_TGT_INIT_START_SUBSYSTEMS;
		}
		nvmf_tgt_advance_state();
	}
}

static void
nvmf_tgt_create_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg;

	pg = calloc(1, sizeof(*pg));
	if (!pg) {
		SPDK_ERRLOG("Not enough memory to allocate poll groups\n");
		g_tgt_state = NVMF_TGT_ERROR;
		nvmf_tgt_advance_state();
		return;
	}

	pg->thread = spdk_get_thread();
	pg->group = spdk_nvmf_poll_group_create(g_spdk_nvmf_tgt);

	spdk_thread_send_msg(g_tgt_init_thread, nvmf_tgt_create_poll_group_done, pg);
}

static void
nvmf_tgt_create_poll_groups(void)
{
	uint32_t cpu, count = 0;
	char thread_name[32];
	struct spdk_thread *thread;

	g_tgt_init_thread = spdk_get_thread();
	assert(g_tgt_init_thread != NULL);

	SPDK_ENV_FOREACH_CORE(cpu) {
		if (g_poll_groups_mask && !spdk_cpuset_get_cpu(g_poll_groups_mask, cpu)) {
			continue;
		}
		snprintf(thread_name, sizeof(thread_name), "nvmf_tgt_poll_group_%03u", count++);

		thread = spdk_thread_create(thread_name, g_poll_groups_mask);
		assert(thread != NULL);

		spdk_thread_send_msg(thread, nvmf_tgt_create_poll_group, NULL);
	}
}

static void
nvmf_tgt_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	int rc;

	if (subsystem) {
		rc = spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_started, NULL);
		if (rc) {
			g_tgt_state = NVMF_TGT_FINI_STOP_LISTEN;
			SPDK_ERRLOG("Unable to start NVMe-oF subsystem. Stopping app.\n");
			nvmf_tgt_advance_state();
		}
		return;
	}

	g_tgt_state = NVMF_TGT_RUNNING;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	int rc;

	if (subsystem) {
		rc = spdk_nvmf_subsystem_stop(subsystem, nvmf_tgt_subsystem_stopped, NULL);
		if (rc) {
			SPDK_ERRLOG("Unable to stop NVMe-oF subsystem %s with rc %d, Trying others.\n",
				    spdk_nvmf_subsystem_get_nqn(subsystem), rc);
			nvmf_tgt_subsystem_stopped(subsystem, NULL, 0);
		}
		return;
	}

	g_tgt_state = NVMF_TGT_FINI_DESTROY_SUBSYSTEMS;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_stop_listen(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_listener *listener;
	const struct spdk_nvme_transport_id *trid;
	struct spdk_nvmf_transport *transport;
	int rc;

	for (subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);
	     subsystem != NULL;
	     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
		for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem);
		     listener != NULL;
		     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
			trid = spdk_nvmf_subsystem_listener_get_trid(listener);
			transport = spdk_nvmf_tgt_get_transport(g_spdk_nvmf_tgt, trid->trstring);
			rc = spdk_nvmf_transport_stop_listen(transport, trid);
			if (rc != 0) {
				SPDK_ERRLOG("Unable to stop subsystem %s listener %s:%s, rc %d. Trying others.\n",
					    spdk_nvmf_subsystem_get_nqn(subsystem), trid->traddr, trid->trsvcid, rc);
				continue;
			}
		}
	}

	g_tgt_state = NVMF_TGT_FINI_STOP_SUBSYSTEMS;
}

static void
_nvmf_tgt_subsystem_destroy(void *cb_arg)
{
	struct spdk_nvmf_subsystem *subsystem, *next_subsystem;
	int rc;

	subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);

	while (subsystem != NULL) {
		next_subsystem = spdk_nvmf_subsystem_get_next(subsystem);
		rc = spdk_nvmf_subsystem_destroy(subsystem, _nvmf_tgt_subsystem_destroy, NULL);
		if (rc) {
			if (rc == -EINPROGRESS) {
				/* If ret is -EINPROGRESS, nvmf_tgt_subsystem_destroyed will be called when subsystem
				 * is destroyed, _nvmf_tgt_subsystem_destroy will continue to destroy other subsystems if any */
				return;
			} else {
				SPDK_ERRLOG("Unable to destroy subsystem %s, rc %d. Trying others.\n",
					    spdk_nvmf_subsystem_get_nqn(subsystem), rc);
			}
		}
		subsystem = next_subsystem;
	}

	g_tgt_state = NVMF_TGT_FINI_DESTROY_POLL_GROUPS;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_destroy_done(void *ctx, int status)
{
	g_tgt_state = NVMF_TGT_STOPPED;

	nvmf_tgt_advance_state();
}

static int
nvmf_add_discovery_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_create(g_spdk_nvmf_tgt, SPDK_NVMF_DISCOVERY_NQN,
					       SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT, 0);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		return -1;
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);

	return 0;
}

static int
nvmf_tgt_create_target(void)
{
	g_spdk_nvmf_tgt = spdk_nvmf_tgt_create(&g_spdk_nvmf_tgt_conf.opts);
	if (!g_spdk_nvmf_tgt) {
		SPDK_ERRLOG("spdk_nvmf_tgt_create() failed\n");
		return -1;
	}

	if (nvmf_add_discovery_subsystem() != 0) {
		SPDK_ERRLOG("nvmf_add_discovery_subsystem failed\n");
		return -1;
	}

	return 0;
}

static void
get_log_page_offset_and_len(struct spdk_nvmf_request *req, size_t page_size, uint64_t *offset,
			    size_t *copy_len)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	uint64_t len;
	uint32_t numdl, numdu;

	*offset = (uint64_t)cmd->cdw12 | ((uint64_t)cmd->cdw13 << 32);
	numdl = cmd->cdw10_bits.get_log_page.numdl;
	numdu = cmd->cdw11_bits.get_log_page.numdu;
	len = ((numdu << 16) + numdl + (uint64_t)1) * 4;

	if (*offset > page_size) {
		return;
	}

	*copy_len = spdk_min(page_size - *offset, len);
}

static void
fixup_get_cmds_and_effects_log_page(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmds_and_effect_log_page nvme_log_data = {};
	struct spdk_nvme_cmds_and_effect_log_page nvmf_log_data = {};
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	uint32_t page_size = sizeof(struct spdk_nvme_cmds_and_effect_log_page);
	uint64_t offset;
	size_t datalen, copy_len = 0;

	get_log_page_offset_and_len(req, page_size, &offset, &copy_len);

	if (copy_len == 0) {
		return;
	}

	/* This is cmds_and_effects log page from the NVMe drive */
	datalen = spdk_nvmf_request_copy_to_buf(req, (uint8_t *) &nvme_log_data + offset,
						copy_len);

	/* Those are cmds_and_effects log page from SPDK */
	spdk_nvmf_get_cmds_and_effects_log_page(ctrlr, &nvmf_log_data);

	/* if vendor specific commands are supported, mark it as supported in result stuct */
	if (g_spdk_nvmf_tgt_conf.admin_passthru.vendor_specific) {
		int i;
		for (i = SPDK_NVME_OPC_VENDOR_SPECIFIC_START; i <= SPDK_NVME_MAX_OPC; i++) {
			nvmf_log_data.admin_cmds_supported[i] = nvme_log_data.admin_cmds_supported[i];
		}
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.sanitize) {
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_SANITIZE] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_SANITIZE];
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.security_send_recv) {
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_SECURITY_SEND] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_SECURITY_SEND];
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_SECURITY_RECEIVE] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_SECURITY_RECEIVE];
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.fw_update) {
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_FIRMWARE_COMMIT] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_FIRMWARE_COMMIT];
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD];
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.nvme_mi) {
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_NVME_MI_RECEIVE] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_NVME_MI_RECEIVE];
		nvmf_log_data.admin_cmds_supported[SPDK_NVME_OPC_NVME_MI_SEND] =
			nvme_log_data.admin_cmds_supported[SPDK_NVME_OPC_NVME_MI_SEND];
	}

	/* Copy the fixed SPDK struct to the request */
	spdk_nvmf_request_copy_from_buf(req, (uint8_t *) &nvmf_log_data + offset, datalen);
}

static void
fixup_get_supported_log_pages(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_supported_log_pages nvme_log_data = {};
	struct spdk_nvme_supported_log_pages nvmf_log_data = {};
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	uint32_t page_size = sizeof(struct spdk_nvme_supported_log_pages);
	uint64_t offset;
	size_t datalen, copy_len = 0;

	get_log_page_offset_and_len(req, page_size, &offset, &copy_len);

	if (copy_len == 0) {
		return;
	}

	/* Those are supported log pages from the NVMe drive */
	datalen = spdk_nvmf_request_copy_to_buf(req, (uint8_t *) &nvme_log_data + offset,
						copy_len);

	/* Those are supported log pages from SPDK */
	spdk_nvmf_get_supported_log_pages(ctrlr, &nvmf_log_data);

	/* Make sure that logs handled by SPDK are added as well */
	nvme_log_data.lids[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS] =
		nvmf_log_data.lids[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS];
	nvme_log_data.lids[SPDK_NVME_LOG_CHANGED_NS_LIST] =
		nvmf_log_data.lids[SPDK_NVME_LOG_CHANGED_NS_LIST];
	nvme_log_data.lids[SPDK_NVME_LOG_FEATURE_IDS_EFFECTS] =
		nvmf_log_data.lids[SPDK_NVME_LOG_FEATURE_IDS_EFFECTS];
	nvme_log_data.lids[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG] =
		nvmf_log_data.lids[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG];

	/* Copy the data back to the request */
	spdk_nvmf_request_copy_from_buf(req, (uint8_t *) &nvme_log_data + offset, datalen);
}

static void
fixup_get_feature_ids_effects_log_page(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_feature_ids_effects_log_page nvme_log_data = {};
	struct spdk_nvme_feature_ids_effects_log_page nvmf_log_data = {};
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	uint32_t page_size = sizeof(struct spdk_nvme_feature_ids_effects_log_page);
	uint64_t offset;
	size_t datalen, copy_len = 0;

	get_log_page_offset_and_len(req, page_size, &offset, &copy_len);

	if (copy_len == 0) {
		return;
	}

	/* This is supported features log page from the NVMe drive */
	datalen = spdk_nvmf_request_copy_to_buf(req, (uint8_t *) &nvme_log_data + offset,
						copy_len);

	/* This is supported features log page from SPDK */
	spdk_nvmf_get_feature_ids_effects_log_page(ctrlr, &nvmf_log_data);

	if (!g_spdk_nvmf_tgt_conf.admin_passthru.get_set_features) {
		/* Passthru support to the drive is disabled for get/set features OPC
		 * Report only SPDK supported features.
		 */
		spdk_nvmf_request_copy_from_buf(req, (uint8_t *) &nvmf_log_data + offset, datalen);
		return;
	}

	/* Make sure that features not handled by custom handler are added as well */
	nvme_log_data.fis[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] =
		nvmf_log_data.fis[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION];
	nvme_log_data.fis[SPDK_NVME_FEAT_KEEP_ALIVE_TIMER] =
		nvmf_log_data.fis[SPDK_NVME_FEAT_KEEP_ALIVE_TIMER];
	nvme_log_data.fis[SPDK_NVME_FEAT_NUMBER_OF_QUEUES] =
		nvmf_log_data.fis[SPDK_NVME_FEAT_NUMBER_OF_QUEUES];
	nvme_log_data.fis[SPDK_NVME_FEAT_HOST_RESERVE_MASK] =
		nvmf_log_data.fis[SPDK_NVME_FEAT_HOST_RESERVE_MASK];
	nvme_log_data.fis[SPDK_NVME_FEAT_HOST_RESERVE_PERSIST] =
		nvmf_log_data.fis[SPDK_NVME_FEAT_HOST_RESERVE_PERSIST];

	/* Copy the data back to the request */
	spdk_nvmf_request_copy_from_buf(req, (uint8_t *) &nvme_log_data + offset, datalen);
}

static void
fixup_identify_ctrlr(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_ctrlr_data nvme_cdata = {};
	struct spdk_nvme_ctrlr_data nvmf_cdata = {};
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req);
	size_t datalen;
	int rc;

	/* This is the identify data from the NVMe drive */
	datalen = spdk_nvmf_request_copy_to_buf(req, &nvme_cdata,
						sizeof(nvme_cdata));

	/* Get the NVMF identify data */
	rc = spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, &nvmf_cdata);
	if (rc != SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return;
	}

	/* Fixup NVMF identify data with NVMe identify data */

	/* Serial Number (SN) */
	memcpy(&nvmf_cdata.sn[0], &nvme_cdata.sn[0], sizeof(nvmf_cdata.sn));
	/* Model Number (MN) */
	memcpy(&nvmf_cdata.mn[0], &nvme_cdata.mn[0], sizeof(nvmf_cdata.mn));
	/* Firmware Revision (FR) */
	memcpy(&nvmf_cdata.fr[0], &nvme_cdata.fr[0], sizeof(nvmf_cdata.fr));
	/* IEEE OUI Identifier (IEEE) */
	memcpy(&nvmf_cdata.ieee[0], &nvme_cdata.ieee[0], sizeof(nvmf_cdata.ieee));
	/* FRU Globally Unique Identifier (FGUID) */

	if (g_spdk_nvmf_tgt_conf.admin_passthru.get_log_page) {
		nvmf_cdata.lpa = nvme_cdata.lpa;
		nvmf_cdata.elpe = nvme_cdata.elpe;
		nvmf_cdata.pels = nvme_cdata.pels;
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.sanitize) {
		nvmf_cdata.sanicap = nvme_cdata.sanicap;
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.security_send_recv) {
		nvmf_cdata.oacs.security = nvme_cdata.oacs.security;
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.fw_update) {
		nvmf_cdata.oacs.firmware = nvme_cdata.oacs.firmware;
		nvmf_cdata.frmw = nvme_cdata.frmw;
		nvmf_cdata.fwug = nvme_cdata.fwug;
		nvmf_cdata.mtfa = nvme_cdata.mtfa;
	}
	if (g_spdk_nvmf_tgt_conf.admin_passthru.identify_uuid_list) {
		nvmf_cdata.ctratt.bits.uuid_list = nvme_cdata.ctratt.bits.uuid_list;
	}

	/* Copy the fixed up data back to the response */
	spdk_nvmf_request_copy_from_buf(req, &nvmf_cdata, datalen);
}

static int
nvmf_admin_passthru_generic_hdlr(struct spdk_nvmf_request *req,
				 spdk_nvmf_nvme_passthru_cmd_cb cb_fn)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_subsystem *subsys;
	int rc;

	subsys = spdk_nvmf_request_get_subsystem(req);
	if (subsys == NULL) {
		return -1;
	}

	/* Only procss this request if it has exactly one namespace */
	if (spdk_nvmf_subsystem_get_max_nsid(subsys) != 1) {
		return -1;
	}

	/* Forward to first namespace if it supports NVME admin commands */
	rc = spdk_nvmf_request_get_bdev(1, req, &bdev, &desc, &ch);
	if (rc) {
		/* No bdev found for this namespace */
		return -1;
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN)) {
		return -1;
	}

	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, cb_fn);
}

static int
nvmf_custom_identify_hdlr(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);

	if (cmd->cdw10_bits.identify.cns == SPDK_NVME_IDENTIFY_CTRLR &&
	    g_spdk_nvmf_tgt_conf.admin_passthru.identify_ctrlr) {
		return nvmf_admin_passthru_generic_hdlr(req, fixup_identify_ctrlr);
	}

	if (cmd->cdw10_bits.identify.cns == SPDK_NVME_IDENTIFY_UUID_LIST &&
	    g_spdk_nvmf_tgt_conf.admin_passthru.identify_uuid_list) {
		return nvmf_admin_passthru_generic_hdlr(req, NULL);
	}

	return -1;
}

static int
nvmf_custom_admin_no_cb_hdlr(struct spdk_nvmf_request *req)
{
	return nvmf_admin_passthru_generic_hdlr(req, NULL);
}

static int
nvmf_custom_get_log_page_hdlr(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);

	switch (cmd->cdw10_bits.get_log_page.lid) {
	/* ANA log and Changed NS List have to be handled by SPDK.
	 * Do not passthru them to the drive.
	 */
	case SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS:
	case SPDK_NVME_LOG_CHANGED_NS_LIST:
		return -1;
	case SPDK_NVME_LOG_FEATURE_IDS_EFFECTS:
		return nvmf_admin_passthru_generic_hdlr(req, fixup_get_feature_ids_effects_log_page);
	case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
		return nvmf_admin_passthru_generic_hdlr(req, fixup_get_cmds_and_effects_log_page);
	case SPDK_NVME_LOG_SUPPORTED_LOG_PAGES:
		return nvmf_admin_passthru_generic_hdlr(req, fixup_get_supported_log_pages);
	default:
		return nvmf_admin_passthru_generic_hdlr(req, NULL);
	}
}

static int
nvmf_custom_set_features(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);

	switch (cmd->cdw10_bits.set_features.fid) {
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return -1;
	default:
		return nvmf_admin_passthru_generic_hdlr(req, NULL);
	}
}

static int
nvmf_custom_get_features(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);

	switch (cmd->cdw10_bits.get_features.fid) {
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return -1;
	default:
		return nvmf_admin_passthru_generic_hdlr(req, NULL);
	}
}

static void
nvmf_tgt_advance_state(void)
{
	enum nvmf_tgt_state prev_state;
	int rc = -1;
	int ret;

	do {
		SPDK_DTRACE_PROBE1(nvmf_tgt_state, g_tgt_state);
		prev_state = g_tgt_state;

		switch (g_tgt_state) {
		case NVMF_TGT_INIT_NONE: {
			g_tgt_state = NVMF_TGT_INIT_CREATE_TARGET;
			break;
		}
		case NVMF_TGT_INIT_CREATE_TARGET:
			ret = nvmf_tgt_create_target();
			g_tgt_state = (ret == 0) ? NVMF_TGT_INIT_CREATE_POLL_GROUPS : NVMF_TGT_ERROR;
			break;
		case NVMF_TGT_INIT_CREATE_POLL_GROUPS:
			if (g_spdk_nvmf_tgt_conf.admin_passthru.identify_ctrlr ||
			    g_spdk_nvmf_tgt_conf.admin_passthru.identify_uuid_list) {
				SPDK_NOTICELOG("Custom identify OPC handler enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_IDENTIFY, nvmf_custom_identify_hdlr);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.get_log_page) {
				SPDK_NOTICELOG("Custom get log page handler enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_GET_LOG_PAGE, nvmf_custom_get_log_page_hdlr);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.get_set_features) {
				SPDK_NOTICELOG("Custom get/set_feature commands handlers enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_SET_FEATURES, nvmf_custom_set_features);
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_GET_FEATURES, nvmf_custom_get_features);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.sanitize) {
				SPDK_NOTICELOG("Custom sanitize command handlers enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_SANITIZE, nvmf_custom_admin_no_cb_hdlr);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.security_send_recv) {
				SPDK_NOTICELOG("Custom security send/recv commands handlers enabled\n");
				SPDK_WARNLOG("Warning: Passing Opal keys openly is not secure. Make sure to use transport encryption like nvme/tls or ipsec.\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_SECURITY_SEND, nvmf_custom_admin_no_cb_hdlr);
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_SECURITY_RECEIVE, nvmf_custom_admin_no_cb_hdlr);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.fw_update) {
				SPDK_NOTICELOG("Custom firmware update commands handlers enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_FIRMWARE_COMMIT, nvmf_custom_admin_no_cb_hdlr);
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD,
								    nvmf_custom_admin_no_cb_hdlr);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.nvme_mi) {
				SPDK_NOTICELOG("Custom NVMe-MI send/recv commands handlers enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_NVME_MI_RECEIVE, nvmf_custom_admin_no_cb_hdlr);
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_NVME_MI_SEND, nvmf_custom_admin_no_cb_hdlr);
			}
			if (g_spdk_nvmf_tgt_conf.admin_passthru.vendor_specific) {
				int i;
				SPDK_NOTICELOG("Custom vendor specific commands handlers enabled\n");
				for (i = SPDK_NVME_OPC_VENDOR_SPECIFIC_START; i <= SPDK_NVME_MAX_OPC; i++) {
					spdk_nvmf_set_custom_admin_cmd_hdlr(i, nvmf_custom_admin_no_cb_hdlr);
				}
			}
			/* Create poll group threads, and send a message to each thread
			 * and create a poll group.
			 */
			nvmf_tgt_create_poll_groups();
			break;
		case NVMF_TGT_INIT_START_SUBSYSTEMS: {
			struct spdk_nvmf_subsystem *subsystem;

			subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);

			if (subsystem) {
				ret = spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_started, NULL);
				if (ret) {
					SPDK_ERRLOG("Unable to start NVMe-oF subsystem. Stopping app.\n");
					g_tgt_state = NVMF_TGT_FINI_STOP_LISTEN;
				}
			} else {
				g_tgt_state = NVMF_TGT_RUNNING;
			}
			break;
		}
		case NVMF_TGT_RUNNING:
			spdk_subsystem_init_next(0);
			break;
		case NVMF_TGT_FINI_STOP_LISTEN:
			nvmf_tgt_stop_listen();
			break;
		case NVMF_TGT_FINI_STOP_SUBSYSTEMS: {
			struct spdk_nvmf_subsystem *subsystem;

			subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);

			if (subsystem) {
				ret = spdk_nvmf_subsystem_stop(subsystem, nvmf_tgt_subsystem_stopped, NULL);
				if (ret) {
					nvmf_tgt_subsystem_stopped(subsystem, NULL, 0);
				}
			} else {
				g_tgt_state = NVMF_TGT_FINI_DESTROY_SUBSYSTEMS;
			}
			break;
		}
		case NVMF_TGT_FINI_DESTROY_SUBSYSTEMS:
			_nvmf_tgt_subsystem_destroy(NULL);
			/* Function above can be asynchronous, it will call nvmf_tgt_advance_state() once done.
			 * So just return here */
			return;
		case NVMF_TGT_FINI_DESTROY_POLL_GROUPS:
			/* Send a message to each poll group thread, and terminate the thread */
			nvmf_tgt_destroy_poll_groups();
			break;
		case NVMF_TGT_FINI_DESTROY_TARGET:
			spdk_nvmf_tgt_destroy(g_spdk_nvmf_tgt, nvmf_tgt_destroy_done, NULL);
			break;
		case NVMF_TGT_STOPPED:
			spdk_subsystem_fini_next();
			return;
		case NVMF_TGT_ERROR:
			spdk_subsystem_init_next(rc);
			return;
		}

	} while (g_tgt_state != prev_state);
}

static void
nvmf_subsystem_init(void)
{
	g_tgt_state = NVMF_TGT_INIT_NONE;
	nvmf_tgt_advance_state();
}

static void
nvmf_subsystem_dump_discover_filter(struct spdk_json_write_ctx *w)
{
	static char const *const answers[] = {
		"match_any",
		"transport",
		"address",
		"transport,address",
		"svcid",
		"transport,svcid",
		"address,svcid",
		"transport,address,svcid"
	};

	if ((g_spdk_nvmf_tgt_conf.opts.discovery_filter & ~(SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE |
			SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS |
			SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID)) != 0) {
		SPDK_ERRLOG("Incorrect discovery filter %d\n", g_spdk_nvmf_tgt_conf.opts.discovery_filter);
		assert(0);
		return;
	}

	spdk_json_write_named_string(w, "discovery_filter",
				     answers[g_spdk_nvmf_tgt_conf.opts.discovery_filter]);
}

static void
nvmf_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	int i;

	spdk_json_write_array_begin(w);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "nvmf_set_config");

	spdk_json_write_named_object_begin(w, "params");
	nvmf_subsystem_dump_discover_filter(w);
	spdk_json_write_named_object_begin(w, "admin_cmd_passthru");
	spdk_json_write_named_bool(w, "identify_ctrlr",
				   g_spdk_nvmf_tgt_conf.admin_passthru.identify_ctrlr);
	spdk_json_write_named_bool(w, "identify_uuid_list",
				   g_spdk_nvmf_tgt_conf.admin_passthru.identify_uuid_list);
	spdk_json_write_named_bool(w, "get_log_page",
				   g_spdk_nvmf_tgt_conf.admin_passthru.get_log_page);
	spdk_json_write_named_bool(w, "get_set_features",
				   g_spdk_nvmf_tgt_conf.admin_passthru.get_set_features);
	spdk_json_write_named_bool(w, "sanitize",
				   g_spdk_nvmf_tgt_conf.admin_passthru.sanitize);
	spdk_json_write_named_bool(w, "security_send_recv",
				   g_spdk_nvmf_tgt_conf.admin_passthru.security_send_recv);
	spdk_json_write_named_bool(w, "fw_update",
				   g_spdk_nvmf_tgt_conf.admin_passthru.fw_update);
	spdk_json_write_named_bool(w, "nvme_mi",
				   g_spdk_nvmf_tgt_conf.admin_passthru.nvme_mi);
	spdk_json_write_named_bool(w, "vendor_specific",
				   g_spdk_nvmf_tgt_conf.admin_passthru.vendor_specific);
	spdk_json_write_object_end(w);
	if (g_poll_groups_mask) {
		spdk_json_write_named_string(w, "poll_groups_mask", spdk_cpuset_fmt(g_poll_groups_mask));
	}
	spdk_json_write_named_array_begin(w, "dhchap_digests");
	for (i = 0; i < 32; ++i) {
		if (g_spdk_nvmf_tgt_conf.opts.dhchap_digests & SPDK_BIT(i)) {
			spdk_json_write_string(w, spdk_nvme_dhchap_get_digest_name(i));
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_named_array_begin(w, "dhchap_dhgroups");
	for (i = 0; i < 32; ++i) {
		if (g_spdk_nvmf_tgt_conf.opts.dhchap_dhgroups & SPDK_BIT(i)) {
			spdk_json_write_string(w, spdk_nvme_dhchap_get_dhgroup_name(i));
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);

	spdk_nvmf_tgt_write_config_json(w, g_spdk_nvmf_tgt);
	spdk_json_write_array_end(w);
}

static struct spdk_subsystem g_spdk_subsystem_nvmf = {
	.name = "nvmf",
	.init = nvmf_subsystem_init,
	.fini = nvmf_subsystem_fini,
	.write_config_json = nvmf_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_nvmf)
SPDK_SUBSYSTEM_DEPEND(nvmf, bdev)
SPDK_SUBSYSTEM_DEPEND(nvmf, keyring)
SPDK_SUBSYSTEM_DEPEND(nvmf, sock)
