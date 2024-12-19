/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk/string.h"

#include "common/lib/ut_multithread.c"
#include "bdev/lvol/vbdev_lvol.c"

#include "unit/lib/json_mock.c"

#define SPDK_BS_PAGE_SIZE 0x1000

int g_lvolerrno;
int g_lvserrno;
int g_cluster_size;
int g_num_clusters = 0;
int g_registered_bdevs;
int g_num_lvols = 0;
int g_lvol_open_enomem = -1;
struct spdk_lvol_store *g_lvs = NULL;
struct spdk_lvol *g_lvol = NULL;
struct lvol_store_bdev *g_lvs_bdev = NULL;
struct spdk_bdev *g_base_bdev = NULL;
struct spdk_bdev_io *g_io = NULL;
struct spdk_io_channel *g_ch = NULL;

static struct spdk_bdev g_bdev = {};
static struct spdk_lvol_store *g_lvol_store = NULL;
bool lvol_store_initialize_fail = false;
bool lvol_store_initialize_cb_fail = false;
bool lvol_already_opened = false;
bool g_examine_done = false;
bool g_bdev_alias_already_exists = false;
bool g_lvs_with_name_already_exists = false;
bool g_ext_api_called;
bool g_bdev_is_missing = false;

DEFINE_STUB_V(spdk_bdev_module_fini_start_done, (void));
DEFINE_STUB_V(spdk_bdev_update_bs_blockcnt, (struct spdk_bs_dev *bs_dev));
DEFINE_STUB_V(spdk_blob_set_io_priority_class, (struct spdk_blob *blob, int priority_class));
DEFINE_STUB_V(spdk_lvs_set_leader_by_uuid, (struct spdk_lvol_store *lvs, bool leader));
DEFINE_STUB_V(spdk_lvol_set_leader_by_uuid, (const struct spdk_uuid *uuid, bool leader));
DEFINE_STUB_V(spdk_lvs_update_on_failover, (struct spdk_lvol_store *lvs));
DEFINE_STUB_V(spdk_lvol_update_on_failover, (struct spdk_lvol_store *lvs, struct spdk_lvol *lvol, bool send_md_thread));
DEFINE_STUB_V(spdk_lvs_grow_live, (struct spdk_lvol_store *lvs,
				   spdk_lvs_op_complete cb_fn, void *cb_arg));
DEFINE_STUB(spdk_bdev_get_memory_domains, int, (struct spdk_bdev *bdev,
		struct spdk_memory_domain **domains, int array_size), 0);
DEFINE_STUB(spdk_blob_get_esnap_id, int,
	    (struct spdk_blob *blob, const void **id, size_t *len), -ENOTSUP);
DEFINE_STUB(spdk_blob_is_esnap_clone, bool, (const struct spdk_blob *blob), false);
DEFINE_STUB(spdk_lvol_iter_immediate_clones, int,
	    (struct spdk_lvol *lvol, spdk_lvol_iter_cb cb_fn, void *cb_arg), -ENOTSUP);
DEFINE_STUB(spdk_lvs_esnap_missing_add, int,
	    (struct spdk_lvol_store *lvs, struct spdk_lvol *lvol, const void *esnap_id,
	     uint32_t id_len), -ENOTSUP);
DEFINE_STUB(spdk_blob_get_esnap_bs_dev, struct spdk_bs_dev *, (const struct spdk_blob *blob), NULL);
DEFINE_STUB(spdk_lvol_is_degraded, bool, (const struct spdk_lvol *lvol), false);
DEFINE_STUB(spdk_blob_get_num_allocated_clusters, uint64_t, (struct spdk_blob *blob), 0);
DEFINE_STUB(spdk_blob_get_id, uint64_t, (struct spdk_blob *blob), 0);
DEFINE_STUB(spdk_lvol_copy_blob, int, (struct spdk_lvol *lvol), 0);

struct spdk_blob {
	uint64_t	id;
	char		name[32];
};

struct spdk_blob_store {
	spdk_bs_esnap_dev_create esnap_bs_dev_create;
};

const struct spdk_bdev_aliases_list *
spdk_bdev_get_aliases(const struct spdk_bdev *bdev)
{
	return &bdev->aliases;
}

bool
spdk_lvs_check_active_process(struct spdk_lvol_store *lvs)
{
	return true;
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

int
spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;

	CU_ASSERT(alias != NULL);
	CU_ASSERT(bdev != NULL);
	if (g_bdev_alias_already_exists) {
		return -EEXIST;
	}

	tmp = calloc(1, sizeof(*tmp));
	SPDK_CU_ASSERT_FATAL(tmp != NULL);

	tmp->alias.name = strdup(alias);
	SPDK_CU_ASSERT_FATAL(tmp->alias.name != NULL);

	TAILQ_INSERT_TAIL(&bdev->aliases, tmp, tailq);

	return 0;
}

int
spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;

	CU_ASSERT(bdev != NULL);

	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		SPDK_CU_ASSERT_FATAL(alias != NULL);
		if (strncmp(alias, tmp->alias.name, SPDK_LVOL_NAME_MAX) == 0) {
			TAILQ_REMOVE(&bdev->aliases, tmp, tailq);
			free(tmp->alias.name);
			free(tmp);
			return 0;
		}
	}

	return -ENOENT;
}

void
spdk_bdev_alias_del_all(struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *p, *tmp;

	TAILQ_FOREACH_SAFE(p, &bdev->aliases, tailq, tmp) {
		TAILQ_REMOVE(&bdev->aliases, p, tailq);
		free(p->alias.name);
		free(p);
	}
}

