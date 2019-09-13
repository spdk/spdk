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
#include "bdev/zone_block/vbdev_zone_block.c"
#include "bdev/zone_block/vbdev_zone_block_rpc.c"

#define BLOCK_CNT (1024ul * 1024ul * 1024ul * 1024ul)
#define BLOCK_SIZE 4096

/* Globals */
struct io_output *g_io_output = NULL;
uint32_t g_max_io_size;
uint32_t g_io_output_index;
uint32_t g_io_comp_status;
uint8_t g_rpc_err;
uint8_t g_json_decode_obj_construct;
static TAILQ_HEAD(, spdk_bdev) g_bdev_list = TAILQ_HEAD_INITIALIZER(g_bdev_list);
void *g_rpc_req = NULL;
uint32_t g_rpc_req_size = 0;
static struct spdk_thread *g_thread;

struct io_output {
	struct spdk_bdev_desc       *desc;
	struct spdk_io_channel      *ch;
	uint64_t                    offset_blocks;
	uint64_t                    num_blocks;
	spdk_bdev_io_completion_cb  cb;
	void                        *cb_arg;
	enum spdk_bdev_io_type      iotype;
};

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
init_test_globals(void)
{
	g_io_output = calloc(g_max_io_size, sizeof(struct io_output));
	SPDK_CU_ASSERT_FATAL(g_io_output != NULL);
	g_io_output_index = 0;
}

