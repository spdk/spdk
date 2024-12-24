/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/uuid.h"
#include "spdk/blob.h"

#include "vbdev_lvol.h"

struct vbdev_lvol_io {
	struct spdk_blob_ext_io_opts ext_io_opts;
};

static TAILQ_HEAD(, lvol_store_bdev) g_spdk_lvol_pairs = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_pairs);


// Queue for the the delete lvol requests.
struct lvol_delete_request {
	struct spdk_lvol *lvol;  // Pointer to the lvol to be deleted
	TAILQ_ENTRY(lvol_delete_request) entries;  // Tail queue linkage
};

// TAILQ head structure with size tracking
struct lvol_delete_requests {
	TAILQ_HEAD(, lvol_delete_request) lvol_delete_requests_queue;  // TAILQ head
	bool is_deletion_in_progress; // Any deletion request is in progress.
	size_t size;  // Number of elements in the queue
};

static struct lvol_delete_requests *g_lvol_delete_requests = NULL;

static int vbdev_lvs_init(void);
static void vbdev_lvs_fini_start(void);
static int vbdev_lvs_get_ctx_size(void);
static void vbdev_lvs_examine_config(struct spdk_bdev *bdev);
static void vbdev_lvs_examine_disk(struct spdk_bdev *bdev);
static bool g_shutdown_started = false;

static struct spdk_bdev_module g_lvol_if = {
	.name = "lvol",
	.module_init = vbdev_lvs_init,
	.fini_start = vbdev_lvs_fini_start,
	.async_fini_start = true,
	.examine_config = vbdev_lvs_examine_config,
	.examine_disk = vbdev_lvs_examine_disk,
	.get_ctx_size = vbdev_lvs_get_ctx_size,

};

SPDK_BDEV_MODULE_REGISTER(lvol, &g_lvol_if)

static void _vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg, bool is_sync);
static void _vbdev_lvol_async_delete_internal_cb(void *cb_arg, int lvolerrno);

/* Asyn delete lvol queue implementation Start */
struct vbdev_lvol_async_destroy_internal_ctx {
	char	unique_id[SPDK_LVOL_UNIQUE_ID_MAX];
};

static void lvol_delete_requests_init(void) {
	if(g_lvol_delete_requests == NULL) {
		g_lvol_delete_requests = calloc(1, sizeof(struct lvol_delete_requests));
		if (!g_lvol_delete_requests) {
			SPDK_ERRLOG("Memory full. Delete request will not be queued.");
			return;
		}
		// Initialize TAILQ
		TAILQ_INIT(&g_lvol_delete_requests->lvol_delete_requests_queue);
		g_lvol_delete_requests->size = 0;
		g_lvol_delete_requests->is_deletion_in_progress = false;
	}
}

// Clear the entire queue
static void lvol_delete_requests_clear(void) {
	struct lvol_delete_request *current, *temp;
	if(g_lvol_delete_requests != NULL) {
		TAILQ_FOREACH_SAFE(current, &g_lvol_delete_requests->lvol_delete_requests_queue, entries, temp) {
			TAILQ_REMOVE(&g_lvol_delete_requests->lvol_delete_requests_queue, current, entries);
			free(current);
			current = NULL;
		}
		g_lvol_delete_requests->size = 0;
		g_lvol_delete_requests->is_deletion_in_progress = false;
	}
}

// Enqueue a new delete request (thread-safe)
static int lvol_delete_requests_enqueue(struct spdk_lvol *lvol) {
	struct lvol_delete_request *new_request = NULL;

	if(g_lvol_delete_requests == NULL) {
		return -ENOMEM;
	}

	new_request = calloc(1, sizeof(struct lvol_delete_request));
	if (!new_request) {
		return -ENOMEM;  // Memory allocation failed
	}
	new_request->lvol = lvol;
	TAILQ_INSERT_TAIL(&g_lvol_delete_requests->lvol_delete_requests_queue, new_request, entries);
	g_lvol_delete_requests->size++;

	return 0;
}

// Dequeue and return the first delete request (thread-safe)
static struct spdk_lvol* lvol_delete_requests_dequeue(void) {
	struct lvol_delete_request *first_request = NULL;
	struct spdk_lvol *lvol = NULL;

	if(g_lvol_delete_requests == NULL) {
		return NULL;
	}

	first_request = TAILQ_FIRST(&g_lvol_delete_requests->lvol_delete_requests_queue);
	lvol = first_request ? first_request->lvol : NULL;

	if (first_request) {
		TAILQ_REMOVE(&g_lvol_delete_requests->lvol_delete_requests_queue, first_request, entries);
		free(first_request);
		first_request = NULL;
		g_lvol_delete_requests->size--;
	}
	return lvol;
}

// Check if a specific lvol is already in the queue
static bool lvol_delete_requests_contains(struct spdk_lvol *lvol) {
	// Flag to track if lvol is found
	bool found = false;
	struct lvol_delete_request *current = NULL;

	if(g_lvol_delete_requests == NULL) {
		return found;
	}

    // Traverse the queue
	TAILQ_FOREACH(current, &g_lvol_delete_requests->lvol_delete_requests_queue, entries) {
		if (current->lvol == lvol) {
			found = true;
			break;
		}
	}
	return found;
}
/* Asyn delete lvol queue implementation End */

struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_lvs(struct spdk_lvol_store *lvs_orig)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (lvs == lvs_orig) {
			if (lvs_bdev->removal_in_progress) {
				/* We do not allow access to lvs that are being unloaded or
				 * destroyed */
				SPDK_DEBUGLOG(vbdev_lvol, "lvs %s: removal in progress\n",
					      lvs_orig->name);
				return NULL;
			} else {
				return lvs_bdev;
			}
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	return NULL;
}

static int
_vbdev_lvol_change_bdev_alias(struct spdk_lvol *lvol, const char *new_lvol_name)
{
	struct spdk_bdev_alias *tmp;
	char *old_alias;
	char *alias;
	int rc;
	int alias_number = 0;

	/* bdev representing lvols have only one alias,
	 * while we changed lvs name earlier, we have to iterate alias list to get one,
	 * and check if there is only one alias */

	TAILQ_FOREACH(tmp, spdk_bdev_get_aliases(lvol->bdev), tailq) {
		if (++alias_number > 1) {
			SPDK_ERRLOG("There is more than 1 alias in bdev %s\n", lvol->bdev->name);
			return -EINVAL;
		}

		old_alias = tmp->alias.name;
	}

	if (alias_number == 0) {
		SPDK_ERRLOG("There are no aliases in bdev %s\n", lvol->bdev->name);
		return -EINVAL;
	}

	alias = spdk_sprintf_alloc("%s/%s", lvol->lvol_store->name, new_lvol_name);
	if (alias == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_alias_add(lvol->bdev, alias);
	if (rc != 0) {
		SPDK_ERRLOG("cannot add alias '%s'\n", alias);
		free(alias);
		return rc;
	}
	free(alias);

	rc = spdk_bdev_alias_del(lvol->bdev, old_alias);
	if (rc != 0) {
		SPDK_ERRLOG("cannot remove alias '%s'\n", old_alias);
		return rc;
	}

	return 0;
}

static struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_bdev(struct spdk_bdev *bdev_orig)
{
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		if (lvs_bdev->bdev == bdev_orig) {
			if (lvs_bdev->removal_in_progress) {
				/* We do not allow access to lvs that are being unloaded or
				 * destroyed */
				SPDK_DEBUGLOG(vbdev_lvol, "lvs %s: removal in progress\n",
					      lvs_bdev->lvs->name);
				return NULL;
			} else {
				return lvs_bdev;
			}
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	return NULL;
}

static void
vbdev_lvs_hotremove_cb(struct spdk_bdev *bdev)
{
	struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = vbdev_get_lvs_bdev_by_bdev(bdev);
	if (lvs_bdev != NULL) {
		SPDK_NOTICELOG("bdev %s being removed: closing lvstore %s\n",
			       spdk_bdev_get_name(bdev), lvs_bdev->lvs->name);
		vbdev_lvs_unload(lvs_bdev->lvs, NULL, NULL);
	}
}

static void
vbdev_lvs_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			     void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_lvs_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static void
_vbdev_lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev = req->base_bdev;
	struct spdk_bs_dev *bs_dev = req->bs_dev;

	if (lvserrno != 0) {
		assert(lvs == NULL);
		SPDK_ERRLOG("Cannot create lvol store bdev\n");
		goto end;
	}

	lvserrno = spdk_bs_bdev_claim(bs_dev, &g_lvol_if);
	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store base bdev already claimed by another bdev\n");
		req->bs_dev->destroy(req->bs_dev);
		goto end;
	}

	assert(lvs != NULL);

	lvs_bdev = calloc(1, sizeof(*lvs_bdev));
	if (!lvs_bdev) {
		lvserrno = -ENOMEM;
		goto end;
	}
	lvs_bdev->lvs = lvs;
	lvs_bdev->bdev = bdev;
	lvs_bdev->req = NULL;

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	SPDK_INFOLOG(vbdev_lvol, "Lvol store bdev inserted\n");

end:
	req->cb_fn(req->cb_arg, lvs, lvserrno);
	free(req);

	return;
}