void
spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno)
{
	CU_ASSERT(bdeverrno == 0);
	SPDK_CU_ASSERT_FATAL(bdev->internal.unregister_cb != NULL);
	bdev->internal.unregister_cb(bdev->internal.unregister_ctx, bdeverrno);
}

struct ut_bs_dev {
	struct spdk_bs_dev bs_dev;
	struct spdk_bdev *bdev;
};

static void
ut_bs_dev_destroy(struct spdk_bs_dev *bs_dev)
{
	struct ut_bs_dev *ut_bs_dev = SPDK_CONTAINEROF(bs_dev, struct ut_bs_dev, bs_dev);

	free(ut_bs_dev);
}

int
spdk_bdev_create_bs_dev(const char *bdev_name, bool write,
			struct spdk_bdev_bs_dev_opts *opts, size_t opts_size,
			spdk_bdev_event_cb_t event_cb, void *event_ctx,
			struct spdk_bs_dev **bs_dev)
{
	struct spdk_bdev *bdev;
	struct ut_bs_dev *ut_bs_dev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		return -ENODEV;
	}

	ut_bs_dev = calloc(1, sizeof(*ut_bs_dev));
	SPDK_CU_ASSERT_FATAL(ut_bs_dev != NULL);
	ut_bs_dev->bs_dev.destroy = ut_bs_dev_destroy;
	ut_bs_dev->bdev = bdev;
	*bs_dev = &ut_bs_dev->bs_dev;

	return 0;
}

void
spdk_lvs_grow(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, NULL, -EINVAL);
}

void
spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	if (g_lvs_with_name_already_exists) {
		g_lvolerrno = -EEXIST;
	} else {
		snprintf(lvs->name, sizeof(lvs->name), "%s", new_name);
		g_lvolerrno = 0;
	}

	cb_fn(cb_arg, g_lvolerrno);
}

void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *tmp;

	if (strncmp(lvol->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	TAILQ_FOREACH(tmp, &lvol->lvol_store->lvols, link) {
		if (strncmp(tmp->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
			SPDK_ERRLOG("Lvol %s already exists in lvol store %s\n", new_name, lvol->lvol_store->name);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}

	snprintf(lvol->name, sizeof(lvol->name), "%s", new_name);

	cb_fn(cb_arg, g_lvolerrno);
}

void
spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	int lvolerrno;

	if (g_lvol_open_enomem == lvol->lvol_store->lvols_opened) {
		lvolerrno = -ENOMEM;
		g_lvol_open_enomem = -1;
	} else {
		lvolerrno = g_lvolerrno;
	}

	cb_fn(cb_arg, lvol, lvolerrno);
}

uint64_t
spdk_blob_get_num_clusters(struct spdk_blob *b)
{
	return g_num_clusters;
}

/* Simulation of a blob with:
 * - 1 io_unit per cluster
 * - 20 data cluster
 * - only last cluster allocated
 */
uint64_t g_blob_allocated_io_unit_offset = 20;

uint64_t
spdk_blob_get_next_allocated_io_unit(struct spdk_blob *blob, uint64_t offset)
{
	if (offset <= g_blob_allocated_io_unit_offset) {
		return g_blob_allocated_io_unit_offset;
	} else {
		return UINT64_MAX;
	}
}

uint64_t
spdk_blob_get_next_unallocated_io_unit(struct spdk_blob *blob, uint64_t offset)
{
	if (offset < g_blob_allocated_io_unit_offset) {
		return offset;
	} else {
		return UINT64_MAX;
	}
}

int
spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blobid, spdk_blob_id *ids,
		     size_t *count)
{
	*count = 0;
	return 0;
}

spdk_blob_id
spdk_blob_get_parent_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	return 0;
}

bool g_blob_is_read_only = false;

bool
spdk_blob_is_read_only(struct spdk_blob *blob)
{
	return g_blob_is_read_only;
}

bool
spdk_blob_is_snapshot(struct spdk_blob *blob)
{
	return false;
}

bool
spdk_blob_is_clone(struct spdk_blob *blob)
{
	return false;
}

bool
spdk_blob_is_thin_provisioned(struct spdk_blob *blob)
{
	return false;
}

static struct spdk_lvol *_lvol_create(struct spdk_lvol_store *lvs);

int
spdk_lvol_create_esnap_clone(const void *esnap_id, uint32_t id_len, uint64_t size_bytes,
			     struct spdk_lvol_store *lvs, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *lvol;

	lvol = _lvol_create(lvs);
	snprintf(lvol->name, sizeof(lvol->name), "%s", clone_name);

	cb_fn(cb_arg, lvol, 0);
	return 0;
}

static void
lvs_load(struct spdk_bs_dev *dev, const struct spdk_lvs_opts *lvs_opts,
	 spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs = NULL;
	int i;
	int lvserrno = g_lvserrno;

	if (lvserrno != 0) {
		/* On error blobstore destroys bs_dev itself,
		 * by puttin back io channels.
		 * This operation is asynchronous, and completed
		 * after calling the callback for lvol. */
		cb_fn(cb_arg, g_lvol_store, lvserrno);
		dev->destroy(dev);
		return;
	}

	lvs = calloc(1, sizeof(*lvs));
	SPDK_CU_ASSERT_FATAL(lvs != NULL);
	lvs->blobstore = calloc(1, sizeof(*lvs->blobstore));
	lvs->blobstore->esnap_bs_dev_create = lvs_opts->esnap_bs_dev_create;
	SPDK_CU_ASSERT_FATAL(lvs->blobstore != NULL);
	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);
	TAILQ_INIT(&lvs->retry_open_lvols);
	spdk_uuid_generate(&lvs->uuid);
	lvs->bs_dev = dev;
	for (i = 0; i < g_num_lvols; i++) {
		_lvol_create(lvs);
		lvs->lvol_count++;
	}

	cb_fn(cb_arg, lvs, lvserrno);
}

