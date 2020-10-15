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

#include "spdk/string.h"

#include "spdk/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/init_grp.h"

static struct spdk_iscsi_init_grp *
iscsi_init_grp_create(int tag)
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
iscsi_init_grp_find_initiator(struct spdk_iscsi_init_grp *ig, char *name)
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
iscsi_init_grp_add_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;
	char *p;
	size_t len;

	if (ig->ninitiators >= MAX_INITIATOR) {
		SPDK_ERRLOG("> MAX_INITIATOR(=%d) is not allowed\n", MAX_INITIATOR);
		return -EPERM;
	}

	len = strlen(name);
	if (len > MAX_INITIATOR_NAME) {
		SPDK_ERRLOG("Initiator Name is larger than 223 bytes\n");
		return -EINVAL;
	}

	iname = iscsi_init_grp_find_initiator(ig, name);
	if (iname != NULL) {
		return -EEXIST;
	}

	iname = calloc(1, sizeof(*iname));
	if (iname == NULL) {
		SPDK_ERRLOG("malloc() failed for initiator name str\n");
		return -ENOMEM;
	}

	memcpy(iname->name, name, len);

	/* Replace "ALL" by "ANY" if set */
	p = strstr(iname->name, "ALL");
	if (p != NULL) {
		SPDK_WARNLOG("Please use \"%s\" instead of \"%s\"\n", "ANY", "ALL");
		SPDK_WARNLOG("Converting \"%s\" to \"%s\" automatically\n", "ALL", "ANY");
		memcpy(p, "ANY", 3);
	}

	TAILQ_INSERT_TAIL(&ig->initiator_head, iname, tailq);
	ig->ninitiators++;

	SPDK_DEBUGLOG(iscsi, "InitiatorName %s\n", name);
	return 0;
}

static int
iscsi_init_grp_delete_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;

	iname = iscsi_init_grp_find_initiator(ig, name);
	if (iname == NULL) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&ig->initiator_head, iname, tailq);
	ig->ninitiators--;
	free(iname);
	return 0;
}

static int
iscsi_init_grp_add_initiators(struct spdk_iscsi_init_grp *ig, int num_inames,
			      char **inames)
{
	int i;
	int rc;

	for (i = 0; i < num_inames; i++) {
		rc = iscsi_init_grp_add_initiator(ig, inames[i]);
		if (rc < 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		iscsi_init_grp_delete_initiator(ig, inames[i - 1]);
	}
	return rc;
}

static void
iscsi_init_grp_delete_all_initiators(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_name *iname, *tmp;

	TAILQ_FOREACH_SAFE(iname, &ig->initiator_head, tailq, tmp) {
		TAILQ_REMOVE(&ig->initiator_head, iname, tailq);
		ig->ninitiators--;
		free(iname);
	}
}

static int
iscsi_init_grp_delete_initiators(struct spdk_iscsi_init_grp *ig, int num_inames, char **inames)
{
	int i;
	int rc;

	for (i = 0; i < num_inames; i++) {
		rc = iscsi_init_grp_delete_initiator(ig, inames[i]);
		if (rc < 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		rc = iscsi_init_grp_add_initiator(ig, inames[i - 1]);
		if (rc != 0) {
			iscsi_init_grp_delete_all_initiators(ig);
			break;
		}
	}
	return -1;
}

static struct spdk_iscsi_initiator_netmask *
iscsi_init_grp_find_netmask(struct spdk_iscsi_init_grp *ig, const char *mask)
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
iscsi_init_grp_add_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *imask;
	char *p;
	size_t len;

	if (ig->nnetmasks >= MAX_NETMASK) {
		SPDK_ERRLOG("> MAX_NETMASK(=%d) is not allowed\n", MAX_NETMASK);
		return -EPERM;
	}

	len = strlen(mask);
	if (len > MAX_INITIATOR_ADDR) {
		SPDK_ERRLOG("Initiator Name is larger than %d bytes\n", MAX_INITIATOR_ADDR);
		return -EINVAL;
	}

	imask = iscsi_init_grp_find_netmask(ig, mask);
	if (imask != NULL) {
		return -EEXIST;
	}

	imask = calloc(1, sizeof(*imask));
	if (imask == NULL) {
		SPDK_ERRLOG("malloc() failed for inititator mask str\n");
		return -ENOMEM;
	}

	memcpy(imask->mask, mask, len);

	/* Replace "ALL" by "ANY" if set */
	p = strstr(imask->mask, "ALL");
	if (p != NULL) {
		SPDK_WARNLOG("Please use \"%s\" instead of \"%s\"\n", "ANY", "ALL");
		SPDK_WARNLOG("Converting \"%s\" to \"%s\" automatically\n", "ALL", "ANY");
		memcpy(p, "ANY", 3);
	}

	TAILQ_INSERT_TAIL(&ig->netmask_head, imask, tailq);
	ig->nnetmasks++;

	SPDK_DEBUGLOG(iscsi, "Netmask %s\n", mask);
	return 0;
}

static int
iscsi_init_grp_delete_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *imask;

	imask = iscsi_init_grp_find_netmask(ig, mask);
	if (imask == NULL) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&ig->netmask_head, imask, tailq);
	ig->nnetmasks--;
	free(imask);
	return 0;
}

