/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_KEYRING_H
#define SPDK_KEYRING_H

#include "spdk/stdinc.h"
#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_key;

/**
 * Get a reference to a key from the keyring.  The key must have been added to the keyring by
 * the appropriate keyring module.  The reference will be kept alive until its released via
 * `spdk_keyring_put_key()`.  If the key is removed from the keyring, the reference is kept alive, but
 * the key won't be usable anymore.
 *
 * \param name Name of a key.  The name can be optionally prepended with the name of a keyring to
 * retrieve the key from followed by a ":" character.  For instance, "keyring0:key0" would retrieve
 * a key "key0" from keyring "keyring0".  If omitted, "global" keyring will be used.  To get a key
 * with a ":" character in its name from the global keyring, empty keyring name should be specified
 * (e.g. ":key0:foo" refers to a key "key0:foo" in the global keyring).
 *
 * \return Reference to a key or NULL if the key doesn't exist.
 */
struct spdk_key *spdk_keyring_get_key(const char *name);

/**
 * Release a reference to a key obtained from `spdk_keyring_get_key()`.
 *
 * \param key Reference to a key.  If NULL, this function is a no-op.
 */
void spdk_keyring_put_key(struct spdk_key *key);

/**
 * Get the name of a key.
 *
 * \param key Reference to a key.
 *
 * \return Name of the key.
 */
const char *spdk_key_get_name(struct spdk_key *key);

/**
 * Retrieve keying material from a key reference.
 *
 * \param key Reference to a key.
 * \param buf Buffer to write the data to.
 * \param len Size of the `buf` buffer.
 *
 * \return The number of bytes written to `buf` or negative errno on error.
 */
int spdk_key_get_key(struct spdk_key *key, void *buf, int len);

/**
 * Duplicate a key.  The returned key reference might be a pointer to the same exact object.  After
 * duplicating a key, the new reference should be released via `spdk_keyring_put_key()`.
 *
 * \param key Reference to a key.
 *
 * \return Pointer to the key reference.
 */
struct spdk_key *spdk_key_dup(struct spdk_key *key);

/**
 * Initialize the keyring library.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_init(void);

/**
 * Free any resources acquired by the keyring library.  This function will free all of the keys.
 */
void spdk_keyring_cleanup(void);

struct spdk_keyring;

/** Iterate over all keys including those that were removed, but still have active references */
#define SPDK_KEYRING_FOR_EACH_ALL 0x1

/**
 * Execute a function on each registered key attached to a given keyring.  For now, this function
 * only supports iterating over keys from all keyrings and the `keyring` parameter must be set to
 * NULL.
 *
 * \param keyring Keyring over which to iterate.  If NULL, iterate over keys from all keyrings.
 * \param ctx Context to pass to the function.
 * \param fn Function to call.
 * \param flags Flags controlling the keys to iterate over.
 */
void spdk_keyring_for_each_key(struct spdk_keyring *keyring, void *ctx,
			       void (*fn)(void *ctx, struct spdk_key *key), uint32_t flags);

/**
 * Write keyring configuration to JSON.
 *
 * \param w JSON write context.
 */
void spdk_keyring_write_config(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KEYRING_H */