void
spdk_lvs_load(struct spdk_bs_dev *dev,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	lvs_load(dev, NULL, cb_fn, cb_arg);
}

void
spdk_lvs_load_ext(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *lvs_opts,
		  spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	lvs_load(bs_dev, lvs_opts, cb_fn, cb_arg);
}

int
spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module)
{
	if (lvol_already_opened == true) {
		return -EPERM;
	}

	lvol_already_opened = true;

	return 0;
}

static void
_spdk_bdev_unregister_cb(void *cb_arg, int rc)
{
	CU_ASSERT(rc == 0);
}

void
spdk_bdev_unregister(struct spdk_bdev *vbdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	int rc;

	SPDK_CU_ASSERT_FATAL(vbdev != NULL);
	vbdev->internal.unregister_cb = cb_fn;
	vbdev->internal.unregister_ctx = cb_arg;

	rc = vbdev->fn_table->destruct(vbdev->ctxt);
	CU_ASSERT(rc == 1);
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return SPDK_BS_PAGE_SIZE;
}

uint64_t
spdk_bs_get_io_unit_size(struct spdk_blob_store *bs)
{
	return SPDK_BS_PAGE_SIZE;
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	CU_ASSERT(bs_dev != NULL);
	free(bs_dev);
	lvol_already_opened = false;
}

static struct spdk_bdev *
bdev_blob_get_base_bdev(struct spdk_bs_dev *bs_dev)
{
	CU_ASSERT(bs_dev != NULL);
	return &g_bdev;
}

int
spdk_bdev_create_bs_dev_ext(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
			    void *event_ctx, struct spdk_bs_dev **_bs_dev)
{
	struct spdk_bs_dev *bs_dev;

	if (lvol_already_opened == true) {
		return -EINVAL;
	}

	bs_dev = calloc(1, sizeof(*bs_dev));
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	bs_dev->blocklen = 4096;
	SPDK_CU_ASSERT_FATAL(SPDK_BS_PAGE_SIZE % bs_dev->blocklen == 0);

	g_cluster_size = SPDK_LVS_OPTS_CLUSTER_SZ;
	SPDK_CU_ASSERT_FATAL(g_cluster_size % SPDK_BS_PAGE_SIZE == 0);
	bs_dev->blockcnt = 128;

	g_num_clusters = spdk_divide_round_up(bs_dev->blockcnt, g_cluster_size);

	bs_dev->destroy = bdev_blob_destroy;
	bs_dev->get_base_bdev = bdev_blob_get_base_bdev;

	*_bs_dev = bs_dev;

	return 0;
}

void
spdk_lvs_opts_init(struct spdk_lvs_opts *opts)
{
	opts->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;
	opts->clear_method = LVS_CLEAR_WITH_UNMAP;
	opts->num_md_pages_per_cluster_ratio = 100;
	memset(opts->name, 0, sizeof(opts->name));
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	int error = 0;

	if (lvol_store_initialize_fail) {
		return -1;
	}

	if (lvol_store_initialize_cb_fail) {
		bs_dev->destroy(bs_dev);
		lvs = NULL;
		error = -1;
	} else {
		lvs = calloc(1, sizeof(*lvs));
		SPDK_CU_ASSERT_FATAL(lvs != NULL);
		TAILQ_INIT(&lvs->lvols);
		TAILQ_INIT(&lvs->pending_lvols);
		spdk_uuid_generate(&lvs->uuid);
		snprintf(lvs->name, sizeof(lvs->name), "%s", o->name);
		lvs->bs_dev = bs_dev;
		error = 0;
	}
	cb_fn(cb_arg, lvs, error);

	return 0;
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *lvol, *tmp;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		free(lvol);
	}
	g_lvol_store = NULL;

	lvs->bs_dev->destroy(lvs->bs_dev);
	free(lvs->blobstore);
	free(lvs);

	if (cb_fn != NULL) {
		cb_fn(cb_arg, 0);
	}

	return 0;
}

int
spdk_lvs_destroy(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol *lvol, *tmp;
	char *alias;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);

		alias = spdk_sprintf_alloc("%s/%s", lvs->name, lvol->name);
		if (alias == NULL) {
			SPDK_ERRLOG("Cannot alloc memory for alias\n");
			return -1;
		}
		spdk_bdev_alias_del(lvol->bdev, alias);

		free(alias);
		free(lvol);
	}
	g_lvol_store = NULL;

	lvs->bs_dev->destroy(lvs->bs_dev);
	free(lvs->blobstore);
	free(lvs);

	if (cb_fn != NULL) {
		cb_fn(cb_arg, 0);
	}

	return 0;
}

void
spdk_lvol_resize(struct spdk_lvol *lvol, size_t sz,  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	g_num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(lvol->lvol_store->blobstore));
	cb_fn(cb_arg, 0);
}

void
spdk_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;
	return 0;
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return g_cluster_size;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_uuid uuid;
	int rc;

	if (g_base_bdev == NULL) {
		return NULL;
	}

	if (!strcmp(g_base_bdev->name, bdev_name)) {
		return g_base_bdev;
	}

	rc = spdk_uuid_parse(&uuid, bdev_name);
	if (rc == 0 && spdk_uuid_compare(&uuid, &g_base_bdev->uuid) == 0) {
		return g_base_bdev;
	}

	return NULL;
}

struct spdk_bdev_desc {
	struct spdk_bdev *bdev;
};

