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
#include "spdk/util.h"

#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk_internal/log.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/io_channel.h"

#include "spdk/env.h"
#include "spdk/bdev.h"

#define LVOL_DEFAULT_MEM_SIZE 1024
#define LVOL_MASTER_CORE 0


struct spdk_app_opts opts = {};
static char app_poller_core_mask[64] = "0xf";
static uint32_t app_pollers_core = 1;

static struct spdk_bdev *g_bdev;
static struct spdk_bdev_desc *g_bdev_desc;
static struct spdk_io_channel *g_io_channel;

static struct app_bdev_op *cur_req;

struct app_bdev_op {
	char *bdev_name;
	uint32_t num_blocks;

	/* -- END OF RPC PART --- */
	unsigned char *buf;
	size_t size;

	void (*exec_fn)(struct app_bdev_op *);
	void (*free_fn)(struct app_bdev_op *);
};

static void
app_bdev_req_free(struct app_bdev_op *req)
{
	free(req->bdev_name);
	req->bdev_name = NULL;

	spdk_dma_free(req->buf);
	req->buf = NULL;
	req->size = 0;

	free(req);
	cur_req = NULL;
	SPDK_ERRLOG("REQ: free done\n");
}

static void
app_bdev_removed_cb(void *arg)
{
	SPDK_ERRLOG("Hot-remove?\n");
	abort();
}

static void
app_bdev_io_cleanup(void *arg1, void *arg2)
{
	struct app_bdev_op *req = arg1;

	SPDK_ERRLOG("REQ: done\n");
	req->free_fn(req);
}

static void
app_bdev_do_open_cb(struct app_bdev_op *req)
{
	if (g_bdev_desc) {
		SPDK_ERRLOG("REQ: One BDEV already opened. Close it before opening next one\n");
		goto out;
	}

	g_bdev = spdk_bdev_get_by_name(req->bdev_name);
	if (spdk_bdev_open(g_bdev, true, app_bdev_removed_cb, NULL, &g_bdev_desc)) {
		SPDK_ERRLOG("REQ: spdk_bdev_open failed\n");
		req->free_fn(req);
		return;
	}

	g_io_channel = spdk_bdev_get_io_channel(g_bdev_desc);
	if (!g_io_channel) {
		SPDK_ERRLOG("REQ: spdk_bdev_get_io_channel failed\n");
		req->free_fn(req);
		return;
	}

	SPDK_ERRLOG("REQ: %s bdev opened.\n", req->bdev_name);
out:
	req->free_fn(req);
}

static void
app_bdev_do_close_cb(struct app_bdev_op *req)
{
	if (!g_bdev_desc) {
		SPDK_ERRLOG("REQ: No BDEV opened. Open it before closing next one\n");
		goto out;
	}

	assert(g_io_channel);
	assert(g_bdev_desc);
	assert(g_bdev);

	spdk_put_io_channel(g_io_channel);
	spdk_bdev_close(g_bdev_desc);

	g_io_channel = NULL;
	g_bdev_desc = NULL;
	g_bdev = NULL;

	SPDK_ERRLOG("REQ: closed.\n");
out:
	req->free_fn(req);
}

static void
app_bdev_write_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct app_bdev_op *req = cb_arg;

	SPDK_ERRLOG("REQ: WRITE %s\n", success ? "SUCCESS" : "FAILED");

	spdk_bdev_free_io(bdev_io);
	spdk_event_call(spdk_event_allocate(spdk_env_get_current_core(), app_bdev_io_cleanup, req, NULL));
}

static void
app_bdev_execute_write_op_cb(struct app_bdev_op *req)
{
	if (!g_bdev_desc) {
		SPDK_ERRLOG("No BDEV opened. Open it before issuing IO.\n");
	}

	req->size = req->num_blocks * spdk_bdev_get_block_size(g_bdev);
	req->buf = spdk_dma_zmalloc(req->size, 64, NULL);
	memset(req->buf, 0xDE, req->size);

	if (spdk_bdev_write(g_bdev, g_io_channel, req->buf, 0, req->size, app_bdev_write_complete_cb, req)) {
		SPDK_ERRLOG("REQ: Write failed\n");
		req->free_fn(req);
	} else {
		SPDK_ERRLOG("REQ: ISSUED\n");
	}
}

static void
app_bdev_read_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct app_bdev_op *req = cb_arg;
	size_t i;

	SPDK_ERRLOG("REQ: READ %s\n", success ? "SUCCESS" : "FAILED");

	for (i = 0; i < req->size; i++) {
		if (req->buf[i] == 0xDE) {
			continue;
		}

		SPDK_ERRLOG("REQ: read buffer at pos buf[%zu]=%hhu not equal 0xDE. Not checking further.\n", i, req->buf[i]);
		break;
	}

	spdk_bdev_free_io(bdev_io);
	spdk_event_call(spdk_event_allocate(spdk_env_get_current_core(), app_bdev_io_cleanup, req, NULL));
}

static void
app_bdev_execute_read_op_cb(struct app_bdev_op *req)
{
	if (!g_bdev_desc) {
		SPDK_ERRLOG("No BDEV opened. Open it before issuing IO.\n");
	}

	req->size = req->num_blocks * spdk_bdev_get_block_size(g_bdev);
	req->buf = spdk_dma_zmalloc(req->size, 64, NULL);

	if (spdk_bdev_read(g_bdev, g_io_channel, req->buf, 0, req->size, app_bdev_read_complete_cb, req)) {
		SPDK_ERRLOG("REQ: Write failed\n");
		req->free_fn(req);
	} else {
		SPDK_ERRLOG("REQ: ISSUED\n");
	}
}

