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
#include "spdk_internal/mock.h"
#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE
#include "bdev/ocf/vbdev_ocf.c"
#include "bdev/bdev.c"

int					g_rc;
struct spdk_bdev	g_bdev;
struct spdk_bdev_desc *g_desc;
void				*g_io_target;
int					g_done;
struct spdk_bdev_io *g_bdev_io;

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

static struct spdk_io_channel *
stub_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_io_target);
}

static int
stub_destruct(void *ctx)
{
	return 0;
}

static void
stub_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	g_bdev_io = bdev_io;
}

static struct spdk_bdev_fn_table fn_table = {
	.get_io_channel =	stub_get_io_channel,
	.destruct =		stub_destruct,
	.submit_request =	stub_submit_request,
};

static int
module_init(void)
{
	return 0;
}

static void
module_fini(void)
{
}

static void
init_complete(void)
{
}

static void
fini_start(void)
{
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = module_init,
	.module_fini = module_fini,
	.init_complete = init_complete,
	.fini_start = fini_start,
};

struct ut_bdev_channel {
	TAILQ_HEAD(, spdk_bdev_io)	outstanding_io;
	uint32_t			outstanding_cnt;
	uint32_t			avail_cnt;
};

static void
register_bdev(const char *name)
{
	g_bdev.name = strdup(name);
	g_bdev.fn_table = &fn_table;
	g_bdev.module = &bdev_ut_if;
	g_bdev.blocklen = 4096;
	g_bdev.blockcnt = 1024;

	g_io_target = &g_desc;
	spdk_io_device_register(&g_io_target, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);

	spdk_bdev_register(&g_bdev);
}

static void
finish_cb(void *cb_arg)
{
}

static void
unregister_bdev(void)
{
	spdk_io_device_unregister(&g_io_target, NULL);
	free(g_bdev.name);
	spdk_bdev_finish(finish_cb, NULL);
}

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB(spdk_conf_find_section, struct spdk_conf_section *, (struct spdk_conf *cp,
		const char *name), NULL);
DEFINE_STUB(spdk_conf_section_get_nmval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx1, int idx2), NULL);
DEFINE_STUB(spdk_conf_section_get_intval, int, (struct spdk_conf_section *sp, const char *key), -1);

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_owner, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name, const char *short_name,
		uint16_t tpoint_id, uint8_t owner_type,
		uint8_t object_type, uint8_t new_object,
		uint8_t arg1_is_ptr, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));

DEFINE_STUB(spdk_conf_section_get_nval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx), NULL);

/* OCF Stubs */

DEFINE_STUB(ocf_cache_is_running, bool, (ocf_cache_t cache), false);
DEFINE_STUB(ocf_cache_get_id, ocf_cache_id_t, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_cache_get_line_size, ocf_cache_line_size_t, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_cache_get_mode, ocf_cache_mode_t, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_cache_get_queue, int, (ocf_cache_t cache, unsigned id, ocf_queue_t *q), 0);
DEFINE_STUB(ocf_core_get_id, ocf_core_id_t, (ocf_core_t core), 0);
DEFINE_STUB(ocf_get_cache_mode, ocf_cache_mode_t, (const char *cache_mode), 0);
DEFINE_STUB(ocf_get_cache_modename, const char *, (ocf_cache_mode_t mode), NULL);
DEFINE_STUB(ocf_mngt_cache_add_core, int, (ocf_cache_t cache, ocf_core_t *core,
		struct ocf_mngt_core_config *cfg), 0);
DEFINE_STUB(ocf_mngt_cache_attach, int, (ocf_cache_t cache,
		struct ocf_mngt_cache_device_config *device_cfg), 0);
DEFINE_STUB(ocf_mngt_cache_remove_core, int, (ocf_core_t core), 0);
DEFINE_STUB(ocf_mngt_cache_start, int, (ocf_ctx_t ctx, ocf_cache_t *cache,
					struct ocf_mngt_cache_config *cfg), 0);