int
vbdev_lvs_create(const char *base_bdev_name, const char *name, uint32_t cluster_sz,
		 enum lvs_clear_method clear_method, uint32_t num_md_pages_per_cluster_ratio,
		 spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_dev *bs_dev;
	struct spdk_lvs_with_handle_req *lvs_req;
	struct spdk_lvs_opts opts;
	int rc;
	int len;

	if (base_bdev_name == NULL) {
		SPDK_ERRLOG("missing base_bdev_name param\n");
		return -EINVAL;
	}

	spdk_lvs_opts_init(&opts);
	if (cluster_sz != 0) {
		opts.cluster_sz = cluster_sz;
	}

	if (clear_method != 0) {
		opts.clear_method = clear_method;
	}

	if (num_md_pages_per_cluster_ratio != 0) {
		opts.num_md_pages_per_cluster_ratio = num_md_pages_per_cluster_ratio;
	}

	if (name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		return -EINVAL;
	}

	len = strnlen(name, SPDK_LVS_NAME_MAX);

	if (len == 0 || len == SPDK_LVS_NAME_MAX) {
		SPDK_ERRLOG("name must be between 1 and %d characters\n", SPDK_LVS_NAME_MAX - 1);
		return -EINVAL;
	}
	snprintf(opts.name, sizeof(opts.name), "%s", name);
	opts.esnap_bs_dev_create = vbdev_lvol_esnap_dev_create;

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_create_bs_dev_ext(base_bdev_name, vbdev_lvs_base_bdev_event_cb,
					 NULL, &bs_dev);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot create blobstore device\n");
		free(lvs_req);
		return rc;
	}

	lvs_req->bs_dev = bs_dev;
	lvs_req->base_bdev = bs_dev->get_base_bdev(bs_dev);
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	rc = spdk_lvs_init(bs_dev, &opts, _vbdev_lvs_create_cb, lvs_req);
	if (rc < 0) {
		free(lvs_req);
		bs_dev->destroy(bs_dev);
		return rc;
	}

	return 0;
}

static void
_vbdev_lvs_rename_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *req = cb_arg;
	struct spdk_lvol *tmp;

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store rename failed\n");
	} else {
		TAILQ_FOREACH(tmp, &req->lvol_store->lvols, link) {
			/* We have to pass current lvol name, since only lvs name changed */
			_vbdev_lvol_change_bdev_alias(tmp, tmp->name);
		}
	}

	req->cb_fn(req->cb_arg, lvserrno);
	free(req);
}

void
vbdev_lvs_rename(struct spdk_lvol_store *lvs, const char *new_lvs_name,
		 spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct lvol_store_bdev *lvs_bdev;

	struct spdk_lvs_req *req;

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol_store = lvs;

	spdk_lvs_rename(lvs, new_lvs_name, _vbdev_lvs_rename_cb, req);
}

static void
_vbdev_lvs_remove_cb(void *cb_arg, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct spdk_lvs_req *req = lvs_bdev->req;

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store removed with error: %d.\n", lvserrno);
	}

	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	free(lvs_bdev);

	if (req->cb_fn != NULL) {
		req->cb_fn(req->cb_arg, lvserrno);
	}
	free(req);
}

static void
_vbdev_lvs_remove_lvol_cb(void *cb_arg, int lvolerrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct spdk_lvol_store *lvs = lvs_bdev->lvs;
	struct spdk_lvol *lvol;

	if (lvolerrno != 0) {
		SPDK_DEBUGLOG(vbdev_lvol, "Lvol removed with errno %d\n", lvolerrno);
	}

	if (TAILQ_EMPTY(&lvs->lvols)) {
		spdk_lvs_destroy(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
		return;
	}

	lvol = TAILQ_FIRST(&lvs->lvols);
	while (lvol != NULL) {
		if (spdk_lvol_deletable(lvol)) {
			_vbdev_lvol_destroy(lvol, _vbdev_lvs_remove_lvol_cb, lvs_bdev, false);
			return;
		}
		lvol = TAILQ_NEXT(lvol, link);
	}

	/* If no lvol is deletable, that means there is circular dependency. */
	SPDK_ERRLOG("Lvols left in lvs, but unable to delete.\n");
	assert(false);
}

static bool
_vbdev_lvs_are_lvols_closed(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol *lvol;

	TAILQ_FOREACH(lvol, &lvs->lvols, link) {
		if (lvol->ref_count != 0) {
			return false;
		}
	}
	return true;
}

static void
_vbdev_lvs_remove_bdev_unregistered_cb(void *cb_arg, int bdeverrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct spdk_lvol_store *lvs = lvs_bdev->lvs;

	if (bdeverrno != 0) {
		SPDK_DEBUGLOG(vbdev_lvol, "Lvol unregistered with errno %d\n", bdeverrno);
	}

	/* Lvol store can be unloaded once all lvols are closed. */
	if (_vbdev_lvs_are_lvols_closed(lvs)) {
		spdk_lvs_unload(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
	}
}

static void
_vbdev_lvs_remove(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg,
		  bool destroy)
{
	struct spdk_lvs_req *req;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_lvol *lvol, *tmp;

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENODEV);
		}
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	lvs_bdev->removal_in_progress = true;

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	lvs_bdev->req = req;

	if (_vbdev_lvs_are_lvols_closed(lvs)) {
		if (destroy) {
			spdk_lvs_destroy(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
			return;
		}
		spdk_lvs_unload(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
		return;
	}
	if (destroy) {
		_vbdev_lvs_remove_lvol_cb(lvs_bdev, 0);
		return;
	}
	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		if (lvol->bdev == NULL) {
			spdk_lvol_close(lvol, _vbdev_lvs_remove_bdev_unregistered_cb, lvs_bdev);
			continue;
		}
		spdk_bdev_unregister(lvol->bdev, _vbdev_lvs_remove_bdev_unregistered_cb, lvs_bdev);
	}
}

void
vbdev_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	_vbdev_lvs_remove(lvs, cb_fn, cb_arg, false);
}

void
vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	_vbdev_lvs_remove(lvs, cb_fn, cb_arg, true);
}

struct lvol_store_bdev *
vbdev_lvol_store_first(void)
{
	struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = TAILQ_FIRST(&g_spdk_lvol_pairs);
	if (lvs_bdev) {
		SPDK_INFOLOG(vbdev_lvol, "Starting lvolstore iteration at %p\n", lvs_bdev->lvs);
	}

	return lvs_bdev;
}

struct lvol_store_bdev *
vbdev_lvol_store_next(struct lvol_store_bdev *prev)
{
	struct lvol_store_bdev *lvs_bdev;

	if (prev == NULL) {
		SPDK_ERRLOG("prev argument cannot be NULL\n");
		return NULL;
	}

	lvs_bdev = TAILQ_NEXT(prev, lvol_stores);
	if (lvs_bdev) {
		SPDK_INFOLOG(vbdev_lvol, "Continuing lvolstore iteration at %p\n", lvs_bdev->lvs);
	}

	return lvs_bdev;
}

