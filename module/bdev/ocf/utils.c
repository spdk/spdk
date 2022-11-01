/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"

#include "utils.h"
#include "vbdev_ocf.h"

static char *cache_modes[ocf_cache_mode_max] = {
	[ocf_cache_mode_wt] = "wt",
	[ocf_cache_mode_wb] = "wb",
	[ocf_cache_mode_wa] = "wa",
	[ocf_cache_mode_pt] = "pt",
	[ocf_cache_mode_wi] = "wi",
	[ocf_cache_mode_wo] = "wo",
};

static char *seqcutoff_policies[ocf_seq_cutoff_policy_max] = {
	[ocf_seq_cutoff_policy_always] = "always",
	[ocf_seq_cutoff_policy_full] = "full",
	[ocf_seq_cutoff_policy_never] = "never",
};

ocf_cache_mode_t
ocf_get_cache_mode(const char *cache_mode)
{
	int i;

	for (i = 0; i < ocf_cache_mode_max; i++) {
		if (strcmp(cache_mode, cache_modes[i]) == 0) {
			return i;
		}
	}

	return ocf_cache_mode_none;
}

const char *
ocf_get_cache_modename(ocf_cache_mode_t mode)
{
	if (mode > ocf_cache_mode_none && mode < ocf_cache_mode_max) {
		return cache_modes[mode];
	} else {
		return NULL;
	}
}

int
ocf_get_cache_line_size(ocf_cache_t cache)
{
	return ocf_cache_get_line_size(cache) / KiB;
}

ocf_seq_cutoff_policy
ocf_get_seqcutoff_policy(const char *policy_name)
{
	int policy;

	for (policy = 0; policy < ocf_seq_cutoff_policy_max; policy++)
		if (!strcmp(policy_name, seqcutoff_policies[policy])) {
			return policy;
		}

	return ocf_seq_cutoff_policy_max;
}

int
vbdev_ocf_mngt_start(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn *path,
		     vbdev_ocf_mngt_callback cb, void *cb_arg)
{
	if (vbdev->mngt_ctx.current_step) {
		return -EBUSY;
	}

	memset(&vbdev->mngt_ctx, 0, sizeof(vbdev->mngt_ctx));

	vbdev->mngt_ctx.current_step = path;
	vbdev->mngt_ctx.cb = cb;
	vbdev->mngt_ctx.cb_arg = cb_arg;

	(*vbdev->mngt_ctx.current_step)(vbdev);

	return 0;
}

void
vbdev_ocf_mngt_stop(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn *rollback_path, int status)
{
	if (status) {
		vbdev->mngt_ctx.status = status;
	}

	if (vbdev->mngt_ctx.status && rollback_path) {
		vbdev->mngt_ctx.poller_fn = NULL;
		vbdev->mngt_ctx.current_step = rollback_path;
		(*vbdev->mngt_ctx.current_step)(vbdev);
		return;
	}

	if (vbdev->mngt_ctx.cb) {
		vbdev->mngt_ctx.cb(vbdev->mngt_ctx.status, vbdev, vbdev->mngt_ctx.cb_arg);
	}

	memset(&vbdev->mngt_ctx, 0, sizeof(vbdev->mngt_ctx));
}

void
vbdev_ocf_mngt_continue(struct vbdev_ocf *vbdev, int status)
{
	if (vbdev->mngt_ctx.current_step == NULL) {
		return;
	}

	assert((*vbdev->mngt_ctx.current_step) != NULL);

	vbdev->mngt_ctx.status = status;

	vbdev->mngt_ctx.current_step++;
	if (*vbdev->mngt_ctx.current_step) {
		(*vbdev->mngt_ctx.current_step)(vbdev);
		return;
	}

	vbdev_ocf_mngt_stop(vbdev, NULL, 0);
}

int
vbdev_ocf_mngt_get_status(struct vbdev_ocf *vbdev)
{
	return vbdev->mngt_ctx.status;
}
