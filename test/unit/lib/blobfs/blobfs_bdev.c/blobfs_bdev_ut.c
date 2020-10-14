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

#include "spdk_cunit.h"
#include "spdk/string.h"
#include "spdk/stdinc.h"

#include "blobfs/bdev/blobfs_bdev.c"

int g_fserrno;

bool g_bdev_create_bs_dev_ext_fail = false;
bool g_fs_load_fail = false;
bool g_fs_unload_fail = false;
bool g_bs_bdev_claim_fail = false;
bool g_blobfs_fuse_start_fail = false;
struct blobfs_bdev_operation_ctx *g_fs_ctx;

const char *g_bdev_name = "ut_bdev";
struct spdk_bdev g_bdev;

static void
bs_dev_destroy(struct spdk_bs_dev *dev)
{
}

static struct spdk_bdev *
bs_dev_get_base_bdev(struct spdk_bs_dev *dev)
{
	return &g_bdev;
}

int
spdk_bdev_create_bs_dev_ext(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
			    void *event_ctx, struct spdk_bs_dev **_bs_dev)
{
	static struct spdk_bs_dev bs_dev;

	if (g_bdev_create_bs_dev_ext_fail) {
		return -EINVAL;
	}

	bs_dev.destroy = bs_dev_destroy;
	bs_dev.get_base_bdev = bs_dev_get_base_bdev;

	*_bs_dev = &bs_dev;

	return 0;
}

void
spdk_fs_load(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
	     spdk_fs_op_with_handle_complete cb_fn, void *cb_arg)
{
	int rc = 0;

	if (g_fs_load_fail) {
		rc = -1;
	}

	cb_fn(cb_arg, NULL, rc);

	return;
}

void
spdk_fs_unload(struct spdk_filesystem *fs, spdk_fs_op_complete cb_fn, void *cb_arg)
{
	int rc = 0;

	if (g_fs_unload_fail) {
		rc = -1;
	}

	cb_fn(cb_arg, rc);
	return;
}

void
spdk_fs_init(struct spdk_bs_dev *dev, struct spdk_blobfs_opts *opt,
	     fs_send_request_fn send_request_fn,
	     spdk_fs_op_with_handle_complete cb_fn, void *cb_arg)
{
	int rc = 0;

	if (g_fs_load_fail) {
		rc = -1;
	}

	cb_fn(cb_arg, NULL, rc);
	return;
}

int
spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module)
{
	if (g_bs_bdev_claim_fail) {
		return -1;
	}

	return 0;
}

int
blobfs_fuse_start(const char *bdev_name, const char *mountpoint, struct spdk_filesystem *fs,
		  blobfs_fuse_unmount_cb cb_fn, void *cb_arg, struct spdk_blobfs_fuse **_bfuse)
{
	if (g_blobfs_fuse_start_fail) {
		return -1;
	}

	/* store the ctx for unmount operation */
	g_fs_ctx = cb_arg;

	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
}

int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	fn(ctx);
	return 0;
}

struct spdk_thread *
spdk_get_thread(void)
{
	struct spdk_thread *thd = (struct spdk_thread *)0x1;

	return thd;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return g_bdev_name;
}

void
spdk_fs_opts_init(struct spdk_blobfs_opts *opts)
{
}

void
blobfs_fuse_send_request(fs_request_fn fn, void *arg)
{
}

void
blobfs_fuse_stop(struct spdk_blobfs_fuse *bfuse)
{
}

static void
blobfs_bdev_op_complete(void *cb_arg, int fserrno)
{
	g_fserrno = fserrno;
}

static void
spdk_blobfs_bdev_detect_test(void)
{
	/* spdk_bdev_create_bs_dev_ext() fails */
	g_bdev_create_bs_dev_ext_fail = true;
	spdk_blobfs_bdev_detect(g_bdev_name, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_bdev_create_bs_dev_ext_fail = false;

	/* spdk_fs_load() fails */
	g_fs_load_fail = true;
	spdk_blobfs_bdev_detect(g_bdev_name, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_fs_load_fail = false;

	/* spdk_fs_unload() fails */
	g_fs_unload_fail = true;
	spdk_blobfs_bdev_detect(g_bdev_name, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_fs_unload_fail = false;

	/* no fail */
	spdk_blobfs_bdev_detect(g_bdev_name, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno == 0);
}

static void
spdk_blobfs_bdev_create_test(void)
{
	uint32_t cluster_sz = 1024 * 1024;

	/* spdk_bdev_create_bs_dev_ext() fails */
	g_bdev_create_bs_dev_ext_fail = true;
	spdk_blobfs_bdev_create(g_bdev_name, cluster_sz, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_bdev_create_bs_dev_ext_fail = false;

	/* spdk_bs_bdev_claim() fails */
	g_bs_bdev_claim_fail = true;
	spdk_blobfs_bdev_create(g_bdev_name, cluster_sz, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_bs_bdev_claim_fail = false;

	/* spdk_fs_init() fails */
	g_fs_load_fail = true;
	spdk_blobfs_bdev_create(g_bdev_name, cluster_sz, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_fs_load_fail = false;

	/* spdk_fs_unload() fails */
	g_fs_unload_fail = true;
	spdk_blobfs_bdev_create(g_bdev_name, cluster_sz, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_fs_unload_fail = false;

	/* no fail */
	spdk_blobfs_bdev_create(g_bdev_name, cluster_sz, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno == 0);
}

static void
spdk_blobfs_bdev_mount_test(void)
{
#ifdef SPDK_CONFIG_FUSE
	const char *mountpoint = "/mnt";

	/* spdk_bdev_create_bs_dev_ext() fails */
	g_bdev_create_bs_dev_ext_fail = true;
	spdk_blobfs_bdev_mount(g_bdev_name, mountpoint, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_bdev_create_bs_dev_ext_fail = false;

	/* spdk_bs_bdev_claim() fails */
	g_bs_bdev_claim_fail = true;
	spdk_blobfs_bdev_mount(g_bdev_name, mountpoint, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_bs_bdev_claim_fail = false;

	/* spdk_fs_load() fails */
	g_fs_load_fail = true;
	spdk_blobfs_bdev_mount(g_bdev_name, mountpoint, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_fs_load_fail = false;

	/* blobfs_fuse_start() fails */
	g_blobfs_fuse_start_fail = true;
	spdk_blobfs_bdev_mount(g_bdev_name, mountpoint, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno != 0);

	g_blobfs_fuse_start_fail = false;

	/* no fail */
	spdk_blobfs_bdev_mount(g_bdev_name, mountpoint, blobfs_bdev_op_complete, NULL);
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(g_fs_ctx != NULL);

	/* after mount operation success , we need make sure unmount operation success */
	blobfs_bdev_unmount(g_fs_ctx);
	CU_ASSERT(g_fserrno == 0);
#endif
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("blobfs_bdev_ut", NULL, NULL);

	CU_ADD_TEST(suite, spdk_blobfs_bdev_detect_test);
	CU_ADD_TEST(suite, spdk_blobfs_bdev_create_test);
	CU_ADD_TEST(suite, spdk_blobfs_bdev_mount_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