static struct spdk_lvol_store *
_vbdev_get_lvol_store_by_uuid(const struct spdk_uuid *uuid)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (spdk_uuid_compare(&lvs->uuid, uuid) == 0) {
			return lvs;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}
	return NULL;
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_uuid(const char *uuid_str)
{
	struct spdk_uuid uuid;

	if (spdk_uuid_parse(&uuid, uuid_str)) {
		return NULL;
	}

	return _vbdev_get_lvol_store_by_uuid(&uuid);
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_name(const char *name)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (strncmp(lvs->name, name, sizeof(lvs->name)) == 0) {
			return lvs;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}
	return NULL;
}

struct vbdev_lvol_destroy_ctx {
	struct spdk_lvol *lvol;
	bool is_sync;
	spdk_lvol_op_complete cb_fn;
	void *cb_arg;
};

static void
_vbdev_lvol_unregister_unload_lvs(void *cb_arg, int lvserrno)
{
	struct lvol_bdev *lvol_bdev = cb_arg;
	struct lvol_store_bdev *lvs_bdev = lvol_bdev->lvs_bdev;

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store removed with error: %d.\n", lvserrno);
	}

	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	free(lvs_bdev);

	spdk_bdev_destruct_done(&lvol_bdev->bdev, lvserrno);
	free(lvol_bdev);
}

static void
_vbdev_lvol_unregister_cb(void *ctx, int lvolerrno)
{
	struct lvol_bdev *lvol_bdev = ctx;
	struct lvol_store_bdev *lvs_bdev = lvol_bdev->lvs_bdev;

	if (g_shutdown_started && _vbdev_lvs_are_lvols_closed(lvs_bdev->lvs)) {
		spdk_lvs_unload(lvs_bdev->lvs, _vbdev_lvol_unregister_unload_lvs, lvol_bdev);
		return;
	}

	spdk_bdev_destruct_done(&lvol_bdev->bdev, lvolerrno);
	free(lvol_bdev);
}

static int
vbdev_lvol_unregister(void *ctx)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_bdev *lvol_bdev;

	assert(lvol != NULL);
	lvol_bdev = SPDK_CONTAINEROF(lvol->bdev, struct lvol_bdev, bdev);

	spdk_bdev_alias_del_all(lvol->bdev);
	spdk_lvol_close(lvol, _vbdev_lvol_unregister_cb, lvol_bdev);

	/* return 1 to indicate we have an operation that must finish asynchronously before the
	 *  lvol is closed
	 */
	return 1;
}

static void
check_and_process_delete_lvol_from_queue(void) {
	struct spdk_lvol *lvol = NULL;
	struct vbdev_lvol_async_destroy_internal_ctx *ctx = NULL;
	// Check is there are any delete requests in the queue to process.
	if(g_lvol_delete_requests->size > 0) {
		// Dequeue the next lvol delete request and process it.
		lvol = lvol_delete_requests_dequeue();
		if(lvol != NULL){
			ctx = calloc(1, sizeof(struct vbdev_lvol_async_destroy_internal_ctx));
			if(ctx) {
				strcpy(ctx->unique_id,lvol->unique_id);
				_vbdev_lvol_destroy(lvol, _vbdev_lvol_async_delete_internal_cb, ctx, false);
			}
		}
	}
	else {
		g_lvol_delete_requests->is_deletion_in_progress = false;
	}
	return;
}

static void
_vbdev_lvol_async_delete_internal_cb(void *cb_arg, int lvolerrno)
{
	struct vbdev_lvol_async_destroy_internal_ctx *ctx = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Error deleting lvol %s, errorcode %d. \n", ctx->unique_id, lvolerrno);
		// Process the queued request in case of error.
		check_and_process_delete_lvol_from_queue();
	}

	if (ctx)
	{
		free(ctx);
	}
	return;
}

static void
bdev_lvol_async_delete_cb(void *cb_arg, int lvolerrno)
{
	struct vbdev_lvol_async_destroy_internal_ctx *ctx = cb_arg;

	if (lvolerrno != 0) {
		// Set the previous error. This will be used to check is the asyn delete lvol request has failed.
		SPDK_ERRLOG("Error deleting lvol %s, errorcode %d. \n", ctx->unique_id, lvolerrno);
	}
	else {
		SPDK_NOTICELOG("lvol %s deleted. \n", ctx->unique_id);
	}

	if(ctx) {
		free(ctx);
	}
	check_and_process_delete_lvol_from_queue();
	return;
}


static void
_vbdev_lvol_destroy_cb(void *cb_arg, int bdeverrno)
{
	struct vbdev_lvol_destroy_ctx *ctx = cb_arg;
	struct spdk_lvol *lvol = ctx->lvol;
	struct vbdev_lvol_async_destroy_internal_ctx *async_ctx = NULL;

	if (bdeverrno < 0) {
		SPDK_INFOLOG(vbdev_lvol, "Could not unregister bdev during lvol (%s) destroy\n",
			     lvol->unique_id);
		ctx->cb_fn(ctx->cb_arg, bdeverrno);
		free(ctx);
		return;
	}

	if(ctx->is_sync == true) {
		spdk_lvol_destroy(lvol, ctx->cb_fn, ctx->cb_arg);
	}
	else {
		// Return immediately and check the delete lvol status later.
		async_ctx = calloc(1, sizeof(struct vbdev_lvol_async_destroy_internal_ctx));
		if(async_ctx){
			strcpy(async_ctx->unique_id, lvol->unique_id);
			spdk_lvol_destroy(lvol, bdev_lvol_async_delete_cb, async_ctx);
			ctx->cb_fn(ctx->cb_arg, 0);
		}
		else {
			ctx->cb_fn(ctx->cb_arg, -ENOMEM);
		}
	}
	if(ctx){
		free(ctx);
	}
}

static void
_vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg, bool is_sync)
{
	struct vbdev_lvol_destroy_ctx *ctx;
	size_t count;

	assert(lvol != NULL);
	assert(cb_fn != NULL);

	/* Callers other than _vbdev_lvs_remove() must ensure the lvstore is not being removed. */
	assert(cb_fn == _vbdev_lvs_remove_lvol_cb ||
	       vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store) != NULL);

	/* Check if it is possible to delete lvol */
	spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
	if (count > 1) {
		/* throw an error */
		SPDK_ERRLOG("Cannot delete lvol\n");
		cb_fn(cb_arg, -EPERM);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->lvol = lvol;
	ctx->is_sync = is_sync;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	if (spdk_lvol_is_degraded(lvol)) {
		spdk_lvol_close(lvol, _vbdev_lvol_destroy_cb, ctx);
		return;
	}

	spdk_bdev_unregister(lvol->bdev, _vbdev_lvol_destroy_cb, ctx);
}

void
vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg, bool is_sync)
{
	struct lvol_store_bdev *lvs_bdev;
	int ret = 0;

	if (lvol->action_in_progress == true) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	/*
	 * During destruction of an lvolstore, _vbdev_lvs_unload() iterates through lvols until they
	 * are all deleted. There may be some IO required
	 */

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	if (lvs_bdev == NULL) {
		SPDK_DEBUGLOG(vbdev_lvol, "lvol %s: lvolstore is being removed\n",
			      lvol->unique_id);
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	if (!lvol->lvol_store->leader) {
		// check blob state it must be CLEAN
		// copy the blob
		SPDK_NOTICELOG("Deleting blob 0x%" PRIx64 " in secondary mode.\n", lvol->blob_id);
		if (spdk_lvol_copy_blob(lvol)) {
			cb_fn(cb_arg, -ENODEV);
			return;
		}
	}
	//Check if any other deletion request is in progress
	if(is_sync == true) {
		if(g_lvol_delete_requests->is_deletion_in_progress == true)
		{
			// Operation not permitted as there is already async delete request in progress.
			SPDK_NOTICELOG("Async delete lvol is already in progress for other LVOLs.\n");
			cb_fn(cb_arg, -EPERM);
			return;
		}
	} 
	else {
		if(g_lvol_delete_requests->is_deletion_in_progress == true)
		{
			// Check is delete request for this lvol is already in queue.
			if(lvol_delete_requests_contains(lvol) == true) {
				// Queue this request and return.
				SPDK_NOTICELOG("Delete lvol request for the lvol %s is already in queue.\n", lvol->unique_id);
				cb_fn(cb_arg, 0);
				return;	
			}
			SPDK_NOTICELOG("Delete lvol for %s is queued, as there are other lvol delete requests in progress.\n",lvol->unique_id);
			ret = lvol_delete_requests_enqueue(lvol);
			cb_fn(cb_arg, ret);
			return;
		} else {
			// Mark lvol deletion is in progress.
			g_lvol_delete_requests->is_deletion_in_progress = true;
		}
	}
	_vbdev_lvol_destroy(lvol, cb_fn, cb_arg, is_sync);
}

static char *
vbdev_lvol_find_name(struct spdk_lvol *lvol, spdk_blob_id blob_id)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *_lvol;

	assert(lvol != NULL);

	lvs = lvol->lvol_store;

	assert(lvs);

	TAILQ_FOREACH(_lvol, &lvs->lvols, link) {
		if (_lvol->blob_id == blob_id) {
			return _lvol->name;
		}
	}

	return NULL;
}