DEFINE_STUB(ocf_mngt_cache_stop, int, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_new_io, struct ocf_io *, (ocf_core_t core), NULL);
DEFINE_STUB(ocf_queue_get_id, uint32_t, (ocf_queue_t q), 0);
DEFINE_STUB(ocf_queue_pending_io, uint32_t, (ocf_queue_t q), 0);
DEFINE_STUB_V(ocf_queue_run, (ocf_queue_t q));
DEFINE_STUB_V(ocf_queue_set_priv, (ocf_queue_t q, void *priv));
DEFINE_STUB(ocf_submit_discard, int, (struct ocf_io *io), 0);
DEFINE_STUB(ocf_submit_flush, int, (struct ocf_io *io), 0);
DEFINE_STUB(ocf_submit_io_mode, int, (struct ocf_io *io, ocf_cache_mode_t cache_mode), 0);
DEFINE_STUB_V(ocf_io_put, (struct ocf_io *io));
DEFINE_STUB_V(ocf_mngt_cache_unlock, (ocf_cache_t cache));
DEFINE_STUB(ocf_mngt_cache_lock, int, (ocf_cache_t cache), 0);
DEFINE_STUB(ocf_core_get, int, (ocf_cache_t cache, ocf_core_id_t id, ocf_core_t *core), 0);
DEFINE_STUB_V(vbdev_ocf_volume_cleanup, (void));
DEFINE_STUB(vbdev_ocf_volume_init, int, (void), 0);
DEFINE_STUB(ocf_core_get_front_volume, ocf_volume_t, (ocf_core_t core), NULL);
DEFINE_STUB(ocf_volume_new_io, struct ocf_io *, (ocf_volume_t volume), NULL);
DEFINE_STUB_V(ocf_volume_submit_io, (struct ocf_io *io));
DEFINE_STUB_V(ocf_volume_submit_flush, (struct ocf_io *io));
DEFINE_STUB_V(ocf_volume_submit_discard, (struct ocf_io *io));
DEFINE_STUB_V(ocf_queue_put, (ocf_queue_t queue));
DEFINE_STUB(ocf_queue_create, int, (ocf_cache_t cache, ocf_queue_t *queue,
				    const struct ocf_queue_ops *ops), 0);
DEFINE_STUB(ocf_cache_has_pending_requests, bool, (ocf_cache_t cache), 0);

DEFINE_STUB(ocf_queue_get_priv, void *, (ocf_queue_t q), NULL);
DEFINE_STUB_V(ocf_queue_run_single, (ocf_queue_t q));

/* VBDEV_OCF STUBS */

DEFINE_STUB_V(vbdev_ocf_dobj_cleanup, (void));
DEFINE_STUB_V(vbdev_ocf_ctx_cleanup, (void));
DEFINE_STUB(vbdev_ocf_ctx_init, int, (void), 0);
DEFINE_STUB(vbdev_ocf_dobj_init, int, (void), 0);
DEFINE_STUB(vbdev_ocf_data_from_spdk_io, struct bdev_ocf_data *, (struct spdk_bdev_io *bdev_io),
	    NULL);
DEFINE_STUB_V(vbdev_ocf_mngt_poll, (struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn fn));
DEFINE_STUB(vbdev_ocf_mngt_start, int, (struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn *path,
					vbdev_ocf_mngt_callback cb, void *cb_arg), 0);
DEFINE_STUB_V(vbdev_ocf_mngt_continue, (struct vbdev_ocf *vbdev, int status));


ocf_ctx_t vbdev_ocf_ctx;


static int prepare_suite(void)
{
	const char *cache_name = "fast";

	allocate_threads(1);
	set_thread(0);
	register_bdev(cache_name);

	return 0;
}

static int finish_suite(void)
{
	unregister_bdev();
	free_threads();

	return 0;
}
static void delete_cb(void *ctx, int rc)
{
	g_rc = rc;
}

static void ut_ocf_init(void)
{
	struct vbdev_ocf *vbdev;
	struct vbdev_ocf_base *base;
	const char *vbdev_name = "cache1";
	const char *cache_mode_name = "wt";
	const char *cache_name = "fast";
	const char *core_name = "slow";
	int rc;

	spdk_bdev_initialize(bdev_init_cb, &g_done);
	rc = vbdev_ocf_construct(vbdev_name, cache_mode_name, cache_name, core_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	vbdev = vbdev_ocf_get_by_name(vbdev_name);
	SPDK_CU_ASSERT_FATAL(vbdev != NULL);

	base = vbdev_ocf_get_base_by_name(core_name);
	SPDK_CU_ASSERT_FATAL(base != NULL);

	rc = vbdev_ocf_delete(vbdev, delete_cb, NULL);;
	SPDK_CU_ASSERT_FATAL(rc == 0);
}

static void ut_ocf_io(void)
{
	int rc = 0;
	struct spdk_io_channel *ch;

	char buf[4096];

	memset(buf, 0, sizeof(buf));

	spdk_bdev_open(&g_bdev, true, NULL, NULL, &g_desc);
	SPDK_CU_ASSERT_FATAL(g_desc != NULL);

	ch = spdk_get_io_channel(__bdev_to_io_dev(g_desc->bdev));

	rc = spdk_bdev_write_blocks(g_desc, ch, buf, 0, 1, io_cb, NULL);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_threads();

	rc = spdk_bdev_read_blocks(g_desc, ch, buf, 0, 1, io_cb, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	spdk_put_io_channel(ch);
	spdk_put_io_channel(ch);

	poll_threads();

	spdk_bdev_close(g_desc);
	poll_threads();
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ocf", prepare_suite, finish_suite);
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