static void
free_test_globals(void)
{
	if (g_io_output) {
		free(g_io_output);
		g_io_output = NULL;
	}
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
	*_desc = (void *)0x1;
	return 0;
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

int spdk_json_write_named_uint64(struct spdk_json_write_ctx *w, const char *name, uint64_t val)
{
	struct rpc_construct_vbdev *req = g_rpc_req;
	if (strcmp(name, "zone_size") == 0) {
		CU_ASSERT(req->zone_size == val);
	} else if (strcmp(name, "optimal_open_zones") == 0) {
		CU_ASSERT(req->optimal_open_zones == val);
	}

	return 0;
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
		_out->bdev_name = strdup(req->bdev_name);
		SPDK_CU_ASSERT_FATAL(_out->bdev_name != NULL);
		_out->zone_size = req->zone_size;
		_out->optimal_open_zones = req->optimal_open_zones;
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

static void
verify_config_present(const char *name, bool presence)
{
	struct bdev_zone_block_config *cfg;
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
	struct bdev_zone_block *bdev;
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

static void
create_test_req(struct rpc_construct_vbdev *r, const char *vbdev_name, const char *base_name,
		uint64_t zone_size, uint64_t optimal_open_zones, bool create_base_bdev)
{
	r->name = strdup(vbdev_name);
	SPDK_CU_ASSERT_FATAL(r->name != NULL);
	r->bdev_name = strdup(base_name);
	SPDK_CU_ASSERT_FATAL(r->bdev_name != NULL);
	r->zone_size = zone_size;
	r->optimal_open_zones = optimal_open_zones;

	if (create_base_bdev == true) {
		create_nvme_bdev();
	}
	g_rpc_req = r;
	g_rpc_req_size = sizeof(*r);
}

static void
free_test_req(struct rpc_construct_vbdev *r)
{
	free(r->name);
	free(r->bdev_name);

	g_rpc_req = NULL;
}

static void
initialize_create_req(struct rpc_construct_vbdev *r, const char *vbdev_name, const char *base_name,
		      uint64_t zone_size, uint64_t optimal_open_zones, bool create_base_bdev)
{
	create_test_req(r, vbdev_name, base_name, zone_size, optimal_open_zones, create_base_bdev);

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
verify_zone_config(struct rpc_construct_vbdev *r, bool presence)
{
	struct bdev_zone_block_config *cfg = NULL;

	TAILQ_FOREACH(cfg, &g_bdev_configs, link) {
		if (strcmp(r->name, cfg->vbdev_name) == 0) {
			if (presence == false) {
				break;
			}
			CU_ASSERT(strcmp(r->bdev_name, cfg->bdev_name) == 0);
			CU_ASSERT(r->zone_size == cfg->zone_size);
			CU_ASSERT(spdk_max(r->optimal_open_zones, 1) == cfg->optimal_open_zones);
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
verify_zone_bdev(struct rpc_construct_vbdev *r, bool presence)
{
	struct block_zone *zone;
	struct bdev_zone_block *bdev;
	bool   bdev_found;
	uint32_t i;
	uint64_t expected_num_zones;
	uint64_t expected_optimal_open_zones;

	bdev_found = false;
	TAILQ_FOREACH(bdev, &g_bdev_nodes, link) {
		if (strcmp(bdev->bdev.name, r->name) == 0) {
			bdev_found = true;
			if (presence == false) {
				break;
			}

			expected_optimal_open_zones = spdk_max(r->optimal_open_zones, 1);
			expected_num_zones = BLOCK_CNT / r->zone_size / expected_optimal_open_zones;
			expected_num_zones *= expected_optimal_open_zones;

			CU_ASSERT(bdev->num_zones == expected_num_zones);
			CU_ASSERT(bdev->bdev.zoned == true);
			CU_ASSERT(bdev->bdev.blockcnt == expected_num_zones * r->zone_size);
			CU_ASSERT(bdev->bdev.blocklen == BLOCK_SIZE);
			CU_ASSERT(bdev->bdev.ctxt == bdev);
			CU_ASSERT(bdev->bdev.fn_table == &zone_block_fn_table);
			CU_ASSERT(bdev->bdev.module == &bdev_zoned_if);
			CU_ASSERT(bdev->bdev.write_unit_size == 1);
			CU_ASSERT(bdev->bdev.zone_size == r->zone_size);
			CU_ASSERT(bdev->bdev.optimal_open_zones == expected_optimal_open_zones);
			CU_ASSERT(bdev->bdev.max_open_zones == 0);

			for (i = 0; i < bdev->num_zones; i++) {
				zone = &bdev->zones[i];
				CU_ASSERT(zone->zone_info.state == SPDK_BDEV_ZONE_STATE_FULL);
				CU_ASSERT(zone->zone_info.capacity == r->zone_size);
			}
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
send_create_vbdev(char *vdev_name, char *name, uint64_t zone_size, uint64_t optimal_open_zones,
		  bool create_bdev, bool success)
{
	struct rpc_construct_vbdev req;

	initialize_create_req(&req, vdev_name, name, zone_size, optimal_open_zones, true);
	rpc_vbdev_block_create(NULL, NULL);
	CU_ASSERT(g_rpc_err != success);
	verify_zone_config(&req, success);
	verify_zone_bdev(&req, success);
	free_test_req(&req);
}

static void
send_delete_vbdev(char *name, bool success)
{
	struct rpc_delete_vbdev delete_req;

	create_delete_req(&delete_req, name);
	rpc_vbdev_block_delete(NULL, NULL);
	verify_config_present(name, false);
	verify_bdev_present(name, false);
	CU_ASSERT(g_rpc_err != success);
	g_rpc_req = NULL;
}

static void
test_zone_block_create(void)
{
	struct spdk_bdev *bdev;
	struct rpc_construct_vbdev req;
	char *name = "Nvme0n1";
	size_t num_zones = 20;
	size_t zone_size = BLOCK_CNT / num_zones;

	CU_ASSERT(zone_block_init() == 0);

	/* Create zoned virtual device before nvme device */
	verify_config_present("zone_dev1", false);
	verify_bdev_present("zone_dev1", false);
	initialize_create_req(&req, "zone_dev1", name, zone_size, 1, false);
	rpc_vbdev_block_create(NULL, NULL);
	CU_ASSERT(g_rpc_err == 0);
	verify_zone_config(&req, true);
	verify_zone_bdev(&req, false);
	bdev = create_nvme_bdev();
	zone_block_examine(bdev);
	verify_zone_bdev(&req, true);
	free_test_req(&req);

	/* Delete bdev */
	send_delete_vbdev("zone_dev1", true);

	/* Create zoned virtual device and verify its correctness */
	verify_config_present("zone_dev1", false);
	send_create_vbdev("zone_dev1", name, zone_size, 1, false, true);
	send_delete_vbdev("zone_dev1", true);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	zone_block_finish();
	base_bdevs_cleanup();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_list));
	free_test_globals();
}

static void
test_zone_block_create_invalid(void)
{
	char *name = "Nvme0n1";
	size_t num_zones = 10;
	size_t zone_size = BLOCK_CNT / num_zones;

	CU_ASSERT(zone_block_init() == 0);

	/* Create zoned virtual device and verify its correctness */
	verify_config_present("zone_dev1", false);
	verify_bdev_present("zone_dev1", false);
	send_create_vbdev("zone_dev1", name, zone_size, 1, true, true);

	/* Try to create another zoned virtual device on the same bdev */
	send_create_vbdev("zone_dev2", name, zone_size, 1, false, false);

	/* Try to create zoned virtual device on the zoned bdev */
	send_create_vbdev("zone_dev2", "zone_dev1", zone_size, 1, false, false);

	/* Unclaim the base bdev */
	send_delete_vbdev("zone_dev1", true);

	/* Try to create zoned virtual device with 0 zone size */
	send_create_vbdev("zone_dev1", name, 0, 1, false, false);

	/* Try to create zoned virtual device with 0 optimal number of zones */
	send_create_vbdev("zone_dev1", name, zone_size, 0, false, true);
	send_delete_vbdev("zone_dev1", true);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

	zone_block_finish();
	base_bdevs_cleanup();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_list));
	free_test_globals();
}

static void
bdev_io_zone_info_initialize(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev,
			     uint64_t zone_id, uint32_t num_zones)
{
	bdev_io->bdev = bdev;
	bdev_io->type = SPDK_BDEV_IO_TYPE_GET_ZONE_INFO;

	bdev_io->u.zone_mgmt.zone_id = zone_id;

	bdev_io->u.zone_mgmt.num_zones = num_zones;
	if (num_zones) {
		bdev_io->u.zone_mgmt.buf = calloc(num_zones, sizeof(struct spdk_bdev_zone_info));
	}
}

static void
bdev_io_zone_cleanup(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io->u.zone_mgmt.buf) {
		free(bdev_io->u.zone_mgmt.buf);
	}
	free(bdev_io);
}

static struct bdev_zone_block *
create_and_get_vbdev(char *vdev_name, char *name, uint64_t num_zones, uint64_t optimal_open_zones,
		     bool create_bdev)
{
	size_t zone_size = BLOCK_CNT / num_zones;
	struct bdev_zone_block *bdev = NULL;
	send_create_vbdev(vdev_name, name, zone_size, optimal_open_zones, vdev_name, true);

	TAILQ_FOREACH(bdev, &g_bdev_nodes, link) {
		if (strcmp(bdev->bdev.name, vdev_name) == 0) {
			break;
		}
	}

	return bdev;
}

static void
test_supported_io_types(void)
{
	struct bdev_zone_block *bdev;
	char *name = "Nvme0n1";
	uint32_t num_zones = 10;

	init_test_globals();
	CU_ASSERT(zone_block_init() == 0);

	/* Create zone dev */
	bdev = create_and_get_vbdev("zone_dev1", name, num_zones, 1, true);
	CU_ASSERT(bdev != NULL);

	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT) == true);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZONE_APPEND) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE) == false);

	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO_MD) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == false);
	CU_ASSERT(zone_block_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY) == false);

	send_delete_vbdev("zone_dev1", true);
	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

	zone_block_finish();
	base_bdevs_cleanup();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_bdev_list));
	free_test_globals();
}

