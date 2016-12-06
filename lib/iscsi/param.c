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

#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "spdk/string.h"
#include "iscsi/iscsi.h"
#include "iscsi/param.h"
#include "iscsi/conn.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#define MAX_TMPBUF 1024

/* whose value may be bigger than 255 */
static const char *non_simple_value_params[] = {
	"CHAP_C",
	"CHAP_R",
	NULL,
};

void
spdk_iscsi_param_free(struct iscsi_param *params)
{
	struct iscsi_param *param, *next_param;

	if (params == NULL)
		return;
	for (param = params; param != NULL; param = next_param) {
		next_param = param->next;
		if (param->list)
			free(param->list);
		free(param->val);
		free(param->key);
		free(param);
	}
}

static int
spdk_iscsi_find_key_in_array(const char *key, const char *array[])
{
	int i;

	for (i = 0; array[i] != NULL; i++) {
		if (strcasecmp(key, array[i]) == 0)
			return 1;
	}
	return 0;
}

struct iscsi_param *
spdk_iscsi_param_find(struct iscsi_param *params, const char *key)
{
	struct iscsi_param *param;

	if (params == NULL || key == NULL)
		return NULL;
	for (param = params; param != NULL; param = param->next) {
		if (param->key != NULL && param->key[0] == key[0]
		    && strcasecmp(param->key, key) == 0) {
			return param;
		}
	}
	return NULL;
}

int
spdk_iscsi_param_del(struct iscsi_param **params, const char *key)
{
	struct iscsi_param *param, *prev_param = NULL;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "del %s\n", key);
	if (params == NULL || key == NULL)
		return 0;
	for (param = *params; param != NULL; param = param->next) {
		if (param->key != NULL && param->key[0] == key[0]
		    && strcasecmp(param->key, key) == 0) {
			if (prev_param != NULL) {
				prev_param->next = param->next;
			} else {
				*params = param->next;
			}
			param->next = NULL;
			spdk_iscsi_param_free(param);
			return 0;
		}
		prev_param = param;
	}
	return -1;
}

int
spdk_iscsi_param_add(struct iscsi_param **params, const char *key,
		     const char *val, const char *list, int type)
{
	struct iscsi_param *param, *last_param;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "add %s=%s, list=[%s], type=%d\n",
		      key, val, list, type);
	if (key == NULL)
		return -1;

	param = spdk_iscsi_param_find(*params, key);
	if (param != NULL)
		spdk_iscsi_param_del(params, key);

	param = malloc(sizeof * param);
	if (!param) {
		perror("param");
		return -ENOMEM;
	}

	memset(param, 0, sizeof * param);
	param->next = NULL;
	param->key = xstrdup(key);
	param->val = xstrdup(val);
	param->list = xstrdup(list);
	param->type = type;

	last_param = *params;
	if (last_param != NULL) {
		while (last_param->next != NULL) {
			last_param = last_param->next;
		}
		last_param->next = param;
	} else {
		*params = param;
	}

	return 0;
}

int
spdk_iscsi_param_set(struct iscsi_param *params, const char *key,
		     const char *val)
{
	struct iscsi_param *param;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set %s=%s\n", key, val);
	param = spdk_iscsi_param_find(params, key);
	if (param == NULL) {
		SPDK_ERRLOG("no key %s\n", key);
		return -1;
	}

	free(param->val);

	param->val = xstrdup(val);

	return 0;
}

int
spdk_iscsi_param_set_int(struct iscsi_param *params, const char *key, uint32_t val)
{
	char buf[MAX_TMPBUF];
	struct iscsi_param *param;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set %s=%d\n", key, val);
	param = spdk_iscsi_param_find(params, key);
	if (param == NULL) {
		SPDK_ERRLOG("no key %s\n", key);
		return -1;
	}

	free(param->val);
	snprintf(buf, sizeof buf, "%d", val);

	param->val = strdup(buf);

	return 0;
}

/**
 * Parse a single KEY=VAL pair
 *
 * data = "KEY=VAL<NUL>"
 */
