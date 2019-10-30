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
#include "spdk_cunit.h"
#include "spdk/env.h"
#include "spdk_internal/mock.h"
#include "spdk/thread.h"
#include "common/lib/test_env.c"
#include "bdev/bs_adapter/vbdev_bs_adapter.c"
#include "bdev/bs_adapter/vbdev_bs_adapter_rpc.c"

#define BLOCK_CNT (1024ul * 1024ul * 1024ul * 1024ul)
#define BLOCK_SIZE 4096

/* Globals */
static TAILQ_HEAD(, spdk_bdev) g_bdev_list = TAILQ_HEAD_INITIALIZER(g_bdev_list);
static struct spdk_thread *g_thread;
uint32_t g_io_comp_status;
uint8_t g_json_decode_obj_construct;
uint8_t g_rpc_err;
uint32_t g_rpc_req_size = 0;
void *g_rpc_req = NULL;
uint8_t *g_backend_buf = NULL;
uint64_t g_backend_blocks;

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_json_decode_string, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uint64, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_json_write_name, int, (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_named_string, int, (struct spdk_json_write_ctx *w,
		const char *name, const char *val), 0);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), true);
DEFINE_STUB(spdk_json_write_bool, int, (struct spdk_json_write_ctx *w, bool val), 0);
DEFINE_STUB(spdk_json_write_named_object_begin, int, (struct spdk_json_write_ctx *w,
		const char *name), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB_V(spdk_rpc_register_method, (const char *method, spdk_rpc_method_handler func,
		uint32_t state_mask));
DEFINE_STUB_V(spdk_jsonrpc_end_result, (struct spdk_jsonrpc_request *request,
					struct spdk_json_write_ctx *w));
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc),
	    (void *)1);


static void
init_test_globals(uint64_t blocks)
{
	g_backend_blocks = blocks;
	g_backend_buf = calloc(blocks, 512);
	for (uint64_t i = 0; i < blocks; i++) {
		memset(g_backend_buf + i * 512, (uint8_t)i, 512);
	}
}


static void
free_test_globals(void)
{
	free(g_backend_buf);
	g_backend_buf = NULL;
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
	*_desc = (void *)bdev;
	return 0;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return (void *)desc;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	CU_ASSERT_PTR_NULL(spdk_bdev_get_by_name(bdev->name));
	TAILQ_INSERT_TAIL(&g_bdev_list, bdev, internal.link);

	return 0;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(bdev->name), bdev);
	TAILQ_REMOVE(&g_bdev_list, bdev, internal.link);

	bdev->fn_table->destruct(bdev->ctxt);

	if (cb_fn) {
		cb_fn(cb_arg, 0);
	}
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb((void*)bdev_io->internal.ch, bdev_io, true);
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name;
}

bool
spdk_bdev_is_zoned(const struct spdk_bdev *bdev)
{
	return bdev->zoned;
}

int
spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val)
{
	return 0;
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module *module)
{
	if (bdev->internal.claim_module != NULL) {
		return -1;
	}
	bdev->internal.claim_module = module;
	return 0;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	CU_ASSERT(bdev->internal.claim_module != NULL);
	bdev->internal.claim_module = NULL;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev;

	if (!TAILQ_EMPTY(&g_bdev_list)) {
		TAILQ_FOREACH(bdev, &g_bdev_list, internal.link) {
			if (strcmp(bdev_name, bdev->name) == 0) {
				return bdev;
			}
		}
	}

	return NULL;
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
{
	g_rpc_err = 1;
}

void
spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
				     int error_code, const char *fmt, ...)
{
	g_rpc_err = 1;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_io_comp_status = ((status == SPDK_BDEV_IO_STATUS_SUCCESS) ? true : false);
}

int
spdk_json_decode_object(const struct spdk_json_val *values,
			const struct spdk_json_object_decoder *decoders, size_t num_decoders,
			void *out)
{
	struct rpc_construct_vbdev *req, *_out;

	if (g_json_decode_obj_construct) {
		req = g_rpc_req;
		_out = out;

		_out->name = strdup(req->name);
		SPDK_CU_ASSERT_FATAL(_out->name != NULL);
		_out->base_bdev = strdup(req->base_bdev);
		SPDK_CU_ASSERT_FATAL(_out->base_bdev != NULL);
	} else {
		memcpy(out, g_rpc_req, g_rpc_req_size);
	}

	return 0;
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	return (void *)1;
}

