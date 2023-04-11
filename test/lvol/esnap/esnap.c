/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/string.h"
#include "spdk/init.h"

#include "common/lib/ut_multithread.c"

#include "bdev/bdev.c"
#include "lvol/lvol.c"
#include "bdev/malloc/bdev_malloc.c"
#include "bdev/lvol/vbdev_lvol.c"
#include "accel/accel_sw.c"
#include "bdev/part.c"
#include "blob/blobstore.h"
#include "bdev/aio/bdev_aio.h"

#include "unit/lib/json_mock.c"

#ifdef SPDK_CONFIG_PMDK
DEFINE_STUB(pmem_msync, int, (const void *addr, size_t len), 0);
DEFINE_STUB(pmem_memcpy_persist, void *, (void *pmemdest, const void *src, size_t len), NULL);
DEFINE_STUB(pmem_is_pmem, int, (const void *addr, size_t len), 0);
DEFINE_STUB(pmem_memset_persist, void *, (void *pmemdest, int c, size_t len), NULL);
#endif

char g_testdir[PATH_MAX];

static void
set_testdir(const char *path)
{
	char *tmp;

	tmp = realpath(path, NULL);
	snprintf(g_testdir, sizeof(g_testdir), "%s", tmp ? dirname(tmp) : ".");
	free(tmp);
}

static int
make_test_file(size_t size, char *path, size_t len, const char *name)
{
	int fd;
	int rc;

	CU_ASSERT(len <= INT32_MAX);
	if (snprintf(path, len, "%s/%s", g_testdir, name) >= (int)len) {
		return -ENAMETOOLONG;
	}
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return -errno;
	}
	rc = ftruncate(fd, size);
	if (rc != 0) {
		rc = -errno;
		unlink(path);
	}
	close(fd);
	return rc;
}

static void
unregister_cb(void *ctx, int bdeverrno)
{
	int *rc = ctx;

	if (rc != NULL) {
		*rc = bdeverrno;
	}
}

struct op_with_handle_data {
	union {
		struct spdk_lvol_store *lvs;
		struct spdk_lvol *lvol;
	} u;
	int lvserrno;
};

static struct op_with_handle_data *
clear_owh(struct op_with_handle_data *owh)
{
	memset(owh, 0, sizeof(*owh));
	owh->lvserrno = 0xbad;

	return owh;
}

/* spdk_poll_threads() doesn't have visibility into uncompleted aio operations. */
static void
poll_error_updated(int *error)
{
	while (*error == 0xbad) {
		poll_threads();
	}
}

static void
lvs_op_with_handle_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct op_with_handle_data *data = cb_arg;

	data->u.lvs = lvs;
	data->lvserrno = lvserrno;
}

static void
lvol_op_with_handle_cb(void *cb_arg, struct spdk_lvol *lvol, int lvserrno)
{
	struct op_with_handle_data *data = cb_arg;

	data->u.lvol = lvol;
	data->lvserrno = lvserrno;
}

static void
lvol_op_complete_cb(void *cb_arg, int lvolerrno)
{
	int *err = cb_arg;

	*err = lvolerrno;
}

static void
ut_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

static void
io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	int *err = cb_arg;

	spdk_bdev_free_io(bdev_io);
	SPDK_CU_ASSERT_FATAL(success);

	*err = 0;
}

static void
prepare_block(char *buf, size_t bufsz, const char *uuid_str, uint64_t block)
{
	memset(buf, 0, bufsz);
	snprintf(buf, bufsz, "%s %8" PRIu64, uuid_str, block);
}