static int
spdk_iscsi_parse_param(struct iscsi_param **params, const uint8_t *data)
{
	int rc;
	uint8_t *key_copy;
	const uint8_t *key_end, *val;
	int key_len, val_len;
	int max_len;

	key_end = strchr(data, '=');
	if (!key_end) {
		SPDK_ERRLOG("'=' not found\n");
		return -1;
	}

	key_len = key_end - data;
	if (key_len == 0) {
		SPDK_ERRLOG("Empty key\n");
		return -1;
	}
	/*
	 * RFC 7143 6.1
	 */
	if (key_len > ISCSI_TEXT_MAX_KEY_LEN) {
		SPDK_ERRLOG("Key name length is bigger than 63\n");
		return -1;
	}

	key_copy = malloc(key_len + 1);
	if (!key_copy) {
		perror("key_copy");
		return -ENOMEM;
	}

	memcpy(key_copy, data, key_len);
	key_copy[key_len] = '\0';
	/* check whether this key is duplicated */
	if (NULL != spdk_iscsi_param_find(*params, key_copy)) {
		SPDK_ERRLOG("Duplicated Key %s\n", key_copy);
		free(key_copy);
		return -1;
	}

	val = key_end + 1; /* +1 to skip over the '=' */
	val_len = strlen(val);
	/*
	 * RFC 3720 5.1
	 * If not otherwise specified, the maximum length of a simple-value
	 * (not its encoded representation) is 255 bytes, not including the delimiter
	 * (comma or zero byte).
	 */
	/*
	 * comma or zero is counted in, otherwise we need to iterate each parameter
	 * value
	 */
	max_len = spdk_iscsi_find_key_in_array(key_copy, non_simple_value_params) ?
		  ISCSI_TEXT_MAX_VAL_LEN : ISCSI_TEXT_MAX_SIMPLE_VAL_LEN;
	if (val_len > max_len) {
		SPDK_ERRLOG("Overflow Val %d\n", val_len);
		free(key_copy);
		return -1;
	}

	rc = spdk_iscsi_param_add(params, key_copy, val, NULL, 0);
	free(key_copy);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_add() failed\n");
		return -1;
	}

	/* return number of bytes consumed
	 * +1 for '=' and +1 for NUL
	 */
	return key_len + 1 + val_len + 1;
}

/**
 * Parse a sequence of KEY=VAL pairs.
 *
 * \param data "KEY=VAL<NUL>KEY=VAL<NUL>..."
 * \param len length of data in bytes
 */
int
spdk_iscsi_parse_params(struct iscsi_param **params, const uint8_t *data,
			int len, bool cbit_enabled, char **partial_parameter)
{
	int rc, offset = 0;
	char *p;
	int i;

	/* strip the partial text parameters if previous PDU have C enabled */
	if (partial_parameter && *partial_parameter) {
		for (i = 0; i < len && data[i] != '\0'; i++)
			;
		p = spdk_sprintf_alloc("%s%s", *partial_parameter, (const char *)data);
		if (!p) {
			return -1;
		}
		rc = spdk_iscsi_parse_param(params, p);
		free(p);
		if (rc < 0)
			return -1;
		free(*partial_parameter);
		*partial_parameter = NULL;

		data = data + i + 1;
		len = len - (i + 1);
	}

	/* strip the partial text parameters if C bit is enabled */
	if (cbit_enabled) {
		if (partial_parameter == NULL) {
			SPDK_ERRLOG("C bit set but no partial parameters provided\n");
			return -1;
		}

		/*
		 * reverse iterate the string from the tail not including '\0'
		 * index of last '\0' is len -1.
		 */
		for (i = len - 2; data[i] != '\0' && i > 0; i--)
			;
		*partial_parameter = xstrdup(&data[i == 0 ? 0 : i + 1]);
		len = (i == 0 ? 0 : i + 1);
	}

	while (offset < len && data[offset] != '\0') {
		rc = spdk_iscsi_parse_param(params, data + offset);
		if (rc < 0) {
			return -1;
		}
		offset += rc;
	}
	return 0;
}

char *
spdk_iscsi_param_get_val(struct iscsi_param *params, const char *key)
{
	struct iscsi_param *param;

	param = spdk_iscsi_param_find(params, key);
	if (param == NULL)
		return NULL;
	return param->val;
}