int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		   void *event_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		return -ENODEV;
	}

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		return -ENOMEM;
	}

	desc->bdev = bdev;
	*_desc = desc;
	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	free(desc);
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
}

void
spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	lvol->ref_count--;

	SPDK_CU_ASSERT_FATAL(cb_fn != NULL);
	cb_fn(cb_arg, 0);
}

bool
spdk_lvol_deletable(struct spdk_lvol *lvol)
{
	return true;
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	if (lvol->ref_count != 0) {
		cb_fn(cb_arg, -ENODEV);
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);

	SPDK_CU_ASSERT_FATAL(cb_fn != NULL);
	cb_fn(cb_arg, 0);

	g_lvol = NULL;
	free(lvol);
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
}

struct spdk_io_channel *spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	CU_ASSERT(lvol == g_lvol);
	return g_ch;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	CU_ASSERT(cb == lvol_get_buf_cb);
}

void
spdk_blob_io_read(struct spdk_blob *blob, struct spdk_io_channel *channel,
		  void *payload, uint64_t offset, uint64_t length,
		  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_write(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   void *payload, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_unmap(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_write_zeroes(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
		    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_writev_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
			struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg,
			struct spdk_blob_ext_io_opts *io_opts)
{
	struct vbdev_lvol_io *lvol_io = (struct vbdev_lvol_io *)g_io->driver_ctx;

	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	CU_ASSERT(io_opts == &lvol_io->ext_io_opts);
	g_ext_api_called = true;
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_readv_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg,
		       struct spdk_blob_ext_io_opts *io_opts)
{
	struct vbdev_lvol_io *lvol_io = (struct vbdev_lvol_io *)g_io->driver_ctx;

	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	CU_ASSERT(io_opts == &lvol_io->ext_io_opts);
	g_ext_api_called = true;
	cb_fn(cb_arg, 0);
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

int
spdk_bdev_register(struct spdk_bdev *vbdev)
{
	TAILQ_INIT(&vbdev->aliases);

	g_registered_bdevs++;
	return 0;
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module *module)
{
	SPDK_CU_ASSERT_FATAL(g_examine_done != true);
	g_examine_done = true;
}

static struct spdk_lvol *
_lvol_create(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol *lvol = calloc(1, sizeof(*lvol));

	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	lvol->lvol_store = lvs;
	lvol->ref_count++;
	snprintf(lvol->unique_id, sizeof(lvol->unique_id), "%s", "UNIT_TEST_UUID");

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	return lvol;
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, size_t sz,
		 bool thin_provision, enum lvol_clear_method clear_method, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol *lvol;

	lvol = _lvol_create(lvs);
	snprintf(lvol->name, sizeof(lvol->name), "%s", name);
	g_num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(lvol->lvol_store->blobstore));
	cb_fn(cb_arg, lvol, 0);

	return 0;
}

void
spdk_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
			  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *snap;

	snap = _lvol_create(lvol->lvol_store);
	snprintf(snap->name, sizeof(snap->name), "%s", snapshot_name);
	cb_fn(cb_arg, snap, 0);
}

void
spdk_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
		       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *clone;

	clone = _lvol_create(lvol->lvol_store);
	snprintf(clone->name, sizeof(clone->name), "%s", clone_name);
	cb_fn(cb_arg, clone, 0);
}

bool
spdk_lvs_notify_hotplug(const void *esnap_id, uint32_t id_len,
			spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_uuid uuid = { 0 };
	char uuid_str[SPDK_UUID_STRING_LEN] = "bad";

	CU_ASSERT(id_len == SPDK_UUID_STRING_LEN);
	CU_ASSERT(spdk_uuid_parse(&uuid, esnap_id) == 0);
	CU_ASSERT(spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &uuid) == 0);
	CU_ASSERT(strcmp(esnap_id, uuid_str) == 0);

	return g_bdev_is_missing;
}

int
spdk_lvol_shallow_copy(struct spdk_lvol *lvol, struct spdk_bs_dev *ext_dev,
		       spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
		       spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	if (lvol == NULL) {
		return -ENODEV;
	}

	if (ext_dev == NULL) {
		return -ENODEV;
	}

	cb_fn(cb_arg, 0);
	return 0;
}

