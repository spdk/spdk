/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/assert.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"
#include "spdk/uuid.h"
#include "spdk/json.h"
#include "spdk/file.h"
#include "spdk/bit_array.h"
#include "spdk/bdev.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk_internal/utf.h"
#include "spdk_internal/usdt.h"
#include "nvmf_reservation.h"

static bool  ns_rbd_is_ptpl_capable(const struct spdk_nvmf_ns *ns);

static int ns_rdb_update(const struct spdk_nvmf_ns *ns,
			 const struct spdk_nvmf_reservation_info *info);

static int  ns_rdb_load(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info);
static bool rbd_ops_set = false;

int ns_rbd_metadata_updated(void *ns_p);

static struct spdk_nvmf_ns_reservation_ops g_rbd_ops = {
	.is_ptpl_capable = ns_rbd_is_ptpl_capable,
	.update = ns_rdb_update,
	.load = ns_rdb_load,
};

void
spdk_try_rbd_reservation_ops_set(struct spdk_bdev *bdev)
{
	if (bdev && bdev->fn_table->get_module_type) {
		if (bdev->fn_table->get_module_type(NULL) == SPDK_BDEV_RDB) {
			if (rbd_ops_set == false) {
				spdk_nvmf_set_custom_ns_reservation_ops(&g_rbd_ops);
				SPDK_NOTICELOG("reservation custom ops set for for bdev_rbd \n");
				rbd_ops_set = true;
			}
		}
	}
}

static bool
ns_rbd_is_ptpl_capable(const struct spdk_nvmf_ns *ns)
{
	bool rc = false;
	struct spdk_bdev *bdev = ns->bdev;
	if (ns->ptpl_file) {
		rc = bdev->fn_table->ns_reservation_is_ptpl_enabled(bdev, (void *)ns,
				(void *)ns_rbd_metadata_updated);
	}
	return rc;
}

static int
ns_rdb_update(const struct spdk_nvmf_ns *ns,
	      const struct spdk_nvmf_reservation_info *info)
{
	struct spdk_json_write_ctx *w;
	uint32_t i;
	int rc = 0;
	struct spdk_bdev *bdev = ns->bdev;

	if (g_rbd_ops.is_ptpl_capable(ns) == false) {
		goto exit;
	}
	rc = bdev->fn_table->ns_reservation_update_json(bdev, &w);
	if (rc) {
		SPDK_ERRLOG("reservation metadata update failed for NS %d\n", ns->nsid);
		goto exit;
	}

	spdk_json_write_named_bool(w, "ptpl", info->ptpl_activated);
	spdk_json_write_named_uint32(w, "rtype", info->rtype);
	spdk_json_write_named_uint64(w, "crkey", info->crkey);
	spdk_json_write_named_string(w, "bdev_uuid", info->bdev_uuid);
	spdk_json_write_named_string(w, "holder_uuid", info->holder_uuid);

	spdk_json_write_named_array_begin(w, "registrants");
	for (i = 0; i < info->num_regs; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "rkey", info->registrants[i].rkey);
		spdk_json_write_named_string(w, "host_uuid", info->registrants[i].host_uuid);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	rc = spdk_json_write_end(w);
	SPDK_INFOLOG(reservation, "updated persistent storage for NS %d bdev %s\n", ns->nsid,
		     info->bdev_uuid);
exit:
	return rc;
}

static const struct spdk_json_object_decoder nvmf_ns_pr_reg_decoders[] = {
	{"rkey", offsetof(struct _nvmf_ns_registrant, rkey), spdk_json_decode_uint64},
	{"host_uuid", offsetof(struct _nvmf_ns_registrant, host_uuid), spdk_json_decode_string},
};

static int
nvmf_decode_ns_pr_reg(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrant *reg = out;

	return spdk_json_decode_object(val, nvmf_ns_pr_reg_decoders,
				       SPDK_COUNTOF(nvmf_ns_pr_reg_decoders), reg);
}

static int
nvmf_decode_ns_pr_regs(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrants *regs = out;

	return spdk_json_decode_array(val, nvmf_decode_ns_pr_reg, regs->reg,
				      SPDK_NVMF_MAX_NUM_REGISTRANTS, &regs->num_regs,
				      sizeof(struct _nvmf_ns_registrant));
}

