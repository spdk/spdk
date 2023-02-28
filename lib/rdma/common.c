/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include <rdma/rdma_cma.h>

#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk_internal/rdma.h"
#include "spdk_internal/assert.h"

struct spdk_rdma_device {
	struct ibv_pd				*pd;
	struct ibv_context			*context;
	int					ref;
	bool					removed;
	TAILQ_ENTRY(spdk_rdma_device)		tailq;
};

struct spdk_rdma_mem_map {
	struct spdk_mem_map		*map;
	struct ibv_pd			*pd;
	struct spdk_nvme_rdma_hooks	*hooks;
	uint32_t ref_count;
	enum spdk_rdma_memory_map_role role;
	LIST_ENTRY(spdk_rdma_mem_map) link;
};

static pthread_mutex_t g_dev_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ibv_context **g_ctx_list = NULL;
static TAILQ_HEAD(, spdk_rdma_device) g_dev_list = TAILQ_HEAD_INITIALIZER(g_dev_list);

static LIST_HEAD(, spdk_rdma_mem_map) g_rdma_mr_maps = LIST_HEAD_INITIALIZER(&g_rdma_mr_maps);
static pthread_mutex_t g_rdma_mr_maps_mutex = PTHREAD_MUTEX_INITIALIZER;

static int
rdma_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
		enum spdk_mem_map_notify_action action,
		void *vaddr, size_t size)
{
	struct spdk_rdma_mem_map *rmap = cb_ctx;
	struct ibv_pd *pd = rmap->pd;
	struct ibv_mr *mr;
	uint32_t access_flags = 0;
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (rmap->hooks && rmap->hooks->get_rkey) {
			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, rmap->hooks->get_rkey(pd, vaddr,
							  size));
		} else {
			switch (rmap->role) {
			case SPDK_RDMA_MEMORY_MAP_ROLE_TARGET:
				access_flags = IBV_ACCESS_LOCAL_WRITE;
				if (pd->context->device->transport_type == IBV_TRANSPORT_IWARP) {
					/* IWARP requires REMOTE_WRITE permission for RDMA_READ operation */
					access_flags |= IBV_ACCESS_REMOTE_WRITE;
				}
				break;
			case SPDK_RDMA_MEMORY_MAP_ROLE_INITIATOR:
				access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
				break;
			default:
				SPDK_UNREACHABLE();
			}
#ifdef IBV_ACCESS_OPTIONAL_FIRST
			access_flags |= IBV_ACCESS_RELAXED_ORDERING;
#endif
			mr = ibv_reg_mr(pd, vaddr, size, access_flags);
			if (mr == NULL) {
				SPDK_ERRLOG("ibv_reg_mr() failed\n");
				return -1;
			} else {
				rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
			}
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		if (rmap->hooks == NULL || rmap->hooks->get_rkey == NULL) {
			mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr, NULL);
			if (mr) {
				ibv_dereg_mr(mr);
			}
		}
		rc = spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return rc;
}

static int
rdma_check_contiguous_entries(uint64_t addr_1, uint64_t addr_2)
{
	/* Two contiguous mappings will point to the same address which is the start of the RDMA MR. */
	return addr_1 == addr_2;
}

const struct spdk_mem_map_ops g_rdma_map_ops = {
	.notify_cb = rdma_mem_notify,
	.are_contiguous = rdma_check_contiguous_entries
};

static void
_rdma_free_mem_map(struct spdk_rdma_mem_map *map)
{
	assert(map);

	if (map->hooks) {
		spdk_free(map);
	} else {
		free(map);
	}
}

struct spdk_rdma_mem_map *
spdk_rdma_create_mem_map(struct ibv_pd *pd, struct spdk_nvme_rdma_hooks *hooks,
			 enum spdk_rdma_memory_map_role role)
{
	struct spdk_rdma_mem_map *map;

	pthread_mutex_lock(&g_rdma_mr_maps_mutex);
	/* Look up existing mem map registration for this pd */
	LIST_FOREACH(map, &g_rdma_mr_maps, link) {
		if (map->pd == pd && map->role == role) {
			map->ref_count++;
			pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
			return map;
		}
	}

