/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
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

void iscsi_param_free(struct iscsi_param *params);
struct iscsi_param *
iscsi_param_find(struct iscsi_param *params, const char *key);
int iscsi_param_del(struct iscsi_param **params, const char *key);
int iscsi_param_add(struct iscsi_param **params, const char *key,
		    const char *val, const char *list, int type);
int iscsi_param_set(struct iscsi_param *params, const char *key,
		    const char *val);
int iscsi_param_set_int(struct iscsi_param *params, const char *key, uint32_t val);
int iscsi_parse_params(struct iscsi_param **params, const uint8_t *data,
		       int len, bool cbit_enabled, char **partial_parameter);
char *iscsi_param_get_val(struct iscsi_param *params, const char *key);
int iscsi_param_eq_val(struct iscsi_param *params, const char *key,
		       const char *val);

int iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
			   struct iscsi_param **params_p, uint8_t *data,
			   int alloc_len, int data_len);
int iscsi_copy_param2var(struct spdk_iscsi_conn *conn);

int iscsi_conn_params_init(struct iscsi_param **params);
int iscsi_sess_params_init(struct iscsi_param **params);

#endif /* SPDK_ISCSI_PARAM_H */