static const struct spdk_json_object_decoder nvmf_ns_pr_decoders[] = {
	{"ptpl", offsetof(struct _nvmf_ns_reservation, ptpl_activated), spdk_json_decode_bool, true},
	{"rtype", offsetof(struct _nvmf_ns_reservation, rtype), spdk_json_decode_uint32, true},
	{"crkey", offsetof(struct _nvmf_ns_reservation, crkey), spdk_json_decode_uint64, true},
	{"bdev_uuid", offsetof(struct _nvmf_ns_reservation, bdev_uuid), spdk_json_decode_string},
	{"holder_uuid", offsetof(struct _nvmf_ns_reservation, holder_uuid), spdk_json_decode_string, true},
	{"registrants", offsetof(struct _nvmf_ns_reservation, regs), nvmf_decode_ns_pr_regs},
};

static int
ns_rdb_load(const struct spdk_nvmf_ns *ns,
	    struct spdk_nvmf_reservation_info *info)
{
	int json_size;
	int values_cnt, rc = 0;
	void *json = NULL, *end;
	struct spdk_json_val *values = NULL;
	struct _nvmf_ns_reservation res = {};
	uint32_t i;

	struct spdk_bdev *bdev = ns->bdev;
	info->ptpl_activated = 0;
	info->num_regs = 0;
	rc = bdev->fn_table->ns_reservation_load_json(bdev, &json, &json_size);
	if (rc != 0) {
		SPDK_NOTICELOG("Subsystem load reservation failed, rc %d, ns %d\n", rc,  ns->nsid);
		rc = 0;// this is not a fatal error so we cannot fail on creating NS
		goto exit;
	}
	SPDK_INFOLOG(reservation, "Loaded Json string for NS %d  %s, size %u\n", ns->nsid,
		     (const char *)json, json_size);

	rc = spdk_json_parse(json, json_size, NULL, 0, &end, 0);
	if (rc < 0) {
		SPDK_NOTICELOG("Parsing JSON configuration failed (%d)\n", rc);
		goto exit;
	}

	values_cnt = rc;
	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	if (values == NULL) {
		goto exit;
	}

	rc = spdk_json_parse(json, json_size, values, values_cnt, &end, 0);
	if (rc != values_cnt) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%d)\n", rc);
		goto exit;
	}

	/* Decode json */
	if (spdk_json_decode_object_relaxed(values, nvmf_ns_pr_decoders,
				    SPDK_COUNTOF(nvmf_ns_pr_decoders), &res)) {
		SPDK_ERRLOG("Invalid objects in the persist file\n");
		rc = -EINVAL;
		goto exit;
	}

	if (res.regs.num_regs > SPDK_NVMF_MAX_NUM_REGISTRANTS) {
		SPDK_ERRLOG("Can only support up to %u registrants\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
		rc = -ERANGE;
		goto exit;
	}

	rc = 0;
	info->ptpl_activated = res.ptpl_activated;
	info->rtype = res.rtype;
	info->crkey = res.crkey;
	snprintf(info->bdev_uuid, sizeof(info->bdev_uuid), "%s", res.bdev_uuid);
	snprintf(info->holder_uuid, sizeof(info->holder_uuid), "%s", res.holder_uuid);
	info->num_regs = res.regs.num_regs;
	for (i = 0; i < res.regs.num_regs; i++) {
		info->registrants[i].rkey = res.regs.reg[i].rkey;
		snprintf(info->registrants[i].host_uuid, sizeof(info->registrants[i].host_uuid), "%s",
			 res.regs.reg[i].host_uuid);
	}
	/* to update the epoch */
	bdev->fn_table->ns_reservation_increment_epoch(bdev);
exit:
	free(json);
	free(values);
	free(res.bdev_uuid);
	free(res.holder_uuid);
	for (i = 0; i < res.regs.num_regs; i++) {
		free(res.regs.reg[i].host_uuid);
	}
	return rc;
}

int
ns_rbd_metadata_updated(void *ns_p)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)ns_p;
	struct spdk_nvmf_reservation_info info;

	pthread_mutex_lock(&ns->subsystem->mutex);
	int rc = g_rbd_ops.load(ns, &info);
	if (rc != 0) {
		SPDK_NOTICELOG("Subsystem load reservation failed, rc %d\n", rc);
		goto exit;
	}
	nvmf_ns_reservation_clear_all_registrants(ns);
	rc = nvmf_ns_reservation_restore(ns, &info);
	if (rc) {
		SPDK_ERRLOG("Subsystem restore reservation failed\n");
		goto exit;
	}
	SPDK_INFOLOG(reservation, "reservation change was loaded for NS %d\n", ns->nsid);
exit:
	pthread_mutex_unlock(&ns->subsystem->mutex);
	return rc;
}