	if (hooks) {
		map = spdk_zmalloc(sizeof(*map), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	} else {
		map = calloc(1, sizeof(*map));
	}
	if (!map) {
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}
	map->pd = pd;
	map->ref_count = 1;
	map->hooks = hooks;
	map->role = role;
	map->map = spdk_mem_map_alloc(0, &g_rdma_map_ops, map);
	if (!map->map) {
		SPDK_ERRLOG("Unable to create memory map\n");
		_rdma_free_mem_map(map);
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		return NULL;
	}
	LIST_INSERT_HEAD(&g_rdma_mr_maps, map, link);

	pthread_mutex_unlock(&g_rdma_mr_maps_mutex);

	return map;
}

void
spdk_rdma_free_mem_map(struct spdk_rdma_mem_map **_map)
{
	struct spdk_rdma_mem_map *map;

	if (!_map) {
		return;
	}

	map = *_map;
	if (!map) {
		return;
	}
	*_map = NULL;

	pthread_mutex_lock(&g_rdma_mr_maps_mutex);
	assert(map->ref_count > 0);
	map->ref_count--;
	if (map->ref_count != 0) {
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		return;
	}

	LIST_REMOVE(map, link);
	pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
	if (map->map) {
		spdk_mem_map_free(&map->map);
	}
	_rdma_free_mem_map(map);
}

int
spdk_rdma_get_translation(struct spdk_rdma_mem_map *map, void *address,
			  size_t length, struct spdk_rdma_memory_translation *translation)
{
	uint64_t real_length = length;

	assert(map);
	assert(address);
	assert(translation);

	if (map->hooks && map->hooks->get_rkey) {
		translation->translation_type = SPDK_RDMA_TRANSLATION_KEY;
		translation->mr_or_key.key = spdk_mem_map_translate(map->map, (uint64_t)address, &real_length);
	} else {
		translation->translation_type = SPDK_RDMA_TRANSLATION_MR;
		translation->mr_or_key.mr = (struct ibv_mr *)spdk_mem_map_translate(map->map, (uint64_t)address,
					    &real_length);
		if (spdk_unlikely(!translation->mr_or_key.mr)) {
			SPDK_ERRLOG("No translation for ptr %p, size %zu\n", address, length);
			return -EINVAL;
		}
	}

	assert(real_length >= length);

	return 0;
}

struct spdk_rdma_srq *
spdk_rdma_srq_create(struct spdk_rdma_srq_init_attr *init_attr)
{
	assert(init_attr);
	assert(init_attr->pd);

	struct spdk_rdma_srq *rdma_srq = calloc(1, sizeof(*rdma_srq));

	if (!rdma_srq) {
		SPDK_ERRLOG("Can't allocate memory for SRQ handle\n");
		return NULL;
	}

	if (init_attr->stats) {
		rdma_srq->stats = init_attr->stats;
		rdma_srq->shared_stats = true;
	} else {
		rdma_srq->stats = calloc(1, sizeof(*rdma_srq->stats));
		if (!rdma_srq->stats) {
			SPDK_ERRLOG("SRQ statistics memory allocation failed");
			free(rdma_srq);
			return NULL;
		}
	}

	rdma_srq->srq = ibv_create_srq(init_attr->pd, &init_attr->srq_init_attr);
	if (!rdma_srq->srq) {
		if (!init_attr->stats) {
			free(rdma_srq->stats);
		}
		SPDK_ERRLOG("Unable to create SRQ, errno %d (%s)\n", errno, spdk_strerror(errno));
		free(rdma_srq);
		return NULL;
	}

	return rdma_srq;
}

int
spdk_rdma_srq_destroy(struct spdk_rdma_srq *rdma_srq)
{
	int rc;

	if (!rdma_srq) {
		return 0;
	}

	assert(rdma_srq->srq);

	if (rdma_srq->recv_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying RDMA SRQ with queued recv WRs\n");
	}

	rc = ibv_destroy_srq(rdma_srq->srq);
	if (rc) {
		SPDK_ERRLOG("SRQ destroy failed with %d\n", rc);
	}

	if (!rdma_srq->shared_stats) {
		free(rdma_srq->stats);
	}

	free(rdma_srq);

	return rc;
}

static inline bool
rdma_queue_recv_wrs(struct spdk_rdma_recv_wr_list *recv_wrs, struct ibv_recv_wr *first,
		    struct spdk_rdma_wr_stats *recv_stats)
{
	struct ibv_recv_wr *last;