void
spdk_lvol_set_external_parent(struct spdk_lvol *lvol, const void *esnap_id, uint32_t id_len,
			      spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

static void
lvol_store_op_complete(void *cb_arg, int lvserrno)
{
	g_lvserrno = lvserrno;
	return;
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	g_lvserrno = lvserrno;
	g_lvol_store = lvs;
	return;
}

static void
vbdev_lvol_create_complete(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
	g_lvol = lvol;
}

static void
vbdev_lvol_resize_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
vbdev_lvol_set_read_only_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
vbdev_lvol_rename_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
vbdev_lvol_shallow_copy_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
vbdev_lvol_op_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
ut_lvs_destroy(void)
{
	int rc = 0;
	int sz = 10;
	struct spdk_lvol_store *lvs;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	spdk_uuid_generate(&lvs->uuid);

	/* Successfully create lvol, which should be unloaded with lvs later */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Unload lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
assert_blockcnt(struct spdk_lvol *lvol, int sz)
{
	CU_ASSERT(lvol->bdev->blockcnt == spdk_divide_round_up(sz, g_cluster_size) *
		  (g_cluster_size / lvol->bdev->blocklen));
}

static void
ut_lvol_init(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvol->bdev != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	assert_blockcnt(g_lvol, sz);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_snapshot(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;
	struct spdk_lvol *lvol = NULL;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	lvol = g_lvol;

	/* Successful snap create */
	vbdev_lvol_create_snapshot(lvol, "snap", vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Successful snap destroy */
	g_lvol = lvol;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_clone(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;
	struct spdk_lvol *lvol = NULL;
	struct spdk_lvol *snap = NULL;
	struct spdk_lvol *clone = NULL;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	lvol = g_lvol;

	/* Successful snap create */
	vbdev_lvol_create_snapshot(lvol, "snap", vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	snap = g_lvol;

	/* Successful clone create */
	vbdev_lvol_create_clone(snap, "clone", vbdev_lvol_create_complete, NULL);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	clone = g_lvol;

	/* Successful lvol destroy */
	g_lvol = lvol;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Successful clone destroy */
	g_lvol = clone;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Successful lvol destroy */
	g_lvol = snap;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_hotremove(void)
{
	int rc = 0;

	lvol_store_initialize_fail = false;
	lvol_store_initialize_cb_fail = false;
	lvol_already_opened = false;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	/* Hot remove callback with NULL - stability check */
	vbdev_lvs_hotremove_cb(NULL);

	/* Hot remove lvs on bdev removal */
	vbdev_lvs_hotremove_cb(&g_bdev);

	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_lvol_pairs));

}

static void
ut_lvol_examine_config(void)
{
	/* No esnap clone needs the bdev. */
	g_bdev_is_missing = false;
	g_examine_done = false;
	vbdev_lvs_examine_config(&g_bdev);
	CU_ASSERT(g_examine_done);

	g_bdev_is_missing = true;
	g_examine_done = false;
	vbdev_lvs_examine_config(&g_bdev);
	CU_ASSERT(g_examine_done);

	g_examine_done = false;
}

static void
ut_lvs_examine_check(bool success)
{
	struct lvol_store_bdev *lvs_bdev;

	/* Examine was finished regardless of result */
	CU_ASSERT(g_examine_done == true);
	g_examine_done = false;

	if (success) {
		SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_spdk_lvol_pairs));
		lvs_bdev = TAILQ_FIRST(&g_spdk_lvol_pairs);
		SPDK_CU_ASSERT_FATAL(lvs_bdev != NULL);
		g_lvol_store = lvs_bdev->lvs;
		SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
		SPDK_CU_ASSERT_FATAL(g_lvol_store->blobstore != NULL);
		CU_ASSERT(g_lvol_store->blobstore->esnap_bs_dev_create != NULL);
		CU_ASSERT(g_lvol_store->bs_dev != NULL);
		CU_ASSERT(g_lvol_store->lvols_opened == spdk_min(g_num_lvols, g_registered_bdevs));
	} else {
		SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_spdk_lvol_pairs));
		g_lvol_store = NULL;
	}
}

static void
ut_lvol_examine_disk(void)
{
	/* Examine unsuccessfully - bdev already opened */
	g_lvserrno = -1;
	lvol_already_opened = true;
	vbdev_lvs_examine_disk(&g_bdev);
	ut_lvs_examine_check(false);

	/* Examine unsuccessfully - fail on lvol store */
	g_lvserrno = -1;
	lvol_already_opened = false;
	vbdev_lvs_examine_disk(&g_bdev);
	ut_lvs_examine_check(false);

	/* Examine successfully
	 * - one lvol fails to load
	 * - lvs is loaded with no lvols present */
	g_lvserrno = 0;
	g_lvolerrno = -1;
	g_num_lvols = 1;
	lvol_already_opened = false;
	g_registered_bdevs = 0;
	vbdev_lvs_examine_disk(&g_bdev);
	ut_lvs_examine_check(true);
	CU_ASSERT(g_registered_bdevs == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->lvols));
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Examine successfully */
	g_lvserrno = 0;
	g_lvolerrno = 0;
	g_registered_bdevs = 0;
	lvol_already_opened = false;
	vbdev_lvs_examine_disk(&g_bdev);
	ut_lvs_examine_check(true);
	CU_ASSERT(g_registered_bdevs != 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_lvol_store->lvols));
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Examine multiple lvols successfully */
	g_num_lvols = 4;
	g_registered_bdevs = 0;
	lvol_already_opened = false;
	vbdev_lvs_examine_disk(&g_bdev);
	ut_lvs_examine_check(true);
	CU_ASSERT(g_registered_bdevs == g_num_lvols);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_lvol_store->lvols));
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Examine multiple lvols successfully - fail one with -ENOMEM on lvol open */
	g_num_lvols = 4;
	g_lvol_open_enomem = 2;
	g_registered_bdevs = 0;
	lvol_already_opened = false;
	vbdev_lvs_examine_disk(&g_bdev);
	ut_lvs_examine_check(true);
	CU_ASSERT(g_registered_bdevs == g_num_lvols);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_lvol_store->lvols));
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
}

