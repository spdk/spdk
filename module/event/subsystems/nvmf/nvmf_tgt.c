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

struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf = {
	.admin_passthru.identify_ctrlr = false
};

struct spdk_cpuset *g_poll_groups_mask = NULL;
struct spdk_nvmf_tgt *g_spdk_nvmf_tgt = NULL;
uint32_t g_spdk_nvmf_tgt_max_subsystems = 0;
uint16_t g_spdk_nvmf_tgt_crdt[3] = {0, 0, 0};

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
		g_tgt_state = NVMF_TGT_FINI_STOP_SUBSYSTEMS;
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
		snprintf(thread_name, sizeof(thread_name), "nvmf_tgt_poll_group_%u", count++);

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
			g_tgt_state = NVMF_TGT_FINI_STOP_SUBSYSTEMS;
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
					       SPDK_NVMF_SUBTYPE_DISCOVERY, 0);
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
	struct spdk_nvmf_target_opts opts = {
		.name = "nvmf_tgt"
	};

	opts.max_subsystems = g_spdk_nvmf_tgt_max_subsystems;
	opts.crdt[0] = g_spdk_nvmf_tgt_crdt[0];
	opts.crdt[1] = g_spdk_nvmf_tgt_crdt[1];
	opts.crdt[2] = g_spdk_nvmf_tgt_crdt[2];
	opts.discovery_filter = g_spdk_nvmf_tgt_conf.discovery_filter;
	g_spdk_nvmf_tgt = spdk_nvmf_tgt_create(&opts);
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

	/* Copy the fixed up data back to the response */
	spdk_nvmf_request_copy_from_buf(req, &nvmf_cdata, datalen);
}

static int
nvmf_custom_identify_hdlr(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_subsystem *subsys;
	int rc;

	if (cmd->cdw10_bits.identify.cns != SPDK_NVME_IDENTIFY_CTRLR) {
		return -1; /* continue */
	}

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
		/* No bdev found for this namespace. Continue. */
		return -1;
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN)) {
		return -1;
	}

	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, fixup_identify_ctrlr);
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
			if (g_spdk_nvmf_tgt_conf.admin_passthru.identify_ctrlr) {
				SPDK_NOTICELOG("Custom identify ctrlr handler enabled\n");
				spdk_nvmf_set_custom_admin_cmd_hdlr(SPDK_NVME_OPC_IDENTIFY, nvmf_custom_identify_hdlr);
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
					g_tgt_state = NVMF_TGT_FINI_STOP_SUBSYSTEMS;
				}
			} else {
				g_tgt_state = NVMF_TGT_RUNNING;
			}
			break;
		}
		case NVMF_TGT_RUNNING:
			spdk_subsystem_init_next(0);
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

	if ((g_spdk_nvmf_tgt_conf.discovery_filter & ~(SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE |
			SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS |
			SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID)) != 0) {
		SPDK_ERRLOG("Incorrect discovery filter %d\n", g_spdk_nvmf_tgt_conf.discovery_filter);
		assert(0);
		return;
	}

	spdk_json_write_named_string(w, "discovery_filter", answers[g_spdk_nvmf_tgt_conf.discovery_filter]);
}

static void
nvmf_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "nvmf_set_config");

	spdk_json_write_named_object_begin(w, "params");
	nvmf_subsystem_dump_discover_filter(w);
	spdk_json_write_named_object_begin(w, "admin_cmd_passthru");
	spdk_json_write_named_bool(w, "identify_ctrlr",
				   g_spdk_nvmf_tgt_conf.admin_passthru.identify_ctrlr);
	spdk_json_write_object_end(w);
	if (g_poll_groups_mask) {
		spdk_json_write_named_string(w, "poll_groups_mask", spdk_cpuset_fmt(g_poll_groups_mask));
	}
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
SPDK_SUBSYSTEM_DEPEND(nvmf, sock)
