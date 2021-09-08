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
#include "spdk/jsonrpc.h"
#include "spdk_internal/mock.h"
#include "common/lib/test_env.c"
#include "spdk/log.h"

#include "rpc/rpc.c"

static int g_rpc_err;
void fn_rpc_method_handler(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params);

DEFINE_STUB_V(spdk_jsonrpc_end_result, (struct spdk_jsonrpc_request *request,
					struct spdk_json_write_ctx *w));
DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_string, int, (struct spdk_json_write_ctx *w, const char *val), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_named_string_fmt, int, (struct spdk_json_write_ctx *w, const char *name,
		const char *fmt, ...), 0);
DEFINE_STUB(spdk_json_write_named_object_begin, int, (struct spdk_json_write_ctx *w,
		const char *name), 0);
DEFINE_STUB(spdk_json_write_named_uint32, int, (struct spdk_json_write_ctx *w, const char *name,
		uint32_t val), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_jsonrpc_begin_result, struct spdk_json_write_ctx *,
	    (struct spdk_jsonrpc_request *request), (void *)1);
DEFINE_STUB(spdk_json_decode_bool, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_jsonrpc_server_listen, struct spdk_jsonrpc_server *, (int domain, int protocol,
		struct sockaddr *listen_addr, socklen_t addrlen, spdk_jsonrpc_handle_request_fn handle_request),
	    (struct spdk_jsonrpc_server *)0Xdeaddead);
DEFINE_STUB(spdk_jsonrpc_server_poll, int, (struct spdk_jsonrpc_server *server), 0);
DEFINE_STUB_V(spdk_jsonrpc_server_shutdown, (struct spdk_jsonrpc_server *server));

DECLARE_WRAPPER(open, int, (const char *pathname, int flags, mode_t mode));
DECLARE_WRAPPER(close, int, (int fd));
DECLARE_WRAPPER(flock, int, (int fd, int operation));
DEFINE_WRAPPER(open, int, (const char *pathname, int flags, mode_t mode), (pathname, flags, mode));
DEFINE_WRAPPER(close, int, (int fd), (fd));
DEFINE_WRAPPER(flock, int, (int fd, int operation), (fd, operation));

int spdk_json_decode_object(const struct spdk_json_val *values,
			    const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out)
{
	if (values ->type == SPDK_JSON_VAL_INVALID) {
		return 1;
	}
	return 0;
}

bool
spdk_json_strequal(const struct spdk_json_val *val, const char *str)
{
	size_t len;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NAME) {
		return false;
	}

	len = strlen(str);
	if (val->len != len) {
		return false;
	}

	return memcmp(val->start, str, len) == 0;
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
{
	g_rpc_err = error_code;
}

void
spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
				     int error_code, const char *fmt, ...)
{
	g_rpc_err = error_code;
}

void fn_rpc_method_handler(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	g_rpc_err = 0;
}

static void
test_jsonrpc_handler(void)
{
	struct spdk_jsonrpc_request *request = (struct spdk_jsonrpc_request *)0xdeadbeef;
	struct spdk_json_val method = {};
	struct spdk_json_val params = {};
	char *str = "test";
	struct spdk_rpc_method m = {
		.name = "test",
	};

	struct spdk_rpc_method is_alias_of = {
		.name = "aliastest",
		.is_deprecated = false,
		.deprecation_warning_printed = false,
		.func = fn_rpc_method_handler,
		.state_mask = SPDK_RPC_STARTUP,
	};

	/* Case 1: Method not found */
	method.type = SPDK_JSON_VAL_INVALID;
	jsonrpc_handler(request, &method, &params);
	CU_ASSERT(g_rpc_err == SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND);

	/* Case 2:  Method is alias */
	method.type = SPDK_JSON_VAL_STRING;
	method.start = str;
	method.len = 4;
	m.is_alias_of = &is_alias_of;
	m.is_deprecated = true;
	m.deprecation_warning_printed = false;
	m.state_mask = SPDK_RPC_STARTUP;
	SLIST_INSERT_HEAD(&g_rpc_methods, &m, slist);

	/* m->state_mask & g_rpc_state == g_rpc_state */
	g_rpc_err = -1;
	g_rpc_state = SPDK_RPC_STARTUP;
	jsonrpc_handler(request, &method, &params);
	CU_ASSERT(g_rpc_err == 0);

	/* g_rpc_state == SPDK_RPC_STARTUP */
	is_alias_of.state_mask = SPDK_RPC_RUNTIME;
	g_rpc_err = -1;
	g_rpc_state = SPDK_RPC_STARTUP;
	jsonrpc_handler(request, &method, &params);
	CU_ASSERT(g_rpc_err == SPDK_JSONRPC_ERROR_INVALID_STATE);

	/* SPDK_RPC_RUNTIME is invalid for the aliastest RPC */
	is_alias_of.state_mask = SPDK_RPC_STARTUP;
	g_rpc_err = -1;
	g_rpc_state = SPDK_RPC_RUNTIME;
	jsonrpc_handler(request, &method, &params);
	CU_ASSERT(g_rpc_err == SPDK_JSONRPC_ERROR_INVALID_STATE);

	SLIST_REMOVE_HEAD(&g_rpc_methods, slist);
}

