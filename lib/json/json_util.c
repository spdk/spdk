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

#include "spdk_internal/utf.h"

size_t
spdk_json_val_len(const struct spdk_json_val *val)
{
	if (val == NULL) {
		return 0;
	}

	if (val->type == SPDK_JSON_VAL_ARRAY_BEGIN || val->type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		return val->len + 2;
	}

	return 1;
}

bool
spdk_json_strequal(const struct spdk_json_val *val, const char *str)
{
	size_t len;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NAME) {
		return false;
	}

	len = strlen(str);
	if (val->len != len) {
		return false;
	}

	return memcmp(val->start, str, len) == 0;
}

char *
spdk_json_strdup(const struct spdk_json_val *val)
{
	size_t len;
	char *s;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NAME) {
		return NULL;
	}

	len = val->len;

	if (memchr(val->start, '\0', len)) {
		/* String contains embedded NUL, so it is not a valid C string. */
		return NULL;
	}

	s = malloc(len + 1);
	if (s == NULL) {
		return s;
	}

	memcpy(s, val->start, len);
	s[len] = '\0';

	return s;
}

struct spdk_json_num {
	bool negative;
	uint64_t significand;
	int64_t exponent;
};

static int
spdk_json_number_split(const struct spdk_json_val *val, struct spdk_json_num *num)
{
	const char *iter;
	size_t remaining;
	uint64_t *pval;
	uint64_t frac_digits = 0;
	uint64_t exponent_u64 = 0;
	bool exponent_negative = false;
	enum {
		NUM_STATE_INT,
		NUM_STATE_FRAC,
		NUM_STATE_EXP,
	} state;

	memset(num, 0, sizeof(*num));

	if (val->type != SPDK_JSON_VAL_NUMBER) {
		return -EINVAL;
	}

	remaining = val->len;
	if (remaining == 0) {
		return -EINVAL;
	}

	iter = val->start;
	if (*iter == '-') {
		num->negative = true;
		iter++;
		remaining--;
	}

	state = NUM_STATE_INT;
	pval = &num->significand;
	while (remaining--) {
		char c = *iter++;

		if (c == '.') {
			state = NUM_STATE_FRAC;
		} else if (c == 'e' || c == 'E') {
			state = NUM_STATE_EXP;
			pval = &exponent_u64;
		} else if (c == '-') {
			assert(state == NUM_STATE_EXP);
			exponent_negative = true;
		} else if (c == '+') {
			assert(state == NUM_STATE_EXP);
			/* exp_negative = false; */ /* already false by default */
		} else {
			uint64_t new_val;

			assert(c >= '0' && c <= '9');
			new_val = *pval * 10 + c - '0';
			if (new_val < *pval) {
				return -ERANGE;
			}

			if (state == NUM_STATE_FRAC) {
				frac_digits++;
			}

			*pval = new_val;
		}
	}

	if (exponent_negative) {
		if (exponent_u64 > 9223372036854775808ULL) { /* abs(INT64_MIN) */
			return -ERANGE;
		}
		num->exponent = (int64_t) - exponent_u64;
	} else {
		if (exponent_u64 > INT64_MAX) {
			return -ERANGE;
		}
		num->exponent = exponent_u64;
	}
	num->exponent -= frac_digits;

	/* Apply as much of the exponent as possible without overflow or truncation */
	if (num->exponent < 0) {
		while (num->exponent && num->significand >= 10 && num->significand % 10 == 0) {
			num->significand /= 10;
			num->exponent++;
		}
	} else { /* positive exponent */
		while (num->exponent) {
			uint64_t new_val = num->significand * 10;

			if (new_val < num->significand) {
				break;
			}

			num->significand = new_val;
			num->exponent--;
		}
	}

	return 0;
}

int
spdk_json_number_to_uint16(const struct spdk_json_val *val, uint16_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = spdk_json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;
	}

	if (split_num.significand > UINT16_MAX) {
		return -ERANGE;
	}
	*num = (uint16_t)split_num.significand;
	return 0;
}

