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
#include "spdk/net.h"

#include "spdk_internal/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/tgt_node.h"
#include "iscsi/conn.h"
#include "iscsi/init_grp.h"


/* Read spdk iscsi target's config file and create initiator group */
int
spdk_iscsi_init_grp_create_from_configfile(struct spdk_conf_section *sp)
{
	int i, rc = 0;
	const char *val = NULL;
	int num_initiator_names;
	int num_initiator_masks;
	char **initiators = NULL, **netmasks = NULL;
	int tag = spdk_conf_section_get_num(sp);

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "add initiator group %d\n", tag);

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Comment %s\n", val);
	}

	/* counts number of definitions */
	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "InitiatorName", i);
		if (val == NULL)
			break;
	}
	if (i == 0) {
		SPDK_ERRLOG("num_initiator_names = 0\n");
		goto cleanup;
	}
	num_initiator_names = i;
	if (num_initiator_names > MAX_INITIATOR) {
		SPDK_ERRLOG("%d > MAX_INITIATOR\n", num_initiator_names);
		return -1;
	}
	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "Netmask", i);
		if (val == NULL)
			break;
	}
	if (i == 0) {
		SPDK_ERRLOG("num_initiator_mask = 0\n");
		goto cleanup;
	}
	num_initiator_masks = i;
	if (num_initiator_masks > MAX_NETMASK) {
		SPDK_ERRLOG("%d > MAX_NETMASK\n", num_initiator_masks);
		return -1;
	}

	initiators = calloc(num_initiator_names, sizeof(char *));
	if (!initiators) {
		perror("initiators");
		return -1;
	}
	for (i = 0; i < num_initiator_names; i++) {
		val = spdk_conf_section_get_nval(sp, "InitiatorName", i);
		if (!val) {
			SPDK_ERRLOG("InitiatorName %d not found\n", i);
			rc = -EINVAL;
			goto cleanup;
		}
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "InitiatorName %s\n", val);
		initiators[i] = strdup(val);
		if (!initiators[i]) {
			perror("initiator name copy");
			rc = -ENOMEM;
			goto cleanup;
		}
	}
	netmasks = calloc(num_initiator_masks, sizeof(char *));
	if (!netmasks) {
		perror("netmasks");
		rc = -ENOMEM;
		goto cleanup;
	}
	for (i = 0; i < num_initiator_masks; i++) {
		val = spdk_conf_section_get_nval(sp, "Netmask", i);
		if (!val) {
			SPDK_ERRLOG("Netmask %d not found\n", i);
			rc = -EINVAL;
			goto cleanup;
		}
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Netmask %s\n", val);
		netmasks[i] = strdup(val);
		if (!netmasks[i]) {
			perror("initiator netmask copy");
			rc = -ENOMEM;
			goto cleanup;
		}
	}

	rc = spdk_iscsi_init_grp_create_from_initiator_list(tag,
			num_initiator_names, initiators, num_initiator_masks, netmasks);
	if (rc < 0) {
		goto cleanup;
	}
	return rc;

cleanup:
	if (initiators) {
		for (i = 0; i < num_initiator_names; i++) {
			if (initiators[i]) {
				free(initiators[i]);
			}
		}
		free(initiators);
	}
	if (netmasks) {
		for (i = 0; i < num_initiator_masks; i++) {
			if (netmasks[i]) {
				free(netmasks[i]);
			}
		}
		free(netmasks);
	}
	return rc;
}

/*
 * Create initiator group from list of initiator ip/hostnames and netmasks
 * The initiator hostname/netmask lists are allocated by the caller on the
 * heap.  Freed later by common initiator_group_destroy() code
 */
