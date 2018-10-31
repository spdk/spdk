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

#include "spdk/stdinc.h"
#include "spdk/json.h"

#define DEFAULT_RUNTIME 30 /* seconds */
#define MAX_RUNTIME_S 86400 /* 24 hours */
#define IO_TIMEOUT_S 5

#define UNSIGNED_2BIT_MAX ((1 << 2) - 1)
#define UNSIGNED_4BIT_MAX ((1 << 4) - 1)
#define UNSIGNED_8BIT_MAX ((1 << 8) - 1)

typedef bool (*json_parse_fn)(void *ele, struct spdk_json_val *val, size_t num_vals);

/* Fill a buffer of given size with random bytes. Uses the reentrant version of rand. */
void fuzz_fill_random_bytes(char *character_repr, size_t len, unsigned int *rand_seed);

uint64_t fuzz_refresh_timeout(void);

char *fuzz_get_value_base_64_buffer(void *item, size_t len);

int fuzz_get_base_64_buffer_value(void *item, size_t len, char *buf, size_t buf_len);

void fuzz_free_base_64_buffer(char *buf);

uint64_t fuzz_parse_args_into_array(const char *file, void **arr, size_t ele_size,
				    const char *obj_name, json_parse_fn cb_fn);

int fuzz_parse_json_num(struct spdk_json_val *val, uint64_t max_value, uint64_t *ptr);