static void
free_test_req(struct rpc_construct_vbdev *r)
{
	free(r->name);
	free(r->base_bdev);

	g_rpc_req = NULL;
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt,
			       uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *child_io;
	struct iovec *iovs;
	uint64_t block_scaling = (BLOCK_SIZE / 512);

	child_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(child_io != NULL);

	if (!iov) {
		child_io->u.bdev.iovs = &child_io->iov;
		child_io->u.bdev.iovcnt = 1;
		iovs = child_io->u.bdev.iovs;

		child_io->internal.buf = calloc(num_blocks, BLOCK_SIZE);
		SPDK_CU_ASSERT_FATAL(child_io->internal.buf != NULL);
		iovs[0].iov_base = child_io->internal.buf;
		iovs[0].iov_len = num_blocks * BLOCK_SIZE;
	} else {
		child_io->u.bdev.iovs = iov;
		child_io->u.bdev.iovcnt = iovcnt;
		iovs = iov;
	}

	if (offset_blocks * block_scaling < g_backend_blocks) {
		memcpy(iovs[0].iov_base, g_backend_buf + offset_blocks * block_scaling, BLOCK_SIZE * num_blocks);
	}

	cb(child_io, true, cb_arg);

	return 0;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	free(bdev_io->internal.buf);
	free(bdev_io);
}

static void
verify_adapter_config(struct rpc_construct_vbdev *r, bool presence)
{
	struct bdev_adapter_config *cfg = NULL;

	TAILQ_FOREACH(cfg, &g_bdev_configs, link) {
		if (strcmp(r->name, cfg->vbdev_name) == 0) {
			if (presence == false) {
				break;
			}
			CU_ASSERT(strcmp(r->base_bdev, cfg->bdev_name) == 0);
			break;
		}
	}

	if (presence) {
		CU_ASSERT(cfg != NULL);
	} else {
		CU_ASSERT(cfg == NULL);
	}
}

static void
verify_adapter_bdev(struct rpc_construct_vbdev *r, bool presence)
{
	struct bdev_adapter *bdev;
	bool   bdev_found;

	bdev_found = false;
	TAILQ_FOREACH(bdev, &g_bdev_nodes, link) {
		if (strcmp(bdev->bdev.name, r->name) == 0) {
			bdev_found = true;
			if (presence == false) {
				break;
			}

			CU_ASSERT(bdev->bdev.blockcnt == BLOCK_CNT * 4096 / 512);
			CU_ASSERT(bdev->bdev.blocklen == 512);
			CU_ASSERT(bdev->bdev.ctxt == bdev);
			CU_ASSERT(bdev->bdev.fn_table == &adapter_fn_table);
			CU_ASSERT(bdev->bdev.module == &bdev_adapter_if);
			break;
		}
	}

	if (presence == true) {
		CU_ASSERT(bdev_found == true);
	} else {
		CU_ASSERT(bdev_found == false);
	}
}

static void
verify_config_present(const char *name, bool presence)
{
	struct bdev_adapter_config *cfg;
	bool cfg_found;

	cfg_found = false;

	TAILQ_FOREACH(cfg, &g_bdev_configs, link) {
		if (cfg->vbdev_name != NULL) {
			if (strcmp(name, cfg->vbdev_name) == 0) {
				cfg_found = true;
				break;
			}
		}
	}

	if (presence == true) {
		CU_ASSERT(cfg_found == true);
	} else {
		CU_ASSERT(cfg_found == false);
	}
}

static void
verify_bdev_present(const char *name, bool presence)
{
	struct bdev_adapter *bdev;
	bool   bdev_found;

	bdev_found = false;
	TAILQ_FOREACH(bdev, &g_bdev_nodes, link) {
		if (strcmp(bdev->bdev.name, name) == 0) {
			bdev_found = true;
			break;
		}
	}
	if (presence == true) {
		CU_ASSERT(bdev_found == true);
	} else {
		CU_ASSERT(bdev_found == false);
	}
}

static struct spdk_bdev *
create_nvme_bdev(void)
{
	struct spdk_bdev *base_bdev;
	char *name = "Nvme0n1";
	base_bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(base_bdev != NULL);
	base_bdev->name = strdup(name);
	SPDK_CU_ASSERT_FATAL(base_bdev->name != NULL);
	base_bdev->blocklen = BLOCK_SIZE;
	base_bdev->blockcnt = BLOCK_CNT;
	base_bdev->write_unit_size = 1;
	TAILQ_INSERT_TAIL(&g_bdev_list, base_bdev, internal.link);

	return base_bdev;
}

