/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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
 * Set of workarounds for rte_vhost to make it work with device types
 * other than vhost-net.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"

#include "spdk_internal/vhost_user.h"

#ifndef SPDK_CONFIG_VHOST_INTERNAL_LIB
extern const struct vhost_device_ops g_spdk_vhost_ops;

static enum rte_vhost_msg_result
spdk_extern_vhost_pre_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;

	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to unitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}

	switch (msg->request) {
	case VHOST_USER_SET_VRING_BASE:
	case VHOST_USER_SET_VRING_ADDR:
	case VHOST_USER_SET_VRING_NUM:
		/* We might be forcefully polling the session, so stop it now */
		if (vsession->lcore != -1) {
			g_spdk_vhost_ops.destroy_device(vid);
		}
		break;
	case VHOST_USER_SET_MEM_TABLE:
		if (vsession->lcore != -1) {
			g_spdk_vhost_ops.destroy_device(vid);
			vsession->needs_restart = true;
		}
		break;
	case VHOST_USER_GET_CONFIG: {
		struct vhost_user_config *cfg = &msg->payload.cfg;
		int rc = 0;

		spdk_vhost_lock();
		if (vsession->vdev->backend->vhost_get_config) {
			rc = vsession->vdev->backend->vhost_get_config(vsession->vdev,
				cfg->region, cfg->size);
			if (rc != 0) {
				msg->size = 0;
			}
		}
		spdk_vhost_unlock();

		return RTE_VHOST_MSG_RESULT_REPLY;
	}
	case VHOST_USER_SET_CONFIG: {
		struct vhost_user_config *cfg = &msg->payload.cfg;
		int rc = 0;

		spdk_vhost_lock();
		if (vsession->vdev->backend->vhost_set_config) {
			rc = vsession->vdev->backend->vhost_set_config(vsession->vdev,
				cfg->region, cfg->offset, cfg->size, cfg->flags);
		}
		spdk_vhost_unlock();

		return rc == 0 ? RTE_VHOST_MSG_RESULT_OK : RTE_VHOST_MSG_RESULT_ERR;
	}
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

static enum rte_vhost_msg_result
spdk_extern_vhost_post_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;

	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to unitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}

	if (vsession->needs_restart) {
		g_spdk_vhost_ops.new_device(vid);
		vsession->needs_restart = false;
	}

	switch (msg->request) {
	case VHOST_USER_SET_MEM_TABLE:
		break;
	case VHOST_USER_SET_OWNER:
		vsession->needs_forced_poll = true;
		break;
	case VHOST_USER_SET_VRING_KICK:
		if (vsession->needs_forced_poll) {
			g_spdk_vhost_ops.new_device(vid);
			vsession->needs_forced_poll = false;
		}
		break;
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

struct rte_vhost_user_extern_ops g_spdk_extern_vhost_ops = {
	.pre_msg_handle = spdk_extern_vhost_pre_msg_handler,
	.post_msg_handle = spdk_extern_vhost_post_msg_handler,
};

void
spdk_vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession)
{
	int rc;

	rc = rte_vhost_extern_callback_register(vsession->vid, &g_spdk_extern_vhost_ops, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("rte_vhost_extern_callback_register() failed for vid = %d\n",
			    vsession->vid);
		return;
	}
}

void
spdk_vhost_dev_install_rte_compat_hooks(struct spdk_vhost_dev *vdev)
{
	uint64_t protocol_features = 0;

	rte_vhost_driver_get_protocol_features(vdev->path, &protocol_features);
	protocol_features |= (1ULL << VHOST_USER_PROTOCOL_F_CONFIG);
	rte_vhost_driver_set_protocol_features(vdev->path, protocol_features);
}

#else /* SPDK_CONFIG_VHOST_INTERNAL_LIB */
void
spdk_vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession)
{
	/* nothing to do. all the changes are already incorporated into rte_vhost */
}

void
spdk_vhost_dev_install_rte_compat_hooks(struct spdk_vhost_dev *vdev)
{
	/* nothing to do */
}
#endif
