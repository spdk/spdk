/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/json.h"
#include "spdk_internal/mock.h"

DEFINE_STUB(spdk_json_write_begin, struct spdk_json_write_ctx *, (spdk_json_write_cb write_cb,
		void *cb_ctx, uint32_t flags), NULL);

DEFINE_STUB(spdk_json_write_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_null, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_bool, int, (struct spdk_json_write_ctx *w, bool val), 0);
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
