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

#include "spdk/stdinc.h"

#include "spdk/conf.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct spdk_conf_value {
	struct spdk_conf_value *next;
	char *value;
};

struct spdk_conf_item {
	struct spdk_conf_item *next;
	char *key;
	struct spdk_conf_value *val;
};

struct spdk_conf_section {
	struct spdk_conf_section *next;
	char *name;
	int num;
	struct spdk_conf_item *item;
};

struct spdk_conf {
	char *file;
	struct spdk_conf_section *current_section;
	struct spdk_conf_section *section;
	bool merge_sections;
};

#define CF_DELIM " \t"
#define CF_DELIM_KEY " \t="

#define LIB_MAX_TMPBUF 1024

static struct spdk_conf *default_config = NULL;

struct spdk_conf *
spdk_conf_allocate(void)
{
	struct spdk_conf *ret = calloc(1, sizeof(struct spdk_conf));

	if (ret) {
		ret->merge_sections = true;
	}

	return ret;
}

static void
free_conf_value(struct spdk_conf_value *vp)
{
	if (vp == NULL) {
		return;
	}

	if (vp->value) {
		free(vp->value);
	}

	free(vp);
}

static void
free_all_conf_value(struct spdk_conf_value *vp)
{
	struct spdk_conf_value *next;

	if (vp == NULL) {
		return;
	}

	while (vp != NULL) {
		next = vp->next;
		free_conf_value(vp);
		vp = next;
	}
}

static void
free_conf_item(struct spdk_conf_item *ip)
{
	if (ip == NULL) {
		return;
	}

	if (ip->val != NULL) {
		free_all_conf_value(ip->val);
	}

	if (ip->key != NULL) {
		free(ip->key);
	}

	free(ip);
}

static void
free_all_conf_item(struct spdk_conf_item *ip)
{
	struct spdk_conf_item *next;

	if (ip == NULL) {
		return;
	}

	while (ip != NULL) {
		next = ip->next;
		free_conf_item(ip);
		ip = next;
	}
}

static void
free_conf_section(struct spdk_conf_section *sp)
{
	if (sp == NULL) {
		return;
	}

	if (sp->item) {
		free_all_conf_item(sp->item);
	}

	if (sp->name) {
		free(sp->name);
	}

	free(sp);
}

static void
free_all_conf_section(struct spdk_conf_section *sp)
{
	struct spdk_conf_section *next;

	if (sp == NULL) {
		return;
	}

	while (sp != NULL) {
		next = sp->next;
		free_conf_section(sp);
		sp = next;
	}
}

void
spdk_conf_free(struct spdk_conf *cp)
{
	if (cp == NULL) {
		return;
	}

	if (cp->section != NULL) {
		free_all_conf_section(cp->section);
	}

	if (cp->file != NULL) {
		free(cp->file);
	}

	free(cp);
}

static struct spdk_conf_section *
allocate_cf_section(void)
{
	return calloc(1, sizeof(struct spdk_conf_section));
}

static struct spdk_conf_item *
allocate_cf_item(void)
{
	return calloc(1, sizeof(struct spdk_conf_item));
}

static struct spdk_conf_value *
allocate_cf_value(void)
{
	return calloc(1, sizeof(struct spdk_conf_value));
}


#define CHECK_CP_OR_USE_DEFAULT(cp) (((cp) == NULL) && (default_config != NULL)) ? default_config : (cp)

struct spdk_conf_section *
spdk_conf_find_section(struct spdk_conf *cp, const char *name)
{
	struct spdk_conf_section *sp;

	if (name == NULL || name[0] == '\0') {
		return NULL;
	}

	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	if (cp == NULL) {
		return NULL;
	}

	for (sp = cp->section; sp != NULL; sp = sp->next) {
		if (sp->name != NULL && sp->name[0] == name[0]
		    && strcasecmp(sp->name, name) == 0) {
			return sp;
		}
	}

	return NULL;
}

struct spdk_conf_section *
spdk_conf_first_section(struct spdk_conf *cp)
{
	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	if (cp == NULL) {
		return NULL;
	}

	return cp->section;
}

struct spdk_conf_section *
spdk_conf_next_section(struct spdk_conf_section *sp)
{
	if (sp == NULL) {
		return NULL;
	}

	return sp->next;
}

static void
append_cf_section(struct spdk_conf *cp, struct spdk_conf_section *sp)
{
	struct spdk_conf_section *last;

	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	if (cp == NULL) {
		SPDK_ERRLOG("cp == NULL\n");
		return;
	}

	if (cp->section == NULL) {
		cp->section = sp;
		return;
	}

	for (last = cp->section; last->next != NULL; last = last->next)
		;
	last->next = sp;
}

static struct spdk_conf_item *
find_cf_nitem(struct spdk_conf_section *sp, const char *key, int idx)
{
	struct spdk_conf_item *ip;
	int i;

	if (key == NULL || key[0] == '\0') {
		return NULL;
	}

	i = 0;
	for (ip = sp->item; ip != NULL; ip = ip->next) {
		if (ip->key != NULL && ip->key[0] == key[0]
		    && strcasecmp(ip->key, key) == 0) {
			if (i == idx) {
				return ip;
			}
			i++;
		}
	}

	return NULL;
}

static void
append_cf_item(struct spdk_conf_section *sp, struct spdk_conf_item *ip)
{
	struct spdk_conf_item *last;

	if (sp == NULL) {
		return;
	}

	if (sp->item == NULL) {
		sp->item = ip;
		return;
	}

	for (last = sp->item; last->next != NULL; last = last->next)
		;
	last->next = ip;
}