static int
iscsi_init_grp_add_netmasks(struct spdk_iscsi_init_grp *ig, int num_imasks, char **imasks)
{
	int i;
	int rc;

	for (i = 0; i < num_imasks; i++) {
		rc = iscsi_init_grp_add_netmask(ig, imasks[i]);
		if (rc != 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		iscsi_init_grp_delete_netmask(ig, imasks[i - 1]);
	}
	return rc;
}

static void
iscsi_init_grp_delete_all_netmasks(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_netmask *imask, *tmp;

	TAILQ_FOREACH_SAFE(imask, &ig->netmask_head, tailq, tmp) {
		TAILQ_REMOVE(&ig->netmask_head, imask, tailq);
		ig->nnetmasks--;
		free(imask);
	}
}

static int
iscsi_init_grp_delete_netmasks(struct spdk_iscsi_init_grp *ig, int num_imasks, char **imasks)
{
	int i;
	int rc;

	for (i = 0; i < num_imasks; i++) {
		rc = iscsi_init_grp_delete_netmask(ig, imasks[i]);
		if (rc != 0) {
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; i > 0; --i) {
		rc = iscsi_init_grp_add_netmask(ig, imasks[i - 1]);
		if (rc != 0) {
			iscsi_init_grp_delete_all_netmasks(ig);
			break;
		}
	}
	return -1;
}

int
iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_init_grp *tmp;
	int rc = -1;

	assert(ig != NULL);

	pthread_mutex_lock(&g_iscsi.mutex);
	tmp = iscsi_init_grp_find_by_tag(ig->tag);
	if (tmp == NULL) {
		TAILQ_INSERT_TAIL(&g_iscsi.ig_head, ig, tailq);
		rc = 0;
	}
	pthread_mutex_unlock(&g_iscsi.mutex);

	return rc;
}

/*
 * Create initiator group from list of initiator ip/hostnames and netmasks
 * The initiator hostname/netmask lists are allocated by the caller on the
 * heap.  Freed later by common initiator_group_destroy() code
 */
int
iscsi_init_grp_create_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	struct spdk_iscsi_init_grp *ig = NULL;

	SPDK_DEBUGLOG(iscsi,
		      "add initiator group (from initiator list) tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	ig = iscsi_init_grp_create(tag);
	if (!ig) {
		SPDK_ERRLOG("initiator group create error (%d)\n", tag);
		return rc;
	}

	rc = iscsi_init_grp_add_initiators(ig, num_initiator_names,
					   initiator_names);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator name error\n");
		goto cleanup;
	}

	rc = iscsi_init_grp_add_netmasks(ig, num_initiator_masks,
					 initiator_masks);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator netmask error\n");
		goto cleanup;
	}

	rc = iscsi_init_grp_register(ig);
	if (rc < 0) {
		SPDK_ERRLOG("initiator group register error (%d)\n", tag);
		goto cleanup;
	}
	return 0;

cleanup:
	iscsi_init_grp_destroy(ig);
	return rc;
}

