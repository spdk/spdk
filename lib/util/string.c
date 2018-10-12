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

#include "spdk/string.h"

char *
spdk_vsprintf_alloc(const char *format, va_list args)
{
	va_list args_copy;
	char *buf;
	size_t bufsize;
	int rc;

	/* Try with a small buffer first. */
	bufsize = 32;

	/* Limit maximum buffer size to something reasonable so we don't loop forever. */
	while (bufsize <= 1024 * 1024) {
		buf = malloc(bufsize);
		if (buf == NULL) {
			return NULL;
		}

		va_copy(args_copy, args);
		rc = vsnprintf(buf, bufsize, format, args_copy);
		va_end(args_copy);

		/*
		 * If vsnprintf() returned a count within our current buffer size, we are done.
		 * The count does not include the \0 terminator, so rc == bufsize is not OK.
		 */
		if (rc >= 0 && (size_t)rc < bufsize) {
			return buf;
		}

		/*
		 * vsnprintf() should return the required space, but some libc versions do not
		 * implement this correctly, so just double the buffer size and try again.
		 *
		 * We don't need the data in buf, so rather than realloc(), use free() and malloc()
		 * again to avoid a copy.
		 */
		free(buf);
		bufsize *= 2;
	}

	return NULL;
}

char *
spdk_sprintf_alloc(const char *format, ...)
{
	va_list args;
	char *ret;

	va_start(args, format);
	ret = spdk_vsprintf_alloc(format, args);
	va_end(args);

	return ret;
}

char *
spdk_strlwr(char *s)
{
	char *p;

	if (s == NULL) {
		return NULL;
	}

	p = s;
	while (*p != '\0') {
		*p = tolower(*p);
		p++;
	}

	return s;
}

char *
spdk_strsepq(char **stringp, const char *delim)
{
	char *p, *q, *r;
	int quoted = 0, bslash = 0;

	p = *stringp;
	if (p == NULL) {
		return NULL;
	}

	r = q = p;
	while (*q != '\0' && *q != '\n') {
		/* eat quoted characters */
		if (bslash) {
			bslash = 0;
			*r++ = *q++;
			continue;
		} else if (quoted) {
			if (quoted == '"' && *q == '\\') {
				bslash = 1;
				q++;
				continue;
			} else if (*q == quoted) {
				quoted = 0;
				q++;
				continue;
			}
			*r++ = *q++;
			continue;
		} else if (*q == '\\') {
			bslash = 1;
			q++;
			continue;
		} else if (*q == '"' || *q == '\'') {
			quoted = *q;
			q++;
			continue;
		}

		/* separator? */
		if (strchr(delim, *q) == NULL) {
			*r++ = *q++;
			continue;
		}

		/* new string */
		q++;
		break;
	}
	*r = '\0';

	/* skip tailer */
	while (*q != '\0' && strchr(delim, *q) != NULL) {
		q++;
	}
	if (*q != '\0') {
		*stringp = q;
	} else {
		*stringp = NULL;
	}

	return p;
}

char *
spdk_str_trim(char *s)
{
	char *p, *q;

	if (s == NULL) {
		return NULL;
	}

	/* remove header */
	p = s;
	while (*p != '\0' && isspace(*p)) {
		p++;
	}

	/* remove tailer */
	q = p + strlen(p);
	while (q - 1 >= p && isspace(*(q - 1))) {
		q--;
		*q = '\0';
	}

	/* if remove header, move */
	if (p != s) {
		q = s;
		while (*p != '\0') {
			*q++ = *p++;
		}
		*q = '\0';
	}

	return s;
}

void
spdk_strcpy_pad(void *dst, const char *src, size_t size, int pad)
{
	size_t len;

	len = strlen(src);
	if (len < size) {
		memcpy(dst, src, len);
		memset((char *)dst + len, pad, size - len);
	} else {
		memcpy(dst, src, size);
	}
}

size_t
spdk_strlen_pad(const void *str, size_t size, int pad)
{
	const uint8_t *start;
	const uint8_t *iter;
	uint8_t pad_byte;

	pad_byte = (uint8_t)pad;
	start = (const uint8_t *)str;

	if (size == 0) {
		return 0;
	}

	iter = start + size - 1;
	while (1) {
		if (*iter != pad_byte) {
			return iter - start + 1;
		}

		if (iter == start) {
			/* Hit the start of the string finding only pad_byte. */
			return 0;
		}
		iter--;
	}
}

int
spdk_parse_ip_addr(char *ip, char **host, char **port)
{
	char *p;

	if (ip == NULL) {
		return -EINVAL;
	}

	*host = NULL;
	*port = NULL;

	if (ip[0] == '[') {
		/* IPv6 */
		p = strchr(ip, ']');
		if (p == NULL) {
			return -EINVAL;
		}
		*host = &ip[1];
		*p = '\0';

		p++;
		if (*p == '\0') {
			return 0;
		} else if (*p != ':') {
			return -EINVAL;
		}

		p++;
		if (*p == '\0') {
			return 0;
		}

		*port = p;
	} else {
		/* IPv4 */
		p = strchr(ip, ':');
		if (p == NULL) {
			*host = ip;
			return 0;
		}

		*host = ip;
		*p = '\0';

		p++;
		if (*p == '\0') {
			return 0;
		}

		*port = p;
	}

	return 0;
}

size_t
spdk_str_chomp(char *s)
{
	size_t len = strlen(s);
	size_t removed = 0;

	while (len > 0) {
		if (s[len - 1] != '\r' && s[len - 1] != '\n') {
			break;
		}

		s[len - 1] = '\0';
		len--;
		removed++;
	}

	return removed;
}

void
spdk_strerror_r(int errnum, char *buf, size_t buflen)
{
	int rc;

#if defined(__USE_GNU)
	char *new_buffer;
	new_buffer = strerror_r(errnum, buf, buflen);
	if (new_buffer != NULL) {
		snprintf(buf, buflen, "%s", new_buffer);
		rc = 0;
	} else {
		rc = 1;
	}
#else
	rc = strerror_r(errnum, buf, buflen);
#endif

	if (rc != 0) {
		snprintf(buf, buflen, "Unknown error %d", errnum);
	}
}

int
spdk_parse_capacity(const char *cap_str, uint64_t *cap, bool *has_prefix)
{
	int rc;
	char bin_prefix;

	rc = sscanf(cap_str, "%"SCNu64"%c", cap, &bin_prefix);
	if (rc == 1) {
		*has_prefix = false;
		return 0;
	} else if (rc == 0) {
		if (errno == 0) {
			/* No scanf matches - the string does not start with a digit */
			return -EINVAL;
		} else {
			/* Parsing error */
			return -errno;
		}
	}

	*has_prefix = true;
	switch (bin_prefix) {
	case 'k':
	case 'K':
		*cap *= 1024;
		break;
	case 'm':
	case 'M':
		*cap *= 1024 * 1024;
		break;
	case 'g':
	case 'G':
		*cap *= 1024 * 1024 * 1024;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

bool
spdk_mem_all_zero(const void *data, size_t size)
{
	const uint8_t *buf = data;

	while (size--) {
		if (*buf++ != 0) {
			return false;
		}
	}

	return true;
}
