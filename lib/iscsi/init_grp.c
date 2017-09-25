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

static struct spdk_iscsi_init_grp *
spdk_iscsi_init_grp_create(int tag)
{
	struct spdk_iscsi_init_grp *ig;

	if (spdk_iscsi_init_grp_find_by_tag(tag)) {
		SPDK_ERRLOG("duplicate init group tag (%d)\n", tag);
		return NULL;
	}

	ig = malloc(sizeof(*ig));
	if (ig == NULL) {
		SPDK_ERRLOG("initiator group malloc error (%d)\n", tag);
		return NULL;
	}

	memset(ig, 0, sizeof(*ig));

	ig->ref = 0;
	ig->tag = tag;
	ig->ninitiators = 0;
	ig->nnetmasks = 0;
	SLIST_INIT(&ig->initiator_head);
	SLIST_INIT(&ig->netmask_head);
	ig->state = GROUP_INIT;
	return ig;
}

static struct spdk_iscsi_initiator_name *
spdk_iscsi_init_grp_find_initiator(struct spdk_iscsi_init_grp *ig,
				   char *name)
{
	struct spdk_iscsi_initiator_name *iname;

	SLIST_FOREACH(iname, &ig->initiator_head, slist) {
		if (!strcmp(iname->name, name)) {
			return iname;
		}
	}
	return NULL;
}

static int
spdk_iscsi_init_grp_add_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;

	if (ig->ninitiators >= MAX_INITIATOR) {
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "InitiatorName %s\n", name);

	iname = spdk_iscsi_init_grp_find_initiator(ig, name);
	if (iname != NULL) {
		return -1;
	}
	iname = malloc(sizeof(*iname));
	if (iname == NULL) {
		return -1;
	}
	iname->name = strdup(name);
	if (iname->name == NULL) {
		free(iname);
		return -1;
	}
	SLIST_INSERT_HEAD(&ig->initiator_head, iname, slist);
	ig->ninitiators++;
	return 0;
}
	
static int
spdk_iscsi_init_grp_delete_initiator(struct spdk_iscsi_init_grp *ig,
                                     char *name)
{
	struct spdk_iscsi_initiator_name *iname;

	iname = spdk_iscsi_init_grp_find_initiator(ig, name);
	if (iname == NULL) {
		return -1;
	}

	SLIST_REMOVE(&ig->initiator_head, name, spdk_iscsi_initiator_name,
		     slist);
	ig->ninitiators--;
	free(iname->name);
	free(iname);
	return 0;
}

static void
spdk_iscsi_init_grp_delete_all_initiators(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_name *iname;

	while (!SLIST_EMPTY(&ig->initiator_head)) {
		iname = SLIST_FIRST(&ig->initiator_head);
		SLIST_REMOVE_HEAD(&ig->initiator_head, slist);
		ig->ninitiators--;
		free(iname->name);
		free(iname);
	}
}

static int
spdk_iscsi_init_grp_add_initiators(struct spdk_iscsi_init_grp *ig,
				   int num_inames, char **inames)
{
	int i;
	int rc;

	for (i = 0; i < num_inames; i++) {
		rc = spdk_iscsi_init_grp_add_initiator(ig, inames[i]);
		if (rc < 0) {
			goto invalid;
		}
	}
	return 0;

invalid:
	for ( ; i > 0; --i) {
		spdk_iscsi_init_grp_delete_initiator(ig, inames[i-1]);
	}
	return -1;
}

static struct spdk_iscsi_initiator_netmask *
spdk_iscsi_init_grp_find_netmask(struct spdk_iscsi_init_grp *ig,
				 const char *mask)
{
	struct spdk_iscsi_initiator_netmask *nmask;

	SLIST_FOREACH(nmask, &ig->netmask_head, slist) {
	if (!strcmp(nmask->mask, mask)) {
			return nmask;
		}
	}
	return NULL;
}

static int
spdk_iscsi_init_grp_add_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *nmask;

	if (ig->nnetmasks >= MAX_NETMASK) {
		return -1;
	}

	if (spdk_iscsi_init_grp_find_netmask(ig, mask)) {
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Netmask %s\n", mask);

	nmask = malloc(sizeof(*nmask));
	if (nmask == NULL) {
		return -1;
	}
	nmask->mask = strdup(mask);
	if (nmask->mask == NULL) {
		free(nmask);
		return -1;
	}
	SLIST_INSERT_HEAD(&ig->netmask_head, nmask, slist);
	ig->nnetmasks++;
	return 0;
}

static int
spdk_iscsi_init_grp_delete_netmask(struct spdk_iscsi_init_grp *ig,
				   const char *mask)
{
	struct spdk_iscsi_initiator_netmask *nmask;

	nmask = spdk_iscsi_init_grp_find_netmask(ig, mask);
	if (nmask == NULL) {
		return -1;
	}

	SLIST_REMOVE(&ig->netmask_head, nmask, spdk_iscsi_initiator_netmask,
		     slist);
	ig->nnetmasks--;
	free(nmask->mask);
	free(nmask);
	return 0;
}

static void
spdk_iscsi_init_grp_delete_all_netmasks(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_netmask *nmask;

	while (!SLIST_EMPTY(&ig->netmask_head)) {
		nmask = SLIST_FIRST(&ig->netmask_head);
		SLIST_REMOVE_HEAD(&ig->netmask_head, slist);
		ig->nnetmasks--;
		free(nmask->mask);
		free(nmask);
	}
}

static int
spdk_iscsi_init_grp_add_netmasks(struct spdk_iscsi_init_grp *ig,
				 int num_nmasks, char **nmasks)
{
	int i;
	int rc;

	for (i = 0; i < num_nmasks; i++) {
		rc = spdk_iscsi_init_grp_add_netmask(ig, nmasks[i]);
		if (rc != 0) {
			goto invalid;
		}
	}
	return 0;

invalid:
	for ( ; i > 0; --i) {
		spdk_iscsi_init_grp_delete_netmask(ig, nmasks[i-1]);
        }
        return -1;
}

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
	int rc = -1;
	struct spdk_iscsi_init_grp *ig;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
		      "add initiator group (from initiator list) tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	ig = spdk_iscsi_init_grp_create(tag);
	if (!ig) {
		SPDK_ERRLOG("initiator group create error\n");
		goto cleanup;
	}

	rc = spdk_iscsi_init_grp_add_initiators(ig, num_initiator_names,
						initiator_names);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator name error\n");
		goto cleanup;
	}

	rc = spdk_iscsi_init_grp_add_netmasks(ig, num_initiator_masks,
					      initiator_masks);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator netmask error\n");
		goto cleanup;
	}

	ig->state = GROUP_INIT;
	spdk_iscsi_init_grp_register(ig);

	return 0;

cleanup:
	spdk_iscsi_init_grp_destroy(ig);
	return rc;
}

void
spdk_iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig)
{
	if (!ig) {
		return;
	}

	spdk_iscsi_init_grp_delete_all_initiators(ig);
	spdk_iscsi_init_grp_delete_all_netmasks(ig);
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

void
spdk_iscsi_init_grp_release(struct spdk_iscsi_init_grp *ig)
{
	spdk_initiator_group_unregister(ig);
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	spdk_iscsi_init_grp_destroy(ig);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}