static void
base_bdevs_cleanup(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *bdev_next;

	if (!TAILQ_EMPTY(&g_bdev_list)) {
		TAILQ_FOREACH_SAFE(bdev, &g_bdev_list, internal.link, bdev_next) {
			free(bdev->name);
			TAILQ_REMOVE(&g_bdev_list, bdev, internal.link);
			free(bdev);
		}
	}
}

static void
create_test_req(struct rpc_construct_vbdev *r, const char *vbdev_name, const char *base_name,
		bool create_base_bdev)
{
	r->name = strdup(vbdev_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);
	r->base_bdev = strdup(base_name);
	SPDK_CU_ASSERT_FATAL(r->base_bdev != NULL);

	if (create_base_bdev == true) {
		create_nvme_bdev();
	}
	g_rpc_req = r;
	g_rpc_req_size = sizeof(*r);
}

static void
initialize_create_req(struct rpc_construct_vbdev *r, const char *vbdev_name, const char *base_name,
		      bool create_base_bdev)
{
	create_test_req(r, vbdev_name, base_name, create_base_bdev);

	g_rpc_err = 0;
	g_json_decode_obj_construct = 1;
}

static void
create_delete_req(struct rpc_delete_vbdev *r, const char *vbdev_name)
{
	r->name = strdup(vbdev_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);

	g_rpc_req = r;
	g_rpc_req_size = sizeof(*r);
	g_rpc_err = 0;
	g_json_decode_obj_construct = 0;
}

static void
send_create_vbdev(char *vdev_name, char *name, bool create_bdev, bool success)
{
	struct rpc_construct_vbdev req;

	initialize_create_req(&req, vdev_name, name, true);
	rpc_vbdev_bs_adapter_create(NULL, NULL);
	CU_ASSERT(g_rpc_err != success);
	verify_adapter_config(&req, success);
	verify_adapter_bdev(&req, success);
	free_test_req(&req);
}

static void
send_delete_vbdev(char *name, bool success)
{
	struct rpc_delete_vbdev delete_req;

	create_delete_req(&delete_req, name);
	rpc_vbdev_bs_adapter_delete(NULL, NULL);
	verify_config_present(name, false);
	verify_bdev_present(name, false);
	CU_ASSERT(g_rpc_err != success);
	g_rpc_req = NULL;
}

static void
bdev_io_initialize(struct spdk_bdev_io *bdev_io, struct spdk_io_channel *ch, struct spdk_bdev *bdev,
		   uint64_t lba, uint64_t blocks, int16_t iotype)
{
	bdev_io->bdev = bdev;
	SPDK_CU_ASSERT_FATAL(bdev_io->bdev != NULL);
	bdev_io->u.bdev.offset_blocks = lba;
	bdev_io->u.bdev.num_blocks = blocks;
	bdev_io->type = iotype;
	bdev_io->internal.ch = (void*)ch;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_UNMAP || bdev_io->type == SPDK_BDEV_IO_TYPE_FLUSH) {
		return;
	}

	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.iovs = calloc(1, sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iovs != NULL);
	bdev_io->u.bdev.iovs->iov_base = calloc(1, bdev_io->u.bdev.num_blocks * 512);
	SPDK_CU_ASSERT_FATAL(bdev_io->u.bdev.iovs->iov_base != NULL);
	bdev_io->u.bdev.iovs->iov_len = bdev_io->u.bdev.num_blocks * 512;
}

static void
bdev_io_cleanup(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io->u.bdev.iovs) {
		free(bdev_io->u.bdev.iovs->iov_base);
		free(bdev_io->u.bdev.iovs);
	}
	free(bdev_io);
}

static void
send_read(struct bdev_adapter *bdev, struct spdk_io_channel *ch, uint64_t lba,
	       uint64_t blocks, bool success)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct adapter_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io_initialize(bdev_io, ch, &bdev->bdev, lba, blocks, SPDK_BDEV_IO_TYPE_READ);

	g_io_comp_status = !success;
	adapter_submit_request(ch, bdev_io);

	CU_ASSERT(g_io_comp_status == success);
	if (success) {
		for (uint64_t i = 0; i < blocks; i++) {
			CU_ASSERT(((uint8_t*)bdev_io->u.bdev.iovs[0].iov_base)[512 * i] == (uint8_t)(lba + i));
		}
	}
	bdev_io_cleanup(bdev_io);
}

