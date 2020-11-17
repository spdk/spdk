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
#include "spdk/env.h"
#include "spdk/file.h"
#include "spdk/base64.h"
#include "spdk/json.h"

#define DEFAULT_RUNTIME 30 /* seconds */
#define MAX_RUNTIME_S 86400 /* 24 hours */
#define IO_TIMEOUT_S 5

#define UNSIGNED_2BIT_MAX ((1 << 2) - 1)
#define UNSIGNED_4BIT_MAX ((1 << 4) - 1)
#define UNSIGNED_8BIT_MAX ((1 << 8) - 1)

typedef bool (*json_parse_fn)(void *ele, struct spdk_json_val *val, size_t num_vals);

static void
fuzz_fill_random_bytes(char *character_repr, size_t len, unsigned int *rand_seed)
{
	size_t i;

	for (i = 0; i < len; i++) {
		character_repr[i] = rand_r(rand_seed) % UINT8_MAX;
	}
}

static uint64_t
fuzz_refresh_timeout(void)
{
	uint64_t current_ticks;
	uint64_t new_timeout_ticks;

	current_ticks = spdk_get_ticks();

	new_timeout_ticks = current_ticks + IO_TIMEOUT_S * spdk_get_ticks_hz();
	assert(new_timeout_ticks > current_ticks);

	return new_timeout_ticks;
}

static char *
fuzz_get_value_base_64_buffer(void *item, size_t len)
{
	char *value_string;
	size_t total_size;
	int rc;

	/* Null pointer */
	total_size = spdk_base64_get_encoded_strlen(len) + 1;

	value_string = calloc(1, total_size);
	if (value_string == NULL) {
		return NULL;
	}

	rc = spdk_base64_encode(value_string, item, len);
	if (rc < 0) {
		free(value_string);
		return NULL;
	}

	return value_string;
}

static int
fuzz_get_base_64_buffer_value(void *item, size_t len, char *buf, size_t buf_len)
{
	size_t size_of_data;
	char *new_buf;
	int rc;

	new_buf = malloc(buf_len + 1);
	if (new_buf == NULL) {
		return -ENOMEM;
	}

	snprintf(new_buf, buf_len + 1, "%s", buf);

	size_of_data = spdk_base64_get_decoded_len(buf_len);

	if (size_of_data < len) {
		free(new_buf);
		return -EINVAL;
	}

	rc = spdk_base64_decode(item, &size_of_data, new_buf);

	if (rc || size_of_data != len) {
		free(new_buf);
		return -EINVAL;
	}

	free(new_buf);
	return 0;
}

static ssize_t
read_json_into_buffer(const char *filename, struct spdk_json_val **values, void **file_data)
{
	FILE *file = fopen(filename, "r");
	size_t file_data_size;
	ssize_t num_json_values = 0, rc;

	if (file == NULL) {
		/* errno is set by fopen */
		return 0;
	}

	*file_data = spdk_posix_file_load(file, &file_data_size);
	if (*file_data == NULL) {
		fclose(file);
		return 0;
	}

	fclose(file);

	num_json_values = spdk_json_parse(*file_data, file_data_size, NULL, 0, NULL,
					  SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);

	*values = calloc(num_json_values, sizeof(**values));
	if (values == NULL) {
		free(*file_data);
		*file_data = NULL;
		return 0;
	}

	rc = spdk_json_parse(*file_data, file_data_size, *values, num_json_values, NULL,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (num_json_values != rc) {
		free(*values);
		*values = NULL;
		free(*file_data);
		*file_data = NULL;
		return 0;
	}

	return num_json_values;
}

static size_t
double_arr_size(void **buffer, size_t num_ele, size_t ele_size)
{
	void *tmp;
	size_t new_num_ele, allocation_size;

	if (num_ele > SIZE_MAX / 2) {
		return 0;
	}

	new_num_ele = num_ele * 2;

	if (new_num_ele > SIZE_MAX / ele_size) {
		return 0;
	}

	allocation_size = new_num_ele * ele_size;

	tmp = realloc(*buffer, allocation_size);
	if (tmp != NULL) {
		*buffer = tmp;
		return new_num_ele;
	}

	return 0;
}

static uint64_t
fuzz_parse_args_into_array(const char *file, void **arr, size_t ele_size, const char *obj_name,
			   json_parse_fn cb_fn)
{
	ssize_t i, num_json_values;
	struct spdk_json_val *values = NULL, *values_head = NULL, *obj_start;
	void *file_data = NULL;;
	char *arr_idx_pointer;
	size_t num_arr_elements, arr_elements_used, values_in_obj;
	bool rc;

	num_json_values = read_json_into_buffer(file, &values_head, &file_data);
	values = values_head;
	if (num_json_values == 0 || values == NULL) {
		if (file_data != NULL) {
			free(file_data);
		}
		fprintf(stderr, "The file provided does not exist or we were unable to parse it.\n");
		return 0;
	}

	num_arr_elements = 10;
	arr_elements_used = 0;
	*arr = calloc(num_arr_elements, ele_size);
	arr_idx_pointer = (char *)*arr;
	if (arr_idx_pointer == NULL) {
		free(values);
		free(file_data);
		return 0;
	}

	i = 0;
	while (i < num_json_values) {
		if (values->type != SPDK_JSON_VAL_NAME) {
			i++;
			values++;
			continue;
		}

		if (!strncmp(values->start, obj_name, values->len)) {
			i++;
			values++;
			assert(values->type == SPDK_JSON_VAL_OBJECT_BEGIN);
			obj_start = values;
			values_in_obj = spdk_json_val_len(obj_start);
			values += values_in_obj;
			i += values_in_obj;

			rc = cb_fn((void *)arr_idx_pointer, obj_start, values_in_obj);
			if (rc == false) {
				fprintf(stderr, "failed to parse file after %zu elements.\n", arr_elements_used);
				goto fail;
			}

			arr_idx_pointer += ele_size;
			arr_elements_used++;
			if (arr_elements_used == num_arr_elements) {
				num_arr_elements = double_arr_size(arr, num_arr_elements, ele_size);
				if (num_arr_elements == 0) {
					fprintf(stderr, "failed to allocate enough space for all json elements in your file.\n");
					goto fail;
				} else {
					/* reset the array element position in case the pointer changed. */
					arr_idx_pointer = ((char *)*arr) + arr_elements_used * ele_size;
				}
			}

			continue;
		} else {
			i++;
			values++;
			continue;
		}
	}

	if (arr_elements_used == 0) {
		goto fail;
	}

	free(values_head);
	free(file_data);
	return arr_elements_used;
fail:
	free(values_head);
	free(file_data);
	free(*arr);
	*arr = NULL;
	return 0;
}

static int
fuzz_parse_json_num(struct spdk_json_val *val, uint64_t max_val, uint64_t *val_ptr)
{
	uint64_t tmp_val;
	int rc;

	rc = spdk_json_number_to_uint64(val, &tmp_val);
	if (rc || tmp_val > max_val) {
		return -EINVAL;
	} else {
		*val_ptr = tmp_val;
		return 0;
	}
}