int
spdk_iscsi_param_eq_val(struct iscsi_param *params, const char *key,
			const char *val)
{
	struct iscsi_param *param;

	param = spdk_iscsi_param_find(params, key);
	if (param == NULL)
		return 0;
	if (strcasecmp(param->val, val) == 0)
		return 1;
	return 0;
}

struct iscsi_param_table {
	const char *key;
	const char *val;
	const char *list;
	int type;
};

static const struct iscsi_param_table conn_param_table[] = {
	{ "HeaderDigest", "None", "CRC32C,None", ISPT_LIST },
	{ "DataDigest", "None", "CRC32C,None", ISPT_LIST },
	{ "MaxRecvDataSegmentLength", "8192", "512,16777215", ISPT_NUMERICAL_DECLARATIVE },
	{ "OFMarker", "No", "Yes,No", ISPT_BOOLEAN_AND },
	{ "IFMarker", "No", "Yes,No", ISPT_BOOLEAN_AND },
	{ "OFMarkInt", "1", "1,65535", ISPT_NUMERICAL_MIN },
	{ "IFMarkInt", "1", "1,65535", ISPT_NUMERICAL_MIN },
	{ "AuthMethod", "None", "CHAP,None", ISPT_LIST },
	{ "CHAP_A", "5", "5", ISPT_LIST },
	{ "CHAP_N", "", "", ISPT_DECLARATIVE },
	{ "CHAP_R", "", "", ISPT_DECLARATIVE },
	{ "CHAP_I", "", "", ISPT_DECLARATIVE },
	{ "CHAP_C", "", "", ISPT_DECLARATIVE },
	{ NULL, NULL, NULL, ISPT_INVALID },
};

static const struct iscsi_param_table sess_param_table[] = {
	{ "MaxConnections", "1", "1,65535", ISPT_NUMERICAL_MIN },
#if 0
	/* need special handling */
	{ "SendTargets", "", "", ISPT_DECLARATIVE },
#endif
	{ "TargetName", "", "", ISPT_DECLARATIVE },
	{ "InitiatorName", "", "", ISPT_DECLARATIVE },
	{ "TargetAlias", "", "", ISPT_DECLARATIVE },
	{ "InitiatorAlias", "", "", ISPT_DECLARATIVE },
	{ "TargetAddress", "", "", ISPT_DECLARATIVE },
	{ "TargetPortalGroupTag", "1", "1,65535", ISPT_NUMERICAL_DECLARATIVE },
	{ "InitialR2T", "Yes", "Yes,No", ISPT_BOOLEAN_OR },
	{ "ImmediateData", "Yes", "Yes,No", ISPT_BOOLEAN_AND },
	{ "MaxBurstLength", "262144", "512,16777215", ISPT_NUMERICAL_MIN },
	{ "FirstBurstLength", "65536", "512,16777215", ISPT_NUMERICAL_MIN },
	{ "DefaultTime2Wait", "2", "0,3600", ISPT_NUMERICAL_MAX },
	{ "DefaultTime2Retain", "20", "0,3600", ISPT_NUMERICAL_MIN },
	{ "MaxOutstandingR2T", "1", "1,65536", ISPT_NUMERICAL_MIN },
	{ "DataPDUInOrder", "Yes", "Yes,No", ISPT_BOOLEAN_OR },
	{ "DataSequenceInOrder", "Yes", "Yes,No", ISPT_BOOLEAN_OR },
	{ "ErrorRecoveryLevel", "0", "0,2", ISPT_NUMERICAL_MIN },
	{ "SessionType", "Normal", "Normal,Discovery", ISPT_DECLARATIVE },
	{ NULL, NULL, NULL, ISPT_INVALID },
};

static int
spdk_iscsi_params_init_internal(struct iscsi_param **params,
				const struct iscsi_param_table *table)
{
	int rc;
	int i;
	struct iscsi_param *param;

	for (i = 0; table[i].key != NULL; i++) {
		rc = spdk_iscsi_param_add(params, table[i].key, table[i].val,
					  table[i].list, table[i].type);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_param_add() failed\n");
			return -1;
		}
		param = spdk_iscsi_param_find(*params, table[i].key);
		if (param != NULL) {
			param->state_index = i;
		} else {
			SPDK_ERRLOG("spdk_iscsi_param_find() failed\n");
			return -1;
		}
	}

	return 0;
}

