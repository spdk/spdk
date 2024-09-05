/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_KEYRING_FILE_H
#define SPDK_KEYRING_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Add a file-based key to the keyring.
 *
 * \param name Name of a key.
 * \param path Path to a file containing the key.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_file_add_key(const char *name, const char *path);

/**
 * Remove a file-based key to the keyring.
 *
 * \param name Name of a key.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_file_remove_key(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KEYRING_FILE_H */