static void
ut_lvol_rename(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	struct spdk_lvol *lvol2;
	int sz = 10;
	int rc;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvols create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol = g_lvol;

	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol2", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol2 = g_lvol;

	/* Successful rename lvol */
	vbdev_lvol_rename(lvol, "new_lvol_name", vbdev_lvol_rename_complete, NULL);
	SPDK_CU_ASSERT_FATAL(g_lvolerrno == 0);
	CU_ASSERT_STRING_EQUAL(lvol->name, "new_lvol_name");

	/* Renaming lvol with name already existing */
	g_bdev_alias_already_exists = true;
	vbdev_lvol_rename(lvol2, "new_lvol_name", vbdev_lvol_rename_complete, NULL);
	g_bdev_alias_already_exists = false;
	SPDK_CU_ASSERT_FATAL(g_lvolerrno != 0);
	CU_ASSERT_STRING_NOT_EQUAL(lvol2->name, "new_lvol_name");

	/* Renaming lvol with it's own name */
	vbdev_lvol_rename(lvol, "new_lvol_name", vbdev_lvol_rename_complete, NULL);
	SPDK_CU_ASSERT_FATAL(g_lvolerrno == 0);
	CU_ASSERT_STRING_EQUAL(lvol->name, "new_lvol_name");

	/* Successful lvols destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	vbdev_lvol_destroy(lvol2, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_bdev_finish(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	struct spdk_lvol *lvol2;
	int sz = 10;
	int rc;

	/* Scenario 1
	 * Test unload of lvs with no lvols during bdev finish. */

	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	/* Start bdev finish */
	vbdev_lvs_fini_start();
	CU_ASSERT(g_shutdown_started == true);

	/* During shutdown, lvs with no lvols should be unloaded */
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_lvol_pairs));

	/* Revert module state back to normal */
	g_shutdown_started = false;

	/* Scenario 2
	 * Test creating lvs with two lvols. Delete first lvol explicitly,
	 * then start bdev finish. This should unload the remaining lvol and
	 * lvol store. */

	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0,
			       vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol = g_lvol;

	rc = vbdev_lvol_create(lvs, "lvol2", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0,
			       vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol2 = g_lvol;

	/* Destroy explicitly first lvol */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Start bdev finish and unregister remaining lvol */
	vbdev_lvs_fini_start();
	CU_ASSERT(g_shutdown_started == true);
	spdk_bdev_unregister(lvol2->bdev, _spdk_bdev_unregister_cb, NULL);

	/* During shutdown, removal of last lvol should unload lvs */
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_lvol_pairs));

	/* Revert module state back to normal */
	g_shutdown_started = false;
}