int
spdk_iscsi_conn_params_init(struct iscsi_param **params)
{
	return spdk_iscsi_params_init_internal(params, &conn_param_table[0]);
}

int
spdk_iscsi_sess_params_init(struct iscsi_param **params)
{
	return spdk_iscsi_params_init_internal(params, &sess_param_table[0]);
}

static const char *chap_type[] = {
	"CHAP_A",
	"CHAP_N",
	"CHAP_R",
	"CHAP_I",
	"CHAP_C",
	NULL,
};

static const char *discovery_ignored_param[] = {
	"MaxConnections",
	"InitialR2T",
	"ImmediateData",
	"MaxBurstLength",
	"FirstBurstLength"
	"MaxOutstandingR2T",
	"DataPDUInOrder",
	NULL,
};

static const char *multi_negot_conn_params[] = {
	"MaxRecvDataSegmentLength",
	NULL,
};

/* The following params should be declared by target */
static const char *target_declarative_params[] = {
	"TargetAlias",
	"TargetAddress",
	"TargetPortalGroupTag",
	NULL,
};

/* This function is used to contruct the data from the special param (e.g.,
 * MaxRecvDataSegmentLength)
 * return:
 * normal: the total len of the data
 * error: -1
 */
static int
spdk_iscsi_special_param_construction(struct spdk_iscsi_conn *conn,
				      struct iscsi_param *param,
				      bool FirstBurstLength_flag, char *data,
				      int alloc_len, int total)
{
	int len;
	struct iscsi_param *param_first;
	struct iscsi_param *param_max;
	uint32_t FirstBurstLength;
	uint32_t MaxBurstLength;
	char *val;

	val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	if (!val) {
		perror("val");
		return -ENOMEM;
	}

	if (strcasecmp(param->key, "MaxRecvDataSegmentLength") == 0) {
		/*
		 * MaxRecvDataSegmentLength is sent by both
		 *      initiator and target, but is declarative - meaning
		 *      each direction can have different values.
		 * So when MaxRecvDataSegmentLength is found in the
		 *      the parameter set sent from the initiator, add SPDK
		 *      iscsi target's MaxRecvDataSegmentLength value to
		 *      the returned parameter list.
		 */
		if (alloc_len - total < 1) {
			SPDK_ERRLOG("data space small %d\n", alloc_len);
			free(val);
			return -1;
		}

		SPDK_TRACELOG(SPDK_TRACE_ISCSI,
			      "returning MaxRecvDataSegmentLength=%d\n",
			      g_spdk_iscsi.MaxRecvDataSegmentLength);
		len = snprintf((char *)data + total, alloc_len - total,
			       "MaxRecvDataSegmentLength=%d",
			       g_spdk_iscsi.MaxRecvDataSegmentLength);
		total += len + 1;
	}

	if (strcasecmp(param->key, "MaxBurstLength") == 0 &&
	    !FirstBurstLength_flag) {
		if (alloc_len - total < 1) {
			SPDK_ERRLOG("data space small %d\n", alloc_len);
			free(val);
			return -1;
		}

		param_first = spdk_iscsi_param_find(conn->sess->params,
						    "FirstBurstLength");
		if (param_first != NULL) {
			FirstBurstLength = (uint32_t)strtol(param_first->val, NULL, 10);
		} else {
			FirstBurstLength = SPDK_ISCSI_FIRST_BURST_LENGTH;
		}
		param_max = spdk_iscsi_param_find(conn->sess->params,
						  "MaxBurstLength");
		if (param_max != NULL) {
			MaxBurstLength = (uint32_t)strtol(param_max->val, NULL, 10);
		} else {
			MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
		}

		if (FirstBurstLength > MaxBurstLength) {
			FirstBurstLength = MaxBurstLength;
			if (param_first != NULL) {
				free(param_first->val);
				snprintf(val, ISCSI_TEXT_MAX_VAL_LEN, "%d",
					 FirstBurstLength);
				param_first->val = xstrdup(val);
			}
		}
		len = snprintf((char *)data + total, alloc_len - total,
			       "FirstBurstLength=%d", FirstBurstLength);
		total += len + 1;
	}

	free(val);
	return total;

}

