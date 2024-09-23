/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk_internal/xlio.h"
#include "spdk/log.h"

#ifndef SPDK_CONFIG_STATIC_XLIO
static char g_default_xlio_path[] = "libxlio.so";
struct xlio_api_t *g_xlio_api;
static void *g_xlio_handle;
#endif
static bool g_xlio_is_initialized = false;

int (*g_xlio_getsockopt)(int sockfd, int level, int optname, void *restrict optval,
			 socklen_t *restrict optlen);


#ifndef SPDK_CONFIG_STATIC_XLIO
static int
xlio_load(const char *xlio_path)
{
	g_xlio_handle = dlopen(xlio_path, RTLD_NOW);
	if (!g_xlio_handle) {
		SPDK_ERRLOG("Failed to load XLIO library: path %s, error %s\n",
			    xlio_path, dlerror());
		return -1;
	}

	g_xlio_getsockopt = dlsym(g_xlio_handle, "getsockopt");
	if (!g_xlio_getsockopt) {
		SPDK_ERRLOG("Failed to find getsockopt in XLIO library\n");
		dlclose(g_xlio_handle);
		g_xlio_handle = NULL;
		return -1;
	}

	return 0;
}

static void
xlio_unload(void)
{
	int rc;

	if (g_xlio_handle) {
		int (*xlio_exit)(void) = dlsym(g_xlio_handle, "xlio_exit");

		if (xlio_exit) {
			xlio_exit();
		}

		rc = dlclose(g_xlio_handle);
		if (rc) {
			SPDK_ERRLOG("Closing libxlio failed: rc %d %s\n",
				    rc, dlerror());
		}

		SPDK_NOTICELOG("Unloaded libxlio\n");
		g_xlio_handle = NULL;
	}
}

static struct xlio_api_t *
_xlio_get_api(void)
{
	struct xlio_api_t *api_ptr = NULL;
	socklen_t len = sizeof(api_ptr);

	int err = g_xlio_getsockopt(-2, SOL_SOCKET, SO_XLIO_GET_API, &api_ptr, &len);
	if (err < 0) {
		return NULL;
	}

	return api_ptr;
}
#endif

static void *
xlio_alloc(size_t size)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
}

static void
xlio_free(void *buf)
{
	/* For some reason XLIO destructor is not called from dlclose
	 * but is called later after DPDK cleanup. And this leads to a
	 * crash since all DPDK structures are already freed.
	 *
	 * The check below works this around. If XLIO didn't free the
	 * memory while in dlcose(), don't try to free it. DPDK will
	 * do or has already done the cleanup.
	 */
#ifndef SPDK_CONFIG_STATIC_XLIO
	if (g_xlio_handle) {
		spdk_free(buf);
	}
#else
	spdk_free(buf);
#endif
}

int
spdk_xlio_init(void)
{
	int rc;
	struct xlio_init_attr iattr = {
		.flags = 0,
		.memory_alloc = &xlio_alloc,
		.memory_free = &xlio_free,
	};

#ifndef SPDK_CONFIG_STATIC_XLIO
	uint64_t required_caps;
	char *xlio_path;

	/* Before init, g_xlio_api must be NULL */
	assert(g_xlio_api == NULL);

	xlio_path = getenv("SPDK_XLIO_PATH");
	if (!xlio_path) {
		SPDK_NOTICELOG("SPDK_XLIO_PATH is not defined. XLIO socket implementation is disabled.\n");
		return 0;
	} else if (strnlen(xlio_path, 1) == 0) {
		xlio_path = g_default_xlio_path;
		SPDK_NOTICELOG("SPDK_XLIO_PATH is defined but empty. Using default: %s\n", g_default_xlio_path);
	}

	if (xlio_load(xlio_path) != 0) {
		return -1;
	}

	g_xlio_api = _xlio_get_api();
	if (!g_xlio_api) {
		SPDK_ERRLOG("Failed to get XLIO API\n");
		return -1;
	}
	SPDK_NOTICELOG("Got XLIO API %p\n", g_xlio_api);

	if (g_xlio_api->magic != XLIO_MAGIC_NUMBER) {
		SPDK_ERRLOG("Unexpected XLIO API magic number: expected %" PRIx64 ", got %" PRIx64 "\n",
			    (uint64_t)XLIO_MAGIC_NUMBER, g_xlio_api->magic);
		g_xlio_api = NULL;
		return -1;
	}

	required_caps = XLIO_EXTRA_API_XLIO_ULTRA;
	if ((g_xlio_api->cap_mask & required_caps) != required_caps) {
		SPDK_ERRLOG("Required XLIO caps are missing: required %" PRIx64 ", got %" PRIx64 "\n",
			    required_caps, g_xlio_api->cap_mask);
		g_xlio_api = NULL;
		return -1;
	}
#endif
	rc = xlio_init_ex(&iattr);
	if (rc) {
		SPDK_ERRLOG("xlio_init_ex rc %d (errno=%d)\n", rc, errno);
		g_xlio_is_initialized = false;
	} else {
		g_xlio_is_initialized = true;
	}

	return rc;
}

void
spdk_xlio_fini(void)
{
#ifndef SPDK_CONFIG_STATIC_XLIO
	xlio_unload();
	g_xlio_api = NULL;
#else
	/* FIXME: call xlio_exit may cause memory corruption */
	/** xlio_exit(); */
#endif
	g_xlio_is_initialized = false;
}

bool
spdk_xlio_is_initialized(void)
{
	return g_xlio_is_initialized;
}