static int
vbdev_lvol_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev;
	struct spdk_blob *blob;
	spdk_blob_id *ids = NULL;
	size_t count, i;
	char *name;
	int rc = 0;

	spdk_json_write_named_object_begin(w, "lvol");

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		rc = -ENODEV;
		goto end;
	}

	bdev = lvs_bdev->bdev;

	spdk_json_write_named_uuid(w, "lvol_store_uuid", &lvol->lvol_store->uuid);

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(bdev));

	blob = lvol->blob;

	spdk_json_write_named_bool(w, "thin_provision", spdk_blob_is_thin_provisioned(blob));

	spdk_json_write_named_uint64(w, "num_allocated_clusters",
				     spdk_blob_get_num_allocated_clusters(blob));

	spdk_json_write_named_bool(w, "snapshot", spdk_blob_is_snapshot(blob));

	spdk_json_write_named_bool(w, "clone", spdk_blob_is_clone(blob));
	spdk_json_write_named_bool(w, "lvol_leadership", lvol->leader);
	spdk_json_write_named_bool(w, "lvs_leadership", lvol->lvol_store->leader);
	spdk_json_write_named_uint64(w, "blobid", spdk_blob_get_id(blob));
	spdk_json_write_named_uint32(w, "open_ref", spdk_blob_get_open_ref(blob));
	spdk_json_write_named_uint8(w, "lvol_priority_class", lvol->priority_class);

	if (spdk_blob_is_clone(blob)) {
		spdk_blob_id snapshotid = spdk_blob_get_parent_snapshot(lvol->lvol_store->blobstore, lvol->blob_id);
		if (snapshotid != SPDK_BLOBID_INVALID) {
			name = vbdev_lvol_find_name(lvol, snapshotid);
			if (name != NULL) {
				spdk_json_write_named_string(w, "base_snapshot", name);
			} else {
				SPDK_ERRLOG("Cannot obtain snapshots name\n");
			}
		}
	}

	if (spdk_blob_is_snapshot(blob)) {
		/* Take a number of clones */
		rc = spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
		if (rc == -ENOMEM && count > 0) {
			ids = malloc(sizeof(spdk_blob_id) * count);
			if (ids == NULL) {
				SPDK_ERRLOG("Cannot allocate memory\n");
				rc = -ENOMEM;
				goto end;
			}

			rc = spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, ids, &count);
			if (rc == 0) {
				spdk_json_write_named_array_begin(w, "clones");
				for (i = 0; i < count; i++) {
					name = vbdev_lvol_find_name(lvol, ids[i]);
					if (name != NULL) {
						spdk_json_write_string(w, name);
					} else {
						SPDK_ERRLOG("Cannot obtain clone name\n");
					}

				}
				spdk_json_write_array_end(w);
			}
			free(ids);
		}

	}

	spdk_json_write_named_bool(w, "esnap_clone", spdk_blob_is_esnap_clone(blob));

	if (spdk_blob_is_esnap_clone(blob)) {
		const char *name;
		size_t name_len;

		rc = spdk_blob_get_esnap_id(blob, (const void **)&name, &name_len);
		if (rc == 0 && name != NULL && strnlen(name, name_len) + 1 == name_len) {
			spdk_json_write_named_string(w, "external_snapshot_name", name);
		}
	}

end:
	spdk_json_write_object_end(w);

	return rc;
}

static void
vbdev_lvol_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* Nothing to dump as lvol configuration is saved on physical device. */
}

static struct spdk_io_channel *
vbdev_lvol_get_io_channel(void *ctx)
{
	struct spdk_lvol *lvol = ctx;

	return spdk_lvol_get_io_channel(lvol);
}

static bool
vbdev_lvol_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct spdk_lvol *lvol = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return !spdk_blob_is_read_only(lvol->blob);
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_SEEK_DATA:
	case SPDK_BDEV_IO_TYPE_SEEK_HOLE:
		return true;
	default:
		return false;
	}
}

