/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/file.h"

void *
spdk_posix_file_load(FILE *file, size_t *size)
{
	uint8_t *newbuf, *buf = NULL;
	size_t rc, buf_size, cur_size = 0;

	*size = 0;
	buf_size = 128 * 1024;

	while (buf_size <= 1024 * 1024 * 1024) {
		newbuf = realloc(buf, buf_size);
		if (newbuf == NULL) {
			free(buf);
			return NULL;
		}
		buf = newbuf;

		rc = fread(buf + cur_size, 1, buf_size - cur_size, file);
		cur_size += rc;

		if (feof(file)) {
			*size = cur_size;
			return buf;
		}

		if (ferror(file)) {
			free(buf);
			return NULL;
		}

		buf_size *= 2;
	}

	free(buf);
	return NULL;
}

void *
spdk_posix_file_load_from_name(const char *file_name, size_t *size)
{
	FILE *file = fopen(file_name, "r");
	void *data;

	if (file == NULL) {
		return NULL;
	}

	data = spdk_posix_file_load(file, size);
	fclose(file);

	return data;
}