/**
* spdk_iscsi_construct_data_from_param:
* 	To construct the data which will be returned to the initiator
* 	return: length of the negotiated data, -1 inidicates error;
*/
static int
spdk_iscsi_construct_data_from_param(struct iscsi_param *param, char *new_val,
				     char *data, int alloc_len, int total)

{
	int len;

	if (param->type != ISPT_DECLARATIVE &&
	    param->type != ISPT_NUMERICAL_DECLARATIVE) {
		if (alloc_len - total < 1) {
			SPDK_ERRLOG("data space small %d\n", alloc_len);
			return -1;
		}

		SPDK_TRACELOG(SPDK_TRACE_ISCSI, "negotiated %s=%s\n",
			      param->key, new_val);
		len = snprintf((char *)data + total, alloc_len - total, "%s=%s",
			       param->key, new_val);
		total += len + 1;
	}
	return total;

}

/**
* To negotiate param with
* 	type = ISPT_LIST
	return: the negotiated value of the key
*/
static char *spdk_iscsi_negotiate_param_list(int *add_param_value,
		struct iscsi_param *param,
		char *valid_list, char *in_val,
		char *cur_val)
{
	char *val_start, *val_end;
	char *in_start, *in_end;
	int flag = 0;

	if (add_param_value == NULL)
		return NULL;

	in_start = in_val;
	do {
		if ((in_end = strchr(in_start, (int)',')) != NULL) {
			*in_end = '\0';
		}
		val_start = valid_list;
		do {
			if ((val_end = strchr(val_start, (int)',')) != NULL) {
				*val_end = '\0';
			}
			if (strcasecmp(in_start, val_start) == 0) {
				SPDK_TRACELOG(SPDK_TRACE_DEBUG, "match %s\n",
					      val_start);
				flag = 1;
				break;
			}
			if (val_end) {
				*val_end = ',';
				val_start = val_end + 1;
			}
		} while (val_end);
		if (flag)
			break;
		if (in_end) {
			*in_end = ',';
			in_start = in_end + 1;
		}
	} while (in_end);

	return flag ? val_start : NULL;
}

/**
* To negotiate param with
* 	type = ISPT_NUMERICAL_MIN/MAX, ISPT_NUMERICAL_DECLARATIVE
	return: the negotiated value of the key
*/
static char *spdk_iscsi_negotiate_param_numerical(int *add_param_value,
		struct iscsi_param *param,
		char *valid_list, char *in_val,
		char *cur_val)
{
	char *valid_next;
	char *new_val = NULL;
	char *min_val, *max_val;
	int val_i, cur_val_i;
	int min_i, max_i;

	if (add_param_value == NULL)
		return NULL;

	val_i = (int)strtol(param->val, NULL, 10);
	/* check whether the key is FirstBurstLength, if that we use in_val */
	if (strcasecmp(param->key, "FirstBurstLength") == 0)
		val_i = (int)strtol(in_val, NULL, 10);

	cur_val_i = (int)strtol(cur_val, NULL, 10);
	valid_next = valid_list;
	min_val = spdk_strsepq(&valid_next, ",");
	max_val = spdk_strsepq(&valid_next, ",");
	min_i = (min_val != NULL) ? (int)strtol(min_val, NULL, 10) : 0;
	max_i = (max_val != NULL) ? (int)strtol(max_val, NULL, 10) : 0;
	if (val_i < min_i || val_i > max_i) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "key %.64s reject\n", param->key);
		new_val = NULL;
	} else {
		switch (param->type) {
		case ISPT_NUMERICAL_MIN:
			if (val_i > cur_val_i) {
				val_i = cur_val_i;
			}
			break;
		case ISPT_NUMERICAL_MAX:
			if (val_i < cur_val_i) {
				val_i = cur_val_i;
			}
			break;
		default:
			break;
		}
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN, "%d", val_i);
		new_val = in_val;
	}

	return new_val;
}