static void
scribble(struct spdk_bdev_desc *desc, uint64_t start, uint64_t count)
{
	struct spdk_bdev *bdev = desc->bdev;
	const uint32_t blocklen = desc->bdev->blocklen;
	struct spdk_io_channel *ch = spdk_bdev_get_io_channel(desc);
	char uuid_str[SPDK_UUID_STRING_LEN];
	char buf[count][blocklen];
	int err = 0xbad;
	uint64_t i;

	SPDK_CU_ASSERT_FATAL(count > 0 && count < INT32_MAX);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);

	for (i = 0; i < count; i++) {
		prepare_block(buf[i], sizeof(buf[i]), uuid_str, start + i);
	}

	spdk_bdev_write(desc, ch, buf, start * blocklen, sizeof(buf), io_done, &err);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(err == 0);
	spdk_put_io_channel(ch);
	poll_threads();
}

#define verify(desc, bdev, start, count) _verify(desc, bdev, start, count, __FILE__, __LINE__)

static bool
_verify(struct spdk_bdev_desc *desc, struct spdk_bdev *bdev, uint64_t start, uint64_t count,
	const char *file, int line)
{
	struct spdk_io_channel *ch = spdk_bdev_get_io_channel(desc);
	const uint32_t blocklen = desc->bdev->blocklen;
	char uuid_str[SPDK_UUID_STRING_LEN];
	char buf[blocklen];
	char expect[blocklen];
	int err = 0xbad;
	bool ret = true;
	uint64_t i;

	SPDK_CU_ASSERT_FATAL(count > 0 && count < INT32_MAX);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);

	for (i = 0; i < count; i++) {
		uint64_t block = start + i;

		spdk_bdev_read(desc, ch, buf, block * blocklen, sizeof(buf), io_done, &err);
		poll_threads();
		SPDK_CU_ASSERT_FATAL(err == 0);
		prepare_block(expect, sizeof(expect), uuid_str, block);
		if (memcmp(expect, buf, blocklen) != 0) {
			printf("%s:%d: ERROR: expected '%s' got '%s'\n", file, line,
			       expect, buf);
			ret = false;
		}
	}

	spdk_put_io_channel(ch);
	poll_threads();

	return ret;
}

static bool
cluster_is_allocated(struct spdk_blob *blob, uint32_t cluster)
{
	return bs_io_unit_is_allocated(blob, cluster * blob->bs->pages_per_cluster);
}