static void
append_cf_value(struct spdk_conf_item *ip, struct spdk_conf_value *vp)
{
	struct spdk_conf_value *last;

	if (ip == NULL) {
		return;
	}

	if (ip->val == NULL) {
		ip->val = vp;
		return;
	}

	for (last = ip->val; last->next != NULL; last = last->next)
		;
	last->next = vp;
}

bool
spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix)
{
	return strncasecmp(sp->name, name_prefix, strlen(name_prefix)) == 0;
}

const char *
spdk_conf_section_get_name(const struct spdk_conf_section *sp)
{
	return sp->name;
}

int
spdk_conf_section_get_num(const struct spdk_conf_section *sp)
{
	return sp->num;
}

char *
spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key, int idx1, int idx2)
{
	struct spdk_conf_item *ip;
	struct spdk_conf_value *vp;
	int i;

	ip = find_cf_nitem(sp, key, idx1);
	if (ip == NULL) {
		return NULL;
	}

	vp = ip->val;
	if (vp == NULL) {
		return NULL;
	}

	for (i = 0; vp != NULL; vp = vp->next, i++) {
		if (i == idx2) {
			return vp->value;
		}
	}

	return NULL;
}

char *
spdk_conf_section_get_nval(struct spdk_conf_section *sp, const char *key, int idx)
{
	struct spdk_conf_item *ip;
	struct spdk_conf_value *vp;

	ip = find_cf_nitem(sp, key, idx);
	if (ip == NULL) {
		return NULL;
	}

	vp = ip->val;
	if (vp == NULL) {
		return NULL;
	}

	return vp->value;
}

char *
spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key)
{
	return spdk_conf_section_get_nval(sp, key, 0);
}

int
spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key)
{
	const char *v;
	int value;

	v = spdk_conf_section_get_nval(sp, key, 0);
	if (v == NULL) {
		return -1;
	}

	value = (int)spdk_strtol(v, 10);
	return value;
}

bool
spdk_conf_section_get_boolval(struct spdk_conf_section *sp, const char *key, bool default_val)
{
	const char *v;

	v = spdk_conf_section_get_nval(sp, key, 0);
	if (v == NULL) {
		return default_val;
	}

	if (!strcasecmp(v, "Yes") || !strcasecmp(v, "Y") || !strcasecmp(v, "True")) {
		return true;
	}

	if (!strcasecmp(v, "No") || !strcasecmp(v, "N") || !strcasecmp(v, "False")) {
		return false;
	}

	return default_val;
}

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
			num = (int)spdk_strtol(p, 10);
		} else {
			num = 0;
		}

		if (cp->merge_sections) {
			sp = spdk_conf_find_section(cp, key);
		} else {
			sp = NULL;
		}

		if (sp == NULL) {
			sp = allocate_cf_section();
			if (sp == NULL) {
				SPDK_ERRLOG("cannot allocate cf section\n");
				return -1;
			}
			append_cf_section(cp, sp);

			sp->name = strdup(key);
			if (sp->name == NULL) {
				SPDK_ERRLOG("cannot duplicate %s to sp->name\n", key);
				return -1;
			}
		}
		cp->current_section = sp;


		sp->num = num;
	} else {
		/* parameters */
		sp = cp->current_section;
		if (sp == NULL) {
			SPDK_ERRLOG("unknown section\n");
			return -1;
		}
		key = spdk_strsepq(&arg, CF_DELIM_KEY);
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
		ip->key = strdup(key);
		if (ip->key == NULL) {
			SPDK_ERRLOG("cannot make duplicate of %s\n", key);
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
				vp->value = strdup(val);
				if (vp->value == NULL) {
					SPDK_ERRLOG("cannot duplicate %s to vp->value\n", val);
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

	dst = p = malloc(LIB_MAX_TMPBUF);
	if (!dst) {
		return NULL;
	}

	dst[0] = '\0';
	total = 0;

	while (fgets(p, LIB_MAX_TMPBUF, fp) != NULL) {
		len = strlen(p);
		total += len;
		if (len + 1 < LIB_MAX_TMPBUF || dst[total - 1] == '\n') {
			dst2 = realloc(dst, total + 1);
			if (!dst2) {
				free(dst);
				return NULL;
			} else {
				return dst2;
			}
		}

		dst2 = realloc(dst, total + LIB_MAX_TMPBUF);
		if (!dst2) {
			free(dst);
			return NULL;
		} else {
			dst = dst2;
		}

		p = dst + total;
	}

	if (feof(fp) && total != 0) {
		dst2 = realloc(dst, total + 2);
		if (!dst2) {
			free(dst);
			return NULL;
		} else {
			dst = dst2;
		}

		dst[total] = '\n';
		dst[total + 1] = '\0';
		return dst;
	}

	free(dst);

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

	cp->file = strdup(file);
	if (cp->file == NULL) {
		SPDK_ERRLOG("cannot duplicate %s to cp->file\n", file);
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

			q = malloc(n + n2 + 1);
			if (!q) {
				free(lp2);
				free(lp);
				SPDK_ERRLOG("malloc failed at line %d of %s\n", line, cp->file);
				fclose(fp);
				return -1;
			}

			memcpy(q, p, n);
			memcpy(q + n, lp2, n2);
			q[n + n2] = '\0';
			free(lp2);
			free(lp);
			p = lp = q;
			n += n2;
		}

		/* parse one line */
		if (parse_line(cp, p) < 0) {
			SPDK_ERRLOG("parse error at line %d of %s\n", line, cp->file);
		}
next_line:
		line++;
		free(lp);
	}

	fclose(fp);
	return 0;
}

void
spdk_conf_set_as_default(struct spdk_conf *cp)
{
	default_config = cp;
}

void
spdk_conf_disable_sections_merge(struct spdk_conf *cp)
{
	cp->merge_sections = false;
}
