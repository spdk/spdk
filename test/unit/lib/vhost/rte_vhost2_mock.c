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

#include "spdk/stdinc.h"

#include <rte_vhost2.h>

#define UT_MAX_VHOST_TARGETS 8
#define UT_MAX_VHOST_DEVS_PER_TARGET 8

struct ut_vhost_dev {
	bool occupied;
	struct rte_vhost2_dev dev;
	struct ut_vhost_tgt *vtgt;
	uint64_t features;
	int op_rc;
};

struct ut_vhost_tgt {
	bool occupied;
	const char *trtype;
	char *trid;
	uint64_t trflags;
	void *trctx;
	const struct rte_vhost2_tgt_ops *ops;
	uint64_t features;
	struct ut_vhost_dev vdevs[UT_MAX_VHOST_DEVS_PER_TARGET];

	bool unregistered;
	void (*unregister_cb_fn)(void *arg);
	void *unregister_cb_ctx;
};

static struct ut_vhost_tgt g_ut_vhost_tgts[UT_MAX_VHOST_TARGETS];
pthread_mutex_t g_ut_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_ut_vhost_cond = PTHREAD_COND_INITIALIZER;

static void
wake_ut_thread(void)
{
	pthread_mutex_lock(&g_ut_vhost_mutex);
	pthread_cond_signal(&g_ut_vhost_cond);
	pthread_mutex_unlock(&g_ut_vhost_mutex);
}

static struct ut_vhost_tgt *
ut_vhost_tgt_find(const char *trid)
{
	struct ut_vhost_tgt *vtgt;
	int i = 0;

	for (i = 0; i < UT_MAX_VHOST_TARGETS; i++) {
		vtgt = &g_ut_vhost_tgts[i];
		if (vtgt->occupied && strcmp(vtgt->trid, trid) == 0) {
			return vtgt;
		}
	}

	return NULL;
}

static struct ut_vhost_dev *
ut_vhost_tgt_create_device(struct ut_vhost_tgt *vtgt)
{
	struct ut_vhost_dev *vdev;
	int i = 0;

	pthread_mutex_lock(&g_ut_vhost_mutex);
	assert(!vtgt->unregistered);

	for (i = 0; i < UT_MAX_VHOST_DEVS_PER_TARGET; i++) {
		vdev = &vtgt->vdevs[i];
		if (!vdev->occupied) {
			break;
		}
	}

	if (i == UT_MAX_VHOST_DEVS_PER_TARGET) {
		fprintf(stderr, "please increase UT_MAX_VHOST_DEVS_PER_TARGET\n");
		abort();
	}

	memset(vdev, 0, sizeof(*vdev));
	vdev->vtgt = vtgt;
	vdev->occupied = true;

	vtgt->ops->device_create(&vdev->dev, vtgt->trtype, vtgt->trid);
	pthread_cond_wait(&g_ut_vhost_cond, &g_ut_vhost_mutex);
	pthread_mutex_unlock(&g_ut_vhost_mutex);

	if (vdev->op_rc != 0) {
		vdev->occupied = false;
		return NULL;
	}

	return vdev;
}

static int
ut_vhost_tgt_destroy_device(struct ut_vhost_dev *vdev)
{
	int i = 0;

	pthread_mutex_lock(&g_ut_vhost_mutex);
	vdev->vtgt->ops->device_destroy(&vdev->dev);
	pthread_cond_wait(&g_ut_vhost_cond, &g_ut_vhost_mutex);
	vdev->occupied = false;
	pthread_mutex_unlock(&g_ut_vhost_mutex);

	return vdev->op_rc;
}

int
rte_vhost2_tgt_register(const char *trtype, const char *trid,
		uint64_t trflags, void *trctx,
		const struct rte_vhost2_tgt_ops *tgt_ops,
		uint64_t features)
{
	struct ut_vhost_tgt *vtgt;
	int i = 0;

	pthread_mutex_lock(&g_ut_vhost_mutex);
	for (i = 0; i < UT_MAX_VHOST_TARGETS; i++) {
		vtgt = &g_ut_vhost_tgts[i];
		if (!vtgt->occupied) {
			break;
		}
	}

	if (i == UT_MAX_VHOST_TARGETS) {
		fprintf(stderr, "please increase UT_MAX_VHOST_TARGETS\n");
		abort();
	}

	memset(vtgt, 0, sizeof(*vtgt));
	vtgt->trtype = "ut";
	vtgt->trid = strdup(trid);
	if (vtgt->trid == NULL) {
		pthread_mutex_unlock(&g_ut_vhost_mutex);
		return -ENOMEM;
	}
	vtgt->trflags = trflags;
	vtgt->trctx = trctx;
	vtgt->ops = tgt_ops;
	vtgt->features = features;
	vtgt->occupied = true;
	pthread_mutex_unlock(&g_ut_vhost_mutex);
}

void
rte_vhost2_dev_op_complete(struct rte_vhost2_dev *dev, int rc)
{
	struct ut_vhost_dev *vdev = SPDK_CONTAINEROF(dev, struct ut_vhost_dev, dev);

	vdev->op_rc = rc;
	wake_ut_thread();
}

static void *
tgt_unregister_cb(void *arg)
{
	struct ut_vhost_tgt *vtgt = arg;
	void (*cb_fn)(void *arg);
	void *cb_ctx;
	int i;

	pthread_mutex_lock(&g_ut_vhost_mutex);
	vtgt->occupied = false;
	cb_fn = vtgt->unregister_cb_fn;
	cb_ctx = vtgt->unregister_cb_ctx;
	pthread_mutex_unlock(&g_ut_vhost_mutex);
	cb_fn(cb_ctx);
	return NULL;
}

int
rte_vhost2_tgt_unregister(const char *trtype, const char *trid,
		void (*cb_fn)(void *arg), void *cb_ctx)
{
	struct ut_vhost_tgt *vtgt;
	pthread_t tid;
	int i, rc;

	pthread_mutex_lock(&g_ut_vhost_mutex);
	vtgt = ut_vhost_tgt_find(trid);
	if (vtgt == NULL || vtgt->unregistered) {
		return -ENODEV;
	}

	vtgt->unregistered = true;
	vtgt->unregister_cb_fn = cb_fn;
	vtgt->unregister_cb_ctx = cb_ctx;
	int rc;

	/* the callback must be deferred */
	rc = pthread_create(&tid, NULL, &tgt_unregister_cb, vtgt);
	if (rc < 0) {
		fprintf(stderr, "pthread_create failed\n");
		abort();
	}

	pthread_detach(tid);
	pthread_mutex_unlock(&g_ut_vhost_mutex);
}