int
spdk_json_number_to_int32(const struct spdk_json_val *val, int32_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = spdk_json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent) {
		return -ERANGE;
	}

	if (split_num.negative) {
		if (split_num.significand > 2147483648) { /* abs(INT32_MIN) */
			return -ERANGE;
		}
		*num = (int32_t) - (int64_t)split_num.significand;
		return 0;
	}

	/* positive */
	if (split_num.significand > INT32_MAX) {
		return -ERANGE;
	}
	*num = (int32_t)split_num.significand;
	return 0;
}

int
spdk_json_number_to_uint32(const struct spdk_json_val *val, uint32_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = spdk_json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;
	}

	if (split_num.significand > UINT32_MAX) {
		return -ERANGE;
	}
	*num = (uint32_t)split_num.significand;
	return 0;
}

int
spdk_json_number_to_uint64(const struct spdk_json_val *val, uint64_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = spdk_json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;
	}

	*num = split_num.significand;
	return 0;
}

int
spdk_json_decode_object(const struct spdk_json_val *values,
			const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out)
{
	uint32_t i;
	bool invalid = false;
	size_t decidx;
	bool *seen;

	if (values == NULL || values->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -1;
	}

	seen = calloc(sizeof(bool), num_decoders);
	if (seen == NULL) {
		return -1;
	}

	for (i = 0; i < values->len;) {
		const struct spdk_json_val *name = &values[i + 1];
		const struct spdk_json_val *v = &values[i + 2];
		bool found = false;

		for (decidx = 0; decidx < num_decoders; decidx++) {
			const struct spdk_json_object_decoder *dec = &decoders[decidx];
			if (spdk_json_strequal(name, dec->name)) {
				void *field = (void *)((uintptr_t)out + dec->offset);

				found = true;

				if (seen[decidx]) {
					/* duplicate field name */
					invalid = true;
				} else {
					seen[decidx] = true;
					if (dec->decode_func(v, field)) {
						invalid = true;
						/* keep going to fill out any other valid keys */
					}
				}
				break;
			}
		}

		if (!found) {
			invalid = true;
		}

		i += 1 + spdk_json_val_len(v);
	}

	for (decidx = 0; decidx < num_decoders; decidx++) {
		if (!decoders[decidx].optional && !seen[decidx]) {
			/* required field is missing */
			invalid = true;
			break;
		}
	}

	free(seen);
	return invalid ? -1 : 0;
}

int
spdk_json_decode_array(const struct spdk_json_val *values, spdk_json_decode_fn decode_func,
		       void *out, size_t max_size, size_t *out_size, size_t stride)
{
	uint32_t i;
	char *field;
	char *out_end;

	if (values == NULL || values->type != SPDK_JSON_VAL_ARRAY_BEGIN) {
		return -1;
	}

	*out_size = 0;
	field = out;
	out_end = field + max_size * stride;
	for (i = 0; i < values->len;) {
		const struct spdk_json_val *v = &values[i + 1];

		if (field == out_end) {
			return -1;
		}

		if (decode_func(v, field)) {
			return -1;
		}

		i += spdk_json_val_len(v);
		field += stride;
		(*out_size)++;
	}

	return 0;
}

int
spdk_json_decode_bool(const struct spdk_json_val *val, void *out)
{
	bool *f = out;

	if (val->type != SPDK_JSON_VAL_TRUE && val->type != SPDK_JSON_VAL_FALSE) {
		return -1;
	}

	*f = val->type == SPDK_JSON_VAL_TRUE;
	return 0;
}

int
spdk_json_decode_uint16(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;

	return spdk_json_number_to_uint16(val, i);
}

int
spdk_json_decode_int32(const struct spdk_json_val *val, void *out)
{
	int32_t *i = out;

	return spdk_json_number_to_int32(val, i);
}

int
spdk_json_decode_uint32(const struct spdk_json_val *val, void *out)
{
	uint32_t *i = out;

	return spdk_json_number_to_uint32(val, i);
}

int
spdk_json_decode_uint64(const struct spdk_json_val *val, void *out)
{
	uint64_t *i = out;

	return spdk_json_number_to_uint64(val, i);
}

int
spdk_json_decode_string(const struct spdk_json_val *val, void *out)
{
	char **s = out;

	free(*s);

	*s = spdk_json_strdup(val);

	if (*s) {
		return 0;
	} else {
		return -1;
	}
}
