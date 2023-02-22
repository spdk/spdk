/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/uuid.h"
#include "spdk/config.h"
#include "spdk/log.h"

#ifndef SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1
#include <openssl/evp.h>
#endif /* SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1 */

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

int
spdk_uuid_generate_sha1(struct spdk_uuid *uuid, struct spdk_uuid *ns_uuid, const char *name,
			size_t len)
{
#ifdef SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1
	uuid_generate_sha1((void *)uuid, (void *)ns_uuid, name, len);
	return 0;
#else
	EVP_MD_CTX *mdctx;
	const EVP_MD *md;
	unsigned char md_value[EVP_MAX_MD_SIZE];
	unsigned int md_len;

	md = EVP_sha1();
	assert(md != NULL);

	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL) {
		return -ENOMEM;
	}

	if (EVP_DigestInit_ex(mdctx, md, NULL) != 1) {
		SPDK_ERRLOG("Could not initialize EVP digest!\n");
		goto err;
	}
	if (EVP_DigestUpdate(mdctx, ns_uuid, sizeof(struct spdk_uuid)) != 1) {
		SPDK_ERRLOG("Could update EVP digest with namespace UUID!\n");
		goto err;
	}
	if (EVP_DigestUpdate(mdctx, name, len) != 1) {
		SPDK_ERRLOG("Could update EVP digest with assigned name!\n");
		goto err;
	}
	if (EVP_DigestFinal_ex(mdctx, md_value, &md_len) != 1) {
		SPDK_ERRLOG("Could not generate EVP digest!\n");
		goto err;
	}
	EVP_MD_CTX_free(mdctx);

	memcpy(uuid, md_value, 16);
	/* This part mimics original uuid_generate_sha1() from libuuid/src/gen_uuid.c.
	 * The original uuid structure included from uuid.h looks like this:
	 * struct uuid {
	 *	uint32_t	time_low;
	 *	uint16_t	time_mid;
	 *	uint16_t	time_hi_and_version;
	 *	uint16_t	clock_seq;
	 *	uint8_t		node[6];
	 * };
	 * so uuid->u.raw[6] and uuid->u.raw[8] are time_hi_and_version and clock_seq respectively.
	 */
	uuid->u.raw[6] = (uuid->u.raw[6] & 0x0f) | 0x50;
	uuid->u.raw[8] = (uuid->u.raw[8] & 0x3f) | 0x80;

	return 0;

err:
	EVP_MD_CTX_free(mdctx);
	return -EINVAL;

#endif /* SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1 */
}
