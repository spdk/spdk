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

#include "spdk/conf.h"
#include "spdk/json.h"
#include "spdk/util.h"

#include "iscsi.h"
#include "iscsi_json_conf.h"

#include "spdk_internal/log.h"

#define	MAX_AUTH_GROUP	256

struct jc_auth_group {
	int32_t	group;
	char	*user;
	char	*secret;
	char 	*muser;
	char	*msecret;
};

struct jc_auth_group_list {
	size_t	num_auth_groups;
	struct	jc_auth_group auth_groups[MAX_AUTH_GROUP];
};

struct jc_auth_config {
	struct jc_auth_group_list auth_group_list;
};

static void
free_jc_auth_group(struct jc_auth_group *p)
{
	free(p->user);
	free(p->secret);
	free(p->muser);
	free(p->msecret);
}

static void
free_jc_auth_group_list(struct jc_auth_group_list *p)
{
	size_t i;

	for (i = 0; i < p->num_auth_groups; i++) {
		free_jc_auth_group(&p->auth_groups[i]);
	}
	p->num_auth_groups = 0;
}

static void
free_jc_auth_config(struct jc_auth_config *p)
{
	free_jc_auth_group_list(&p->auth_group_list);
	free(p);
}

static const struct spdk_json_object_decoder jc_auth_group_decoders[] = {
	{"group", offsetof(struct jc_auth_group, group), spdk_json_decode_int32},
	{"user", offsetof(struct jc_auth_group, user), spdk_json_decode_string},
	{"secret", offsetof(struct jc_auth_group, secret), spdk_json_decode_string, true},
	{"muser", offsetof(struct jc_auth_group, muser), spdk_json_decode_string, true},
	{"msecret", offsetof(struct jc_auth_group, msecret), spdk_json_decode_string, true},
};

static int
decode_jc_auth_group(const struct spdk_json_val *val, void *out)
{
	struct jc_auth_group *group = out;

	return spdk_json_decode_object(val, jc_auth_group_decoders,
				       SPDK_COUNTOF(jc_auth_group_decoders), group);
}

static int
decode_jc_auth_group_list(const struct spdk_json_val *val, void *out)
{
	struct jc_auth_group_list *list = out;

	return spdk_json_decode_array(val, decode_jc_auth_group, list->auth_groups,
				      MAX_AUTH_GROUP, &list->num_auth_groups,
				      sizeof(struct jc_auth_group));
}

static const struct spdk_json_object_decoder jc_auth_config_decoder[] = {
	{"authconfig", offsetof(struct jc_auth_config, auth_group_list), decode_jc_auth_group_list},
};

static struct jc_auth_config *
spdk_jsonc_get_iscsi_chap_authinfo(const char *file)
{
	void *buffer = NULL;
	size_t buffer_size = 0;
	struct spdk_json_val *json_vals = NULL;
	struct jc_auth_config *conf = NULL;
	int rc;

	rc = spdk_conf_load_file(file, &buffer, &buffer_size);
	if (rc != 0) {
		return NULL;
	}

	rc = spdk_json_load_object(buffer, buffer_size, &json_vals);
	if (rc < 0) {
		goto error;
	}

	conf = calloc(1, sizeof(*conf));
	if (conf == NULL) {
		goto error;
	}

	if (spdk_json_decode_object(json_vals, jc_auth_config_decoder,
				    SPDK_COUNTOF(jc_auth_config_decoder),
				    conf)) {
		goto error;
	}

	free(buffer);
	free(json_vals);
	return conf;

error:
	free(conf);
	free(buffer);
	free(json_vals);
	return NULL;
}

int
spdk_iscsi_chap_get_authinfo_json(struct iscsi_chap_auth *auth,
				  const char *authfile,
				  const char *authuser, int ag_tag)
{
	struct jc_auth_config *conf = NULL;
	struct jc_auth_group *group;
	size_t i;

	conf = spdk_jsonc_get_iscsi_chap_authinfo(authfile);
	if (conf == NULL) {
		return -1;
	}

	for (i = 0; i < conf->auth_group_list.num_auth_groups; i++) {
		group = &conf->auth_group_list.auth_groups[i];
		if (group->group == 0) {
			SPDK_ERRLOG("Group 0 is invalid\n");
			goto error;
		}
		if (ag_tag != group->group) {
			continue;
		}
		if (strcasecmp(authuser, group->user) == 0) {
			auth->user = xstrdup(group->user);
			auth->secret = xstrdup(group->secret);
			auth->muser = xstrdup(group->muser);
			auth->msecret = xstrdup(group->msecret);

			free_jc_auth_config(conf);
			return 0;
		}
	}

error:
	free_jc_auth_config(conf);
	return -1;
}