	recv_stats->num_submitted_wrs++;
	last = first;
	while (last->next != NULL) {
		last = last->next;
		recv_stats->num_submitted_wrs++;
	}

	if (recv_wrs->first == NULL) {
		recv_wrs->first = first;
		recv_wrs->last = last;
		return true;
	} else {
		recv_wrs->last->next = first;
		recv_wrs->last = last;
		return false;
	}
}

bool
spdk_rdma_srq_queue_recv_wrs(struct spdk_rdma_srq *rdma_srq, struct ibv_recv_wr *first)
{
	assert(rdma_srq);
	assert(first);

	return rdma_queue_recv_wrs(&rdma_srq->recv_wrs, first, rdma_srq->stats);
}

int
spdk_rdma_srq_flush_recv_wrs(struct spdk_rdma_srq *rdma_srq, struct ibv_recv_wr **bad_wr)
{
	int rc;

	if (spdk_unlikely(rdma_srq->recv_wrs.first == NULL)) {
		return 0;
	}

	rc = ibv_post_srq_recv(rdma_srq->srq, rdma_srq->recv_wrs.first, bad_wr);

	rdma_srq->recv_wrs.first = NULL;
	rdma_srq->stats->doorbell_updates++;

	return rc;
}

bool
spdk_rdma_qp_queue_recv_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_recv_wr *first)
{
	assert(spdk_rdma_qp);
	assert(first);

	return rdma_queue_recv_wrs(&spdk_rdma_qp->recv_wrs, first, &spdk_rdma_qp->stats->recv);
}

int
spdk_rdma_qp_flush_recv_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_recv_wr **bad_wr)
{
	int rc;

	if (spdk_unlikely(spdk_rdma_qp->recv_wrs.first == NULL)) {
		return 0;
	}

	rc = ibv_post_recv(spdk_rdma_qp->qp, spdk_rdma_qp->recv_wrs.first, bad_wr);

	spdk_rdma_qp->recv_wrs.first = NULL;
	spdk_rdma_qp->stats->recv.doorbell_updates++;

	return rc;
}

static struct spdk_rdma_device *
rdma_add_dev(struct ibv_context *context)
{
	struct spdk_rdma_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate RDMA device object.\n");
		return NULL;
	}

	dev->pd = ibv_alloc_pd(context);
	if (dev->pd == NULL) {
		SPDK_ERRLOG("ibv_alloc_pd() failed: %s (%d)\n", spdk_strerror(errno), errno);
		free(dev);
		return NULL;
	}

	dev->context = context;
	TAILQ_INSERT_TAIL(&g_dev_list, dev, tailq);

	return dev;
}

static void
rdma_remove_dev(struct spdk_rdma_device *dev)
{
	if (!dev->removed || dev->ref > 0) {
		return;
	}

	/* Deallocate protection domain only if the device is already removed and
	 * there is no reference.
	 */
	TAILQ_REMOVE(&g_dev_list, dev, tailq);
	ibv_dealloc_pd(dev->pd);
	free(dev);
}

static int
ctx_cmp(const void *_c1, const void *_c2)
{
	struct ibv_context *c1 = *(struct ibv_context **)_c1;
	struct ibv_context *c2 = *(struct ibv_context **)_c2;

	return c1 < c2 ? -1 : c1 > c2;
}

