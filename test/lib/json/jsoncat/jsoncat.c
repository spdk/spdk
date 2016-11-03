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

/* Simple JSON "cat" utility */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "spdk/json.h"

static void
usage(const char *prog)
{
	printf("usage: %s [-c] [-f] file.json\n", prog);
	printf("Options:\n");
	printf("-c\tallow comments in input (non-standard)\n");
	printf("-f\tformatted output (default: compact output)\n");
}

static void
print_json_error(FILE *pf, int rc, const char *filename)
{
	switch (rc) {
	case SPDK_JSON_PARSE_INVALID:
		fprintf(pf, "%s: invalid JSON\n", filename);
		break;
	case SPDK_JSON_PARSE_INCOMPLETE:
		fprintf(pf, "%s: incomplete JSON\n", filename);
		break;
	case SPDK_JSON_PARSE_MAX_DEPTH_EXCEEDED:
		fprintf(pf, "%s: maximum nesting depth exceeded\n", filename);
		break;
	default:
		fprintf(pf, "%s: unknown JSON parse error\n", filename);
		break;
	}
}

static int
json_write_cb(void *cb_ctx, const void *data, size_t size)
{
	FILE *f = cb_ctx;
	size_t rc;

	rc = fwrite(data, 1, size, f);
	return rc == size ? 0 : -1;
}

static void *
read_file(FILE *f, size_t *psize)
{
	void *buf, *newbuf;
	size_t cur_size, buf_size, rc;

	buf = NULL;
	cur_size = 0;
	buf_size = 128 * 1024;

	while (buf_size <= 1024 * 1024 * 1024) {
		newbuf = realloc(buf, buf_size);
		if (newbuf == NULL) {
			free(buf);
			return NULL;
		}
		buf = newbuf;

		rc = fread(buf + cur_size, 1, buf_size - cur_size, f);
		cur_size += rc;

		if (feof(f)) {
			*psize = cur_size;
			return buf;
		}

		if (ferror(f)) {
			free(buf);
			return NULL;
		}

		buf_size *= 2;
	}

	free(buf);
	return NULL;
}

static int
process_file(const char *filename, FILE *f, uint32_t parse_flags, uint32_t write_flags)
{
	size_t size;
	void *buf, *end;
	ssize_t rc;
	struct spdk_json_val *values;
	size_t num_values;
	struct spdk_json_write_ctx *w;

	buf = read_file(f, &size);
	if (buf == NULL) {
		fprintf(stderr, "%s: file read error\n", filename);
		return 1;
	}

	rc = spdk_json_parse(buf, size, NULL, 0, NULL, parse_flags);
	if (rc <= 0) {
		print_json_error(stderr, rc, filename);
		free(buf);
		return 1;
	}

	num_values = (size_t)rc;
	values = calloc(num_values, sizeof(*values));
	if (values == NULL) {
		perror("values calloc");
		free(buf);
		return 1;
	}

	rc = spdk_json_parse(buf, size, values, num_values, &end,
			     parse_flags | SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (rc <= 0) {
		print_json_error(stderr, rc, filename);
		free(values);
		free(buf);
		return 1;
	}

	w = spdk_json_write_begin(json_write_cb, stdout, write_flags);
	if (w == NULL) {
		fprintf(stderr, "json_write_begin failed\n");
		free(values);
		free(buf);
		return 1;
	}

	spdk_json_write_val(w, values);
	spdk_json_write_end(w);
	printf("\n");

	if (end != buf + size) {
		fprintf(stderr, "%s: garbage at end of file\n", filename);
		free(values);
		free(buf);
		return 1;
	}

	free(values);
	free(buf);
	return 0;
}

int
main(int argc, char **argv)
{
	FILE *f;
	int ch;
	int rc;
	uint32_t parse_flags = 0, write_flags = 0;
	const char *filename;

	while ((ch = getopt(argc, argv, "cf")) != -1) {
		switch (ch) {
		case 'c':
			parse_flags |= SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS;
			break;
		case 'f':
			write_flags |= SPDK_JSON_WRITE_FLAG_FORMATTED;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc) {
		filename = "-";
	} else if (optind == argc - 1) {
		filename = argv[optind];
	} else {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(filename, "-") == 0) {
		f = stdin;
	} else {
		f = fopen(filename, "r");
		if (f == NULL) {
			perror("fopen");
			return 1;
		}
	}

	rc = process_file(filename, f, parse_flags, write_flags);

	if (f != stdin) {
		fclose(f);
	}

	return rc;
}