int
spdk_iscsi_init_grp_create_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int i, rc = 0;
	struct spdk_iscsi_init_grp *ig = NULL;

	/* Make sure there are no duplicate initiator group tags */
	if (spdk_iscsi_init_grp_find_by_tag(tag)) {
		SPDK_ERRLOG("initiator group creation failed.  duplicate initiator group tag (%d)\n", tag);
		rc = -EEXIST;
		goto cleanup;
	}

	if (num_initiator_names > MAX_INITIATOR) {
		SPDK_ERRLOG("%d > MAX_INITIATOR\n", num_initiator_names);
		rc = -1;
		goto cleanup;
	}

	if (num_initiator_masks > MAX_NETMASK) {
		SPDK_ERRLOG("%d > MAX_NETMASK\n", num_initiator_masks);
		rc = -1;
		goto cleanup;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
		      "add initiator group (from initiator list) tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	ig = malloc(sizeof(*ig));
	if (!ig) {
		SPDK_ERRLOG("initiator group malloc error (%d)\n", tag);
		rc = -ENOMEM;
		goto cleanup;
	}

	memset(ig, 0, sizeof(*ig));
	ig->ref = 0;
	ig->tag = tag;

	ig->ninitiators = num_initiator_names;
	ig->nnetmasks = num_initiator_masks;
	ig->initiators = initiator_names;
	for (i = 0; i < num_initiator_names; i++)
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "InitiatorName %s\n",
			      ig->initiators[i]);

	ig->netmasks = initiator_masks;
	for (i = 0; i < num_initiator_masks; i++)
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Netmask %s\n",
			      ig->netmasks[i]);

	ig->state = GROUP_INIT;
	spdk_iscsi_init_grp_register(ig);

	return 0;

cleanup:
	free(ig);
	return rc;
}

void
spdk_iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig)
{
	int i;

	if (!ig) {
		return;
	}

	for (i = 0; i < ig->ninitiators; i++) {
		free(ig->initiators[i]);
	}

	for (i = 0; i < ig->nnetmasks; i++) {
		free(ig->netmasks[i]);
	}

	free(ig->initiators);
	free(ig->netmasks);

	free(ig);
};

struct spdk_iscsi_init_grp *
spdk_iscsi_init_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_init_grp *ig;

	TAILQ_FOREACH(ig, &g_spdk_iscsi.ig_head, tailq) {
		if (ig->tag == tag) {
			return ig;
		}
	}

	return NULL;
}

void
spdk_iscsi_init_grp_destroy_by_tag(int tag)
{
	spdk_iscsi_init_grp_destroy(spdk_iscsi_init_grp_find_by_tag(tag));
}

void
spdk_iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig)
{
	assert(ig != NULL);

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	ig->state = GROUP_READY;
	TAILQ_INSERT_TAIL(&g_spdk_iscsi.ig_head, ig, tailq);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

int
spdk_iscsi_init_grp_array_create(void)
{
	struct spdk_conf_section *sp;
	int rc;

	TAILQ_INIT(&g_spdk_iscsi.ig_head);
	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "InitiatorGroup")) {
			if (spdk_conf_section_get_num(sp) == 0) {
				SPDK_ERRLOG("Group 0 is invalid\n");
				return -1;
			}
			rc = spdk_iscsi_init_grp_create_from_configfile(sp);
			if (rc < 0) {
				SPDK_ERRLOG("add_initiator_group() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

void
spdk_iscsi_init_grp_array_destroy(void)
{
	struct spdk_iscsi_init_grp *ig, *tmp;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_init_grp_array_destroy\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(ig, &g_spdk_iscsi.ig_head, tailq, tmp) {
		ig->state = GROUP_DESTROY;
		TAILQ_REMOVE(&g_spdk_iscsi.ig_head, ig, tailq);
		spdk_iscsi_init_grp_destroy(ig);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}
static inline void
spdk_initiator_group_unregister(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_init_grp *initiator_group;
	struct spdk_iscsi_init_grp *initiator_group_tmp;

	assert(ig != NULL);

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(initiator_group, &g_spdk_iscsi.ig_head, tailq, initiator_group_tmp) {
		if (ig->tag == initiator_group->tag)
			TAILQ_REMOVE(&g_spdk_iscsi.ig_head, initiator_group, tailq);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

int
spdk_iscsi_init_grp_deletable(int tag)
{
	int ret = 0;
	struct spdk_iscsi_init_grp *ig;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	ig = spdk_iscsi_init_grp_find_by_tag(tag);
	if (ig == NULL) {
		ret = -1;
		goto out;
	}

	if (ig->state != GROUP_READY) {
		ret = -1;
		goto out;
	}

	if (ig->ref == 0) {
		ret = 0;
		goto out;
	}

out:
	if (ret == 0)
		ig->state = GROUP_DESTROY;
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return ret;
}

void
spdk_iscsi_init_grp_release(struct spdk_iscsi_init_grp *ig)
{
	spdk_initiator_group_unregister(ig);
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	spdk_iscsi_init_grp_destroy(ig);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}