/**
* To negotiate param with
* 	type = ISPT_BOOLEAN_OR, ISPT_BOOLEAN_AND
	return: the negotiated value of the key
*/
static char *spdk_iscsi_negotiate_param_boolean(int *add_param_value,
		struct iscsi_param *param,
		char *in_val, char *cur_val,
		const char *value)
{
	char *new_val = NULL;

	if (add_param_value == NULL)
		return NULL;

	/* Make sure the val is Yes or No */
	if (!((strcasecmp(in_val, "Yes") == 0) ||
	      (strcasecmp(in_val, "No") == 0))) {
		/* unknown value */
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "Reject");
		new_val = in_val;
		*add_param_value = 1;
		return new_val;
	}

	if (strcasecmp(cur_val, value) == 0) {
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", value);
		new_val = in_val;
	} else
		new_val = param->val;

	return new_val;
}

/**
 * The entry function to handle each type of the param
 * return value: the new negotiated value
*/
static char *
spdk_iscsi_negotiate_param_all(int *add_param_value, struct iscsi_param *param,
			       char *valid_list, char *in_val, char *cur_val)
{
	char *new_val;
	switch (param->type) {
	case ISPT_LIST:
		new_val = spdk_iscsi_negotiate_param_list(add_param_value,
				param,
				valid_list,
				in_val,
				cur_val);
		break;

	case ISPT_NUMERICAL_MIN:
	case ISPT_NUMERICAL_MAX:
	case ISPT_NUMERICAL_DECLARATIVE:
		new_val = spdk_iscsi_negotiate_param_numerical(add_param_value,
				param,
				valid_list,
				in_val,
				cur_val);
		break;

	case ISPT_BOOLEAN_OR:
		new_val = spdk_iscsi_negotiate_param_boolean(add_param_value,
				param,
				in_val,
				cur_val,
				"Yes");
		break;
	case ISPT_BOOLEAN_AND:
		new_val = spdk_iscsi_negotiate_param_boolean(add_param_value,
				param,
				in_val,
				cur_val,
				"No");
		break;

	default:
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", param->val);
		new_val = in_val;
		break;
	}

	return new_val;
}

/**
 * This function is used to judge whether the param is in session's params or
 * connection's params
*/
static int
spdk_iscsi_negotiate_param_init(struct spdk_iscsi_conn *conn,
				struct iscsi_param **cur_param_p,
				struct iscsi_param **params_dst_p,
				struct iscsi_param *param)
{
	int index;

	*cur_param_p = spdk_iscsi_param_find(*params_dst_p, param->key);
	if (*cur_param_p == NULL) {
		*params_dst_p = conn->sess->params;
		*cur_param_p = spdk_iscsi_param_find(*params_dst_p, param->key);
		if (*cur_param_p == NULL) {
			if ((strncasecmp(param->key, "X-", 2) == 0) ||
			    (strncasecmp(param->key, "X#", 2) == 0)) {
				/* Extension Key */
				SPDK_TRACELOG(SPDK_TRACE_DEBUG,
					      "extension key %.64s\n",
					      param->key);
			} else {
				SPDK_ERRLOG("unknown key %.64s\n", param->key);
			}
			return 1;
		} else {
			index = (*cur_param_p)->state_index;
			if (conn->sess_param_state_negotiated[index] &&
			    !spdk_iscsi_find_key_in_array(param->key,
							  target_declarative_params))
				return SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE;
			conn->sess_param_state_negotiated[index] = true;
		}
	} else {
		index = (*cur_param_p)->state_index;
		if (conn->conn_param_state_negotiated[index] &&
		    !spdk_iscsi_find_key_in_array(param->key,
						  multi_negot_conn_params))
			return SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE;
		conn->conn_param_state_negotiated[index] = true;
	}

	return 0;
}