static void
lvol_op_comp(void *cb_arg, int bserrno)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (bserrno != 0) {
		struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
		SPDK_NOTICELOG("FAILED IO blob: %" PRIu64 " LBA: %" PRIu64 " CNT %" PRIu64 " type %d, rc %d \n",
		 	lvol->blob_id, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, bdev_io->type, bserrno);
		if (bserrno == -ENOMEM) {
			status = SPDK_BDEV_IO_STATUS_NOMEM;
		} else {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	spdk_bdev_io_complete(bdev_io, status);
}

static void
lvol_unmap(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	spdk_blob_io_unmap(blob, ch, start_page, num_pages, lvol_op_comp, bdev_io);
}

static void
lvol_seek_data(struct spdk_lvol *lvol, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.seek.offset = spdk_blob_get_next_allocated_io_unit(lvol->blob,
				      bdev_io->u.bdev.offset_blocks);

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
lvol_seek_hole(struct spdk_lvol *lvol, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.seek.offset = spdk_blob_get_next_unallocated_io_unit(lvol->blob,
				      bdev_io->u.bdev.offset_blocks);

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
lvol_write_zeroes(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	spdk_blob_io_write_zeroes(blob, ch, start_page, num_pages, lvol_op_comp, bdev_io);
}

static void
lvol_read(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	struct spdk_blob *blob = lvol->blob;
	struct vbdev_lvol_io *lvol_io = (struct vbdev_lvol_io *)bdev_io->driver_ctx;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	lvol_io->ext_io_opts.size = sizeof(lvol_io->ext_io_opts);
	lvol_io->ext_io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	lvol_io->ext_io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;

	spdk_blob_io_readv_ext(blob, ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_page,
			       num_pages, lvol_op_comp, bdev_io, &lvol_io->ext_io_opts);
}

static void
lvol_write(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;
	struct vbdev_lvol_io *lvol_io = (struct vbdev_lvol_io *)bdev_io->driver_ctx;
	
	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	lvol_io->ext_io_opts.size = sizeof(lvol_io->ext_io_opts);
	lvol_io->ext_io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	lvol_io->ext_io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;

	spdk_blob_io_writev_ext(blob, ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_page,
				num_pages, lvol_op_comp, bdev_io, &lvol_io->ext_io_opts);
}

static int
lvol_reset(struct spdk_bdev_io *bdev_io)
{
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	SPDK_NOTICELOG("FAILED reset IO OP in blob: %" PRIu64 " blocks at LBA: %" PRIu64 " blocks CNT %" PRIu64 " and the type is %d \n",
		 lvol->blob_id, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, bdev_io->type);
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);	
	return 0;
}

static void
lvol_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
		SPDK_NOTICELOG("FAILED getbuf IO OP in blob: %" PRIu64 " blocks at LBA: %" PRIu64 " blocks CNT %" PRIu64 " and the type is %d \n",
		 			lvol->blob_id, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	lvol_read(ch, bdev_io);
}

static void
vbdev_lvol_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	bool allow_active = false;
	if (!lvol->lvol_store->leader && !lvol->lvol_store->update_in_progress) {
		allow_active = spdk_lvs_check_active_process(lvol->lvol_store);
		if (allow_active) {
			spdk_lvs_update_on_failover(lvol->lvol_store);			
		}
	}

	if (!lvol->leader && !lvol->update_in_progress) {
		spdk_lvol_update_on_failover(lvol->lvol_store, lvol, true);
	}

	if (lvol->failed_on_update || lvol->lvol_store->failed_on_update) {
		SPDK_NOTICELOG("FAILED IO - update failed blob: %" PRIu64 "  Lba: %" PRIu64 "  Cnt %" PRIu64 "  t %d \n",
		 				lvol->blob_id, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, lvol_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		lvol_write(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		lvol_reset(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		lvol_unmap(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		lvol_write_zeroes(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_SEEK_DATA:
		lvol_seek_data(lvol, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_SEEK_HOLE:
		lvol_seek_hole(lvol, bdev_io);
		break;
	default:
		SPDK_INFOLOG(vbdev_lvol, "lvol: unsupported I/O type %d\n", bdev_io->type);
		SPDK_NOTICELOG("FAILED IO OP in blob: %" PRIu64 "  LBA: %" PRIu64 "  CNT %" PRIu64 "  type is %d \n", 
				lvol->blob_id, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	return;
}

static int
vbdev_lvol_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct spdk_lvol *lvol = ctx;
	struct spdk_bdev *base_bdev, *esnap_bdev;
	struct spdk_bs_dev *bs_dev;
	struct spdk_lvol_store *lvs;
	int base_cnt, esnap_cnt;

	lvs = lvol->lvol_store;
	base_bdev = lvs->bs_dev->get_base_bdev(lvol->lvol_store->bs_dev);

	base_cnt = spdk_bdev_get_memory_domains(base_bdev, domains, array_size);
	if (base_cnt < 0) {
		return base_cnt;
	}

	if (lvol->blob == NULL) {
		/*
		 * This is probably called due to an open happening during blobstore load. Another
		 * open will follow shortly that has lvol->blob set.
		 */
		return -EAGAIN;
	}

	if (!spdk_blob_is_esnap_clone(lvol->blob)) {
		return base_cnt;
	}

	bs_dev = spdk_blob_get_esnap_bs_dev(lvol->blob);
	if (bs_dev == NULL) {
		assert(false);
		SPDK_ERRLOG("lvol %s is an esnap clone but has no esnap device\n", lvol->unique_id);
		return base_cnt;
	}

	if (bs_dev->get_base_bdev == NULL) {
		/*
		 * If this were a blob_bdev, we wouldn't be here. We are probably here because an
		 * lvol bdev is being registered with spdk_bdev_register() before the external
		 * snapshot bdev is loaded. Ideally, the load of a missing esnap would trigger an
		 * event that causes the lvol bdev's memory domain information to be updated.
		 */
		return base_cnt;
	}

	esnap_bdev = bs_dev->get_base_bdev(bs_dev);
	if (esnap_bdev == NULL) {
		/*
		 * The esnap bdev has not yet been loaded. Anyone that has opened at this point may
		 * miss out on using memory domains if base_cnt is zero.
		 */
		SPDK_NOTICELOG("lvol %s reporting %d memory domains, not including missing esnap\n",
			       lvol->unique_id, base_cnt);
		return base_cnt;
	}

	if (base_cnt < array_size) {
		array_size -= base_cnt;
		domains += base_cnt;
	} else {
		array_size = 0;
		domains = NULL;
	}

	esnap_cnt = spdk_bdev_get_memory_domains(esnap_bdev, domains, array_size);
	if (esnap_cnt <= 0) {
		return base_cnt;
	}

	return base_cnt + esnap_cnt;
}

static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_unregister,
	.io_type_supported	= vbdev_lvol_io_type_supported,
	.submit_request		= vbdev_lvol_submit_request,
	.get_io_channel		= vbdev_lvol_get_io_channel,
	.dump_info_json		= vbdev_lvol_dump_info_json,
	.write_config_json	= vbdev_lvol_write_config_json,
	.get_memory_domains	= vbdev_lvol_get_memory_domains,
};

static void
lvol_destroy_cb(void *cb_arg, int bdeverrno)
{
}

static void
_create_lvol_disk_destroy_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_lvol *lvol = cb_arg;

	if (bdeverrno < 0) {
		SPDK_ERRLOG("Could not unregister bdev for lvol %s\n",
			    lvol->unique_id);
		return;
	}

	spdk_lvol_destroy(lvol, lvol_destroy_cb, NULL);
}

static void
_create_lvol_disk_unload_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_lvol *lvol = cb_arg;

	if (bdeverrno < 0) {
		SPDK_ERRLOG("Could not unregister bdev for lvol %s\n",
			    lvol->unique_id);
		return;
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
	free(lvol);
}

static int
_create_lvol_disk(struct spdk_lvol *lvol, bool destroy)
{
	struct spdk_bdev *bdev;
	struct lvol_bdev *lvol_bdev;
	struct lvol_store_bdev *lvs_bdev;
	uint64_t total_size;
	unsigned char *alias;
	int rc;

	if (spdk_lvol_is_degraded(lvol)) {
		SPDK_NOTICELOG("lvol %s: blob is degraded: deferring bdev creation\n",
			       lvol->unique_id);
		return 0;
	}

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	if (lvs_bdev == NULL) {
		SPDK_ERRLOG("No spdk lvs-bdev pair found for lvol %s\n", lvol->unique_id);
		assert(false);
		return -ENODEV;
	}

	lvol_bdev = calloc(1, sizeof(struct lvol_bdev));
	if (!lvol_bdev) {
		SPDK_ERRLOG("Cannot alloc memory for lvol bdev\n");
		return -ENOMEM;
	}

	lvol_bdev->lvol = lvol;
	lvol_bdev->lvs_bdev = lvs_bdev;

	bdev = &lvol_bdev->bdev;
	bdev->name = lvol->unique_id;
	bdev->product_name = "Logical Volume";
	bdev->blocklen = spdk_bs_get_io_unit_size(lvol->lvol_store->blobstore);
	total_size = spdk_blob_get_num_clusters(lvol->blob) *
		     spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	assert((total_size % bdev->blocklen) == 0);
	bdev->blockcnt = total_size / bdev->blocklen;
	bdev->uuid = lvol->uuid;
	bdev->required_alignment = lvs_bdev->bdev->required_alignment;
	bdev->split_on_optimal_io_boundary = true;
	bdev->optimal_io_boundary = spdk_bs_get_cluster_size(lvol->lvol_store->blobstore) / bdev->blocklen;

	bdev->ctxt = lvol;
	bdev->fn_table = &vbdev_lvol_fn_table;
	bdev->module = &g_lvol_if;

	/* Set default bdev reset waiting time. This value indicates how much
	 * time a reset should wait before forcing a reset down to the underlying
	 * bdev module.
	 * Setting this parameter is mainly to avoid "empty" resets to a shared
	 * bdev that may be used by multiple lvols. */
	bdev->reset_io_drain_timeout = SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE;

	rc = spdk_bdev_register(bdev);
	if (rc) {
		free(lvol_bdev);
		return rc;
	}
	lvol->bdev = bdev;

	alias = spdk_sprintf_alloc("%s/%s", lvs_bdev->lvs->name, lvol->name);
	if (alias == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
		spdk_bdev_unregister(lvol->bdev, (destroy ? _create_lvol_disk_destroy_cb :
						  _create_lvol_disk_unload_cb), lvol);
		return -ENOMEM;
	}

	rc = spdk_bdev_alias_add(bdev, alias);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot add alias to lvol bdev\n");
		spdk_bdev_unregister(lvol->bdev, (destroy ? _create_lvol_disk_destroy_cb :
						  _create_lvol_disk_unload_cb), lvol);
	}
	free(alias);

	return rc;
}

static void
_vbdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;

	if (lvolerrno < 0) {
		goto end;
	}

	lvol->priority_class = req->lvol_priority_class;
	vbdev_lvol_set_io_priority_class(lvol);

	lvolerrno = _create_lvol_disk(lvol, true);

end:
	req->cb_fn(req->cb_arg, lvol, lvolerrno);
	free(req);
}

static void
spdk_bsdump_done(void *arg, int bserrno)
{
	struct spdk_lvol_with_handle_req *req = arg;	
	if (bserrno != 0) {
		SPDK_ERRLOG("lvs dump failed.\n");
	}
	SPDK_INFOLOG(vbdev_lvol, "lvs dumping done successfully.\n");
	fclose(req->fp);
	req->cb_fn(req->cb_arg, NULL, bserrno);
	free(req);
}


int
vbdev_lvs_dump(struct spdk_lvol_store *lvs, const char *file, spdk_lvol_op_with_handle_complete cb_fn,
		  void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	FILE *fp = NULL;

	fp = fopen(file, "w");  // Open the file in write mode

	// Check if the file opened successfully
	if (fp == NULL) {
		SPDK_ERRLOG("Error opening file for writing\n");
		return -1;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}	
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->fp = fp;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}
	
	spdk_bs_dumpv2(lvs->blobstore, fp, spdk_bsdump_done, req);
	return 0;
}

int
vbdev_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		  bool thin_provision, enum lvol_clear_method clear_method, int8_t lvol_priority_class,
		  spdk_lvol_op_with_handle_complete cb_fn,
		  void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}
	req->lvol_priority_class = lvol_priority_class;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	rc = spdk_lvol_create(lvs, name, sz, thin_provision, clear_method,
			      _vbdev_lvol_create_cb, req);
	if (rc != 0) {
		free(req);
	}

	return rc;
}

int
vbdev_lvol_register(struct spdk_lvol_store *lvs, const char *name, const char *registered_uuid, uint64_t blobid,
		  bool thin_provision, enum lvol_clear_method clear_method, int8_t lvol_priority_class,
		  spdk_lvol_op_with_handle_complete cb_fn,
		  void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}
	req->lvol_priority_class = lvol_priority_class;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	rc = spdk_lvol_register_live(lvs, name, registered_uuid, blobid, thin_provision, clear_method,
			      _vbdev_lvol_create_cb, req);
	if (rc != 0) {
		free(req);
	}

	return rc;
}