static void
esnap_clone_io(void)
{
	struct spdk_lvol_store *lvs = NULL;
	struct spdk_bdev *bs_bdev = NULL;
	struct spdk_bdev *esnap_bdev = NULL;
	struct spdk_bdev *lvol_bdev = NULL;
	struct spdk_bdev_desc *esnap_desc = NULL;
	struct spdk_bdev_desc *lvol_desc = NULL;
	const char bs_malloc_uuid[SPDK_UUID_STRING_LEN] = "11110049-cf29-4681-ab4b-5dd16de6cd81";
	const char esnap_uuid[SPDK_UUID_STRING_LEN] = "222251be-1ece-434d-8513-6944d5c93a53";
	struct malloc_bdev_opts malloc_opts = { 0 };
	const uint32_t bs_size_bytes = 10 * 1024 * 1024;
	const uint32_t bs_block_size = 4096;
	const uint32_t cluster_size = 32 * 1024;
	const uint32_t blocks_per_cluster = cluster_size / bs_block_size;
	const uint32_t esnap_size_bytes = 4 * cluster_size;
	struct op_with_handle_data owh_data = { 0 };
	struct lvol_bdev *_lvol_bdev;
	struct spdk_blob *blob;
	int rc;

	g_bdev_opts.bdev_auto_examine = false;

	/* Create device for lvstore */
	spdk_uuid_parse(&malloc_opts.uuid, bs_malloc_uuid);
	malloc_opts.name = "bs_malloc";
	malloc_opts.num_blocks = bs_size_bytes / bs_block_size;
	malloc_opts.block_size = bs_block_size;
	rc = create_malloc_disk(&bs_bdev, &malloc_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Create lvstore */
	rc = vbdev_lvs_create("bs_malloc", "lvs1", cluster_size, 0, 0,
			      lvs_op_with_handle_cb, clear_owh(&owh_data));
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvs != NULL);
	lvs = owh_data.u.lvs;

	/* Create esnap device */
	memset(&malloc_opts, 0, sizeof(malloc_opts));
	spdk_uuid_parse(&malloc_opts.uuid, esnap_uuid);
	malloc_opts.name = "esnap_malloc";
	malloc_opts.num_blocks = esnap_size_bytes / bs_block_size;
	malloc_opts.block_size = bs_block_size;
	rc = create_malloc_disk(&esnap_bdev, &malloc_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Fill esnap device with pattern */
	rc = spdk_bdev_open_ext(esnap_uuid, true, ut_event_cb, NULL, &esnap_desc);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	scribble(esnap_desc, 0, esnap_bdev->blockcnt);

	/* Reopen the external snapshot read-only for verification later */
	spdk_bdev_close(esnap_desc);
	poll_threads();
	rc = spdk_bdev_open_ext(esnap_uuid, false, ut_event_cb, NULL, &esnap_desc);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Create esnap clone */
	vbdev_lvol_create_bdev_clone(esnap_uuid, lvs, "clone1",
				     lvol_op_with_handle_cb, clear_owh(&owh_data));
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvol != NULL);

	/* Open the esnap clone */
	rc = spdk_bdev_open_ext("lvs1/clone1", true, ut_event_cb, NULL, &lvol_desc);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	lvol_bdev = lvol_desc->bdev;
	_lvol_bdev = (struct lvol_bdev *)lvol_bdev;
	blob = _lvol_bdev->lvol->blob;
	CU_ASSERT(blob->active.num_clusters == 4);
	CU_ASSERT(!cluster_is_allocated(blob, 0));
	CU_ASSERT(!cluster_is_allocated(blob, 1));
	CU_ASSERT(!cluster_is_allocated(blob, 2));
	CU_ASSERT(!cluster_is_allocated(blob, 3));

	/* Be sure the esnap and the clone see the same content. */
	CU_ASSERT(verify(esnap_desc, esnap_bdev, 0, esnap_bdev->blockcnt));
	CU_ASSERT(verify(lvol_desc, esnap_bdev, 0, esnap_bdev->blockcnt));

	/* Overwrite the second block of the first cluster then verify the whole first cluster */
	scribble(lvol_desc, 1, 1);
	CU_ASSERT(cluster_is_allocated(blob, 0));
	CU_ASSERT(!cluster_is_allocated(blob, 1));
	CU_ASSERT(!cluster_is_allocated(blob, 2));
	CU_ASSERT(!cluster_is_allocated(blob, 3));
	CU_ASSERT(verify(lvol_desc, esnap_bdev, 0, 1));
	CU_ASSERT(verify(lvol_desc, lvol_bdev, 1, 1));
	CU_ASSERT(verify(lvol_desc, esnap_bdev, 2, blocks_per_cluster - 2));
	/* And be sure no writes made it to the external snapshot */
	CU_ASSERT(verify(esnap_desc, esnap_bdev, 0, esnap_bdev->blockcnt));

	/* Overwrite the two blocks that span the end of the first cluster and the start of the
	 * second cluster
	 */
	scribble(lvol_desc, blocks_per_cluster - 1, 2);
	/* The first part of the first cluster was written previously - it should be the same. */
	CU_ASSERT(cluster_is_allocated(blob, 0));
	CU_ASSERT(cluster_is_allocated(blob, 1));
	CU_ASSERT(!cluster_is_allocated(blob, 2));
	CU_ASSERT(!cluster_is_allocated(blob, 3));
	CU_ASSERT(verify(lvol_desc, esnap_bdev, 0, 1));
	CU_ASSERT(verify(lvol_desc, lvol_bdev, 1, 1));
	CU_ASSERT(verify(lvol_desc, esnap_bdev, 2, blocks_per_cluster - 2 - 1));
	/* Check the newly written area spanning the first two clusters. */
	CU_ASSERT(verify(lvol_desc, lvol_bdev, blocks_per_cluster - 1, 2));
	/* The rest should not have changed. */
	CU_ASSERT(verify(lvol_desc, esnap_bdev, blocks_per_cluster + 1,
			 esnap_bdev->blockcnt - blocks_per_cluster - 1));
	/* And be sure no writes made it to the external snapshot */
	CU_ASSERT(verify(esnap_desc, esnap_bdev, 0, esnap_bdev->blockcnt));

	/* Clean up */
	bdev_close(lvol_bdev, lvol_desc);
	bdev_close(esnap_bdev, esnap_desc);
	delete_malloc_disk("esnap_malloc", NULL, 0);
	/* This triggers spdk_lvs_unload() */
	delete_malloc_disk("bs_malloc", NULL, 0);
	poll_threads();
}

