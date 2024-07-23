/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/file.h"
#include "spdk/string.h"

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

static int
read_sysfs_attribute(char **attribute_p, const char *format, va_list args)
{
	char *attribute;
	FILE *file;
	char *path;
	size_t len = 0;
	ssize_t read;
	int errsv;

	path = spdk_vsprintf_alloc(format, args);
	if (path == NULL) {
		return -ENOMEM;
	}

	file = fopen(path, "r");
	errsv = errno;
	free(path);
	if (file == NULL) {
		assert(errsv != 0);
		return -errsv;
	}

	*attribute_p = NULL;
	read = getline(attribute_p, &len, file);
	errsv = errno;
	fclose(file);
	attribute = *attribute_p;
	if (read == -1) {
		/* getline man page says line should be freed even on failure. */
		free(attribute);
		assert(errsv != 0);
		return -errsv;
	}

	/* len is the length of the allocated buffer, which may be more than
	 * the string's length. Reuse len to hold the actual strlen.
	 */
	len = strlen(attribute);
	if (attribute[len - 1] == '\n') {
		attribute[len - 1] = '\0';
	}

	return 0;
}

int
spdk_read_sysfs_attribute(char **attribute_p, const char *path_format, ...)
{
	va_list args;
	int rc;

	va_start(args, path_format);
	rc = read_sysfs_attribute(attribute_p, path_format, args);
	va_end(args);

	return rc;
}

int
spdk_read_sysfs_attribute_uint32(uint32_t *attribute, const char *path_format, ...)
{
	char *attribute_str = NULL;
	long long int val;
	va_list args;
	int rc;

	va_start(args, path_format);
	rc = read_sysfs_attribute(&attribute_str, path_format, args);
	va_end(args);

	if (rc != 0) {
		return rc;
	}

	val = spdk_strtoll(attribute_str, 0);
	free(attribute_str);
	if (val < 0 || val > UINT32_MAX) {
		return -EINVAL;
	}

	*attribute = (uint32_t)val;
	return 0;
}