int
spdk_iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
			    struct iscsi_param *params, uint8_t *data, int alloc_len,
			    int data_len)
{
	struct iscsi_param *param;
	struct iscsi_param *cur_param;
	char *valid_list, *in_val;
	char *cur_val;
	char *new_val;
	int discovery;
	int total;
	int rc;
	uint32_t FirstBurstLength;
	uint32_t MaxBurstLength;
	bool FirstBurstLength_flag = false;
	int type;
	total = data_len;
	if (alloc_len < 1) {
		return 0;
	}
	if (total > alloc_len) {
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	if (params == NULL) {
		/* no input */
		return total;
	}

	/* discovery? */
	discovery = 0;
	cur_param = spdk_iscsi_param_find(params, "SessionType");
	if (cur_param == NULL) {
		cur_param = spdk_iscsi_param_find(conn->sess->params, "SessionType");
		if (cur_param == NULL) {
			/* no session type */
		} else {
			if (strcasecmp(cur_param->val, "Discovery") == 0) {
				discovery = 1;
			}
		}
	} else {
		if (strcasecmp(cur_param->val, "Discovery") == 0) {
			discovery = 1;
		}
	}

	/* for temporary store */
	valid_list = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	if (!valid_list) {
		perror("valid_list");
		return -ENOMEM;
	}

	in_val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	if (!in_val) {
		perror("in_val");
		free(valid_list);
		return -ENOMEM;
	}

	cur_val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	if (!cur_val) {
		perror("cur_val");
		free(valid_list);
		free(in_val);
		return -ENOMEM;
	}

	/* To adjust the location of FirstBurstLength location and put it to
	*  the end, then we can always firstly determine the MaxBurstLength
	*/
	param = spdk_iscsi_param_find(params, "MaxBurstLength");
	if (param != NULL) {
		param = spdk_iscsi_param_find(params, "FirstBurstLength");

		/*check the existence of FirstBurstLength*/
		if (param != NULL) {
			FirstBurstLength_flag = true;
			if (param->next != NULL) {
				snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", param->val);
				type = param->type;
				spdk_iscsi_param_add(&params, "FirstBurstLength",
						     in_val, NULL, type);
			}
		}
	}

	for (param = params; param != NULL; param = param->next) {
		struct iscsi_param *params_dst = conn->params;
		int add_param_value = 0;
		new_val = NULL;
		param->type = ISPT_INVALID;

		/* sendtargets is special */
		if (strcasecmp(param->key, "SendTargets") == 0) {
			continue;
		}
		/* CHAP keys */
		if (spdk_iscsi_find_key_in_array(param->key, chap_type))
			continue;

		/* 12.2, 12.10, 12.11, 12.13, 12.14, 12.17, 12.18, 12.19 */
		if (discovery &&
		    spdk_iscsi_find_key_in_array(param->key,
						 discovery_ignored_param)) {
			snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "Irrelevant");
			new_val = in_val;
			add_param_value = 1;
		} else {
			rc = spdk_iscsi_negotiate_param_init(conn,
							     &cur_param,
							     &params_dst,
							     param);
			if (rc < 0) {
				free(valid_list);
				free(in_val);
				free(cur_val);
				return rc;
			} else if (rc > 0) {
				snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "NotUnderstood");
				new_val = in_val;
				add_param_value = 1;
			} else {
				snprintf(valid_list, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", cur_param->list);
				snprintf(cur_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", cur_param->val);
				param->type = cur_param->type;
			}
		}

		if (param->type > 0) {
			snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", param->val);

			/* "NotUnderstood" value shouldn't be assigned to "Understood" key */
			if (strcasecmp(in_val, "NotUnderstood") == 0) {
				free(in_val);
				free(valid_list);
				free(cur_val);
				return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
			}

			if (strcasecmp(param->key, "FirstBurstLength") == 0) {
				FirstBurstLength = (uint32_t)strtol(param->val, NULL,
								    10);
				new_val = spdk_iscsi_param_get_val(conn->sess->params,
								   "MaxBurstLength");
				if (new_val != NULL) {
					MaxBurstLength = (uint32_t) strtol(new_val, NULL,
									   10);
				} else {
					MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
				}
				if (FirstBurstLength < MAX_FIRSTBURSTLENGTH &&
				    FirstBurstLength > MaxBurstLength) {
					FirstBurstLength = MaxBurstLength;
					snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN, "%d",
						 FirstBurstLength);
				}
			}

			/* prevent target's declarative params from being changed by initiator */
			if (spdk_iscsi_find_key_in_array(param->key, target_declarative_params)) {
				add_param_value = 1;
			}

			new_val = spdk_iscsi_negotiate_param_all(&add_param_value,
					param,
					valid_list,
					in_val,
					cur_val);
		}

		/* check the negotiated value of the key */
		if (new_val != NULL) {
			/* add_param_value = 0 means updating the value of
			 *      existed key in the connection's parameters
			 */
			if (add_param_value == 0)
				spdk_iscsi_param_set(params_dst, param->key,
						     new_val);

			total = spdk_iscsi_construct_data_from_param(param,
					new_val,
					data,
					alloc_len,
					total);
			if (total < 0)
				goto final_return;

			total = spdk_iscsi_special_param_construction(conn,
					param,
					FirstBurstLength_flag,
					data,
					alloc_len,
					total);
			if (total < 0)
				goto final_return;
		} else {
			total = -1;
			break;
		}
	}

final_return:
	free(valid_list);
	free(in_val);
	free(cur_val);

	return total;
}