static void
esnap_wait_for_examine(void *ctx)
{
	int *flag = ctx;

	*flag = 0;
}

static void
esnap_hotplug(void)
{
	const char *uuid_esnap = "22218fb6-6743-483d-88b1-de643dc7c0bc";
	struct malloc_bdev_opts malloc_opts = { 0 };
	const uint32_t bs_size_bytes = 10 * 1024 * 1024;
	const uint32_t bs_block_size = 4096;
	const uint32_t cluster_size = 32 * 1024;
	const uint32_t esnap_size_bytes = 2 * cluster_size;
	struct op_with_handle_data owh_data = { 0 };
	struct spdk_bdev *malloc_bdev = NULL, *bdev;
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	char aiopath[PATH_MAX];
	int rc, rc2;

	g_bdev_opts.bdev_auto_examine = true;

	/* Create aio device to hold the lvstore. */
	rc = make_test_file(bs_size_bytes, aiopath, sizeof(aiopath), "esnap_hotplug.aio");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = create_aio_bdev("aio1", aiopath, bs_block_size, false);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_threads();

	rc = vbdev_lvs_create("aio1", "lvs1", cluster_size, 0, 0,
			      lvs_op_with_handle_cb, clear_owh(&owh_data));
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvs != NULL);
	lvs = owh_data.u.lvs;

	/* Create esnap device */
	spdk_uuid_parse(&malloc_opts.uuid, uuid_esnap);
	malloc_opts.name = "esnap_malloc";
	malloc_opts.num_blocks = esnap_size_bytes / bs_block_size;
	malloc_opts.block_size = bs_block_size;
	rc = create_malloc_disk(&malloc_bdev, &malloc_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Create esnap clone */
	vbdev_lvol_create_bdev_clone(uuid_esnap, lvs, "clone1",
				     lvol_op_with_handle_cb, clear_owh(&owh_data));
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvol != NULL);

	/* Verify that lvol bdev exists */
	bdev = spdk_bdev_get_by_name("lvs1/clone1");
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	/* Unload the lvstore and verify the bdev is gone. */
	rc = rc2 = 0xbad;
	bdev_aio_delete("aio1", unregister_cb, &rc);
	CU_ASSERT(spdk_bdev_get_by_name(uuid_esnap) != NULL)
	delete_malloc_disk(malloc_bdev->name, unregister_cb, &rc2);
	malloc_bdev = NULL;
	poll_error_updated(&rc);
	poll_error_updated(&rc2);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(rc2 == 0);
	SPDK_CU_ASSERT_FATAL(spdk_bdev_get_by_name("lvs1/clone1") == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_bdev_get_by_name(uuid_esnap) == NULL);

	/* Trigger the reload of the lvstore */
	rc = create_aio_bdev("aio1", aiopath, bs_block_size, false);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = 0xbad;
	spdk_bdev_wait_for_examine(esnap_wait_for_examine, &rc);
	poll_error_updated(&rc);

	/* Verify the lvol is loaded without creating a bdev. */
	lvol = spdk_lvol_get_by_names("lvs1", "clone1");
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/clone1") == NULL)
	SPDK_CU_ASSERT_FATAL(lvol != NULL);
	SPDK_CU_ASSERT_FATAL(lvol->degraded_set != NULL);

	/* Create the esnap device and verify that the bdev is created. */
	rc = create_malloc_disk(&malloc_bdev, &malloc_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_threads();
	CU_ASSERT(malloc_bdev != NULL);
	CU_ASSERT(lvol->degraded_set == NULL);
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/clone1") != NULL);

	/* Clean up */
	rc = rc2 = 0xbad;
	bdev_aio_delete("aio1", unregister_cb, &rc);
	poll_error_updated(&rc);
	CU_ASSERT(rc == 0);
	if (malloc_bdev != NULL) {
		delete_malloc_disk(malloc_bdev->name, unregister_cb, &rc2);
		poll_threads();
		CU_ASSERT(rc2 == 0);
	}
	rc = unlink(aiopath);
	CU_ASSERT(rc == 0);
}

