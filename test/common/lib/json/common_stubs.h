/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Dell Inc, or its subsidiaries.
 *   All rights reserved.
 */

#ifndef SPDK_TEST_JSON_COMMON_STUBS_H
#define SPDK_TEST_JSON_COMMON_STUBS_H

DEFINE_STUB(spdk_json_decode_array, int,
	    (const struct spdk_json_val *values, spdk_json_decode_fn decode_func,
	     void *out, size_t max_size, size_t *out_size, size_t stride), 0);
DEFINE_STUB(spdk_json_decode_bool,   int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_int32,  int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_string, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uint8,  int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uint16, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uint32, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uint64, int, (const struct spdk_json_val *val, void *out), 0);
DEFINE_STUB(spdk_json_decode_uuid,   int, (const struct spdk_json_val *val, void *out), 0);

DEFINE_STUB(spdk_json_strdup,   char *, (const struct spdk_json_val *val), NULL);

#endif /* SPDK_TEST_JSON_COMMON_STUBS_H */