static void
test_spdk_rpc_is_method_allowed(void)
{
	const char method[] = "test";
	uint32_t state_mask = SPDK_RPC_STARTUP;
	struct spdk_rpc_method m = {};
	int rc = 0;

	/* Case 1: Expect return -EPERM */
	m.name = method;
	m.state_mask = SPDK_RPC_RUNTIME;
	SLIST_INSERT_HEAD(&g_rpc_methods, &m, slist);
	rc = spdk_rpc_is_method_allowed(method, state_mask);
	CU_ASSERT(rc == -EPERM);

	/* Case 2: Expect return 0 */
	state_mask = SPDK_RPC_RUNTIME;
	rc = spdk_rpc_is_method_allowed(method, state_mask);
	CU_ASSERT(rc == 0);

	/* Case 3: Expect return -ENOENT */
	SLIST_REMOVE_HEAD(&g_rpc_methods, slist);
	rc = spdk_rpc_is_method_allowed(method, state_mask);
	CU_ASSERT(rc == -ENOENT);
}

static void
test_rpc_get_methods(void)
{
	struct spdk_jsonrpc_request *request = (struct spdk_jsonrpc_request *)0xbeefbeef;
	struct spdk_json_val params = {};
	struct spdk_rpc_method m = {};

	/* Case 1: spdk_json_decode_object failed */
	g_rpc_err = -1;
	params.type = SPDK_JSON_VAL_INVALID;
	rpc_get_methods(request, &params);
	CU_ASSERT(g_rpc_err == SPDK_JSONRPC_ERROR_INVALID_PARAMS);

	/* Case 2: Expect pass */
	params.type = SPDK_JSON_VAL_TRUE;
	m.state_mask = SPDK_RPC_RUNTIME;
	g_rpc_state = SPDK_RPC_STARTUP;
	SLIST_INSERT_HEAD(&g_rpc_methods, &m, slist);
	rpc_get_methods(request, &params);
	SLIST_REMOVE_HEAD(&g_rpc_methods, slist);
}

static  void
test_rpc_spdk_get_version(void)
{
	struct spdk_jsonrpc_request *request = (struct spdk_jsonrpc_request *)0xdeadbeef;
	struct spdk_json_val params = {};

	/* Case 1: spdk_get_version method requires no parameters */
	g_rpc_err = -1;
	params.type = SPDK_JSON_VAL_INVALID;
	rpc_spdk_get_version(request, &params);
	CU_ASSERT(g_rpc_err == SPDK_JSONRPC_ERROR_INVALID_PARAMS);

	/* Case 2: Expect pass */
	rpc_spdk_get_version(request, NULL);
}

static void
test_spdk_rpc_listen_close(void)
{
	const char listen_addr[128] = "/var/tmp/spdk-rpc-ut.sock";
	char rpc_lock_path[128] = {};

	MOCK_SET(open, 1);
	MOCK_SET(close, 0);
	MOCK_SET(flock, 0);

	spdk_rpc_listen(listen_addr);
	snprintf(rpc_lock_path, sizeof(g_rpc_lock_path), "%s.lock",
		 g_rpc_listen_addr_unix.sun_path);

	CU_ASSERT(g_rpc_listen_addr_unix.sun_family == AF_UNIX);
	CU_ASSERT(strcmp(g_rpc_listen_addr_unix.sun_path, listen_addr) == 0);
	CU_ASSERT(strcmp(g_rpc_lock_path, rpc_lock_path) == 0);
	CU_ASSERT(g_jsonrpc_server == (struct spdk_jsonrpc_server *)0Xdeaddead);

	spdk_rpc_close();

	CU_ASSERT(g_rpc_listen_addr_unix.sun_path[0] == '\0');
	CU_ASSERT(g_jsonrpc_server == NULL);
	CU_ASSERT(g_rpc_lock_fd == -1);
	CU_ASSERT(g_rpc_lock_path[0] == '\0');

	MOCK_CLEAR(open);
	MOCK_CLEAR(close);
	MOCK_CLEAR(flock);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("rpc", NULL, NULL);

	CU_ADD_TEST(suite, test_jsonrpc_handler);
	CU_ADD_TEST(suite, test_spdk_rpc_is_method_allowed);
	CU_ADD_TEST(suite, test_rpc_get_methods);
	CU_ADD_TEST(suite, test_rpc_spdk_get_version);
	CU_ADD_TEST(suite, test_spdk_rpc_listen_close);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