static void
send_zone_info(struct bdev_zone_block *bdev, struct spdk_io_channel *ch, uint64_t zone_id,
	       uint64_t wp,
	       enum spdk_bdev_zone_state state, uint32_t output_index, bool success)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_zone_info *info;

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io_zone_info_initialize(bdev_io, &bdev->bdev, zone_id, 1);
	memset(g_io_output, 0, (g_max_io_size * sizeof(struct io_output)));
	g_io_output_index = output_index;

	g_io_comp_status = !success;
	zone_block_submit_request(ch, bdev_io);
	CU_ASSERT(g_io_comp_status == success);

	if (success) {
		info = (struct spdk_bdev_zone_info *)bdev_io->u.zone_mgmt.buf;
		CU_ASSERT(info->zone_id == zone_id);
		CU_ASSERT(info->capacity == bdev->bdev.zone_size);
		CU_ASSERT(info->write_pointer == wp);
		CU_ASSERT(info->state == state);
	}

	bdev_io_zone_cleanup(bdev_io);
}

static void
test_get_zone_info(void)
{
	struct spdk_io_channel *ch;
	struct bdev_zone_block *bdev;
	struct spdk_bdev_io *bdev_io;
	char *name = "Nvme0n1";
	uint32_t num_zones = 10, i;
	struct spdk_bdev_zone_info *info;

	init_test_globals();
	CU_ASSERT(zone_block_init() == 0);

	/* Create zone dev */
	bdev = create_and_get_vbdev("zone_dev1", name, num_zones, 1, true);
	CU_ASSERT(bdev != NULL);

	ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct zone_block_io_channel));
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* Get info about each zone */
	for (i = 0; i < num_zones; i++) {
		send_zone_info(bdev, ch, i * bdev->bdev.zone_size,
			       i * bdev->bdev.zone_size + bdev->bdev.zone_size, SPDK_BDEV_ZONE_STATE_FULL, 0, true);
	}

	/* Send info asking for 0 zones */
	bdev_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io_zone_info_initialize(bdev_io, &bdev->bdev, 0, 0);
	memset(g_io_output, 0, (g_max_io_size * sizeof(struct io_output)));
	g_io_output_index = 0;
	zone_block_submit_request(ch, bdev_io);
	CU_ASSERT(g_io_comp_status);
	bdev_io_zone_cleanup(bdev_io);

	/* Send info asking for all zones */
	bdev_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io_zone_info_initialize(bdev_io, &bdev->bdev, 0, num_zones);
	memset(g_io_output, 0, (g_max_io_size * sizeof(struct io_output)));
	g_io_output_index = 0;
	zone_block_submit_request(ch, bdev_io);
	CU_ASSERT(g_io_comp_status);

	for (i = 0; i < num_zones; i++) {
		info = &(((struct spdk_bdev_zone_info *)bdev_io->u.zone_mgmt.buf)[i]);
		CU_ASSERT(info->zone_id == i * bdev->bdev.zone_size);
		CU_ASSERT(info->capacity == bdev->bdev.zone_size);
		CU_ASSERT(info->write_pointer == i * bdev->bdev.zone_size + bdev->bdev.zone_size);
		CU_ASSERT(info->state == SPDK_BDEV_ZONE_STATE_FULL);
	}
	bdev_io_zone_cleanup(bdev_io);

	/* Send info asking for too many zones */
	bdev_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io_zone_info_initialize(bdev_io, &bdev->bdev, 0, num_zones + 1);
	memset(g_io_output, 0, (g_max_io_size * sizeof(struct io_output)));
	g_io_output_index = 0;
	zone_block_submit_request(ch, bdev_io);
	CU_ASSERT(!g_io_comp_status);
	bdev_io_zone_cleanup(bdev_io);

	/* Send info with misaligned start LBA */
	send_zone_info(bdev, ch, 1, 0, SPDK_BDEV_ZONE_STATE_FULL, 0, false);

	/* Send info with too high LBA */
	send_zone_info(bdev, ch, num_zones * bdev->bdev.zone_size, 0, SPDK_BDEV_ZONE_STATE_FULL, 0,
		       false);

	/* Delete zone dev */
	send_delete_vbdev("zone_dev1", true);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	free(ch);

	zone_block_finish();
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

	suite = CU_add_suite("zone_block", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_zone_block_create", test_zone_block_create) == NULL ||
		CU_add_test(suite, "test_zone_block_create_invalid", test_zone_block_create_invalid) == NULL ||
		CU_add_test(suite, "test_get_zone_info", test_get_zone_info) == NULL ||
		CU_add_test(suite, "test_supported_io_types", test_supported_io_types) == NULL
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
