/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/json.h"
#include "spdk_internal/mock.h"

DEFINE_STUB(spdk_json_write_begin, struct spdk_json_write_ctx *, (spdk_json_write_cb write_cb,
		void *cb_ctx, uint32_t flags), NULL);

DEFINE_STUB(spdk_json_write_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_null, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_bool, int, (struct spdk_json_write_ctx *w, bool val), 0);
DEFINE_STUB(spdk_json_write_uint8, int, (struct spdk_json_write_ctx *w, uint8_t val), 0);
DEFINE_STUB(spdk_json_write_uint16, int, (struct spdk_json_write_ctx *w, uint16_t val), 0);
DEFINE_STUB(spdk_json_write_int32, int, (struct spdk_json_write_ctx *w, int32_t val), 0);
DEFINE_STUB(spdk_json_write_uint32, int, (struct spdk_json_write_ctx *w, uint32_t val), 0);
DEFINE_STUB(spdk_json_write_int64, int, (struct spdk_json_write_ctx *w, int64_t val), 0);
DEFINE_STUB(spdk_json_write_uint64, int, (struct spdk_json_write_ctx *w, uint64_t val), 0);
DEFINE_STUB(spdk_json_write_string, int, (struct spdk_json_write_ctx *w, const char *val), 0);
DEFINE_STUB(spdk_json_write_string_raw, int, (struct spdk_json_write_ctx *w, const char *val,
		size_t len), 0);
DEFINE_STUB(spdk_json_write_string_fmt, int, (struct spdk_json_write_ctx *w, const char *fmt, ...),
	    0);

DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_name, int, (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_json_write_name_raw, int, (struct spdk_json_write_ctx *w, const char *name,
		size_t len), 0);

/* Utility functions */
DEFINE_STUB(spdk_json_write_named_null, int, (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_json_write_named_bool, int, (struct spdk_json_write_ctx *w, const char *name,
		bool val), 0);
DEFINE_STUB(spdk_json_write_named_uint8, int, (struct spdk_json_write_ctx *w, const char *name,
		uint8_t val), 0);
DEFINE_STUB(spdk_json_write_named_uint16, int, (struct spdk_json_write_ctx *w, const char *name,
		uint16_t val), 0);
DEFINE_STUB(spdk_json_write_named_int32, int, (struct spdk_json_write_ctx *w, const char *name,
		int32_t val), 0);
DEFINE_STUB(spdk_json_write_named_uint32, int, (struct spdk_json_write_ctx *w, const char *name,
		uint32_t val), 0);
DEFINE_STUB(spdk_json_write_named_uint64, int, (struct spdk_json_write_ctx *w, const char *name,
		uint64_t val), 0);
DEFINE_STUB(spdk_json_write_named_int64, int, (struct spdk_json_write_ctx *w, const char *name,
		int64_t val), 0);
DEFINE_STUB(spdk_json_write_named_string, int, (struct spdk_json_write_ctx *w, const char *name,
		const char *val), 0);
DEFINE_STUB(spdk_json_write_named_string_fmt, int, (struct spdk_json_write_ctx *w, const char *name,
		const char *fmt, ...), 0);
DEFINE_STUB(spdk_json_write_named_string_fmt_v, int, (struct spdk_json_write_ctx *w,
		const char *name, const char *fmt, va_list args), 0);

DEFINE_STUB(spdk_json_write_named_array_begin, int, (struct spdk_json_write_ctx *w,
		const char *name), 0);
DEFINE_STUB(spdk_json_write_named_object_begin, int, (struct spdk_json_write_ctx *w,
		const char *name), 0);

DEFINE_STUB(spdk_json_number_to_uint64, int, (const struct spdk_json_val *val, uint64_t *num), 0);