static void
test_adapter_create(void)
{
	struct spdk_bdev *bdev;
	struct rpc_construct_vbdev req;
	char *name = "Nvme0n1";

	CU_ASSERT(adapter_init() == 0);

	/* Create adapter virtual device before nvme device */
	verify_config_present("adapter_dev1", false);
	verify_bdev_present("adapter_dev1", false);
	initialize_create_req(&req, "adapter_dev1", name, false);
	rpc_vbdev_bs_adapter_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_adapter_config(&req, true);
	verify_adapter_bdev(&req, false);
	bdev = create_nvme_bdev();
	adapter_examine(bdev);
	verify_adapter_bdev(&req, true);
	free_test_req(&req);

	/* Delete bdev */
	send_delete_vbdev("adapter_dev1", true);

	/* Create adapter virtual device and verify its correctness */
	verify_config_present("adapter_dev1", false);
	send_create_vbdev("adapter_dev1", name, false, true);
	send_delete_vbdev("adapter_dev1", true);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	adapter_finish();
	base_bdevs_cleanup();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_list));
}

static void
test_adapter_create_invalid(void)
{
	char *name = "Nvme0n1";

	CU_ASSERT(adapter_init() == 0);

	/* Create adapter virtual device and verify its correctness */
	verify_config_present("adapter_dev1", false);
	verify_bdev_present("adapter_dev1", false);
	send_create_vbdev("adapter_dev1", name, true, true);

	/* Try to create another adapter virtual device on the same bdev */
	send_create_vbdev("adapter_dev2", name, false, false);

	/* Try to create adapter virtual device on top of 512B sector size bdev */
	send_create_vbdev("adapter_dev2", "adapter_dev1", false, false);

	/* Unclaim the base bdev */
	send_delete_vbdev("adapter_dev1", true);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

	adapter_finish();
	base_bdevs_cleanup();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_list));
}

static struct bdev_adapter *
create_and_get_vbdev(char *vdev_name, char *name, bool create_bdev)
{
	struct bdev_adapter *bdev = NULL;
	send_create_vbdev(vdev_name, name, true, true);

	TAILQ_FOREACH(bdev, &g_bdev_nodes, link) {
		if (strcmp(bdev->bdev.name, vdev_name) == 0) {
			break;
		}
	}

	return bdev;
}

static void
test_adapter_read(void)
{
	struct spdk_io_channel *ch;
	struct bdev_adapter *bdev;
	char *name = "Nvme0n1";
	uint64_t blocks = 256;
	uint64_t lba, len;

	init_test_globals(blocks);
	CU_ASSERT(adapter_init() == 0);

	/* Create adapter dev */
	bdev = create_and_get_vbdev("adapter_dev1", name, true);
	CU_ASSERT(bdev != NULL);

	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct adapter_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* Send an aligned 4k request */
	lba = 0;
	len = 8;
	send_read(bdev, ch, lba, len, true);

	/* Send an unaligned 4k request */
	lba = 1;
	len = 8;
	send_read(bdev, ch, lba, len, true);

	/* Send a 512B request */
	lba = 2;
	len = 1;
	send_read(bdev, ch, lba, len, true);

	/* Send a 1024B request spanning two 4k blocks */
	lba = 7;
	len = 2;
	send_read(bdev, ch, lba, len, true);

	/* Send a 4608B request spanning two 4k blocks with aligned start lba */
	lba = 0;
	len = 9;
	send_read(bdev, ch, lba, len, true);

	/* Send a 4608B request spanning two 4k blocks with aligned end lba */
	lba = 7;
	len = 9;
	send_read(bdev, ch, lba, len, true);

	/* Send a 5120B request spanning with misaligned start lba and end lba */
	lba = 7;
	len = 10;
	send_read(bdev, ch, lba, len, true);

	/* Send request out of device range */
	lba = bdev->bdev.blockcnt;
	len = 1;
	send_read(bdev, ch, lba, len, false);

	lba = UINT64_MAX;
	len = 10;
	send_read(bdev, ch, lba, len, false);

	lba = bdev->bdev.blockcnt - 1;
	len = 2;
	send_read(bdev, ch, lba, len, false);

	/* Delete adapter dev */
	send_delete_vbdev("adapter_dev1", true);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	free(ch);

	adapter_finish();
	base_bdevs_cleanup();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_list));
	free_test_globals();
}

int main(int argc, char **argv)
{
	CU_pSuite       suite = NULL;
	unsigned int    num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("adapter", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_adapter_create", test_adapter_create) == NULL ||
		CU_add_test(suite, "test_adapter_create_invalid", test_adapter_create_invalid) == NULL ||
		CU_add_test(suite, "test_adapter_read", test_adapter_read) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	g_thread = spdk_thread_create("test", NULL);
	spdk_set_thread(g_thread);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();

	spdk_thread_exit(g_thread);
	spdk_thread_destroy(g_thread);

	CU_cleanup_registry();

	return num_failures;
}
