/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * UUID types and functions
 */

#ifndef SPDK_UUID_H
#define SPDK_UUID_H

#include "spdk/stdinc.h"

#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_uuid {
	union {
		uint8_t raw[16];
	} u;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_uuid) == 16, "Incorrect size");

#define SPDK_UUID_STRING_LEN 37 /* 36 characters + null terminator */

/**
 * Convert UUID in textual format into a spdk_uuid.
 *
 * \param[out] uuid User-provided UUID buffer.
 * \param uuid_str UUID in textual format in C string.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_uuid_parse(struct spdk_uuid *uuid, const char *uuid_str);

/**
 * Convert UUID in spdk_uuid into lowercase textual format.
 *
 * \param uuid_str User-provided string buffer to write the textual format into.
 * \param uuid_str_size Size of uuid_str buffer. Must be at least SPDK_UUID_STRING_LEN.
 * \param uuid UUID to convert to textual format.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid);

/**
 * Compare two UUIDs.
 *
 * \param u1 UUID 1.
 * \param u2 UUID 2.
 *
 * \return 0 if u1 == u2, less than 0 if u1 < u2, greater than 0 if u1 > u2.
 */
int spdk_uuid_compare(const struct spdk_uuid *u1, const struct spdk_uuid *u2);

/**
 * Generate a new UUID.
 *
 * \param[out] uuid User-provided UUID buffer to fill.
 */
void spdk_uuid_generate(struct spdk_uuid *uuid);

/**
 * Generate a new UUID using SHA1 hash.
 *
 * \param[out] uuid User-provided UUID buffer to fill.
 * \param ns_uuid Well-known namespace UUID for generated UUID.
 * \param name Arbitrary, binary string.
 * \param len Length of binary string.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_uuid_generate_sha1(struct spdk_uuid *uuid, struct spdk_uuid *ns_uuid, const char *name,
			    size_t len);

/**
 * Copy a UUID.
 *
 * \param src Source UUID to copy from.
 * \param dst Destination UUID to store.
 */
void spdk_uuid_copy(struct spdk_uuid *dst, const struct spdk_uuid *src);

/**
 * Compare the UUID to the NULL value (all bits equal to zero).
 *
 * \param uuid The UUID to test.
 *
 * \return true if uuid is equal to the NULL value, false if not.
 */
bool spdk_uuid_is_null(const struct spdk_uuid *uuid);

/**
 * Set the value of UUID to the NULL value.
 *
 * \param uuid The UUID to set.
 */
void spdk_uuid_set_null(struct spdk_uuid *uuid);

#ifdef __cplusplus
}
#endif

#endif
