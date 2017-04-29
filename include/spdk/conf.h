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

/** \file
 * Configuration file parser
 */

#ifndef SPDK_CONF_H
#define SPDK_CONF_H

#include "spdk/env.h"

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
};

struct spdk_conf *spdk_conf_allocate(void);
void spdk_conf_free(struct spdk_conf *cp);
int spdk_conf_read(struct spdk_conf *cp, const char *file);
struct spdk_conf_section *spdk_conf_find_section(struct spdk_conf *cp, const char *name);

/* Configuration file iteration */
struct spdk_conf_section *spdk_conf_first_section(struct spdk_conf *cp);
struct spdk_conf_section *spdk_conf_next_section(struct spdk_conf_section *sp);

bool spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix);
const char *spdk_conf_section_get_name(const struct spdk_conf_section *sp);
int spdk_conf_section_get_num(const struct spdk_conf_section *sp);
char *spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key,
				  int idx1, int idx2);
char *spdk_conf_section_get_nval(struct spdk_conf_section *sp, const char *key, int idx);
char *spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key);
int spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key);
bool spdk_conf_section_get_boolval(struct spdk_conf_section *sp, const char *key, bool default_val);

struct spdk_conf_section *allocate_cf_section(void);
struct spdk_conf_item *allocate_cf_item(void);
struct spdk_conf_value *allocate_cf_value(void);
void append_cf_section(struct spdk_conf *cp, struct spdk_conf_section *sp);
struct spdk_conf_item *find_cf_nitem(struct spdk_conf_section *sp, const char *key, int idx);
void append_cf_item(struct spdk_conf_section *sp, struct spdk_conf_item *ip);
void append_cf_value(struct spdk_conf_item *ip, struct spdk_conf_value *vp);



void spdk_conf_set_as_default(struct spdk_conf *cp);

#endif
