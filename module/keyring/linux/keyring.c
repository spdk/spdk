/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#include <keyutils.h>
#include "keyring_linux.h"
#include "spdk/keyring.h"
#include "spdk/keyring_module.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

static struct keyring_linux_opts g_opts;

struct linux_key {
	key_serial_t sn;
};

static struct spdk_keyring_module g_keyring_linux;

void
keyring_linux_get_opts(struct keyring_linux_opts *opts)
{
	memcpy(opts, &g_opts, sizeof(*opts));
}

int
keyring_linux_set_opts(struct keyring_linux_opts *opts)
{
	g_opts.enable = opts->enable;

	return 0;
}

static int
linux_find_key(const char *name, key_serial_t *outsn)
{
	key_serial_t sn;

	sn = request_key("user", name, NULL, KEY_SPEC_SESSION_KEYRING);
	if (sn < 0) {
		return -errno;
	}

	if (outsn != NULL) {
		*outsn = sn;
	}

	return 0;
}

static int
linux_probe_key(const char *name)
{
	struct spdk_key_opts opts = {};
	int rc;

	rc = linux_find_key(name, NULL);
	if (rc != 0) {
		return rc;
	}

	opts.size = SPDK_SIZEOF(&opts, module);
	opts.name = name;
	opts.module = &g_keyring_linux;

	return spdk_keyring_add_key(&opts);
}

static int
linux_add_key(struct spdk_key *key, void *ctx)
{
	struct linux_key *lkey = spdk_key_get_ctx(key);

	return linux_find_key(spdk_key_get_name(key), &lkey->sn);
}

static void
linux_remove_key(struct spdk_key *key)
{
	/* no-op */
}

static int
linux_get_key(struct spdk_key *key, void *buf, int len)
{
	struct linux_key *lkey = spdk_key_get_ctx(key);
	int rc, errsv;

	rc = keyctl_read(lkey->sn, buf, len);
	if (rc < 0) {
		errsv = errno;
		SPDK_ERRLOG("Failed to read key '%s': %s\n", spdk_key_get_name(key),
			    spdk_strerror(errsv));
		return -errsv;
	}

	if (rc > len) {
		SPDK_ERRLOG("Failed to read key '%s': buffer to small\n", spdk_key_get_name(key));
		return -ENOBUFS;
	}

	return rc;
}

static size_t
linux_get_ctx_size(void)
{
	return sizeof(struct linux_key);
}

static void
linux_dump_info(struct spdk_key *key, struct spdk_json_write_ctx *w)
{
	struct linux_key *lkey = spdk_key_get_ctx(key);

	spdk_json_write_named_uint32(w, "sn", lkey->sn);
}

static void
linux_write_config(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "keyring_linux_set_options");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_bool(w, "enable", g_opts.enable);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static int
linux_init(void)
{
	return g_opts.enable ? 0 : -ENODEV;
}

static struct spdk_keyring_module g_keyring_linux = {
	.name = "linux",
	.init = linux_init,
	.probe_key = linux_probe_key,
	.add_key = linux_add_key,
	.remove_key = linux_remove_key,
	.get_key = linux_get_key,
	.get_ctx_size = linux_get_ctx_size,
	.dump_info = linux_dump_info,
	.write_config = linux_write_config,
};
SPDK_KEYRING_REGISTER_MODULE(linux, &g_keyring_linux);
