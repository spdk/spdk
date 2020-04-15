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

#ifndef SPDK_ISCSI_PARAM_H
#define SPDK_ISCSI_PARAM_H

#include "spdk/stdinc.h"

struct spdk_iscsi_conn;

enum iscsi_param_type {
	ISPT_INVALID = -1,
	ISPT_NOTSPECIFIED = 0,
	ISPT_LIST,
	ISPT_NUMERICAL_MIN,
	ISPT_NUMERICAL_MAX,
	ISPT_NUMERICAL_DECLARATIVE,
	ISPT_DECLARATIVE,
	ISPT_BOOLEAN_OR,
	ISPT_BOOLEAN_AND,
};

struct iscsi_param {
	struct iscsi_param *next;
	char *key;
	char *val;
	char *list;
	int type;
	int state_index;
};

void
iscsi_param_free(struct iscsi_param *params);
struct iscsi_param *
iscsi_param_find(struct iscsi_param *params, const char *key);
int
iscsi_param_del(struct iscsi_param **params, const char *key);
int
iscsi_param_add(struct iscsi_param **params, const char *key,
		const char *val, const char *list, int type);
int
iscsi_param_set(struct iscsi_param *params, const char *key,
		const char *val);
int
iscsi_param_set_int(struct iscsi_param *params, const char *key, uint32_t val);
int
iscsi_parse_params(struct iscsi_param **params, const uint8_t *data,
		   int len, bool cbit_enabled, char **partial_parameter);
char *
iscsi_param_get_val(struct iscsi_param *params, const char *key);
int
iscsi_param_eq_val(struct iscsi_param *params, const char *key,
		   const char *val);

int iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
			   struct iscsi_param **params_p, uint8_t *data,
			   int alloc_len, int data_len);
int iscsi_copy_param2var(struct spdk_iscsi_conn *conn);

int iscsi_conn_params_init(struct iscsi_param **params);
int iscsi_sess_params_init(struct iscsi_param **params);

#endif /* SPDK_ISCSI_PARAM_H */
