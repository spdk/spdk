/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_KEYRING_MODULE_H
#define SPDK_KEYRING_MODULE_H

#include "spdk/stdinc.h"
#include "spdk/json.h"
#include "spdk/keyring.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_keyring_module;

struct spdk_key_opts {
	/** Size of this structure */
	size_t size;
	/** Name of the key */
	const char *name;
	/** Keyring module */
	struct spdk_keyring_module *module;
	/** Context passed to the add_key() callback */
	void *ctx;
};

/**
 * Add a key to the keyring.
 *
 * \param opts Key options.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_add_key(const struct spdk_key_opts *opts);

/**
 * Remove a key from the keyring.
 *
 * \param name Name of the key to remove.
 * \param module Module owning the key to remove.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_remove_key(const char *name, struct spdk_keyring_module *module);

struct spdk_keyring_module {
	/** Name of the module */
	const char *name;

	/** Initializes a module, called during keyring's initialization */
	int (*init)(void);
	/** Clean up resources allocated by a module.  Called during keyring's cleanup  */
	void (*cleanup)(void);
	/** Write module configuration to JSON */
	void (*write_config)(struct spdk_json_write_ctx *w);
	/**
	 * Probe if a key with a specified name is available.  If it is, the module should add it to
	 * the keyring and return zero.  Otherwise, -ENOKEY should be returned.
	 */
	int (*probe_key)(const char *name);
	/** Add a key to the keyring */
	int (*add_key)(struct spdk_key *key, void *ctx);
	/** Remove a key from the keyring */
	void (*remove_key)(struct spdk_key *key);
	/** Get keying material from a key */
	int (*get_key)(struct spdk_key *key, void *buf, int len);
	/** Get the size of the context associated with a key */
	size_t (*get_ctx_size)(void);
	/**
	 * Dump information about a key to JSON.  This callback should never dump keying material
	 * itself, only non-sensitive properties of a key must be dumped.
	 */
	void (*dump_info)(struct spdk_key *key, struct spdk_json_write_ctx *w);

	TAILQ_ENTRY(spdk_keyring_module) tailq;
};

/**
 * Register a keyring module.
 *
 * \param module Keyring module to register.
 */
void spdk_keyring_register_module(struct spdk_keyring_module *module);

#define SPDK_KEYRING_REGISTER_MODULE(name, module) \
static void __attribute__((constructor)) _spdk_keyring_register_##name(void) \
{ \
	spdk_keyring_register_module(module); \
}

/**
 * Get pointer to the module context associated with a key.
 *
 * \param key Key.
 *
 * \return Key context.
 */
void *spdk_key_get_ctx(struct spdk_key *key);

/**
 * Get keyring module owning the key.
 *
 * \param key Key.
 *
 * \return Key owner.
 */
struct spdk_keyring_module *spdk_key_get_module(struct spdk_key *key);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KEYRING_H */