static void
ut_lvol_resize(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	int sz = 10;
	int rc = 0;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Successful lvol resize */
	g_lvolerrno = -1;
	sz = 20 * g_cluster_size;
	vbdev_lvol_resize(lvol, sz, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);
	assert_blockcnt(g_lvol, sz);

	/* Resize with NULL lvol */
	vbdev_lvol_resize(NULL, 34 * g_cluster_size, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(g_lvolerrno != 0);
	assert_blockcnt(g_lvol, sz);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_set_read_only(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	int sz = 10;
	int rc = 0;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Successful set lvol as read only */
	g_lvolerrno = -1;
	vbdev_lvol_set_read_only(lvol, vbdev_lvol_set_read_only_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvs_unload(void)
{
	int rc = 0;
	int sz = 10;
	struct spdk_lvol_store *lvs;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	spdk_uuid_generate(&lvs->uuid);

	/* Successfully create lvol, which should be destroyed with lvs later */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Unload lvol store */
	vbdev_lvs_unload(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_lvol != NULL);
}

static void
ut_lvs_init(void)
{
	int rc = 0;
	struct spdk_lvol_store *lvs;

	/* spdk_lvs_init() fails */
	lvol_store_initialize_fail = true;

	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	lvol_store_initialize_fail = false;

	/* spdk_lvs_init_cb() fails */
	lvol_store_initialize_cb_fail = true;

	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);

	lvol_store_initialize_cb_fail = false;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	/* Bdev with lvol store already claimed */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Destruct lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_vbdev_lvol_get_io_channel(void)
{
	struct spdk_io_channel *ch;

	g_lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	ch = vbdev_lvol_get_io_channel(g_lvol);
	CU_ASSERT(ch == g_ch);

	free(g_lvol);
}

static void
ut_vbdev_lvol_io_type_supported(void)
{
	struct spdk_lvol *lvol;
	bool ret;

	lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	g_blob_is_read_only = false;

	/* Supported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_RESET);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_UNMAP);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_SEEK_DATA);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_SEEK_HOLE);
	CU_ASSERT(ret == true);

	/* Unsupported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_FLUSH);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_ADMIN);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_IO);
	CU_ASSERT(ret == false);

	g_blob_is_read_only = true;

	/* Supported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_RESET);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_SEEK_DATA);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_SEEK_HOLE);
	CU_ASSERT(ret == true);

	/* Unsupported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_UNMAP);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_FLUSH);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_ADMIN);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_IO);
	CU_ASSERT(ret == false);

	free(lvol);
}

static void
ut_lvol_read_write(void)
{
	g_io = calloc(1, sizeof(struct spdk_bdev_io) + vbdev_lvs_get_ctx_size());
	SPDK_CU_ASSERT_FATAL(g_io != NULL);
	g_base_bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);
	g_lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_io->bdev = g_base_bdev;
	g_io->bdev->ctxt = g_lvol;
	g_io->u.bdev.offset_blocks = 20;
	g_io->u.bdev.num_blocks = 20;

	lvol_read(g_ch, g_io);
	CU_ASSERT(g_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	lvol_write(g_lvol, g_ch, g_io);
	CU_ASSERT(g_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	g_ext_api_called = false;
	lvol_read(g_ch, g_io);
	CU_ASSERT(g_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_ext_api_called == true);
	g_ext_api_called = false;

	lvol_write(g_lvol, g_ch, g_io);
	CU_ASSERT(g_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_ext_api_called == true);
	g_ext_api_called = false;

	free(g_io);
	free(g_base_bdev);
	free(g_lvol);
}

static void
ut_vbdev_lvol_submit_request(void)
{
	struct spdk_lvol request_lvol = {};
	g_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(g_io != NULL);
	g_base_bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);
	g_io->bdev = g_base_bdev;

	g_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_base_bdev->ctxt = &request_lvol;
	vbdev_lvol_submit_request(g_ch, g_io);

	free(g_io);
	free(g_base_bdev);
}

static void
ut_lvs_rename(void)
{
	int rc = 0;
	int sz = 10;
	struct spdk_lvol_store *lvs;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "old_lvs_name", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	g_base_bdev = calloc(1, sizeof(*g_base_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);

	/* Successfully create lvol, which should be destroyed with lvs later */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Trying to rename lvs with lvols created */
	vbdev_lvs_rename(lvs, "new_lvs_name", lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");
	CU_ASSERT_STRING_EQUAL(TAILQ_FIRST(&g_lvol->bdev->aliases)->alias.name, "new_lvs_name/lvol");

	/* Trying to rename lvs with name already used by another lvs */
	/* This is a bdev_lvol test, so g_lvs_with_name_already_exists simulates
	 * existing lvs with name 'another_new_lvs_name' and this name in fact is not compared */
	g_lvs_with_name_already_exists = true;
	vbdev_lvs_rename(lvs, "another_new_lvs_name", lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EEXIST);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");
	CU_ASSERT_STRING_EQUAL(TAILQ_FIRST(&g_lvol->bdev->aliases)->alias.name, "new_lvs_name/lvol");
	g_lvs_with_name_already_exists = false;

	/* Unload lvol store */
	g_lvol_store = lvs;
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	free(g_base_bdev->name);
	free(g_base_bdev);
}

static void
ut_lvol_seek(void)
{
	g_io = calloc(1, sizeof(struct spdk_bdev_io) + vbdev_lvs_get_ctx_size());
	SPDK_CU_ASSERT_FATAL(g_io != NULL);
	g_base_bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);
	g_lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_io->bdev = g_base_bdev;
	g_io->bdev->ctxt = g_lvol;

	/* Data found */
	g_io->u.bdev.offset_blocks = 10;
	lvol_seek_data(g_lvol, g_io);
	CU_ASSERT(g_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io->u.bdev.seek.offset == g_blob_allocated_io_unit_offset);

	/* Data not found */
	g_io->u.bdev.offset_blocks = 30;
	lvol_seek_data(g_lvol, g_io);
	CU_ASSERT(g_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io->u.bdev.seek.offset == UINT64_MAX);

	/* Hole found */
	g_io->u.bdev.offset_blocks = 10;
	lvol_seek_hole(g_lvol, g_io);
	CU_ASSERT(g_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io->u.bdev.seek.offset == 10);

	/* Hole not found */
	g_io->u.bdev.offset_blocks = 30;
	lvol_seek_hole(g_lvol, g_io);
	CU_ASSERT(g_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io->u.bdev.seek.offset == UINT64_MAX);

	free(g_io);
	free(g_base_bdev);
	free(g_lvol);
}

static void
ut_esnap_dev_create(void)
{
	struct spdk_lvol_store lvs = { 0 };
	struct spdk_lvol lvol = { 0 };
	struct spdk_blob blob = { 0 };
	struct spdk_bdev bdev = { 0 };
	const char uuid_str[SPDK_UUID_STRING_LEN] = "a27fd8fe-d4b9-431e-a044-271016228ce4";
	char bad_uuid_str[SPDK_UUID_STRING_LEN] = "a27fd8fe-d4b9-431e-a044-271016228ce4";
	char *unterminated;
	size_t len;
	struct spdk_bs_dev *bs_dev = NULL;
	int rc;

	bdev.name = "bdev0";
	spdk_uuid_parse(&bdev.uuid, uuid_str);

	/* NULL esnap_id */
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, NULL, 0, &bs_dev);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bs_dev == NULL);

	/* Unterminated UUID: asan should catch reads past end of allocated buffer. */
	len = strlen(uuid_str);
	unterminated = calloc(1, len);
	SPDK_CU_ASSERT_FATAL(unterminated != NULL);
	memcpy(unterminated, uuid_str, len);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, unterminated, len, &bs_dev);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bs_dev == NULL);

	/* Invaid UUID but the right length is invalid */
	bad_uuid_str[2] = 'z';
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, bad_uuid_str, sizeof(uuid_str),
					 &bs_dev);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bs_dev == NULL);

	/* Bdev not found */
	g_base_bdev = NULL;
	MOCK_SET(spdk_lvol_is_degraded, true);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, uuid_str, sizeof(uuid_str), &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bs_dev->destroy == bs_dev_degraded_destroy);
	bs_dev->destroy(bs_dev);

	/* Cannot get a claim */
	/* TODO: This suggests we need a way to wait for a claim to be available. */
	g_base_bdev = &bdev;
	lvol_already_opened = true;
	MOCK_SET(spdk_lvol_is_degraded, true);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, uuid_str, sizeof(uuid_str), &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bs_dev->destroy == bs_dev_degraded_destroy);
	bs_dev->destroy(bs_dev);

	/* Happy path */
	lvol_already_opened = false;
	MOCK_SET(spdk_lvol_is_degraded, false);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, uuid_str, sizeof(uuid_str), &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bs_dev->destroy == ut_bs_dev_destroy);
	bs_dev->destroy(bs_dev);

	g_base_bdev = NULL;
	lvol_already_opened = false;
	free(unterminated);
	MOCK_CLEAR(spdk_lvol_is_degraded);
}

