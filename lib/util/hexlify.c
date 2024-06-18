/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/hexlify.h"
#include "spdk/log.h"

static inline int
__c2v(char c)
{
	if ((c >= '0') && (c <= '9')) {
		return c - '0';
	}
	if ((c >= 'a') && (c <= 'f')) {
		return c - 'a' + 10;
	}
	if ((c >= 'A') && (c <= 'F')) {
		return c - 'A' + 10;
	}
	return -1;
}

static inline signed char
__v2c(int c)
{
	const char hexchar[] = "0123456789abcdef";
	if (c < 0 || c > 15) {
		return -1;
	}
	return hexchar[c];
}

char *
spdk_hexlify(const char *bin, size_t len)
{
	char *hex, *phex;

	hex = malloc((len * 2) + 1);
	if (hex == NULL) {
		return NULL;
	}
	phex = hex;
	for (size_t i = 0; i < len; i++) {
		signed char c0 = __v2c((bin[i] >> 4) & 0x0f);
		signed char c1 = __v2c((bin[i]) & 0x0f);
		if (c0 < 0 || c1 < 0) {
			assert(false);
			free(hex);
			return NULL;
		}
		*phex++ = c0;
		*phex++ = c1;
	}
	*phex = '\0';
	return hex;
}

char *
spdk_unhexlify(const char *hex)
{
	char *res, *pres;
	size_t len = strlen(hex);

	if (len % 2 != 0) {
		SPDK_ERRLOG("Invalid hex string len %d. It must be mod of 2.\n", (int)len);
		return NULL;
	}
	res = malloc(len / 2);
	if (res == NULL) {
		return NULL;
	}
	pres = res;
	for (size_t i = 0; i < len; i += 2) {
		int v0 = __c2v(hex[i]);
		int v1 = __c2v(hex[i + 1]);
		if (v0 < 0 || v1 < 0) {
			SPDK_ERRLOG("Invalid hex string \"%s\"\n", hex);
			free(res);
			return NULL;
		}
		*pres++ = (v0 << 4) + v1;
	}
	return res;
}