int
iscsi_init_grp_add_initiators_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	struct spdk_iscsi_init_grp *ig;

	SPDK_DEBUGLOG(iscsi,
		      "add initiator to initiator group: tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	pthread_mutex_lock(&g_iscsi.mutex);
	ig = iscsi_init_grp_find_by_tag(tag);
	if (!ig) {
		pthread_mutex_unlock(&g_iscsi.mutex);
		SPDK_ERRLOG("initiator group (%d) is not found\n", tag);
		return rc;
	}

	rc = iscsi_init_grp_add_initiators(ig, num_initiator_names,
					   initiator_names);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator name error\n");
		goto error;
	}

	rc = iscsi_init_grp_add_netmasks(ig, num_initiator_masks,
					 initiator_masks);
	if (rc < 0) {
		SPDK_ERRLOG("add initiator netmask error\n");
		iscsi_init_grp_delete_initiators(ig, num_initiator_names,
						 initiator_names);
	}

error:
	pthread_mutex_unlock(&g_iscsi.mutex);
	return rc;
}

int
iscsi_init_grp_delete_initiators_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	struct spdk_iscsi_init_grp *ig;

	SPDK_DEBUGLOG(iscsi,
		      "delete initiator from initiator group: tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);

	pthread_mutex_lock(&g_iscsi.mutex);
	ig = iscsi_init_grp_find_by_tag(tag);
	if (!ig) {
		pthread_mutex_unlock(&g_iscsi.mutex);
		SPDK_ERRLOG("initiator group (%d) is not found\n", tag);
		return rc;
	}

	rc = iscsi_init_grp_delete_initiators(ig, num_initiator_names,
					      initiator_names);
	if (rc < 0) {
		SPDK_ERRLOG("delete initiator name error\n");
		goto error;
	}

	rc = iscsi_init_grp_delete_netmasks(ig, num_initiator_masks,
					    initiator_masks);
	if (rc < 0) {
		SPDK_ERRLOG("delete initiator netmask error\n");
		iscsi_init_grp_add_initiators(ig, num_initiator_names,
					      initiator_names);
		goto error;
	}

error:
	pthread_mutex_unlock(&g_iscsi.mutex);
	return rc;
}

void
iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig)
{
	if (!ig) {
		return;
	}

	iscsi_init_grp_delete_all_initiators(ig);
	iscsi_init_grp_delete_all_netmasks(ig);
	free(ig);
};

struct spdk_iscsi_init_grp *
iscsi_init_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_init_grp *ig;

	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		if (ig->tag == tag) {
			return ig;
		}
	}

	return NULL;
}

void
iscsi_init_grps_destroy(void)
{
	struct spdk_iscsi_init_grp *ig, *tmp;

	SPDK_DEBUGLOG(iscsi, "iscsi_init_grp_array_destroy\n");
	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH_SAFE(ig, &g_iscsi.ig_head, tailq, tmp) {
		TAILQ_REMOVE(&g_iscsi.ig_head, ig, tailq);
		iscsi_init_grp_destroy(ig);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

struct spdk_iscsi_init_grp *
iscsi_init_grp_unregister(int tag)
{
	struct spdk_iscsi_init_grp *ig;

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		if (ig->tag == tag) {
			TAILQ_REMOVE(&g_iscsi.ig_head, ig, tailq);
			pthread_mutex_unlock(&g_iscsi.mutex);
			return ig;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return NULL;
}

static void
iscsi_init_grp_info_json(struct spdk_iscsi_init_grp *ig,
			 struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_initiator_name *iname;
	struct spdk_iscsi_initiator_netmask *imask;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_int32(w, "tag", ig->tag);

	spdk_json_write_named_array_begin(w, "initiators");
	TAILQ_FOREACH(iname, &ig->initiator_head, tailq) {
		spdk_json_write_string(w, iname->name);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_array_begin(w, "netmasks");
	TAILQ_FOREACH(imask, &ig->netmask_head, tailq) {
		spdk_json_write_string(w, imask->mask);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

static void
iscsi_init_grp_config_json(struct spdk_iscsi_init_grp *ig,
			   struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "iscsi_create_initiator_group");

	spdk_json_write_name(w, "params");
	iscsi_init_grp_info_json(ig, w);

	spdk_json_write_object_end(w);
}

void
iscsi_init_grps_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_init_grp *ig;

	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		iscsi_init_grp_info_json(ig, w);
	}
}

void
iscsi_init_grps_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_init_grp *ig;

	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		iscsi_init_grp_config_json(ig, w);
	}
}