static void
esnap_remove_degraded(void)
{
	const char *uuid_esnap = "33358eb9-3dcf-4275-b089-0becc126fc3d";
	struct malloc_bdev_opts malloc_opts = { 0 };
	const uint32_t bs_size_bytes = 10 * 1024 * 1024;
	const uint32_t bs_block_size = 4096;
	const uint32_t cluster_size = 32 * 1024;
	const uint32_t esnap_size_bytes = 2 * cluster_size;
	struct op_with_handle_data owh_data = { 0 };
	struct spdk_lvol_store *lvs;
	struct spdk_bdev *malloc_bdev = NULL;
	struct spdk_lvol *vol1, *vol2, *vol3;
	char aiopath[PATH_MAX];
	int rc, rc2;

	g_bdev_opts.bdev_auto_examine = true;

	/* Create aio device to hold the lvstore. */
	rc = make_test_file(bs_size_bytes, aiopath, sizeof(aiopath), "remove_degraded.aio");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = create_aio_bdev("aio1", aiopath, bs_block_size, false);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_threads();

	rc = vbdev_lvs_create("aio1", "lvs1", cluster_size, 0, 0,
			      lvs_op_with_handle_cb, clear_owh(&owh_data));
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvs != NULL);
	lvs = owh_data.u.lvs;

	/* Create esnap device */
	spdk_uuid_parse(&malloc_opts.uuid, uuid_esnap);
	malloc_opts.name = "esnap";
	malloc_opts.num_blocks = esnap_size_bytes / bs_block_size;
	malloc_opts.block_size = bs_block_size;
	rc = create_malloc_disk(&malloc_bdev, &malloc_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Create a snapshot of vol1.
	 * State:
	 *   esnap <-- vol1
	 */
	vbdev_lvol_create_bdev_clone(uuid_esnap, lvs, "vol1",
				     lvol_op_with_handle_cb, clear_owh(&owh_data));
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvol != NULL);
	vol1 = owh_data.u.lvol;

	/* Create a snapshot of vol1.
	 * State:
	 *   esnap <-- vol2 <-- vol1
	 */
	vbdev_lvol_create_snapshot(vol1, "vol2", lvol_op_with_handle_cb, clear_owh(&owh_data));
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvol != NULL);
	vol2 = owh_data.u.lvol;

	/* Create a clone of vol2.
	 * State:
	 *   esnap <-- vol2 <-- vol1
	 *                `---- vol3
	 */
	vbdev_lvol_create_clone(vol2, "vol3", lvol_op_with_handle_cb, clear_owh(&owh_data));
	poll_error_updated(&owh_data.lvserrno);
	SPDK_CU_ASSERT_FATAL(owh_data.lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(owh_data.u.lvol != NULL);
	vol3 = owh_data.u.lvol;

	/* Unload the lvstore and delete esnap */
	rc = rc2 = 0xbad;
	bdev_aio_delete("aio1", unregister_cb, &rc);
	CU_ASSERT(spdk_bdev_get_by_name(uuid_esnap) != NULL)
	delete_malloc_disk(malloc_bdev->name, unregister_cb, &rc2);
	malloc_bdev = NULL;
	poll_error_updated(&rc);
	poll_error_updated(&rc2);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(rc2 == 0);

	/* Trigger the reload of the lvstore.
	 * State:
	 *   (missing) <-- vol2 <-- vol1
	 *                    `---- vol3
	 */
	rc = create_aio_bdev("aio1", aiopath, bs_block_size, false);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = 0xbad;
	spdk_bdev_wait_for_examine(esnap_wait_for_examine, &rc);
	poll_error_updated(&rc);

	/* Verify vol1 is as described in diagram above */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol1") == NULL);
	vol1 = spdk_lvol_get_by_names("lvs1", "vol1");
	SPDK_CU_ASSERT_FATAL(vol1 != NULL);
	lvs = vol1->lvol_store;
	CU_ASSERT(spdk_blob_is_clone(vol1->blob));
	CU_ASSERT(!spdk_blob_is_esnap_clone(vol1->blob));
	CU_ASSERT(!spdk_blob_is_snapshot(vol1->blob));
	CU_ASSERT(vol1->degraded_set == NULL);

	/* Verify vol2 is as described in diagram above */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol2") == NULL);
	vol2 = spdk_lvol_get_by_names("lvs1", "vol2");
	SPDK_CU_ASSERT_FATAL(vol2 != NULL);
	CU_ASSERT(!spdk_blob_is_clone(vol2->blob));
	CU_ASSERT(spdk_blob_is_esnap_clone(vol2->blob));
	CU_ASSERT(spdk_blob_is_snapshot(vol2->blob));
	CU_ASSERT(RB_MIN(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree) == vol2->degraded_set);
	CU_ASSERT(TAILQ_FIRST(&vol2->degraded_set->lvols) == vol2);

	/* Verify vol3 is as described in diagram above */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol3") == NULL);
	vol3 = spdk_lvol_get_by_names("lvs1", "vol3");
	SPDK_CU_ASSERT_FATAL(vol3 != NULL);
	CU_ASSERT(spdk_blob_is_clone(vol3->blob));
	CU_ASSERT(!spdk_blob_is_esnap_clone(vol3->blob));
	CU_ASSERT(!spdk_blob_is_snapshot(vol3->blob));
	CU_ASSERT(vol3->degraded_set == NULL);

	/* Try to delete vol2. Should fail because it has multiple clones. */
	rc = 0xbad;
	vbdev_lvol_destroy(vol2, lvol_op_complete_cb, &rc);
	poll_error_updated(&rc);
	CU_ASSERT(rc == -EPERM);

	/* Delete vol1
	 * New state:
	 *   (missing) <-- vol2 <-- vol3
	 */
	rc = 0xbad;
	vbdev_lvol_destroy(vol1, lvol_op_complete_cb, &rc);
	poll_error_updated(&rc);
	CU_ASSERT(rc == 0);

	/* Verify vol1 is gone */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol1") == NULL);
	vol1 = spdk_lvol_get_by_names("lvs1", "vol1");
	CU_ASSERT(vol1 == NULL);

	/* Verify vol2 is as described in diagram above */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol2") == NULL);
	vol2 = spdk_lvol_get_by_names("lvs1", "vol2");
	SPDK_CU_ASSERT_FATAL(vol2 != NULL);
	CU_ASSERT(!spdk_blob_is_clone(vol2->blob));
	CU_ASSERT(spdk_blob_is_esnap_clone(vol2->blob));
	CU_ASSERT(spdk_blob_is_snapshot(vol2->blob));
	CU_ASSERT(RB_MIN(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree) == vol2->degraded_set);
	CU_ASSERT(TAILQ_FIRST(&vol2->degraded_set->lvols) == vol2);

	/* Verify vol3 is as described in diagram above */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol3") == NULL);
	vol3 = spdk_lvol_get_by_names("lvs1", "vol3");
	SPDK_CU_ASSERT_FATAL(vol3 != NULL);
	CU_ASSERT(spdk_blob_is_clone(vol3->blob));
	CU_ASSERT(!spdk_blob_is_esnap_clone(vol3->blob));
	CU_ASSERT(!spdk_blob_is_snapshot(vol3->blob));
	CU_ASSERT(vol3->degraded_set == NULL);

	/* Delete vol2
	 * New state:
	 *   (missing) <-- vol3
	 */
	rc = 0xbad;
	vbdev_lvol_destroy(vol2, lvol_op_complete_cb, &rc);
	poll_error_updated(&rc);
	CU_ASSERT(rc == 0);

	/* Verify vol2 is gone */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol2") == NULL);
	vol2 = spdk_lvol_get_by_names("lvs1", "vol2");
	SPDK_CU_ASSERT_FATAL(vol2 == NULL);

	/* Verify vol3 is as described in diagram above */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol3") == NULL);
	vol3 = spdk_lvol_get_by_names("lvs1", "vol3");
	SPDK_CU_ASSERT_FATAL(vol3 != NULL);
	CU_ASSERT(!spdk_blob_is_clone(vol3->blob));
	CU_ASSERT(spdk_blob_is_esnap_clone(vol3->blob));
	CU_ASSERT(!spdk_blob_is_snapshot(vol3->blob));
	CU_ASSERT(RB_MIN(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree) == vol3->degraded_set);
	CU_ASSERT(TAILQ_FIRST(&vol3->degraded_set->lvols) == vol3);

	/* Delete vol3
	 * New state:
	 *   (nothing)
	 */
	rc = 0xbad;
	vbdev_lvol_destroy(vol3, lvol_op_complete_cb, &rc);
	poll_error_updated(&rc);
	CU_ASSERT(rc == 0);

	/* Verify vol3 is gone */
	CU_ASSERT(spdk_bdev_get_by_name("lvs1/vol3") == NULL);
	vol3 = spdk_lvol_get_by_names("lvs1", "vol3");
	SPDK_CU_ASSERT_FATAL(vol3 == NULL);

	/* Nothing depends on the missing bdev, so it is no longer missing. */
	CU_ASSERT(RB_EMPTY(&lvs->degraded_lvol_sets_tree));

	/* Clean up */
	rc = rc2 = 0xbad;
	bdev_aio_delete("aio1", unregister_cb, &rc);
	poll_error_updated(&rc);
	CU_ASSERT(rc == 0);
	if (malloc_bdev != NULL) {
		delete_malloc_disk(malloc_bdev->name, unregister_cb, &rc2);
		poll_threads();
		CU_ASSERT(rc2 == 0);
	}
	rc = unlink(aiopath);
	CU_ASSERT(rc == 0);
}

static void
bdev_init_cb(void *arg, int rc)
{
	assert(rc == 0);
}

static void
subsystem_init_cb(int rc, void *ctx)
{
	assert(rc == 0);
}

static void
bdev_fini_cb(void *arg)
{
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;
	int rc;

	set_testdir(argv[0]);

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("esnap_io", NULL, NULL);

	CU_ADD_TEST(suite, esnap_clone_io);
	CU_ADD_TEST(suite, esnap_hotplug);
	CU_ADD_TEST(suite, esnap_remove_degraded);

	allocate_threads(2);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	/*
	 * This is a non-standard way of initializing libraries. It works for this test but
	 * shouldn't be used as an example elsewhere, except for maybe other tests.
	 */
	spdk_subsystem_init(subsystem_init_cb, NULL);
	rc = spdk_iobuf_initialize();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize iobuf\n");
		abort();
	}
	rc = spdk_accel_initialize();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize accel\n");
		abort();
	}
	spdk_bdev_initialize(bdev_init_cb, NULL);

	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	spdk_bdev_finish(bdev_fini_cb, NULL);
	spdk_accel_finish(bdev_fini_cb, NULL);
	spdk_iobuf_finish(bdev_fini_cb, NULL);

	free_threads();

	return num_failures;
}