void
vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
			   spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_create_snapshot(lvol, snapshot_name, _vbdev_lvol_create_cb, req);
}


static void
vbdev_lvol_update_snapshot_clone_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	req->cb_fn(req->cb_arg, lvol, lvolerrno);
	free(req);
	// if (lvolerrno < 0) {
	// 	goto end;
	// }

	// lvol->priority_class = req->lvol_priority_class;
	// vbdev_lvol_set_io_priority_class(lvol);

	// lvolerrno = _create_lvol_disk(lvol, true);

// end:
// 	req->cb_fn(req->cb_arg, lvol, lvolerrno);
// 	free(req);
}

void
vbdev_lvol_update_snapshot_clone(struct spdk_lvol *lvol, struct spdk_lvol *origlvol,
			   bool clone, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	if (clone) {
		spdk_lvol_update_clone(lvol, vbdev_lvol_update_snapshot_clone_cb, req);
		return;
	}
	spdk_lvol_update_snapshot_clone(lvol, origlvol, vbdev_lvol_update_snapshot_clone_cb, req);
}

void
vbdev_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_create_clone(lvol, clone_name, _vbdev_lvol_create_cb, req);
}

static void
ignore_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

void
vbdev_lvol_create_bdev_clone(const char *esnap_name,
			     struct spdk_lvol_store *lvs, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	char bdev_uuid[SPDK_UUID_STRING_LEN];
	uint64_t sz;
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = spdk_bdev_open_ext(esnap_name, false, ignore_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("bdev '%s' could not be opened: error %d\n", esnap_name, rc);
		cb_fn(cb_arg, NULL, rc);
		return;
	}
	bdev = spdk_bdev_desc_get_bdev(desc);

	rc = spdk_uuid_fmt_lower(bdev_uuid, sizeof(bdev_uuid), spdk_bdev_get_uuid(bdev));
	if (rc != 0) {
		spdk_bdev_close(desc);
		SPDK_ERRLOG("bdev %s: unable to parse UUID\n", esnap_name);
		assert(false);
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		spdk_bdev_close(desc);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	sz = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);
	rc = spdk_lvol_create_esnap_clone(bdev_uuid, sizeof(bdev_uuid), sz, lvs, clone_name,
					  _vbdev_lvol_create_cb, req);
	spdk_bdev_close(desc);
	if (rc != 0) {
		cb_fn(cb_arg, NULL, rc);
		free(req);
	}
}

static void
_vbdev_lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Renaming lvol failed\n");
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
vbdev_lvol_rename(struct spdk_lvol *lvol, const char *new_lvol_name,
		  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	int rc;

	rc = _vbdev_lvol_change_bdev_alias(lvol, new_lvol_name);
	if (rc != 0) {
		SPDK_ERRLOG("renaming lvol to '%s' does not succeed\n", new_lvol_name);
		cb_fn(cb_arg, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_rename(lvol, new_lvol_name, _vbdev_lvol_rename_cb, req);
}

static void
_vbdev_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;
	uint64_t total_size;

	/* change bdev size */
	if (lvolerrno != 0) {
		SPDK_ERRLOG("CB function for bdev lvol %s receive error no: %d.\n", lvol->name, lvolerrno);
		goto finish;
	}

	total_size = spdk_blob_get_num_clusters(lvol->blob) *
		     spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	assert((total_size % lvol->bdev->blocklen) == 0);

	lvolerrno = spdk_bdev_notify_blockcnt_change(lvol->bdev, total_size / lvol->bdev->blocklen);
	if (lvolerrno != 0) {
		SPDK_ERRLOG("Could not change num blocks for bdev lvol %s with error no: %d.\n",
			    lvol->name, lvolerrno);
	}

finish:
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
vbdev_lvol_resize(struct spdk_lvol *lvol, uint64_t sz, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	assert(lvol->bdev != NULL);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->sz = sz;
	req->lvol = lvol;

	spdk_lvol_resize(req->lvol, req->sz, _vbdev_lvol_resize_cb, req);
}

static void
_vbdev_lvol_set_read_only_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Could not set bdev lvol %s as read only due to error: %d.\n", lvol->name, lvolerrno);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
vbdev_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	assert(lvol->bdev != NULL);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_lvol_set_read_only(lvol, _vbdev_lvol_set_read_only_cb, req);
}

static int
vbdev_lvs_init(void)
{
	// Initilize the queue for delete requests.
	lvol_delete_requests_init();
	return 0;
}

static void vbdev_lvs_fini_start_iter(struct lvol_store_bdev *lvs_bdev);

static void
vbdev_lvs_fini_start_unload_cb(void *cb_arg, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct lvol_store_bdev *next_lvs_bdev = vbdev_lvol_store_next(lvs_bdev);

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store removed with error: %d.\n", lvserrno);
	}

	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	free(lvs_bdev);

	vbdev_lvs_fini_start_iter(next_lvs_bdev);
}

static void
vbdev_lvs_fini_start_iter(struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_lvol_store *lvs;

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;

		if (_vbdev_lvs_are_lvols_closed(lvs)) {
			spdk_lvs_unload(lvs, vbdev_lvs_fini_start_unload_cb, lvs_bdev);
			return;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	spdk_bdev_module_fini_start_done();
}

static void
vbdev_lvs_fini_start(void)
{
	g_shutdown_started = true;

	// Clear the queue and delete the global object.
	lvol_delete_requests_clear();
	free(g_lvol_delete_requests);
	g_lvol_delete_requests =  NULL;

	vbdev_lvs_fini_start_iter(vbdev_lvol_store_first());
}

static int
vbdev_lvs_get_ctx_size(void)
{
	return sizeof(struct vbdev_lvol_io);
}

static void
_vbdev_lvs_examine_done(struct spdk_lvs_req *req, int lvserrno)
{
	req->cb_fn(req->cb_arg, lvserrno);
}

static void
_vbdev_lvs_examine_failed(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *req = cb_arg;

	_vbdev_lvs_examine_done(req, req->lvserrno);
}

static void
_vbdev_lvs_examine_finish(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno != 0) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		if (lvolerrno == -ENOMEM) {
			TAILQ_INSERT_TAIL(&lvs->retry_open_lvols, lvol, link);
			return;
		}
		SPDK_ERRLOG("Error opening lvol %s\n", lvol->unique_id);
		lvs->lvol_count--;
		free(lvol);
		goto end;
	}

	if (_create_lvol_disk(lvol, false)) {
		SPDK_ERRLOG("Cannot create bdev for lvol %s\n", lvol->unique_id);
		lvs->lvol_count--;
		goto end;
	}

	lvs->lvols_opened++;
	SPDK_INFOLOG(vbdev_lvol, "Opening lvol %s succeeded\n", lvol->unique_id);

