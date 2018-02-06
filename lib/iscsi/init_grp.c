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

#include "spdk_internal/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/init_grp.h"

static struct spdk_iscsi_init_grp *
spdk_iscsi_init_grp_create(int tag)
{
	struct spdk_iscsi_init_grp *ig;

	ig = calloc(1, sizeof(*ig));
	if (ig == NULL) {
		SPDK_ERRLOG("calloc() failed for initiator group\n");
		return NULL;
	}

	ig->tag = tag;
	TAILQ_INIT(&ig->initiator_head);
	TAILQ_INIT(&ig->netmask_head);
	return ig;
}

static struct spdk_iscsi_initiator_name *
spdk_iscsi_init_grp_find_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;

	TAILQ_FOREACH(iname, &ig->initiator_head, tailq) {
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
	char *p;

	if (ig->ninitiators >= MAX_INITIATOR) {
		SPDK_ERRLOG("> MAX_INITIATOR(=%d) is not allowed\n", MAX_INITIATOR);
		return -EPERM;
	}

	iname = spdk_iscsi_init_grp_find_initiator(ig, name);
	if (iname != NULL) {
		return -EEXIST;
	}

	iname = malloc(sizeof(*iname));
	if (iname == NULL) {
		SPDK_ERRLOG("malloc() failed for initiator name str\n");
		return -ENOMEM;
	}

	iname->name = strdup(name);
	if (iname->name == NULL) {
		SPDK_ERRLOG("strdup() failed for initiator name\n");
		free(iname);
		return -ENOMEM;
	}

	/* Replace "ALL" by "ANY" if set */
	p = strstr(iname->name, "ALL");
	if (p != NULL) {
		SPDK_WARNLOG("Please use \"%s\" instead of \"%s\"\n", "ANY", "ALL");
		SPDK_WARNLOG("Converting \"%s\" to \"%s\" automatically\n", "ALL", "ANY");
		strncpy(p, "ANY", 3);
	}

	TAILQ_INSERT_TAIL(&ig->initiator_head, iname, tailq);
	ig->ninitiators++;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "InitiatorName %s\n", name);
	return 0;
}

static int
spdk_iscsi_init_grp_delete_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;

	iname = spdk_iscsi_init_grp_find_initiator(ig, name);
	if (iname == NULL) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&ig->initiator_head, iname, tailq);
	ig->ninitiators--;
	free(iname->name);
	free(iname);
	return 0;
}

static int
spdk_iscsi_init_grp_add_initiators(struct spdk_iscsi_init_grp *ig, int num_inames, char **inames)
{
	int i;
	int rc;

	for (i = 0; i < num_inames; i++) {
		rc = spdk_iscsi_init_grp_add_initiator(ig, inames[i]);
		if (rc < 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		spdk_iscsi_init_grp_delete_initiator(ig, inames[i - 1]);
	}
	return rc;
}

static void
spdk_iscsi_init_grp_delete_all_initiators(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_name *iname, *tmp;

	TAILQ_FOREACH_SAFE(iname, &ig->initiator_head, tailq, tmp) {
		TAILQ_REMOVE(&ig->initiator_head, iname, tailq);
		ig->ninitiators--;
		free(iname->name);
		free(iname);
	}
}

static int
spdk_iscsi_init_grp_delete_initiators(struct spdk_iscsi_init_grp *ig, int num_inames, char **inames)
{
	int i;
	int rc;

	for (i = 0; i < num_inames; i++) {
		rc = spdk_iscsi_init_grp_delete_initiator(ig, inames[i]);
		if (rc < 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		rc = spdk_iscsi_init_grp_add_initiator(ig, inames[i - 1]);
		if (rc != 0) {
			spdk_iscsi_init_grp_delete_all_initiators(ig);
			break;
		}
	}
	return -1;
}

static struct spdk_iscsi_initiator_netmask *
spdk_iscsi_init_grp_find_netmask(struct spdk_iscsi_init_grp *ig, const char *mask)
{
	struct spdk_iscsi_initiator_netmask *netmask;

	TAILQ_FOREACH(netmask, &ig->netmask_head, tailq) {
		if (!strcmp(netmask->mask, mask)) {
			return netmask;
		}
	}
	return NULL;
}

static int
spdk_iscsi_init_grp_add_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *imask;
	char *p;

	if (ig->nnetmasks >= MAX_NETMASK) {
		SPDK_ERRLOG("> MAX_NETMASK(=%d) is not allowed\n", MAX_NETMASK);
		return -EPERM;
	}

	imask = spdk_iscsi_init_grp_find_netmask(ig, mask);
	if (imask != NULL) {
		return -EEXIST;
	}

	imask = malloc(sizeof(*imask));
	if (imask == NULL) {
		SPDK_ERRLOG("malloc() failed for inititator mask str\n");
		return -ENOMEM;
	}

	imask->mask = strdup(mask);
	if (imask->mask == NULL) {
		SPDK_ERRLOG("strdup() failed for initiator mask\n");
		free(imask);
		return -ENOMEM;
	}

	/* Replace "ALL" by "ANY" if set */
	p = strstr(imask->mask, "ALL");
	if (p != NULL) {
		SPDK_WARNLOG("Please use \"%s\" instead of \"%s\"\n", "ANY", "ALL");
		SPDK_WARNLOG("Converting \"%s\" to \"%s\" automatically\n", "ALL", "ANY");
		strncpy(p, "ANY", 3);
	}

	TAILQ_INSERT_TAIL(&ig->netmask_head, imask, tailq);
	ig->nnetmasks++;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Netmask %s\n", mask);
	return 0;
}

static int
spdk_iscsi_init_grp_delete_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *imask;

	imask = spdk_iscsi_init_grp_find_netmask(ig, mask);
	if (imask == NULL) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&ig->netmask_head, imask, tailq);
	ig->nnetmasks--;
	free(imask->mask);
	free(imask);
	return 0;
}