static void
ut_lvol_esnap_clone_bad_args(void)
{
	struct spdk_bdev bdev = { 0 };
	struct spdk_lvol_store *lvs;
	const char *esnap_uuid = "255f4236-9427-42d0-a9d1-aa17f37dd8db";
	const char *esnap_name = "esnap1";
	int rc;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	rc = spdk_uuid_parse(&bdev.uuid, esnap_uuid);
	CU_ASSERT(rc == 0);
	bdev.name = strdup(esnap_name);
	SPDK_CU_ASSERT_FATAL(bdev.name != NULL);
	bdev.blocklen = 512;
	SPDK_CU_ASSERT_FATAL(SPDK_BS_PAGE_SIZE % bdev.blocklen == 0);
	bdev.blockcnt = 8192;

	g_base_bdev = &bdev;

	/* Error when lvs is NULL */
	g_lvolerrno = 0xbad;
	vbdev_lvol_create_bdev_clone(esnap_uuid, NULL, "clone1", vbdev_lvol_create_complete, NULL);
	CU_ASSERT(g_lvolerrno == -EINVAL);

	/* Error when the bdev does not exist */
	g_base_bdev = NULL;
	g_lvolerrno = 0xbad;
	vbdev_lvol_create_bdev_clone(esnap_uuid, lvs, "clone1", vbdev_lvol_create_complete, NULL);
	CU_ASSERT(g_lvolerrno == -ENODEV);

	/* Success when creating by bdev UUID */
	g_base_bdev = &bdev;
	g_lvolerrno = 0xbad;
	vbdev_lvol_create_bdev_clone(esnap_uuid, lvs, "clone1", vbdev_lvol_create_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Success when creating by bdev name */
	g_lvolerrno = 0xbad;
	vbdev_lvol_create_bdev_clone(esnap_name, lvs, "clone2", vbdev_lvol_create_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);

	g_lvol_store = lvs;
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	free(bdev.name);
	g_base_bdev = NULL;
}

static void
ut_lvol_shallow_copy(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;
	struct spdk_lvol *lvol = NULL;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, 0,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol_sc", sz, false, LVOL_CLEAR_WITH_DEFAULT, 0,
			       vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	lvol = g_lvol;

	/* Shallow copy error with NULL lvol */
	rc = vbdev_lvol_shallow_copy(NULL, "", NULL, NULL, vbdev_lvol_shallow_copy_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Shallow copy error with NULL bdev name */
	rc = vbdev_lvol_shallow_copy(lvol, NULL, NULL, NULL, vbdev_lvol_shallow_copy_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Successful shallow copy */
	g_lvolerrno = -1;
	lvol_already_opened = false;
	rc = vbdev_lvol_shallow_copy(lvol, "bdev_sc", NULL, NULL, vbdev_lvol_shallow_copy_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL, false);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_set_external_parent(void)
{
	struct spdk_lvol_store lvs = { 0 };
	struct spdk_lvol lvol = { 0 };
	struct spdk_bdev bdev = { 0 };
	const char *esnap_uuid = "255f4236-9427-42d0-a9d1-aa17f37dd8db";
	const char *esnap_name = "esnap1";
	int rc;

	lvol.lvol_store = &lvs;

	rc = spdk_uuid_parse(&bdev.uuid, esnap_uuid);
	CU_ASSERT(rc == 0);
	bdev.name = strdup(esnap_name);
	SPDK_CU_ASSERT_FATAL(bdev.name != NULL);
	bdev.blocklen = 512;
	bdev.blockcnt = 8192;

	g_base_bdev = &bdev;

	/* Error when the bdev does not exist */
	g_base_bdev = NULL;
	g_lvolerrno = 0xbad;
	vbdev_lvol_set_external_parent(&lvol, esnap_uuid, vbdev_lvol_op_complete, NULL);
	CU_ASSERT(g_lvolerrno == -ENODEV);

	/* Success when setting parent by bdev UUID */
	g_base_bdev = &bdev;
	g_lvolerrno = 0xbad;
	vbdev_lvol_set_external_parent(&lvol, esnap_uuid, vbdev_lvol_op_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Success when setting parent by bdev name */
	g_lvolerrno = 0xbad;
	vbdev_lvol_set_external_parent(&lvol, esnap_name, vbdev_lvol_op_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);

	free(bdev.name);
	g_base_bdev = NULL;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("lvol", NULL, NULL);

	CU_ADD_TEST(suite, ut_lvs_init);
	CU_ADD_TEST(suite, ut_lvol_init);
	CU_ADD_TEST(suite, ut_lvol_snapshot);
	CU_ADD_TEST(suite, ut_lvol_clone);
	CU_ADD_TEST(suite, ut_lvs_destroy);
	CU_ADD_TEST(suite, ut_lvs_unload);
	CU_ADD_TEST(suite, ut_lvol_resize);
	CU_ADD_TEST(suite, ut_lvol_set_read_only);
	CU_ADD_TEST(suite, ut_lvol_hotremove);
	CU_ADD_TEST(suite, ut_vbdev_lvol_get_io_channel);
	CU_ADD_TEST(suite, ut_vbdev_lvol_io_type_supported);
	CU_ADD_TEST(suite, ut_lvol_read_write);
	CU_ADD_TEST(suite, ut_vbdev_lvol_submit_request);
	CU_ADD_TEST(suite, ut_lvol_examine_config);
	CU_ADD_TEST(suite, ut_lvol_examine_disk);
	CU_ADD_TEST(suite, ut_lvol_rename);
	CU_ADD_TEST(suite, ut_bdev_finish);
	CU_ADD_TEST(suite, ut_lvs_rename);
	CU_ADD_TEST(suite, ut_lvol_seek);
	CU_ADD_TEST(suite, ut_esnap_dev_create);
	CU_ADD_TEST(suite, ut_lvol_esnap_clone_bad_args);
	CU_ADD_TEST(suite, ut_lvol_shallow_copy);
	CU_ADD_TEST(suite, ut_lvol_set_external_parent);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