static void
app_poller_fn(void *arg)
{
	if (cur_req) {
		cur_req->exec_fn(cur_req);
		cur_req = NULL;
	}
}

struct spdk_poller *app_poller_obj;

static void
app_lvol_startup_cb(void *arg1, void *arg2)
{
	SPDK_ERRLOG("Started\n");
	spdk_poller_register(&app_poller_obj, app_poller_fn, NULL, app_pollers_core, 0);
}

static void
app_lvol_shutdown_cb(void)
{
	if (app_poller_obj) {
		spdk_poller_unregister(&app_poller_obj, NULL);
	}

	spdk_app_stop(0);
	SPDK_ERRLOG("Shutdown\n");
}

static void
lvol_app_opts_init(struct spdk_app_opts *opts)
{
	spdk_app_opts_init(opts);
	opts->name = "lvol";
	opts->mem_size = LVOL_DEFAULT_MEM_SIZE;
	opts->master_core = LVOL_MASTER_CORE;
	opts->reactor_mask = app_poller_core_mask;
}

struct spdk_jsonrpc_request;

static const struct spdk_json_object_decoder rpc_bdev_open_close_decoders[] = {
	{"bdev", offsetof(struct app_bdev_op, bdev_name), spdk_json_decode_string},
};

static void
rpc_app_bdev_open_close(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params, bool open)
{
	struct app_bdev_op *req;
	struct spdk_json_write_ctx *w;

	if (cur_req) {
		goto busy;
	}

	req = calloc(1, sizeof(*req));
	req->exec_fn = open ? app_bdev_do_open_cb : app_bdev_do_close_cb;
	req->free_fn = app_bdev_req_free;

	if (spdk_json_decode_object(params, rpc_bdev_open_close_decoders,
				    SPDK_COUNTOF(rpc_bdev_open_close_decoders),
				    req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	cur_req = req;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, "OK - sheduled");
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	if (req) {
		req->free_fn(req);
	}

	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	return;
busy:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Request already in progress");
}

static void
rpc_app_bdev_open(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	rpc_app_bdev_open_close(request, params, true);
}

static void
rpc_app_bdev_close(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	rpc_app_bdev_open_close(request, params, false);
}

static const struct spdk_json_object_decoder rpc_bdev_op_decoders[] = {
	{"num_blocks", offsetof(struct app_bdev_op, num_blocks), spdk_json_decode_uint32},
};

static void
rpc_app_bdev_op(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params, bool read)
{
	struct app_bdev_op *req;
	struct spdk_json_write_ctx *w;

	if (cur_req) {
		goto busy;
	}

	req = calloc(1, sizeof(*req));
	req->exec_fn = read ? app_bdev_execute_read_op_cb : app_bdev_execute_write_op_cb;
	req->free_fn = app_bdev_req_free;

	if (spdk_json_decode_object(params, rpc_bdev_op_decoders,
				    SPDK_COUNTOF(rpc_bdev_op_decoders),
				    req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	cur_req = req;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, "OK - sheduled");
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	if (req) {
		req->free_fn(req);
	}

	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	return;
busy:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Request already in progress");
}

static void
rpc_app_bdev_read(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	rpc_app_bdev_op(request, params, true);
}

static void
rpc_app_bdev_write(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	rpc_app_bdev_op(request, params, false);
}

static void
usage(char *executable_name)
{
	struct spdk_app_opts defaults;

	lvol_app_opts_init(&defaults);

	printf("%s [options]\n", executable_name);
	printf("options:\n");
	printf(" -c config  config file (default: %s)\n", defaults.config_file);
	printf(" -e mask    tracepoint group mask for spdk trace buffers (default: 0x0)\n");
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -s size    memory size in MB for DPDK (default: %dMB)\n", defaults.mem_size);
	spdk_tracelog_usage(stdout, "-t");
	printf(" -h         show this usage\n");
	printf(" -d         disable coredump file enabling\n");
	printf(" -q         disable notice level logging to stderr\n");
}

int
main(int argc, char *argv[])
{
	char ch;
	int rc;
	enum spdk_log_level print_level = SPDK_LOG_NOTICE;

	lvol_app_opts_init(&opts);

	while ((ch = getopt(argc, argv, "c:de:m:p:qs:S:t:h")) != -1) {
		switch (ch) {
		case 'c':
			opts.config_file = optarg;
			break;
		case 'd':
			opts.enable_coredump = false;
			break;
		case 'e':
			opts.tpoint_group_mask = optarg;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'q':
			print_level = SPDK_LOG_WARN;
			break;
		case 's':
			opts.mem_size = strtoul(optarg, NULL, 10);
			break;
			break;
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -t flag.\n",
				argv[0]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
#endif
			break;
		default:
			fprintf(stderr, "%s Unknown option '-%c'.\n", argv[0], ch);
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (print_level > SPDK_LOG_WARN &&
	    isatty(STDERR_FILENO) &&
	    !strncmp(ttyname(STDERR_FILENO), "/dev/tty", strlen("/dev/tty"))) {
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using -q to disable logging to stderr and monitor syslog, or\n");
		printf("redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	spdk_log_set_print_level(print_level);

	opts.shutdown_cb = app_lvol_shutdown_cb;

	spdk_rpc_register_method("bdev_open", rpc_app_bdev_open);
	spdk_rpc_register_method("bdev_close", rpc_app_bdev_close);
	spdk_rpc_register_method("bdev_read", rpc_app_bdev_read);
	spdk_rpc_register_method("bdev_write", rpc_app_bdev_write);


	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, app_lvol_startup_cb, NULL, NULL);

	spdk_app_fini();

	return rc;
}
