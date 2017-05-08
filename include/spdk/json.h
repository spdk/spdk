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

/**
 * \file
 * JSON parsing and encoding
 */

#ifndef SPDK_JSON_H_
#define SPDK_JSON_H_

#include "spdk/stdinc.h"

enum spdk_json_val_type {
	SPDK_JSON_VAL_INVALID,
	SPDK_JSON_VAL_NULL,
	SPDK_JSON_VAL_TRUE,
	SPDK_JSON_VAL_FALSE,
	SPDK_JSON_VAL_NUMBER,
	SPDK_JSON_VAL_STRING,
	SPDK_JSON_VAL_ARRAY_BEGIN,
	SPDK_JSON_VAL_ARRAY_END,
	SPDK_JSON_VAL_OBJECT_BEGIN,
	SPDK_JSON_VAL_OBJECT_END,
	SPDK_JSON_VAL_NAME,
};

struct spdk_json_val {
	/**
	 * Pointer to the location of the value within the parsed JSON input.
	 *
	 * For SPDK_JSON_VAL_STRING and SPDK_JSON_VAL_NAME,
	 *  this points to the beginning of the decoded UTF-8 string without quotes.
	 *
	 * For SPDK_JSON_VAL_NUMBER, this points to the beginning of the number as represented in
	 *  the original JSON (text representation, not converted to a numeric value).
	 */
	void *start;

	/**
	 * Length of value.
	 *
	 * For SPDK_JSON_VAL_STRING, SPDK_JSON_VAL_NUMBER, and SPDK_JSON_VAL_NAME,
	 *  this is the length in bytes of the value starting at \ref start.
	 *
	 * For SPDK_JSON_VAL_ARRAY_BEGIN and SPDK_JSON_VAL_OBJECT_BEGIN,
	 *  this is the number of values contained within the array or object (including
	 *  nested objects and arrays, but not including the _END value).  The array or object _END
	 *  value can be found by advancing len values from the _BEGIN value.
	 */
	uint32_t len;

	/**
	 * Type of value.
	 */
	enum spdk_json_val_type type;
};

/**
 * Invalid JSON syntax.
 */
#define SPDK_JSON_PARSE_INVALID			-1

/**
 * JSON was valid up to the end of the current buffer, but did not represent a complete JSON value.
 */
#define SPDK_JSON_PARSE_INCOMPLETE		-2

#define SPDK_JSON_PARSE_MAX_DEPTH_EXCEEDED	-3

/**
 * Decode JSON strings and names in place (modify the input buffer).
 */
#define SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE	0x000000001

/**
 * Allow parsing of comments.
 *
 * Comments are not allowed by the JSON RFC, so this is not enabled by default.
 */
#define SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS	0x000000002

/*
 * Parse JSON data.
 *
 * \param data Raw JSON data; must be encoded in UTF-8.
 * Note that the data may be modified to perform in-place string decoding.
 *
 * \param size Size of data in bytes.
 *
 * \param end If non-NULL, this will be filled a pointer to the byte just beyond the end
 * of the valid JSON.
 *
 * \return Number of values parsed, or negative on failure:
 * SPDK_JSON_PARSE_INVALID if the provided data was not valid JSON, or
 * SPDK_JSON_PARSE_INCOMPLETE if the provided data was not a complete JSON value.
 */
ssize_t spdk_json_parse(void *json, size_t size, struct spdk_json_val *values, size_t num_values,
			void **end, uint32_t flags);

typedef int (*spdk_json_decode_fn)(const struct spdk_json_val *val, void *out);

struct spdk_json_object_decoder {
	const char *name;
	size_t offset;
	spdk_json_decode_fn decode_func;
	bool optional;
};

int spdk_json_decode_object(const struct spdk_json_val *values,
			    const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out);
int spdk_json_decode_array(const struct spdk_json_val *values, spdk_json_decode_fn decode_func,
			   void *out, size_t max_size, size_t *out_size, size_t stride);

int spdk_json_decode_bool(const struct spdk_json_val *val, void *out);
int spdk_json_decode_int32(const struct spdk_json_val *val, void *out);
int spdk_json_decode_uint32(const struct spdk_json_val *val, void *out);
int spdk_json_decode_string(const struct spdk_json_val *val, void *out);

/**
 * Get length of a value in number of values.
 *
 * This can be used to skip over a value while interpreting parse results.
 *
 * For SPDK_JSON_VAL_ARRAY_BEGIN and SPDK_JSON_VAL_OBJECT_BEGIN,
 *  this returns the number of values contained within this value, plus the _BEGIN and _END values.
 *
 * For all other values, this returns 1.
 */
size_t spdk_json_val_len(const struct spdk_json_val *val);

/**
 * Compare JSON string with null terminated C string.
 *
 * \return true if strings are equal or false if not
 */
bool spdk_json_strequal(const struct spdk_json_val *val, const char *str);

/**
 * Equivalent of strdup() for JSON string values.
 *
 * If val is not representable as a C string (contains embedded '\0' characters),
 * returns NULL.
 *
 * Caller is responsible for passing the result to free() when it is no longer needed.
 */
char *spdk_json_strdup(const struct spdk_json_val *val);

int spdk_json_number_to_double(const struct spdk_json_val *val, double *num);
int spdk_json_number_to_int32(const struct spdk_json_val *val, int32_t *num);
int spdk_json_number_to_uint32(const struct spdk_json_val *val, uint32_t *num);

struct spdk_json_write_ctx;

#define SPDK_JSON_WRITE_FLAG_FORMATTED	0x00000001

typedef int (*spdk_json_write_cb)(void *cb_ctx, const void *data, size_t size);

struct spdk_json_write_ctx *spdk_json_write_begin(spdk_json_write_cb write_cb, void *cb_ctx,
		uint32_t flags);
int spdk_json_write_end(struct spdk_json_write_ctx *w);
int spdk_json_write_null(struct spdk_json_write_ctx *w);
int spdk_json_write_bool(struct spdk_json_write_ctx *w, bool val);
int spdk_json_write_int32(struct spdk_json_write_ctx *w, int32_t val);
int spdk_json_write_uint32(struct spdk_json_write_ctx *w, uint32_t val);
int spdk_json_write_int64(struct spdk_json_write_ctx *w, int64_t val);
int spdk_json_write_uint64(struct spdk_json_write_ctx *w, uint64_t val);
int spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val);
int spdk_json_write_string_raw(struct spdk_json_write_ctx *w, const char *val, size_t len);
int spdk_json_write_string_fmt(struct spdk_json_write_ctx *w, const char *fmt,
			       ...) __attribute__((__format__(__printf__, 2, 3)));
int spdk_json_write_array_begin(struct spdk_json_write_ctx *w);
int spdk_json_write_array_end(struct spdk_json_write_ctx *w);
int spdk_json_write_object_begin(struct spdk_json_write_ctx *w);
int spdk_json_write_object_end(struct spdk_json_write_ctx *w);
int spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name);
int spdk_json_write_name_raw(struct spdk_json_write_ctx *w, const char *name, size_t len);

int spdk_json_write_val(struct spdk_json_write_ctx *w, const struct spdk_json_val *val);

/*
 * Append bytes directly to the output stream without validation.
 *
 * Can be used to write values with specific encodings that differ from the JSON writer output.
 */
int spdk_json_write_val_raw(struct spdk_json_write_ctx *w, const void *data, size_t len);

#endif
