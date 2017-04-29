/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/string.h"
#include "spdk/log.h"

#define CF_DELIM " \t"

#define LIB_MAX_TMPBUF 1024

static int
parse_line(struct spdk_conf *cp, char *lp)
{
	struct spdk_conf_section *sp;
	struct spdk_conf_item *ip;
	struct spdk_conf_value *vp;
	char *arg;
	char *key;
	char *val;
	char *p;
	int num;

	arg = spdk_str_trim(lp);
	if (arg == NULL) {
		SPDK_ERRLOG("no section\n");
		return -1;
	}

	if (arg[0] == '[') {
		/* section */
		arg++;
		key = spdk_strsepq(&arg, "]");
		if (key == NULL || arg != NULL) {
			SPDK_ERRLOG("broken section\n");
			return -1;
		}
		/* determine section number */
		for (p = key; *p != '\0' && !isdigit((int) *p); p++)
			;
		if (*p != '\0') {
			num = (int)strtol(p, NULL, 10);
		} else {
			num = 0;
		}

		sp = spdk_conf_find_section(cp, key);
		if (sp == NULL) {
			sp = allocate_cf_section();
			append_cf_section(cp, sp);
		}
		cp->current_section = sp;
		sp->name = spdk_strdup(key);
		if (sp->name == NULL) {
			SPDK_ERRLOG("spdk_strdup sp->name");
			return -1;
		}

		sp->num = num;
	} else {
		/* parameters */
		sp = cp->current_section;
		if (sp == NULL) {
			SPDK_ERRLOG("unknown section\n");
			return -1;
		}
		key = spdk_strsepq(&arg, CF_DELIM);
		if (key == NULL) {
			SPDK_ERRLOG("broken key\n");
			return -1;
		}

		ip = allocate_cf_item();
		if (ip == NULL) {
			SPDK_ERRLOG("cannot allocate cf item\n");
			return -1;
		}
		append_cf_item(sp, ip);
		ip->key = spdk_strdup(key);
		if (ip->key == NULL) {
			SPDK_ERRLOG("spdk_strdup ip->key");
			return -1;
		}
		ip->val = NULL;
		if (arg != NULL) {
			/* key has value(s) */
			while (arg != NULL) {
				val = spdk_strsepq(&arg, CF_DELIM);
				vp = allocate_cf_value();
				if (vp == NULL) {
					SPDK_ERRLOG("cannot allocate cf value\n");
					return -1;
				}
				append_cf_value(ip, vp);
				vp->value = spdk_strdup(val);
				if (vp->value == NULL) {
					SPDK_ERRLOG("spdk_strdup vp->value");
					return -1;
				}
			}
		}
	}

	return 0;
}

static char *
fgets_line(FILE *fp)
{
	char *dst, *dst2, *p;
	size_t total, len;

	dst = p = spdk_malloc(LIB_MAX_TMPBUF);
	if (!dst) {
		return NULL;
	}

	dst[0] = '\0';
	total = 0;

	while (fgets(p, LIB_MAX_TMPBUF, fp) != NULL) {
		len = strlen(p);
		total += len;
		if (len + 1 < LIB_MAX_TMPBUF || dst[total - 1] == '\n') {
			dst2 = spdk_realloc(dst, total + 1);
			if (!dst2) {
				spdk_free(dst);
				return NULL;
			} else {
				return dst2;
			}
		}

		dst2 = spdk_realloc(dst, total + LIB_MAX_TMPBUF);
		if (!dst2) {
			spdk_free(dst);
			return NULL;
		} else {
			dst = dst2;
		}

		p = dst + total;
	}

	if (feof(fp) && total != 0) {
		dst2 = spdk_realloc(dst, total + 2);
		if (!dst2) {
			spdk_free(dst);
			return NULL;
		} else {
			dst = dst2;
		}

		dst[total] = '\n';
		dst[total + 1] = '\0';
		return dst;
	}

	spdk_free(dst);

	return NULL;
}

int
spdk_conf_read(struct spdk_conf *cp, const char *file)
{
	FILE *fp;
	char *lp, *p;
	char *lp2, *q;
	int line;
	int n, n2;

	if (file == NULL || file[0] == '\0') {
		return -1;
	}

	fp = fopen(file, "r");
	if (fp == NULL) {
		SPDK_ERRLOG("open error: %s\n", file);
		return -1;
	}

	cp->file = spdk_strdup(file);
	if (cp->file == NULL) {
		SPDK_ERRLOG("spdk_strdup cp->file");
		fclose(fp);
		return -1;
	}

	line = 1;
	while ((lp = fgets_line(fp)) != NULL) {
		/* skip spaces */
		for (p = lp; *p != '\0' && isspace((int) *p); p++)
			;
		/* skip comment, empty line */
		if (p[0] == '#' || p[0] == '\0') {
			goto next_line;
		}

		/* concatenate line end with '\' */
		n = strlen(p);
		while (n > 2 && p[n - 1] == '\n' && p[n - 2] == '\\') {
			n -= 2;
			lp2 = fgets_line(fp);
			if (lp2 == NULL) {
				break;
			}

			line++;
			n2 = strlen(lp2);

			q = spdk_malloc(n + n2 + 1);
			if (!q) {
				spdk_free(lp2);
				spdk_free(lp);
				SPDK_ERRLOG("spdk_malloc failed at line %d of %s\n", line, cp->file);
				fclose(fp);
				return -1;
			}

			memcpy(q, p, n);
			memcpy(q + n, lp2, n2);
			q[n + n2] = '\0';
			spdk_free(lp2);
			spdk_free(lp);
			p = lp = q;
			n += n2;
		}

		/* parse one line */
		if (parse_line(cp, p) < 0) {
			SPDK_ERRLOG("parse error at line %d of %s\n", line, cp->file);
		}
next_line:
		line++;
		spdk_free(lp);
	}

	fclose(fp);
	return 0;
}
