/*-
 *   BSD LICENSE
 *
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

#include <ctype.h>

#include "controller.h"
#include "port.h"
#include "host.h"
#include "nvmf_internal.h"
#include "session.h"
#include "subsystem.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

#define SPDK_CN_TAG_MAX 0x0000ffff

static TAILQ_HEAD(, spdk_nvmf_subsystem_grp) g_ssg_head = TAILQ_HEAD_INITIALIZER(g_ssg_head);
static TAILQ_HEAD(, spdk_nvmf_subsystem) g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subs;

	if (subnqn == NULL)
		return NULL;

	TAILQ_FOREACH(subs, &g_subsystems, entries) {
		if (strcasecmp(subnqn, subs->subnqn) == 0) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "found subsystem group with name: %s\n",
				      subnqn);
			return subs;
		}
	}

	fprintf(stderr, "can't find subsystem %s\n", subnqn);
	return NULL;
}

struct spdk_nvmf_subsystem *
nvmf_create_subsystem(int num, char *name, enum spdk_nvmf_subsystem_types sub_type)
{
	struct spdk_nvmf_subsystem	*subsystem;

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	memset(subsystem, 0, sizeof(struct spdk_nvmf_subsystem));
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_create_subsystem: allocated subsystem %p\n", subsystem);

	subsystem->num = num;
	subsystem->subtype = sub_type;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", name);

	TAILQ_INSERT_HEAD(&g_subsystems, subsystem, entries);

	return subsystem;
}

int
nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	if (subsystem == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF,
			      "nvmf_delete_subsystem: there is no subsystem\n");
		return 0;
	}

	if (subsystem->session) {
		spdk_nvmf_session_destruct(subsystem->session);
	}

	TAILQ_REMOVE(&g_subsystems, subsystem, entries);

	free(subsystem);
	return 0;
}

static int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr)
{
	subsystem->ctrlr = ctrlr;

	/* Assume that all I/O will be handled on one thread for now */
	subsystem->io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);
	if (subsystem->io_qpair == NULL) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -1;
	}

	return 0;
}