static int
spdk_iscsi_init_grp_add_netmasks(struct spdk_iscsi_init_grp *ig, int num_imasks, char **imasks)
{
	int i;
	int rc;

	for (i = 0; i < num_imasks; i++) {
		rc = spdk_iscsi_init_grp_add_netmask(ig, imasks[i]);
		if (rc != 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		spdk_iscsi_init_grp_delete_netmask(ig, imasks[i - 1]);
	}
	return rc;
}

static void
spdk_iscsi_init_grp_delete_all_netmasks(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_netmask *imask, *tmp;

	TAILQ_FOREACH_SAFE(imask, &ig->netmask_head, tailq, tmp) {
		TAILQ_REMOVE(&ig->netmask_head, imask, tailq);
		ig->nnetmasks--;
		free(imask->mask);
		free(imask);
	}
}

static int
spdk_iscsi_init_grp_delete_netmasks(struct spdk_iscsi_init_grp *ig, int num_imasks, char **imasks)
{
	int i;
	int rc;

	for (i = 0; i < num_imasks; i++) {
		rc = spdk_iscsi_init_grp_delete_netmask(ig, imasks[i]);
		if (rc != 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		rc = spdk_iscsi_init_grp_add_netmask(ig, imasks[i - 1]);
		if (rc != 0) {
			spdk_iscsi_init_grp_delete_all_netmasks(ig);
			break;
		}
	}
	return -1;
}

/* Read spdk iscsi target's config file and create initiator group */
static int
spdk_iscsi_init_grp_create_from_configfile(struct spdk_conf_section *sp)
{
	int i, rc = 0;
	const char *val = NULL;
	int num_initiator_names;
	int num_initiator_masks;
	char **initiators = NULL, **netmasks = NULL;
	int tag = spdk_conf_section_get_num(sp);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "add initiator group %d\n", tag);

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Comment %s\n", val);
	}

	/* counts number of definitions */
	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "InitiatorName", i);
		if (val == NULL) {
			break;
		}
	}
	if (i == 0) {
		SPDK_ERRLOG("num_initiator_names = 0\n");
		return -EINVAL;
	}
	num_initiator_names = i;
	if (num_initiator_names > MAX_INITIATOR) {
		SPDK_ERRLOG("%d > MAX_INITIATOR\n", num_initiator_names);
		return -E2BIG;
	}
	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "Netmask", i);
		if (val == NULL) {
			break;
		}
	}
	if (i == 0) {
		SPDK_ERRLOG("num_initiator_mask = 0\n");
		return -EINVAL;
	}
	num_initiator_masks = i;
	if (num_initiator_masks > MAX_NETMASK) {
		SPDK_ERRLOG("%d > MAX_NETMASK\n", num_initiator_masks);
		return -E2BIG;
	}

	initiators = calloc(num_initiator_names, sizeof(char *));
	if (!initiators) {
		SPDK_ERRLOG("calloc() failed for temp initiator name array\n");
		return -ENOMEM;
	}
	for (i = 0; i < num_initiator_names; i++) {
		val = spdk_conf_section_get_nval(sp, "InitiatorName", i);
		if (!val) {
			SPDK_ERRLOG("InitiatorName %d not found\n", i);
			rc = -EINVAL;
			goto cleanup;
		}
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "InitiatorName %s\n", val);
		initiators[i] = strdup(val);
		if (!initiators[i]) {
			SPDK_ERRLOG("strdup() failed for temp initiator name\n");
			rc = -ENOMEM;
			goto cleanup;
		}
	}
	netmasks = calloc(num_initiator_masks, sizeof(char *));
	if (!netmasks) {
		SPDK_ERRLOG("malloc() failed for portal group\n");
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
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Netmask %s\n", val);
		netmasks[i] = strdup(val);
		if (!netmasks[i]) {
			SPDK_ERRLOG("strdup() failed for temp initiator mask\n");
			rc = -ENOMEM;
			goto cleanup;
		}
	}

	rc = spdk_iscsi_init_grp_create_from_initiator_list(tag,
			num_initiator_names, initiators, num_initiator_masks, netmasks);

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