end:
	if (!TAILQ_EMPTY(&lvs->retry_open_lvols)) {
		lvol = TAILQ_FIRST(&lvs->retry_open_lvols);
		TAILQ_REMOVE(&lvs->retry_open_lvols, lvol, link);
		TAILQ_INSERT_HEAD(&lvs->lvols, lvol, link);
		spdk_lvol_open(lvol, _vbdev_lvs_examine_finish, req);
		return;
	}
	if (lvs->lvols_opened >= lvs->lvol_count) {
		SPDK_INFOLOG(vbdev_lvol, "Opening lvols finished\n");
		_vbdev_lvs_examine_done(req, 0);
	}
}

/* Walks a tree of clones that are no longer degraded to create bdevs. */
static int
create_esnap_clone_lvol_disks(void *ctx, struct spdk_lvol *lvol)
{
	struct spdk_bdev *bdev = ctx;
	int rc;

	rc = _create_lvol_disk(lvol, false);
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s: failed to create bdev after esnap hotplug of %s: %d\n",
			    lvol->unique_id, spdk_bdev_get_name(bdev), rc);
		/* Do not prevent creation of other clones in case of one failure. */
		return 0;
	}

	return spdk_lvol_iter_immediate_clones(lvol, create_esnap_clone_lvol_disks, ctx);
}

static void
vbdev_lvs_hotplug(void *ctx, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_bdev *esnap_clone_bdev = ctx;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("lvol %s: during examine of bdev %s: not creating clone bdev due to "
			    "error %d\n", lvol->unique_id, spdk_bdev_get_name(esnap_clone_bdev),
			    lvolerrno);
		return;
	}
	create_esnap_clone_lvol_disks(esnap_clone_bdev, lvol);
}

static void
vbdev_lvs_examine_config(struct spdk_bdev *bdev)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);

	if (spdk_lvs_notify_hotplug(uuid_str, sizeof(uuid_str), vbdev_lvs_hotplug, bdev)) {
		SPDK_INFOLOG(vbdev_lvol, "bdev %s: claimed by one or more esnap clones\n",
			     uuid_str);
	}
	spdk_bdev_module_examine_done(&g_lvol_if);
}

static void
_vbdev_lvs_examine_cb(void *arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)arg;
	struct spdk_lvol *lvol, *tmp;
	struct spdk_lvs_req *ori_req = req->cb_arg;

	if (lvserrno == -EEXIST) {
		SPDK_INFOLOG(vbdev_lvol,
			     "Name for lvolstore on device %s conflicts with name for already loaded lvs\n",
			     req->base_bdev->name);
		/* On error blobstore destroys bs_dev itself */
		_vbdev_lvs_examine_done(ori_req, lvserrno);
		goto end;
	} else if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store not found on %s\n", req->base_bdev->name);
		/* On error blobstore destroys bs_dev itself */
		_vbdev_lvs_examine_done(ori_req, lvserrno);
		goto end;
	}

	lvserrno = spdk_bs_bdev_claim(lvol_store->bs_dev, &g_lvol_if);
	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store base bdev already claimed by another bdev\n");
		ori_req->lvserrno = lvserrno;
		spdk_lvs_unload(lvol_store, _vbdev_lvs_examine_failed, ori_req);
		goto end;
	}

	lvs_bdev = calloc(1, sizeof(*lvs_bdev));
	if (!lvs_bdev) {
		SPDK_ERRLOG("Cannot alloc memory for lvs_bdev\n");
		ori_req->lvserrno = lvserrno;
		spdk_lvs_unload(lvol_store, _vbdev_lvs_examine_failed, ori_req);
		goto end;
	}

	lvs_bdev->lvs = lvol_store;
	lvs_bdev->bdev = req->base_bdev;

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);

	SPDK_INFOLOG(vbdev_lvol, "Lvol store found on %s - begin parsing\n",
		     req->base_bdev->name);

	lvol_store->lvols_opened = 0;

	ori_req->lvol_store = lvol_store;

	if (TAILQ_EMPTY(&lvol_store->lvols)) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store examination done\n");
		_vbdev_lvs_examine_done(ori_req, 0);
	} else {
		/* Open all lvols */
		TAILQ_FOREACH_SAFE(lvol, &lvol_store->lvols, link, tmp) {
			spdk_lvol_open(lvol, _vbdev_lvs_examine_finish, ori_req);
		}
	}

end:
	free(req);
}

static void
_vbdev_lvs_examine(struct spdk_bdev *bdev, struct spdk_lvs_req *ori_req,
		   void (*action)(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg))
{
	struct spdk_bs_dev *bs_dev;
	struct spdk_lvs_with_handle_req *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		_vbdev_lvs_examine_done(ori_req, -ENOMEM);
		return;
	}

	rc = spdk_bdev_create_bs_dev_ext(bdev->name, vbdev_lvs_base_bdev_event_cb,
					 NULL, &bs_dev);
	if (rc < 0) {
		SPDK_INFOLOG(vbdev_lvol, "Cannot create bs dev on %s\n", bdev->name);
		_vbdev_lvs_examine_done(ori_req, rc);
		free(req);
		return;
	}

	req->base_bdev = bdev;
	req->cb_arg = ori_req;

	action(bs_dev, _vbdev_lvs_examine_cb, req);
}

static void
vbdev_lvs_examine_done(void *arg, int lvserrno)
{
	struct spdk_lvs_req *req = arg;

	spdk_bdev_module_examine_done(&g_lvol_if);
	free(req);
}

static void
vbdev_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_opts lvs_opts;

	spdk_lvs_opts_init(&lvs_opts);
	lvs_opts.esnap_bs_dev_create = vbdev_lvol_esnap_dev_create;
	spdk_lvs_load_ext(bs_dev, &lvs_opts, cb_fn, cb_arg);
}

static void
vbdev_lvs_examine_disk(struct spdk_bdev *bdev)
{
	struct spdk_lvs_req *req;

	if (spdk_bdev_get_md_size(bdev) != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Cannot create bs dev on %s\n which is formatted with metadata",
			     bdev->name);
		spdk_bdev_module_examine_done(&g_lvol_if);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		spdk_bdev_module_examine_done(&g_lvol_if);
		return;
	}

	req->cb_fn = vbdev_lvs_examine_done;
	req->cb_arg = req;

	_vbdev_lvs_examine(bdev, req, vbdev_lvs_load);
}

struct spdk_lvol *
vbdev_lvol_get_from_bdev(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != &g_lvol_if) {
		return NULL;
	}

	if (bdev->ctxt == NULL) {
		SPDK_ERRLOG("No lvol ctx assigned to bdev %s\n", bdev->name);
		return NULL;
	}

	return (struct spdk_lvol *)bdev->ctxt;
}

/* Begin degraded blobstore device */

/*
 * When an external snapshot is missing, an instance of bs_dev_degraded is used as the blob's
 * back_bs_dev. No bdev is registered, so there should be no IO nor requests for channels. The main
 * purposes of this device are to prevent blobstore from hitting fatal runtime errors and to
 * indicate that the blob is degraded via the is_degraded() callback.
 */

static void
bs_dev_degraded_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	assert(false);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
bs_dev_degraded_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args)
{
	assert(false);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
bs_dev_degraded_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			  struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
			  struct spdk_bs_dev_cb_args *cb_args,
			  struct spdk_blob_ext_io_opts *io_opts)
{
	assert(false);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static bool
bs_dev_degraded_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	assert(false);
	return false;
}

