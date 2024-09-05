/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#include "spdk/keyring_module.h"
#include "spdk/module/keyring/file.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

struct keyring_file_key {
	char *path;
};

static struct spdk_keyring_module g_keyring_file;

static int
keyring_file_check_path(const char *path, int *size)
{
	struct stat st;
	int rc, errsv;

	if (path[0] != '/') {
		SPDK_ERRLOG("Non-absolute paths are not allowed: %s\n", path);
		return -EPERM;
	}

	rc = stat(path, &st);
	if (rc != 0) {
		errsv = errno;
		SPDK_ERRLOG("Could not stat key file '%s': %s\n", path, spdk_strerror(errsv));
		return -errsv;
	}

	if ((st.st_mode & 077) || st.st_uid != getuid()) {
		SPDK_ERRLOG("Invalid permissions for key file '%s': 0%o\n", path, st.st_mode);
		return -EPERM;
	}

	if (size != NULL) {
		*size = st.st_size;
	}

	return 0;
}

static void
keyring_file_write_key_config(void *ctx, struct spdk_key *key)
{
	struct spdk_json_write_ctx *w = ctx;
	struct keyring_file_key *kkey;

	if (spdk_key_get_module(key) != &g_keyring_file) {
		return;
	}

	kkey = spdk_key_get_ctx(key);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "keyring_file_add_key");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", spdk_key_get_name(key));
	spdk_json_write_named_string(w, "path", kkey->path);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
keyring_file_write_config(struct spdk_json_write_ctx *w)
{
	spdk_keyring_for_each_key(NULL, w, keyring_file_write_key_config, 0);
}

static void
keyring_file_dump_info(struct spdk_key *key, struct spdk_json_write_ctx *w)
{
	struct keyring_file_key *kkey = spdk_key_get_ctx(key);

	spdk_json_write_named_string(w, "path", kkey->path);
}

static size_t
keyring_file_get_ctx_size(void)
{
	return sizeof(struct keyring_file_key);
}

static int
keyring_file_get_key(struct spdk_key *key, void *buf, int len)
{
	struct keyring_file_key *kkey = spdk_key_get_ctx(key);
	FILE *file;
	int rc, errsv, size = 0;

	rc = keyring_file_check_path(kkey->path, &size);
	if (rc != 0) {
		return rc;
	}

	if (size > len) {
		SPDK_ERRLOG("Invalid key '%s' size: %d > %d\n", spdk_key_get_name(key), size, len);
		return -ENOBUFS;
	}

	file = fopen(kkey->path, "r");
	if (!file) {
		errsv = errno;
		SPDK_ERRLOG("Could not open key '%s': %s\n", spdk_key_get_name(key),
			    spdk_strerror(errsv));
		return -errsv;
	}

	rc = (int)fread(buf, 1, size, file);
	if (rc != size) {
		SPDK_ERRLOG("Could not load key '%s'\n", spdk_key_get_name(key));
		rc = -EIO;
	}

	fclose(file);

	return rc;
}

static void
keyring_file_remove_key(struct spdk_key *key)
{
	struct keyring_file_key *kkey = spdk_key_get_ctx(key);

	free(kkey->path);
}

static int
keyring_file_add_key(struct spdk_key *key, void *ctx)
{
	struct keyring_file_key *kkey = spdk_key_get_ctx(key);
	const char *path = ctx;
	int rc;

	rc = keyring_file_check_path(path, NULL);
	if (rc != 0) {
		return rc;
	}

	kkey->path = strdup(path);
	if (kkey->path == NULL) {
		return -ENOMEM;
	}

	return 0;
}

int
spdk_keyring_file_add_key(const char *name, const char *path)
{
	struct spdk_key_opts opts = {};

	opts.size = SPDK_SIZEOF(&opts, ctx);
	opts.name = name;
	opts.module = &g_keyring_file;
	opts.ctx = (void *)path;

	return spdk_keyring_add_key(&opts);
}

int
spdk_keyring_file_remove_key(const char *name)
{
	return spdk_keyring_remove_key(name, &g_keyring_file);
}

static struct spdk_keyring_module g_keyring_file = {
	.name = "keyring_file",
	.add_key = keyring_file_add_key,
	.remove_key = keyring_file_remove_key,
	.get_key = keyring_file_get_key,
	.get_ctx_size = keyring_file_get_ctx_size,
	.dump_info = keyring_file_dump_info,
	.write_config = keyring_file_write_config,
};

SPDK_KEYRING_REGISTER_MODULE(keyring_file, &g_keyring_file);