int
spdk_iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_init_grp *tmp;
	int rc = -1;

	assert(ig != NULL);

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	tmp = spdk_iscsi_init_grp_find_by_tag(ig->tag);
	if (tmp == NULL) {
		TAILQ_INSERT_TAIL(&g_spdk_iscsi.ig_head, ig, tailq);
		rc = 0;
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

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
	struct spdk_iscsi_init_grp *ig = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
		      "add initiator group (from initiator list) tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	ig = spdk_iscsi_init_grp_create(tag);
	if (!ig) {
		SPDK_ERRLOG("initiator group create error (%d)\n", tag);
		return rc;
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

	rc = spdk_iscsi_init_grp_register(ig);
	if (rc < 0) {
		SPDK_ERRLOG("initiator group register error (%d)\n", tag);
		goto cleanup;
	}
	return 0;

cleanup:
	spdk_iscsi_init_grp_destroy(ig);
	return rc;
}

int
spdk_iscsi_init_grp_add_initiator_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	struct spdk_iscsi_init_grp *ig;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
		      "add initiator to initiator group: tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	ig = spdk_iscsi_init_grp_find_by_tag(tag);
	if (!ig) {
		pthread_mutex_unlock(&g_spdk_iscsi.mutex);
		SPDK_ERRLOG("initiator group (%d) is not found\n", tag);
		return rc;
	}

	rc = spdk_iscsi_init_grp_add_initiators(ig, num_initiator_names,
						initiator_names);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator name error\n");
		goto error;
	}

	rc = spdk_iscsi_init_grp_add_netmasks(ig, num_initiator_masks,
					      initiator_masks);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator netmask error\n");
		spdk_iscsi_init_grp_delete_initiators(ig, num_initiator_names,
						      initiator_names);
		goto error;
	}

	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;

error:
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return rc;
}

int
spdk_iscsi_init_grp_delete_initiator_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	struct spdk_iscsi_init_grp *ig;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
		      "delete initiator from initiator group: tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	ig = spdk_iscsi_init_grp_find_by_tag(tag);
	if (!ig) {
		pthread_mutex_unlock(&g_spdk_iscsi.mutex);
		SPDK_ERRLOG("initiator group (%d) is not found\n", tag);
		return rc;
	}

	rc = spdk_iscsi_init_grp_delete_initiators(ig, num_initiator_names,
			initiator_names);
	if (rc < 0) {
		SPDK_ERRLOG("delete initiator name error\n");
		goto error;
	}

	rc = spdk_iscsi_init_grp_delete_netmasks(ig, num_initiator_masks,
			initiator_masks);
	if (rc < 0) {
		SPDK_ERRLOG("delete initiator netmask error\n");
		spdk_iscsi_init_grp_add_initiators(ig, num_initiator_names,
						   initiator_names);
		goto error;
	}

	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;

error:
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
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

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_init_grp_array_destroy\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(ig, &g_spdk_iscsi.ig_head, tailq, tmp) {
		TAILQ_REMOVE(&g_spdk_iscsi.ig_head, ig, tailq);
		spdk_iscsi_init_grp_destroy(ig);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

struct spdk_iscsi_init_grp *
spdk_iscsi_init_grp_unregister(int tag)
{
	struct spdk_iscsi_init_grp *ig;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH(ig, &g_spdk_iscsi.ig_head, tailq) {
		if (ig->tag == tag) {
			TAILQ_REMOVE(&g_spdk_iscsi.ig_head, ig, tailq);
			pthread_mutex_unlock(&g_spdk_iscsi.mutex);
			return ig;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return NULL;
}
