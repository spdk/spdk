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
#include "bdev/ocf/vbdev_ocf.c"
#include "spdk_internal/mock.h"
#include "common/lib/ut_multithread.c"
#include "bdev/bdev.c"

int					g_rc;
struct spdk_bdev	g_bdev;
struct spdk_bdev_desc *g_desc;
void				*g_io_target;

static void
bdev_init_cb(void *done, int rc)
{
	CU_ASSERT(rc == 0);
	*(bool *)done = true;
}

static void
io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_rc = success;
}

static int
stub_create_ch(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
stub_destroy_ch(void *io_device, void *ctx_buf)
{
}


static struct spdk_bdev_fn_table fn_table = {};

static void
register_bdev(struct spdk_bdev *bdev, char *name)
{
	g_bdev.name = name;
	g_bdev.fn_table = &fn_table;
	g_bdev.module = &bdev_ut_if;
	g_bdev.blocklen = 4096;
	g_bdev.blockcnt = 1024;

	spdk_bdev_register(bdev);
}

DEFINE_STUB(ocf_cache_is_running, bool, (ocf_cache_t cache), false);
DEFINE_STUB(ocf_cache_get_id, ocf_cache_id_t, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_cache_get_line_size, ocf_cache_line_size_t, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_cache_get_mode, ocf_cache_mode_t, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_cache_get_queue, int, (ocf_cache_t cache, unsigned id, ocf_queue_t *q), 0);
DEFINE_STUB(ocf_core_get_id, ocf_core_id_t, (ocf_core_t core), 0);
DEFINE_STUB(ocf_get_cache_mode, ocf_cache_mode_t, (const char *cache_mode), 0);
DEFINE_STUB(ocf_get_cache_modename, char*, (ocf_cache_mode_t mode), NULL);
DEFINE_STUB(ocf_mngt_cache_add_core, int, (ocf_cache_t cache, ocf_core_t *core, struct ocf_mngt_core_config *cfg), 0);
DEFINE_STUB(ocf_mngt_cache_attach, int, (ocf_cache_t cache, struct ocf_mngt_cache_device_config *device_cfg), 0);
DEFINE_STUB(ocf_mngt_cache_remove_core, int, (ocf_cache_t cache, ocf_core_id_t core_id, bool detach), 0);
DEFINE_STUB(ocf_mngt_cache_start, int, (ocf_ctx_t ctx, ocf_cache_t *cache, struct ocf_mngt_cache_config *cfg), 0);
DEFINE_STUB(ocf_mngt_cache_stop, int, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_new_io, struct ocf_io*, (ocf_core_t core), NULL);
DEFINE_STUB(ocf_queue_get_id, uint32_t, (ocf_queue_t q), 0);
DEFINE_STUB(ocf_queue_pending_io, uint32_t, (ocf_queue_t q), 0);
DEFINE_STUB_V(ocf_queue_run,(ocf_queue_t q));
DEFINE_STUB_V(ocf_queue_set_priv, (ocf_queue_t q, void *priv));
DEFINE_STUB(ocf_submit_discard, int, (struct ocf_io *io), 0);
DEFINE_STUB(ocf_submit_flush, int, (struct ocf_io *io), 0);
DEFINE_STUB(ocf_submit_io_mode, int, (struct ocf_io *io, ocf_cache_mode_t cache_mode), 0);


static void ut_ocf_init(void)
{
	struct vbdev_ocf *vbdev;
	struct vbdev_ocf_base *base;
	const char *vbdev_name = "cache1";
	const char *cache_mode_name = "wt";
	const char *cache_name = "fast";
	const char *core_name = "slow";
	int rc;

	register_bdev(&g_bdev, core_name);

	rc = vbdev_ocf_construct(vbdev_name, cache_mode_name, cache_name, core_name);
	SPDK_CU_ASSERT_FATAL(rc != 0);

	vbdev = vbdev_ocf_get_by_name(vbdev_name);
	SPDK_CU_ASSERT_FATAL(vbdev != NULL);

	base = vbdev_ocf_get_base_by_name(vbdev_name);
	SPDK_CU_ASSERT_FATAL(base != NULL);
	SPDK_CU_ASSERT_FATAL(rc != 0);

	rc = vbdev_ocf_delete(vbdev);
	SPDK_CU_ASSERT_FATAL(rc != 0);
}

static void ut_ocf_io(void)
{
	int rc = 0;
	struct spdk_io_channel *io_ch[2];

	allocate_threads(2);
	set_thread(0);
	spdk_bdev_initialize(bdev_init_cb, &done);
	spdk_io_device_register(&g_io_device, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open(&g_bdev, true, NULL, NULL, &g_desc);

	rc = spdk_bdev_write_blocks(g_desc, io_ch[0], NULL, 0, 1, io_cb, NULL);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	/* TODO: check in ocf callback that request appear */

	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_cb, NULL);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	/* TODO: check data integrity */

	poll_threads();
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ocf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "ut_ocf_init", ut_ocf_init) == NULL ||
		CU_add_test(suite, "ut_ocf_io", ut_ocf_io) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