int
spdk_iscsi_copy_param2var(struct spdk_iscsi_conn *conn)
{
	const char *val;

	val = spdk_iscsi_param_get_val(conn->params, "MaxRecvDataSegmentLength");
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxRecvDataSegmentLength failed\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG,
		      "copy MaxRecvDataSegmentLength=%s\n", val);
	conn->MaxRecvDataSegmentLength = (int)strtol(val, NULL, 10);
	if (conn->MaxRecvDataSegmentLength > SPDK_ISCSI_MAX_SEND_DATA_SEGMENT_LENGTH) {
		conn->MaxRecvDataSegmentLength = SPDK_ISCSI_MAX_SEND_DATA_SEGMENT_LENGTH;
	}

	val = spdk_iscsi_param_get_val(conn->params, "HeaderDigest");
	if (val == NULL) {
		SPDK_ERRLOG("Getval HeaderDigest failed\n");
		return -1;
	}
	if (strcasecmp(val, "CRC32C") == 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set HeaderDigest=1\n");
		conn->header_digest = 1;
	} else {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set HeaderDigest=0\n");
		conn->header_digest = 0;
	}
	val = spdk_iscsi_param_get_val(conn->params, "DataDigest");
	if (val == NULL) {
		SPDK_ERRLOG("Getval DataDigest failed\n");
		return -1;
	}
	if (strcasecmp(val, "CRC32C") == 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set DataDigest=1\n");
		conn->data_digest = 1;
	} else {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set DataDigest=0\n");
		conn->data_digest = 0;
	}

	val = spdk_iscsi_param_get_val(conn->sess->params, "MaxConnections");
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxConnections failed\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "copy MaxConnections=%s\n", val);
	conn->sess->MaxConnections = (uint32_t) strtol(val, NULL, 10);
	val = spdk_iscsi_param_get_val(conn->sess->params, "MaxOutstandingR2T");
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxOutstandingR2T failed\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "copy MaxOutstandingR2T=%s\n", val);
	conn->sess->MaxOutstandingR2T = (uint32_t) strtol(val, NULL, 10);
	val = spdk_iscsi_param_get_val(conn->sess->params, "FirstBurstLength");
	if (val == NULL) {
		SPDK_ERRLOG("Getval FirstBurstLength failed\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "copy FirstBurstLength=%s\n", val);
	conn->sess->FirstBurstLength = (uint32_t) strtol(val, NULL, 10);
	val = spdk_iscsi_param_get_val(conn->sess->params, "MaxBurstLength");
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxBurstLength failed\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "copy MaxBurstLength=%s\n", val);
	conn->sess->MaxBurstLength = (uint32_t) strtol(val, NULL, 10);
	val = spdk_iscsi_param_get_val(conn->sess->params, "InitialR2T");
	if (val == NULL) {
		SPDK_ERRLOG("Getval InitialR2T failed\n");
		return -1;
	}
	if (strcasecmp(val, "Yes") == 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set InitialR2T=1\n");
		conn->sess->InitialR2T = 1;
	} else {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set InitialR2T=0\n");
		conn->sess->InitialR2T = 0;
	}
	val = spdk_iscsi_param_get_val(conn->sess->params, "ImmediateData");
	if (val == NULL) {
		SPDK_ERRLOG("Getval ImmediateData failed\n");
		return -1;
	}
	if (strcasecmp(val, "Yes") == 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set ImmediateData=1\n");
		conn->sess->ImmediateData = 1;
	} else {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "set ImmediateData=0\n");
		conn->sess->ImmediateData = 0;
	}
	return 0;
}