static bool
bs_dev_degraded_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	assert(false);
	return false;
}

static struct spdk_io_channel *
bs_dev_degraded_create_channel(struct spdk_bs_dev *bs_dev)
{
	assert(false);
	return NULL;
}

static void
bs_dev_degraded_destroy_channel(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel)
{
	assert(false);
}

static void
bs_dev_degraded_destroy(struct spdk_bs_dev *bs_dev)
{
}

static bool
bs_dev_degraded_is_degraded(struct spdk_bs_dev *bs_dev)
{
	return true;
}

static struct spdk_bs_dev bs_dev_degraded = {
	.create_channel = bs_dev_degraded_create_channel,
	.destroy_channel = bs_dev_degraded_destroy_channel,
	.destroy = bs_dev_degraded_destroy,
	.read = bs_dev_degraded_read,
	.readv = bs_dev_degraded_readv,
	.readv_ext = bs_dev_degraded_readv_ext,
	.is_zeroes = bs_dev_degraded_is_zeroes,
	.is_range_valid = bs_dev_degraded_is_range_valid,
	.is_degraded = bs_dev_degraded_is_degraded,
	/* Make the device as large as possible without risk of uint64 overflow. */
	.blockcnt = UINT64_MAX / 512,
	/* Prevent divide by zero errors calculating LBAs that will never be read. */
	.blocklen = 512,
};

/* End degraded blobstore device */

/* Begin external snapshot support */

static void
vbdev_lvol_esnap_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			       void *event_ctx)
{
	SPDK_NOTICELOG("bdev name (%s) received unsupported event type %d\n",
		       spdk_bdev_get_name(bdev), type);
}

int
vbdev_lvol_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			    const void *esnap_id, uint32_t id_len,
			    struct spdk_bs_dev **_bs_dev)
{
	struct spdk_lvol_store	*lvs = bs_ctx;
	struct spdk_lvol	*lvol = blob_ctx;
	struct spdk_bs_dev	*bs_dev = NULL;
	struct spdk_uuid	uuid;
	int			rc;
	char			uuid_str[SPDK_UUID_STRING_LEN] = { 0 };

	if (esnap_id == NULL) {
		SPDK_ERRLOG("lvol %s: NULL esnap ID\n", lvol->unique_id);
		return -EINVAL;
	}

	/* Guard against arbitrary names and unterminated UUID strings */
	if (id_len != SPDK_UUID_STRING_LEN) {
		SPDK_ERRLOG("lvol %s: Invalid esnap ID length (%u)\n", lvol->unique_id, id_len);
		return -EINVAL;
	}

	if (spdk_uuid_parse(&uuid, esnap_id)) {
		SPDK_ERRLOG("lvol %s: Invalid esnap ID: not a UUID\n", lvol->unique_id);
		return -EINVAL;
	}

	/* Format the UUID the same as it is in the bdev names tree. */
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &uuid);
	if (strcmp(uuid_str, esnap_id) != 0) {
		SPDK_WARNLOG("lvol %s: esnap_id '%*s' does not match parsed uuid '%s'\n",
			     lvol->unique_id, SPDK_UUID_STRING_LEN, (const char *)esnap_id,
			     uuid_str);
		assert(false);
	}

	rc = spdk_bdev_create_bs_dev(uuid_str, false, NULL, 0,
				     vbdev_lvol_esnap_bdev_event_cb, NULL, &bs_dev);
	if (rc != 0) {
		goto fail;
	}

	rc = spdk_bs_bdev_claim(bs_dev, &g_lvol_if);
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s: unable to claim esnap bdev '%s': %d\n", lvol->unique_id,
			    uuid_str, rc);
		bs_dev->destroy(bs_dev);
		goto fail;
	}

	*_bs_dev = bs_dev;
	return 0;

fail:
	/* Unable to open or claim the bdev. This lvol is degraded. */
	bs_dev = &bs_dev_degraded;
	SPDK_NOTICELOG("lvol %s: bdev %s not available: lvol is degraded\n", lvol->unique_id,
		       uuid_str);

	/*
	 * Be sure not to call spdk_lvs_missing_add() on an lvol that is already degraded. This can
	 * lead to a cycle in the degraded_lvols tailq.
	 */
	if (lvol->degraded_set == NULL) {
		rc = spdk_lvs_esnap_missing_add(lvs, lvol, uuid_str, sizeof(uuid_str));
		if (rc != 0) {
			SPDK_NOTICELOG("lvol %s: unable to register missing esnap device %s: "
				       "it will not be hotplugged if added later\n",
				       lvol->unique_id, uuid_str);
		}
	}

	*_bs_dev = bs_dev;
	return 0;
}

/* End external snapshot support */

static void
_vbdev_lvol_shallow_copy_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		void *event_ctx)
{
}

static void
_vbdev_lvol_shallow_copy_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_copy_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Could not make a shallow copy of lvol %s due to error: %d\n",
			    lvol->name, lvolerrno);
	}

	req->ext_dev->destroy(req->ext_dev);
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

int
vbdev_lvol_shallow_copy(struct spdk_lvol *lvol, const char *bdev_name,
			spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_dev *ext_dev;
	struct spdk_lvol_copy_req *req;
	int rc;

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol must not be NULL\n");
		return -EINVAL;
	}

	if (bdev_name == NULL) {
		SPDK_ERRLOG("lvol %s, bdev name must not be NULL\n", lvol->name);
		return -EINVAL;
	}

	assert(lvol->bdev != NULL);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("lvol %s, cannot alloc memory for lvol copy request\n", lvol->name);
		return -ENOMEM;
	}

	rc = spdk_bdev_create_bs_dev_ext(bdev_name, _vbdev_lvol_shallow_copy_base_bdev_event_cb,
					 NULL, &ext_dev);
	if (rc < 0) {
		SPDK_ERRLOG("lvol %s, cannot create blobstore block device from bdev %s\n", lvol->name, bdev_name);
		free(req);
		return rc;
	}

	rc = spdk_bs_bdev_claim(ext_dev, &g_lvol_if);
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s, unable to claim bdev %s, error %d\n", lvol->name, bdev_name, rc);
		ext_dev->destroy(ext_dev);
		free(req);
		return rc;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	req->ext_dev = ext_dev;

	rc = spdk_lvol_shallow_copy(lvol, ext_dev, status_cb_fn, status_cb_arg, _vbdev_lvol_shallow_copy_cb,
				    req);

	if (rc < 0) {
		ext_dev->destroy(ext_dev);
		free(req);
	}

	return rc;
}

void
vbdev_lvol_set_external_parent(struct spdk_lvol *lvol, const char *esnap_name,
			       spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	char bdev_uuid[SPDK_UUID_STRING_LEN];
	int rc;

	rc = spdk_bdev_open_ext(esnap_name, false, ignore_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("bdev '%s' could not be opened: error %d\n", esnap_name, rc);
		cb_fn(cb_arg, -ENODEV);
		return;
	}
	bdev = spdk_bdev_desc_get_bdev(desc);

	rc = spdk_uuid_fmt_lower(bdev_uuid, sizeof(bdev_uuid), spdk_bdev_get_uuid(bdev));
	if (rc != 0) {
		spdk_bdev_close(desc);
		SPDK_ERRLOG("bdev %s: unable to parse UUID\n", esnap_name);
		assert(false);
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	/*
	 * If lvol store is not loaded from disk, and so vbdev_lvs_load is not called, these
	 * assignments are necessary to let vbdev_lvol_esnap_dev_create be called.
	 */
	lvol->lvol_store->load_esnaps = true;
	lvol->lvol_store->esnap_bs_dev_create = vbdev_lvol_esnap_dev_create;

	spdk_lvol_set_external_parent(lvol, bdev_uuid, sizeof(bdev_uuid), cb_fn, cb_arg);

	spdk_bdev_close(desc);
}

void vbdev_lvol_set_io_priority_class(struct spdk_lvol *lvol) {
	spdk_blob_set_io_priority_class(lvol->blob, lvol->priority_class);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_lvol)
