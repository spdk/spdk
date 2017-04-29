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

static struct spdk_conf *default_config = NULL;

struct spdk_conf *
spdk_conf_allocate(void)
{
	return spdk_calloc(1, sizeof(struct spdk_conf));
}

static void
free_conf_value(struct spdk_conf_value *vp)
{
	if (vp == NULL) {
		return;
	}

	if (vp->value) {
		spdk_free(vp->value);
	}

	spdk_free(vp);
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
		spdk_free(ip->key);
	}

	spdk_free(ip);
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
		spdk_free(sp->name);
	}

	spdk_free(sp);
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
		spdk_free(cp->file);
	}

	spdk_free(cp);
}

struct spdk_conf_section *
allocate_cf_section(void)
{
	return spdk_calloc(1, sizeof(struct spdk_conf_section));
}

struct spdk_conf_item *
allocate_cf_item(void)
{
	return spdk_calloc(1, sizeof(struct spdk_conf_item));
}

struct spdk_conf_value *
allocate_cf_value(void)
{
	return spdk_calloc(1, sizeof(struct spdk_conf_value));
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

void
append_cf_section(struct spdk_conf *cp, struct spdk_conf_section *sp)
{
	struct spdk_conf_section *last;

	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	if (cp == NULL) {
		SPDK_ERRLOG("%s: cp == NULL\n", __func__);
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

struct spdk_conf_item *
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

void
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

void
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

	value = (int)strtol(v, NULL, 10);
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

void
spdk_conf_set_as_default(struct spdk_conf *cp)
{
	default_config = cp;
}