static int
rdma_sync_dev_list(void)
{
	struct ibv_context **new_ctx_list;
	int i, j;
	int num_devs = 0;

	/*
	 * rdma_get_devices() returns a NULL terminated array of opened RDMA devices,
	 * and sets num_devs to the number of the returned devices.
	 */
	new_ctx_list = rdma_get_devices(&num_devs);
	if (new_ctx_list == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		return -ENODEV;
	}

	if (num_devs == 0) {
		rdma_free_devices(new_ctx_list);
		SPDK_ERRLOG("Returned RDMA device array was empty\n");
		return -ENODEV;
	}

	/*
	 * Sort new_ctx_list by addresses to update devices easily.
	 */
	qsort(new_ctx_list, num_devs, sizeof(struct ibv_context *), ctx_cmp);

	if (g_ctx_list == NULL) {
		/* If no old array, this is the first call. Add all devices. */
		for (i = 0; new_ctx_list[i] != NULL; i++) {
			rdma_add_dev(new_ctx_list[i]);
		}

		goto exit;
	}

	for (i = j = 0; new_ctx_list[i] != NULL || g_ctx_list[j] != NULL;) {
		struct ibv_context *new_ctx = new_ctx_list[i];
		struct ibv_context *old_ctx = g_ctx_list[j];
		bool add = false, remove = false;

		/*
		 * If a context exists only in the new array, create a device for it,
		 * or if a context exists only in the old array, try removing the
		 * corresponding device.
		 */

		if (old_ctx == NULL) {
			add = true;
		} else if (new_ctx == NULL) {
			remove = true;
		} else if (new_ctx < old_ctx) {
			add = true;
		} else if (old_ctx < new_ctx) {
			remove = true;
		}

		if (add) {
			rdma_add_dev(new_ctx_list[i]);
			i++;
		} else if (remove) {
			struct spdk_rdma_device *dev, *tmp;

			TAILQ_FOREACH_SAFE(dev, &g_dev_list, tailq, tmp) {
				if (dev->context == g_ctx_list[j]) {
					dev->removed = true;
					rdma_remove_dev(dev);
				}
			}
			j++;
		} else {
			i++;
			j++;
		}
	}

	/* Free the old array. */
	rdma_free_devices(g_ctx_list);

exit:
	/*
	 * Keep the newly returned array so that allocated protection domains
	 * are not freed unexpectedly.
	 */
	g_ctx_list = new_ctx_list;
	return 0;
}

struct ibv_pd *
spdk_rdma_get_pd(struct ibv_context *context)
{
	struct spdk_rdma_device *dev;
	int rc;

	pthread_mutex_lock(&g_dev_mutex);

	rc = rdma_sync_dev_list();
	if (rc != 0) {
		pthread_mutex_unlock(&g_dev_mutex);

		SPDK_ERRLOG("Failed to sync RDMA device list\n");
		return NULL;
	}

	TAILQ_FOREACH(dev, &g_dev_list, tailq) {
		if (dev->context == context && !dev->removed) {
			dev->ref++;
			pthread_mutex_unlock(&g_dev_mutex);

			return dev->pd;
		}
	}

	pthread_mutex_unlock(&g_dev_mutex);

	SPDK_ERRLOG("Failed to get PD\n");
	return NULL;
}

void
spdk_rdma_put_pd(struct ibv_pd *pd)
{
	struct spdk_rdma_device *dev, *tmp;

	pthread_mutex_lock(&g_dev_mutex);

	TAILQ_FOREACH_SAFE(dev, &g_dev_list, tailq, tmp) {
		if (dev->pd == pd) {
			assert(dev->ref > 0);
			dev->ref--;

			rdma_remove_dev(dev);
		}
	}

	rdma_sync_dev_list();

	pthread_mutex_unlock(&g_dev_mutex);
}

__attribute__((destructor)) static void
_rdma_fini(void)
{
	struct spdk_rdma_device *dev, *tmp;

	TAILQ_FOREACH_SAFE(dev, &g_dev_list, tailq, tmp) {
		dev->removed = true;
		dev->ref = 0;
		rdma_remove_dev(dev);
	}

	if (g_ctx_list != NULL) {
		rdma_free_devices(g_ctx_list);
		g_ctx_list = NULL;
	}
}
