/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/uuid.h"

#ifndef __FreeBSD__

#include <uuid/uuid.h>

SPDK_STATIC_ASSERT(sizeof(struct spdk_uuid) == sizeof(uuid_t), "Size mismatch");

int
spdk_uuid_parse(struct spdk_uuid *uuid, const char *uuid_str)
{
	return uuid_parse(uuid_str, (void *)uuid) == 0 ? 0 : -EINVAL;
}

int
spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid)
{
	if (uuid_str_size < SPDK_UUID_STRING_LEN) {
		return -EINVAL;
	}

	uuid_unparse_lower((void *)uuid, uuid_str);
	return 0;
}

int
spdk_uuid_compare(const struct spdk_uuid *u1, const struct spdk_uuid *u2)
{
	return uuid_compare((void *)u1, (void *)u2);
}

void
spdk_uuid_generate(struct spdk_uuid *uuid)
{
	uuid_generate((void *)uuid);
}

void
spdk_uuid_copy(struct spdk_uuid *dst, const struct spdk_uuid *src)
{
	uuid_copy((void *)dst, (void *)src);
}

#else

#include <uuid.h>

SPDK_STATIC_ASSERT(sizeof(struct spdk_uuid) == sizeof(uuid_t), "Size mismatch");

int
spdk_uuid_parse(struct spdk_uuid *uuid, const char *uuid_str)
{
	uint32_t status;

	uuid_from_string(uuid_str, (uuid_t *)uuid, &status);

	return status == 0 ? 0 : -EINVAL;
}

int
spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid)
{
	uint32_t status;
	char *str;

	if (uuid_str_size < SPDK_UUID_STRING_LEN) {
		return -EINVAL;
	}

	uuid_to_string((const uuid_t *)uuid, &str, &status);

	if (status == uuid_s_no_memory) {
		return -ENOMEM;
	}

	snprintf(uuid_str, uuid_str_size, "%s", str);
	free(str);

	return 0;
}

int
spdk_uuid_compare(const struct spdk_uuid *u1, const struct spdk_uuid *u2)
{
	return uuid_compare((const uuid_t *)u1, (const uuid_t *)u2, NULL);
}

void
spdk_uuid_generate(struct spdk_uuid *uuid)
{
	uuid_create((uuid_t *)uuid, NULL);
}

void
spdk_uuid_copy(struct spdk_uuid *dst, const struct spdk_uuid *src)
{
	memcpy(dst, src, sizeof(*dst));
}

#endif