/* nvmf uses NQN format to name target subsystems.  We expect that
   the nvmf subnqn name provided diring connect requests will be
   equivalent to a individual controller name
*/
static int
spdk_check_nvmf_name(const char *name)
{
	size_t len;

	len = strlen(name);
	if (len > SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", name, len, SPDK_NVMF_NQN_MAX_LEN);
		return -1;
	}

	if (strncasecmp(name, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", name);
		return -1;
	}

	/* yyyy-mm. */
	if (!(isdigit(name[4]) && isdigit(name[5]) && isdigit(name[6]) && isdigit(name[7]) &&
	      name[8] == '-' && isdigit(name[9]) && isdigit(name[10]) && name[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", name);
		return -1;
	}

	return 0;
}

static void
spdk_nvmf_subsystem_destruct(struct spdk_nvmf_subsystem_grp *ss_group)
{
	int i;

	if (ss_group == NULL) {
		return;
	}

	free(ss_group->name);

	for (i = 0; i < ss_group->map_count; i++) {
		ss_group->map[i].host->ref--;
	}

	/* Call NVMf library to free the subsystem */
	nvmf_delete_subsystem(ss_group->subsystem);

	free(ss_group);
}

static int
spdk_nvmf_subsystem_add_map(struct spdk_nvmf_subsystem_grp *ss_group,
			    int port_tag, int host_tag)
{
	struct spdk_nvmf_access_map	*map;
	struct spdk_nvmf_port		*port;
	struct spdk_nvmf_host		*host;

	port = spdk_nvmf_port_find_by_tag(port_tag);
	if (port == NULL) {
		SPDK_ERRLOG("%s: Port%d not found\n", ss_group->name, port_tag);
		return -1;
	}
	if (port->state != GROUP_READY) {
		SPDK_ERRLOG("%s: Port%d not active\n", ss_group->name, port_tag);
		return -1;
	}
	host = spdk_nvmf_host_find_by_tag(host_tag);
	if (host == NULL) {
		SPDK_ERRLOG("%s: Host%d not found\n", ss_group->name, host_tag);
		return -1;
	}
	if (host->state != GROUP_READY) {
		SPDK_ERRLOG("%s: Host%d not active\n", ss_group->name, host_tag);
		return -1;
	}
	host->ref++;
	map = &ss_group->map[ss_group->map_count];
	map->port = port;
	map->host = host;
	ss_group->map_count++;

	return 0;
}

static int
spdk_cf_add_nvmf_subsystem(struct spdk_conf_section *sp)
{
	struct spdk_nvmf_subsystem_grp *ss_group;
	const char *port_tag, *ig_tag;
	const char *val, *name;
	int port_tag_i, ig_tag_i;
	struct spdk_nvmf_ctrlr *nvmf_ctrlr;
	int i, ret;

	printf("Provisioning NVMf Subsystem %d:\n", sp->num);

	ss_group = calloc(1, sizeof(*ss_group));
	if (!ss_group) {
		SPDK_ERRLOG("could not allocate new subsystem group\n");
		return -1;
	}

	ss_group->num = sp->num;

	/* read in and verify the NQN for the subsystem */
	name = spdk_conf_section_get_val(sp, "SubsystemName");
	if (name == NULL) {
		SPDK_ERRLOG("Subsystem Group %d: SubsystemName not found\n", ss_group->num);
		goto err0;
	}

	if (strncasecmp(name, "nqn.", 4) != 0) {
		ss_group->name = spdk_sprintf_alloc("%s:%s", g_nvmf_tgt.nodebase, name);
	} else {
		ss_group->name = strdup(name);
	}

	if (!ss_group->name) {
		SPDK_ERRLOG("Could not allocate Controller Node name\n");
		goto err0;
	}

	if (spdk_check_nvmf_name(ss_group->name) != 0) {
		SPDK_ERRLOG("Controller Node name (n=%s) (fn=%s) contains an invalid character or format.\n",
			    name, ss_group->name);
		goto err0;
	}

	printf("    NVMf Subsystem: Name: %s\n", ss_group->name);

	/* Setup initiator and port access mapping */
	val = spdk_conf_section_get_val(sp, "Mapping");
	if (val == NULL) {
		/* no access map */
		SPDK_ERRLOG("Subsystem Group %d: no access Mapping\n", ss_group->num);
		goto err0;
	}

	ss_group->map_count = 0;
	for (i = 0; i < MAX_PER_SUBSYSTEM_ACCESS_MAP; i++) {
		val = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		if (val == NULL)
			break;
		port_tag = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		ig_tag = spdk_conf_section_get_nmval(sp, "Mapping", i, 1);
		if (port_tag == NULL || ig_tag == NULL) {
			SPDK_ERRLOG("LU%d: mapping error\n", ss_group->num);
			goto err0;
		}
		if (strncasecmp(port_tag, "Port",
				strlen("Port")) != 0
		    || sscanf(port_tag, "%*[^0-9]%d", &port_tag_i) != 1) {
			SPDK_ERRLOG("LU%d: mapping port error\n", ss_group->num);
			goto err0;
		}
		if (strncasecmp(ig_tag, "Host",
				strlen("Host")) != 0
		    || sscanf(ig_tag, "%*[^0-9]%d", &ig_tag_i) != 1) {
			SPDK_ERRLOG("LU%d: mapping host error\n", ss_group->num);
			goto err0;
		}
		if (port_tag_i < 1 || ig_tag_i < 1) {
			SPDK_ERRLOG("LU%d: invalid group tag\n", ss_group->num);
			goto err0;
		}

		ret = spdk_nvmf_subsystem_add_map(ss_group, port_tag_i, ig_tag_i);
		if (ret < 0) {
			SPDK_ERRLOG("could not init access map within subsystem group\n");
			goto err0;
		}
	}

	/* register this subsystem with the NVMf library */
	ss_group->subsystem = nvmf_create_subsystem(ss_group->num, ss_group->name, SPDK_NVMF_SUB_NVME);
	if (ss_group->subsystem == NULL) {
		SPDK_ERRLOG("Failed creating new nvmf library subsystem\n");
		goto err0;
	}

	val = spdk_conf_section_get_val(sp, "Controller");
	if (val == NULL) {
		SPDK_ERRLOG("Subsystem %d: missing Controller\n", ss_group->num);
		goto err0;
	}

	/* claim this controller from the available controller list */
	nvmf_ctrlr = spdk_nvmf_ctrlr_claim(val);
	if (nvmf_ctrlr == NULL) {
		SPDK_ERRLOG("Subsystem %d: NVMe controller %s not found\n", ss_group->num, val);
		goto err0;
	}

	ret = nvmf_subsystem_add_ctrlr(ss_group->subsystem, nvmf_ctrlr->ctrlr);
	if (ret < 0) {
		SPDK_ERRLOG("Subsystem %d: adding controller %s failed\n", ss_group->num, val);
		goto err0;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "    NVMf Subsystem: Nvme Controller: %s , %p\n",
		      nvmf_ctrlr->name, nvmf_ctrlr->ctrlr);

	TAILQ_INSERT_TAIL(&g_ssg_head, ss_group, tailq);

	return 0;
err0:
	spdk_nvmf_subsystem_destruct(ss_group);
	return -1;
}

static int
spdk_add_nvmf_discovery_subsystem(void)
{
	struct spdk_nvmf_subsystem_grp *ss_group;

	ss_group = calloc(1, sizeof(*ss_group));
	if (!ss_group) {
		SPDK_ERRLOG("could not allocate discovery subsystem group\n");
		return -1;
	}

	ss_group->num = 0xffff;
	ss_group->name = strdup(SPDK_NVMF_DISCOVERY_NQN);
	if (ss_group->name == NULL) {
		SPDK_ERRLOG("strdup ss_group->name error\n");
		free(ss_group);
		return -1;
	}

	ss_group->subsystem = nvmf_create_subsystem(ss_group->num, ss_group->name, SPDK_NVMF_SUB_DISCOVERY);
	if (ss_group->subsystem == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		free(ss_group);
		return -1;
	}
	TAILQ_INSERT_TAIL(&g_ssg_head, ss_group, tailq);

	return 0;
}

void
spdk_format_discovery_log(struct spdk_nvmf_discovery_log_page *disc_log, uint32_t length)
{
	int i, numrec = 0;
	struct spdk_nvmf_subsystem_grp *ss_group;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_access_map *map;
	struct spdk_nvmf_port *port;
	struct spdk_nvmf_fabric_intf *fabric_intf;
	struct spdk_nvmf_discovery_log_page_entry *entry;

	TAILQ_FOREACH(ss_group, &g_ssg_head, tailq) {
		subsystem = ss_group->subsystem;
		if (subsystem->subtype == SPDK_NVMF_SUB_DISCOVERY)
			continue;

		for (i = 0; i < ss_group->map_count; i++) {
			map = &ss_group->map[i];
			port = map->port;
			if (port != NULL) {
				TAILQ_FOREACH(fabric_intf, &port->head, tailq) {
					/* include the discovery log entry */
					if (length > sizeof(struct spdk_nvmf_discovery_log_page)) {
						if (sizeof(struct spdk_nvmf_discovery_log_page) + (numrec + 1) * sizeof(
							    struct spdk_nvmf_discovery_log_page_entry) > length) {
							break;
						}
						entry = &disc_log->entries[numrec];
						entry->trtype = fabric_intf->trtype;
						entry->adrfam = fabric_intf->adrfam;
						entry->treq = fabric_intf->treq;
						entry->portid = port->tag;
						/* Dynamic controllers */
						entry->cntlid = 0xffff;
						entry->subtype = subsystem->subtype;
						snprintf(entry->trsvcid, 32, "%s", fabric_intf->sin_port);
						snprintf(entry->traddr, 256, "%s", fabric_intf->host);
						snprintf(entry->subnqn, 256, "%s", subsystem->subnqn);
						entry->tsas.rdma.rdma_qptype = port->rdma.rdma_qptype;
						entry->tsas.rdma.rdma_prtype = port->rdma.rdma_prtype;
						entry->tsas.rdma.rdma_cms = port->rdma.rdma_cms;
					}
					numrec++;
				}
			}
		}
	}

	disc_log->numrec = numrec;
}

int
spdk_initialize_nvmf_subsystems(void)
{
	struct spdk_conf_section *sp;
	int rc;

	SPDK_NOTICELOG("\n*** NVMf Controller Subsystems Init ***\n");

	TAILQ_INIT(&g_ssg_head);

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			if (sp->num > SPDK_CN_TAG_MAX) {
				SPDK_ERRLOG("tag %d is invalid\n", sp->num);
				SPDK_TRACELOG(SPDK_TRACE_DEBUG, "tag %d is invalid\n", sp->num);
				return -1;
			}
			rc = spdk_cf_add_nvmf_subsystem(sp);
			if (rc < 0) {
				SPDK_ERRLOG("spdk_cf_add_nvmf_subsystem() failed\n");
				SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_cf_add_nvmf_subsystem() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}

	/* Discovery subsystem */
	rc = spdk_add_nvmf_discovery_subsystem();
	if (rc == 0)
		printf("    Discovery Service Enabled\n");

	return rc;
}

int
spdk_shutdown_nvmf_subsystems(void)
{
	struct spdk_nvmf_subsystem_grp *ss_group;

	while (!TAILQ_EMPTY(&g_ssg_head)) {
		ss_group = TAILQ_FIRST(&g_ssg_head);
		TAILQ_REMOVE(&g_ssg_head, ss_group, tailq);
		spdk_nvmf_subsystem_destruct(ss_group);
	}

	return 0;
}
