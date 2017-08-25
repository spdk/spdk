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

#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_version.h>

#include "iscsi/iscsi.h"
#include "iscsi/init_grp.h"
#include "iscsi/portal_grp.h"
#include "iscsi/acceptor.h"
#include "iscsi/conn.h"
#include "iscsi/task.h"

#include "spdk/env.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#define ISCSI_CONFIG_TMPL \
"[iSCSI]\n" \
"  # node name (not include optional part)\n" \
"  # Users can optionally change this to fit their environment.\n" \
"  NodeBase \"%s\"\n" \
"\n" \
"  # files\n" \
"  AuthFile %s\n" \
"\n" \
"  # socket I/O timeout sec. (polling is infinity)\n" \
"  Timeout %d\n" \
"\n" \
"  # authentication information for discovery session\n" \
"  DiscoveryAuthMethod %s\n" \
"  DiscoveryAuthGroup %s\n" \
"\n" \
"  MaxSessions %d\n" \
"  MaxConnectionsPerSession %d\n" \
"  MaxConnections %d\n" \
"  MaxOutstandingR2T %d\n" \
"\n" \
"  # iSCSI initial parameters negotiate with initiators\n" \
"  # NOTE: incorrect values might crash\n" \
"  DefaultTime2Wait %d\n" \
"  DefaultTime2Retain %d\n" \
"\n" \
"  ImmediateData %s\n" \
"  DataPDUInOrder %s\n" \
"  DataSequenceInOrder %s\n" \
"  ErrorRecoveryLevel %d\n" \
"\n" \
"  # Defines whether iSCSI target will enable configuration via RPC\n" \
"  # RpcConfiguration Yes\n" \
"\n"

static void
spdk_iscsi_config_dump_section(FILE *fp)
{
	const char *authmethod = "None";
	char authgroup[32] = "None";

	if (NULL == fp)
		return;

	if (g_spdk_iscsi.req_discovery_auth)
		authmethod = "CHAP";
	else if (g_spdk_iscsi.req_discovery_auth_mutual)
		authmethod = "CHAP Mutual";
	else if (!g_spdk_iscsi.no_discovery_auth)
		authmethod = "Auto";

	if (g_spdk_iscsi.discovery_auth_group)
		snprintf(authgroup, sizeof(authgroup), "AuthGroup%d", g_spdk_iscsi.discovery_auth_group);

	fprintf(fp, ISCSI_CONFIG_TMPL,
		g_spdk_iscsi.nodebase, g_spdk_iscsi.authfile,
		g_spdk_iscsi.timeout, authmethod, authgroup,
		g_spdk_iscsi.MaxSessions, g_spdk_iscsi.MaxConnectionsPerSession,
		g_spdk_iscsi.MaxConnections, g_spdk_iscsi.MaxOutstandingR2T,
		g_spdk_iscsi.DefaultTime2Wait, g_spdk_iscsi.DefaultTime2Retain,
		(g_spdk_iscsi.ImmediateData == 1) ? "Yes" : "No",
		(g_spdk_iscsi.DataPDUInOrder == 1) ? "Yes" : "No",
		(g_spdk_iscsi.DataSequenceInOrder == 1) ? "Yes" : "No",
		g_spdk_iscsi.ErrorRecoveryLevel);
}


/* Portal groups */
static const char *portal_group_section = \
		"\n"
		"# Users must change the PortalGroup section(s) to match the IP addresses\n"
		"#  for their environment.\n"
		"# PortalGroup sections define which TCP ports the iSCSI server will use\n"
		"#  to listen for incoming connections.  These are also used to determine\n"
		"#  which targets are accessible over each portal group.\n";

#define PORTAL_GROUP_TMPL \
"[PortalGroup%d]\n" \
"  Comment \"Portal%d\"\n"

#define PORTAL_TMPL \
"  Portal DA1 %s:%s\n"

static void
spdk_iscsi_config_dump_portal_groups(FILE *fp)
{
	struct spdk_iscsi_portal *p = NULL;
	struct spdk_iscsi_portal_grp *pg = NULL;

	/* Create portal group section */
	fprintf(fp, "%s", portal_group_section);

	/* Dump portal groups */
	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		if (NULL == pg) continue;
		fprintf(fp, PORTAL_GROUP_TMPL, pg->tag, pg->tag);
		/* Dump portals */
		TAILQ_FOREACH(p, &pg->head, tailq) {
			if (NULL == p) continue;
			fprintf(fp, PORTAL_TMPL, p->host, p->port);
		}
	}
}

/* Initiator Groups */
static const char *initiator_group_section = \
		"\n"
		"# Users must change the InitiatorGroup section(s) to match the IP\n"
		"#  addresses and initiator configuration in their environment.\n"
		"# Netmask can be used to specify a single IP address or a range of IP addresses\n"
		"#  Netmask 192.168.1.20   <== single IP address\n"
		"#  Netmask 192.168.1.0/24 <== IP range 192.168.1.*\n";

#define INITIATOR_GROUP_TMPL \
"[InitiatorGroup%d]\n" \
"  Comment \"Initiator Group%d\"\n"

#define INITIATOR_TMPL \
"  InitiatorName "

#define NETMASK_TMPL \
"  Netmask "

static void
spdk_iscsi_config_dump_initiator_groups(FILE *fp)
{
	int i;
	struct spdk_iscsi_init_grp *ig;

	/* Create initiator group section */
	fprintf(fp, "%s", initiator_group_section);

	/* Dump initiator groups */
	TAILQ_FOREACH(ig, &g_spdk_iscsi.ig_head, tailq) {
		if (NULL == ig) continue;
		fprintf(fp, INITIATOR_GROUP_TMPL, ig->tag, ig->tag);

		/* Dump initiators */
		fprintf(fp, INITIATOR_TMPL);
		for (i = 0; i < ig->ninitiators; i++)
			fprintf(fp, "%s ", ig->initiators[i]);
		fprintf(fp, "\n");

		/* Dump netmasks */
		fprintf(fp, NETMASK_TMPL);
		for (i = 0; i < ig->nnetmasks; i++)
			fprintf(fp, "%s ", ig->netmasks[i]);
		fprintf(fp, "\n");
	}
}

/* Target nodes */
static const char *target_nodes_section = \
		"\n"
		"# Users should change the TargetNode section(s) below to match the\n"
		"#  desired iSCSI target node configuration.\n"
		"# TargetName, Mapping, LUN0 are minimum required\n";

#define TARGET_NODE_TMPL \
"[TargetNode%d]\n" \
"  Comment \"Target%d\"\n" \
"  TargetName %s\n" \
"  TargetAlias \"%s\"\n"

#define TARGET_NODE_PGIG_MAPPING_TMPL \
"  Mapping PortalGroup%d InitiatorGroup%d\n"

#define TARGET_NODE_AUTH_TMPL \
"  AuthMethod %s\n" \
"  AuthGroup %s\n" \
"  UseDigest %s\n"

#define TARGET_NODE_QD_TMPL \
"  QueueDepth %d\n\n"

#define TARGET_NODE_LUN_TMPL \
"  LUN%d %s\n"

static void
spdk_iscsi_config_dump_target_nodes(FILE *fp)
{
	int t = 0, l = 0, m = 0;
	struct spdk_scsi_dev *dev = NULL;
	struct spdk_iscsi_tgt_node *target = NULL;

	/* Create target nodes section */
	fprintf(fp, "%s", target_nodes_section);

	for (t = 0; t < MAX_ISCSI_TARGET_NODE; t++) {
		int idx;
		const char *authmethod = "None";
		char authgroup[32] = "None";
		const char *usedigest = "Auto";

		target = g_spdk_iscsi.target[t];
		if (NULL == target) continue;

		dev = target->dev;
		if (NULL == dev) continue;

		idx = target->num;
		fprintf(fp, TARGET_NODE_TMPL, idx, idx, target->name, spdk_scsi_dev_get_name(dev));

		for (m = 0; m < target->maxmap; m++) {
			if (NULL == target->map[m].pg) continue;
			if (NULL == target->map[m].ig) continue;

			fprintf(fp, TARGET_NODE_PGIG_MAPPING_TMPL,
				target->map[m].pg->tag,
				target->map[m].ig->tag);
		}

		if (target->auth_chap_disabled)
			authmethod = "None";
		else if (!target->auth_chap_required)
			authmethod = "Auto";
		else if (target->auth_chap_mutual)
			authmethod = "CHAP Mutual";
		else
			authmethod = "CHAP";

		if (target->auth_group > 0)
			snprintf(authgroup, sizeof(authgroup), "AuthGroup%d", target->auth_group);

		if (target->header_digest)
			usedigest = "Header";
		else if (target->data_digest)
			usedigest = "Data";

		fprintf(fp, TARGET_NODE_AUTH_TMPL,
			authmethod, authgroup, usedigest);

		for (l = 0; l < SPDK_SCSI_DEV_MAX_LUN; l++) {
			struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(dev, l);

			if (!lun) {
				continue;
			}

			fprintf(fp, TARGET_NODE_LUN_TMPL,
				spdk_scsi_lun_get_id(lun),
				spdk_scsi_lun_get_name(lun));
		}

		fprintf(fp, TARGET_NODE_QD_TMPL,
			target->queue_depth);
	}
}

static void
spdk_mobj_ctor(struct rte_mempool *mp, __attribute__((unused)) void *arg,
	       void *_m, __attribute__((unused)) unsigned i)
{
	struct spdk_mobj *m = _m;
	uint64_t *phys_addr;
	ptrdiff_t off;

	m->mp = mp;
	m->buf = (uint8_t *)m + sizeof(struct spdk_mobj);
	m->buf = (void *)((unsigned long)((uint8_t *)m->buf + 512) & ~511UL);
	off = (uint64_t)(uint8_t *)m->buf - (uint64_t)(uint8_t *)m;

	/*
	 * we store the physical address in a 64bit unsigned integer
	 * right before the 512B aligned buffer area.
	 */
	phys_addr = (uint64_t *)m->buf - 1;
	*phys_addr = rte_mempool_virt2phy(mp, m) + off;
}

#define PDU_POOL_SIZE(iscsi)	(iscsi->MaxConnections * NUM_PDU_PER_CONNECTION)
#define IMMEDIATE_DATA_POOL_SIZE(iscsi)	(iscsi->MaxConnections * 128)
#define DATA_OUT_POOL_SIZE(iscsi)	(iscsi->MaxConnections * MAX_DATA_OUT_PER_CONNECTION)

static int spdk_iscsi_initialize_pdu_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;
	int imm_mobj_size = spdk_get_immediate_data_buffer_size() +
			    sizeof(struct spdk_mobj) + 512;
	int dout_mobj_size = spdk_get_data_out_buffer_size() +
			     sizeof(struct spdk_mobj) + 512;

	/* create PDU pool */
	iscsi->pdu_pool = rte_mempool_create("PDU_Pool",
					     PDU_POOL_SIZE(iscsi),
					     sizeof(struct spdk_iscsi_pdu),
					     256, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);
	if (!iscsi->pdu_pool) {
		SPDK_ERRLOG("create PDU pool failed\n");
		return -1;
	}

	iscsi->pdu_immediate_data_pool =
		rte_mempool_create("PDU_immediate_data_Pool",
				   IMMEDIATE_DATA_POOL_SIZE(iscsi),
				   imm_mobj_size,
				   0, 0, NULL, NULL,
				   spdk_mobj_ctor, NULL,
				   rte_socket_id(), 0);
	if (!iscsi->pdu_immediate_data_pool) {
		SPDK_ERRLOG("create PDU 8k pool failed\n");
		return -1;
	}

	iscsi->pdu_data_out_pool = rte_mempool_create("PDU_data_out_Pool",
				   DATA_OUT_POOL_SIZE(iscsi),
				   dout_mobj_size,
				   0, 0, NULL, NULL,
				   spdk_mobj_ctor, NULL,
				   rte_socket_id(), 0);
	if (!iscsi->pdu_data_out_pool) {
		SPDK_ERRLOG("create PDU 64k pool failed\n");
		return -1;
	}

	return 0;
}

static void spdk_iscsi_sess_ctor(struct rte_mempool *pool, void *arg,
				 void *session_buf, unsigned index)
{
	struct spdk_iscsi_globals		*iscsi = arg;
	struct spdk_iscsi_sess	*sess = session_buf;

	iscsi->session[index] = sess;

	/* tsih 0 is reserved, so start tsih values at 1. */
	sess->tsih = index + 1;
}

#define DEFAULT_TASK_POOL_SIZE 32768

static int
spdk_iscsi_initialize_task_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	/* create scsi_task pool */
	iscsi->task_pool = rte_mempool_create("SCSI_TASK_Pool",
					      DEFAULT_TASK_POOL_SIZE,
					      sizeof(struct spdk_iscsi_task),
					      128, 0,
					      NULL, NULL, NULL, NULL,
					      SOCKET_ID_ANY, 0);
	if (!iscsi->task_pool) {
		SPDK_ERRLOG("create task pool failed\n");
		return -1;
	}

	return 0;
}

#define SESSION_POOL_SIZE(iscsi)	(iscsi->MaxSessions)
static int spdk_iscsi_initialize_session_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	iscsi->session_pool = rte_mempool_create("Session_Pool",
			      SESSION_POOL_SIZE(iscsi),
			      sizeof(struct spdk_iscsi_sess),
			      0, 0,
			      NULL, NULL,
			      spdk_iscsi_sess_ctor, iscsi,
			      SOCKET_ID_ANY, 0);
	if (!iscsi->session_pool) {
		SPDK_ERRLOG("create session pool failed\n");
		return -1;
	}

	return 0;
}

static int
spdk_iscsi_initialize_all_pools(void)
{
	if (spdk_iscsi_initialize_pdu_pool() != 0) {
		return -1;
	}

	if (spdk_iscsi_initialize_session_pool() != 0) {
		return -1;
	}

	if (spdk_iscsi_initialize_task_pool() != 0) {
		return -1;
	}

	return 0;
}

/*
 * Wrapper to provide rte_mempool_avail_count() on older DPDK versions.
 * Drop this if the minimum DPDK version is raised to at least 16.07.
 */
#if RTE_VERSION < RTE_VERSION_NUM(16, 7, 0, 1)
static unsigned rte_mempool_avail_count(const struct rte_mempool *pool)
{
	return rte_mempool_count(pool);
}
#endif

static int
spdk_iscsi_check_pool(struct rte_mempool *pool, uint32_t count)
{
	if (rte_mempool_avail_count(pool) != count) {
		SPDK_ERRLOG("rte_mempool_avail_count(%s) == %d, should be %d\n",
			    pool->name, rte_mempool_avail_count(pool), count);
		return -1;
	} else {
		return 0;
	}
}

static int
spdk_iscsi_check_pools(void)
{
	int rc = 0;
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	rc += spdk_iscsi_check_pool(iscsi->pdu_pool, PDU_POOL_SIZE(iscsi));
	rc += spdk_iscsi_check_pool(iscsi->session_pool, SESSION_POOL_SIZE(iscsi));
	rc += spdk_iscsi_check_pool(iscsi->pdu_immediate_data_pool, IMMEDIATE_DATA_POOL_SIZE(iscsi));
	rc += spdk_iscsi_check_pool(iscsi->pdu_data_out_pool, DATA_OUT_POOL_SIZE(iscsi));
	/* TODO: check the task_pool on exit */

	if (rc == 0) {
		return 0;
	} else {
		return -1;
	}
}

static void
spdk_iscsi_free_pools(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	rte_mempool_free(iscsi->pdu_pool);
	rte_mempool_free(iscsi->session_pool);
	rte_mempool_free(iscsi->pdu_immediate_data_pool);
	rte_mempool_free(iscsi->pdu_data_out_pool);
	rte_mempool_free(iscsi->task_pool);
}

void spdk_put_pdu(struct spdk_iscsi_pdu *pdu)
{
	if (!pdu)
		return;

	pdu->ref--;

	if (pdu->ref < 0) {
		SPDK_ERRLOG("Negative PDU refcount: %p\n", pdu);
		pdu->ref = 0;
	}

	if (pdu->ref == 0) {
		if (pdu->mobj)
			rte_mempool_put(pdu->mobj->mp, (void *)pdu->mobj);

		if (pdu->data && !pdu->data_from_mempool)
			free(pdu->data);

		rte_mempool_put(g_spdk_iscsi.pdu_pool, (void *)pdu);
	}
}

struct spdk_iscsi_pdu *spdk_get_pdu(void)
{
	struct spdk_iscsi_pdu *pdu;
	int rc;

	rc = rte_mempool_get(g_spdk_iscsi.pdu_pool, (void **)&pdu);
	if ((rc < 0) || !pdu) {
		SPDK_ERRLOG("Unable to get PDU\n");
		abort();
	}

	/* we do not want to zero out the last part of the structure reserved for AHS and sense data */
	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	pdu->ref = 1;

	return pdu;
}

static int
spdk_iscsi_app_read_parameters(void)
{
	struct spdk_conf_section *sp;
	const char *ag_tag;
	const char *val;
	int ag_tag_i;
	int MaxSessions;
	int MaxConnectionsPerSession;
	int DefaultTime2Wait;
	int DefaultTime2Retain;
	int InitialR2T;
	int ImmediateData;
	int DataPDUInOrder;
	int DataSequenceInOrder;
	int ErrorRecoveryLevel;
	int timeout;
	int nopininterval;
	int rc;
	int i;
	int AllowDuplicateIsid;
	int min_conn_per_core = 0;
	int conn_idle_interval = 0;
	unsigned long flush_timeout = 0;

	/* Process parameters */
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_app_read_parameters\n");
	sp = spdk_conf_find_section(NULL, "iSCSI");
	if (sp == NULL) {
		SPDK_ERRLOG("iSCSI config section not found.\n");
		return -1;
	}

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Comment %s\n", val);
	}

	val = spdk_conf_section_get_val(sp, "AuthFile");
	if (val == NULL) {
		val = SPDK_ISCSI_DEFAULT_AUTHFILE;
	}

	g_spdk_iscsi.authfile = strdup(val);
	if (!g_spdk_iscsi.authfile) {
		perror("authfile");
		return -ENOMEM;
	}
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AuthFile %s\n", g_spdk_iscsi.authfile);

	/* ISCSI Global */
	val = spdk_conf_section_get_val(sp, "NodeBase");
	if (val == NULL) {
		val = SPDK_ISCSI_DEFAULT_NODEBASE;
	}

	g_spdk_iscsi.nodebase = strdup(val);
	if (!g_spdk_iscsi.nodebase) {
		perror("nodebase");
		free(g_spdk_iscsi.authfile);
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "NodeBase %s\n",
		      g_spdk_iscsi.nodebase);

	MaxSessions = spdk_conf_section_get_intval(sp, "MaxSessions");
	if (MaxSessions < 1) {
		MaxSessions = DEFAULT_MAX_SESSIONS;
	} else if (MaxSessions > 0xffff) {
		/* limited to 16bits - RFC3720(12.2) */
		SPDK_ERRLOG("over 65535 sessions are not supported\n");
		return -1;
	}
	g_spdk_iscsi.MaxSessions = MaxSessions;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "MaxSessions %d\n", g_spdk_iscsi.MaxSessions);

	g_spdk_iscsi.session = spdk_dma_zmalloc(sizeof(void *) * g_spdk_iscsi.MaxSessions, 0, NULL);
	if (!g_spdk_iscsi.session) {
		perror("Unable to allocate session pointer array\n");
		return -1;
	}

	MaxConnectionsPerSession = spdk_conf_section_get_intval(sp, "MaxConnectionsPerSession");
	if (MaxConnectionsPerSession < 1) {
		MaxConnectionsPerSession = DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	}
	g_spdk_iscsi.MaxConnectionsPerSession = MaxConnectionsPerSession;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "MaxConnectionsPerSession %d\n",
		      g_spdk_iscsi.MaxConnectionsPerSession);

	if (MaxConnectionsPerSession > 0xffff) {
		SPDK_ERRLOG("over 65535 connections are not supported\n");
		return -1;
	}

	/*
	 * For now, just support same number of total connections, rather
	 *  than MaxSessions * MaxConnectionsPerSession.  After we add better
	 *  handling for low resource conditions from our various buffer
	 *  pools, we can bump this up to support more connections.
	 */
	g_spdk_iscsi.MaxConnections = g_spdk_iscsi.MaxSessions;

	DefaultTime2Wait = spdk_conf_section_get_intval(sp, "DefaultTime2Wait");
	if (DefaultTime2Wait < 0) {
		DefaultTime2Wait = DEFAULT_DEFAULTTIME2WAIT;
	}
	g_spdk_iscsi.DefaultTime2Wait = DefaultTime2Wait;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "DefaultTime2Wait %d\n",
		      g_spdk_iscsi.DefaultTime2Wait);

	DefaultTime2Retain = spdk_conf_section_get_intval(sp, "DefaultTime2Retain");
	if (DefaultTime2Retain < 0) {
		DefaultTime2Retain = DEFAULT_DEFAULTTIME2RETAIN;
	}
	g_spdk_iscsi.DefaultTime2Retain = DefaultTime2Retain;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "DefaultTime2Retain %d\n",
		      g_spdk_iscsi.DefaultTime2Retain);

	/* check size limit - RFC3720(12.15, 12.16, 12.17) */
	if (g_spdk_iscsi.MaxOutstandingR2T > 65535) {
		SPDK_ERRLOG("MaxOutstandingR2T(%d) > 65535\n", g_spdk_iscsi.MaxOutstandingR2T);
		return -1;
	}
	if (g_spdk_iscsi.DefaultTime2Wait > 3600) {
		SPDK_ERRLOG("DefaultTime2Wait(%d) > 3600\n", g_spdk_iscsi.DefaultTime2Wait);
		return -1;
	}
	if (g_spdk_iscsi.DefaultTime2Retain > 3600) {
		SPDK_ERRLOG("DefaultTime2Retain(%d) > 3600\n", g_spdk_iscsi.DefaultTime2Retain);
		return -1;
	}

	g_spdk_iscsi.FirstBurstLength = SPDK_ISCSI_FIRST_BURST_LENGTH;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "FirstBurstLength %d\n",
		      g_spdk_iscsi.FirstBurstLength);

	g_spdk_iscsi.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "MaxBurstLength %d\n",
		      g_spdk_iscsi.MaxBurstLength);

	g_spdk_iscsi.MaxRecvDataSegmentLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "MaxRecvDataSegmentLength %d\n",
		      g_spdk_iscsi.MaxRecvDataSegmentLength);

	/* check size limit (up to 24bits - RFC3720(12.12)) */
	if (g_spdk_iscsi.MaxBurstLength < 512) {
		SPDK_ERRLOG("MaxBurstLength(%d) < 512\n", g_spdk_iscsi.MaxBurstLength);
		return -1;
	}
	if (g_spdk_iscsi.FirstBurstLength < 512) {
		SPDK_ERRLOG("FirstBurstLength(%d) < 512\n", g_spdk_iscsi.FirstBurstLength);
		return -1;
	}
	if (g_spdk_iscsi.FirstBurstLength > g_spdk_iscsi.MaxBurstLength) {
		SPDK_ERRLOG("FirstBurstLength(%d) > MaxBurstLength(%d)\n",
			    g_spdk_iscsi.FirstBurstLength, g_spdk_iscsi.MaxBurstLength);
		return -1;
	}
	if (g_spdk_iscsi.MaxBurstLength > 0x00ffffff) {
		SPDK_ERRLOG("MaxBurstLength(%d) > 0x00ffffff\n", g_spdk_iscsi.MaxBurstLength);
		return -1;
	}

	val = spdk_conf_section_get_val(sp, "InitialR2T");
	if (val == NULL) {
		InitialR2T = DEFAULT_INITIALR2T;
	} else if (strcasecmp(val, "Yes") == 0) {
		InitialR2T = 1;
	} else if (strcasecmp(val, "No") == 0) {
#if 0
		InitialR2T = 0;
#else
		SPDK_ERRLOG("not supported value %s\n", val);
		return -1;
#endif
	} else {
		SPDK_ERRLOG("unknown value %s\n", val);
		return -1;
	}
	g_spdk_iscsi.InitialR2T = InitialR2T;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "InitialR2T %s\n",
		      g_spdk_iscsi.InitialR2T ? "Yes" : "No");

	val = spdk_conf_section_get_val(sp, "ImmediateData");
	if (val == NULL) {
		ImmediateData = DEFAULT_IMMEDIATEDATA;
	} else if (strcasecmp(val, "Yes") == 0) {
		ImmediateData = 1;
	} else if (strcasecmp(val, "No") == 0) {
		ImmediateData = 0;
	} else {
		SPDK_ERRLOG("unknown value %s\n", val);
		return -1;
	}
	g_spdk_iscsi.ImmediateData = ImmediateData;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "ImmediateData %s\n",
		      g_spdk_iscsi.ImmediateData ? "Yes" : "No");

	val = spdk_conf_section_get_val(sp, "DataPDUInOrder");
	if (val == NULL) {
		DataPDUInOrder = DEFAULT_DATAPDUINORDER;
	} else if (strcasecmp(val, "Yes") == 0) {
		DataPDUInOrder = 1;
	} else if (strcasecmp(val, "No") == 0) {
#if 0
		DataPDUInOrder = 0;
#else
		SPDK_ERRLOG("not supported value %s\n", val);
		return -1;
#endif
	} else {
		SPDK_ERRLOG("unknown value %s\n", val);
		return -1;
	}
	g_spdk_iscsi.DataPDUInOrder = DataPDUInOrder;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "DataPDUInOrder %s\n",
		      g_spdk_iscsi.DataPDUInOrder ? "Yes" : "No");

	/* This option is only for test.
	 * If AllowDuplicateIsid is enabled, it allows different connections carrying
	 * TSIH=0 login the target within the same session.
	 */
	val = spdk_conf_section_get_val(sp, "AllowDuplicateIsid");
	if (val == NULL) {
		AllowDuplicateIsid = 0;
	} else if (strcasecmp(val, "Yes") == 0) {
		AllowDuplicateIsid = 1;
	} else if (strcasecmp(val, "No") == 0) {
		AllowDuplicateIsid = 0;
	} else {
		SPDK_ERRLOG("unknown value %s\n", val);
		return -1;
	}
	g_spdk_iscsi.AllowDuplicateIsid = AllowDuplicateIsid;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AllowDuplicateIsid %s\n",
		      g_spdk_iscsi.AllowDuplicateIsid ? "Yes" : "No");

	val = spdk_conf_section_get_val(sp, "DataSequenceInOrder");
	if (val == NULL) {
		DataSequenceInOrder = DEFAULT_DATASEQUENCEINORDER;
	} else if (strcasecmp(val, "Yes") == 0) {
		DataSequenceInOrder = 1;
	} else if (strcasecmp(val, "No") == 0) {
#if 0
		DataSequenceInOrder = 0;
#else
		SPDK_ERRLOG("not supported value %s\n", val);
		return -1;
#endif
	} else {
		SPDK_ERRLOG("unknown value %s\n", val);
		return -1;
	}
	g_spdk_iscsi.DataSequenceInOrder = DataSequenceInOrder;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "DataSequenceInOrder %s\n",
		      g_spdk_iscsi.DataSequenceInOrder ? "Yes" : "No");

	ErrorRecoveryLevel = spdk_conf_section_get_intval(sp, "ErrorRecoveryLevel");
	if (ErrorRecoveryLevel < 0) {
		ErrorRecoveryLevel = DEFAULT_ERRORRECOVERYLEVEL;
	} else if (ErrorRecoveryLevel > 2) {
		SPDK_ERRLOG("ErrorRecoveryLevel %d not supported,\n", ErrorRecoveryLevel);
		return -1;
	}
	g_spdk_iscsi.ErrorRecoveryLevel = ErrorRecoveryLevel;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "ErrorRecoveryLevel %d\n",
		      g_spdk_iscsi.ErrorRecoveryLevel);

	timeout = spdk_conf_section_get_intval(sp, "Timeout");
	if (timeout < 0) {
		timeout = DEFAULT_TIMEOUT;
	}
	g_spdk_iscsi.timeout = timeout;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Timeout %d\n",
		      g_spdk_iscsi.timeout);

	val = spdk_conf_section_get_val(sp, "FlushTimeout");
	if (val) {
		flush_timeout = strtoul(val, NULL, 10);
	}
	if (flush_timeout == 0) {
		flush_timeout = DEFAULT_FLUSH_TIMEOUT;
	}
	g_spdk_iscsi.flush_timeout = flush_timeout * (spdk_get_ticks_hz() >> 20);
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "FlushTimeout %"PRIu64"\n", g_spdk_iscsi.flush_timeout);

	nopininterval = spdk_conf_section_get_intval(sp, "NopInInterval");
	if (nopininterval < 0) {
		nopininterval = DEFAULT_NOPININTERVAL;
	}
	if (nopininterval > MAX_NOPININTERVAL) {
		SPDK_ERRLOG("%d NopInInterval too big, using %d instead.\n",
			    nopininterval, DEFAULT_NOPININTERVAL);
		nopininterval = DEFAULT_NOPININTERVAL;
	}

	g_spdk_iscsi.nopininterval = nopininterval;
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "NopInInterval %d\n",
		      g_spdk_iscsi.nopininterval);

	val = spdk_conf_section_get_val(sp, "DiscoveryAuthMethod");
	if (val == NULL) {
		g_spdk_iscsi.no_discovery_auth = 0;
		g_spdk_iscsi.req_discovery_auth = 0;
		g_spdk_iscsi.req_discovery_auth_mutual = 0;
	} else {
		g_spdk_iscsi.no_discovery_auth = 0;
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "DiscoveryAuthMethod", 0, i);
			if (val == NULL)
				break;
			if (strcasecmp(val, "CHAP") == 0) {
				g_spdk_iscsi.req_discovery_auth = 1;
			} else if (strcasecmp(val, "Mutual") == 0) {
				g_spdk_iscsi.req_discovery_auth_mutual = 1;
			} else if (strcasecmp(val, "Auto") == 0) {
				g_spdk_iscsi.req_discovery_auth = 0;
				g_spdk_iscsi.req_discovery_auth_mutual = 0;
			} else if (strcasecmp(val, "None") == 0) {
				g_spdk_iscsi.no_discovery_auth = 1;
				g_spdk_iscsi.req_discovery_auth = 0;
				g_spdk_iscsi.req_discovery_auth_mutual = 0;
			} else {
				SPDK_ERRLOG("unknown auth\n");
				return -1;
			}
		}
		if (g_spdk_iscsi.req_discovery_auth_mutual && !g_spdk_iscsi.req_discovery_auth) {
			SPDK_ERRLOG("Mutual but not CHAP\n");
			return -1;
		}
	}
	if (g_spdk_iscsi.no_discovery_auth != 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "DiscoveryAuthMethod None\n");
	} else if (g_spdk_iscsi.req_discovery_auth == 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "DiscoveryAuthMethod Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "DiscoveryAuthMethod %s %s\n",
			      g_spdk_iscsi.req_discovery_auth ? "CHAP" : "",
			      g_spdk_iscsi.req_discovery_auth_mutual ? "Mutual" : "");
	}

	val = spdk_conf_section_get_val(sp, "DiscoveryAuthGroup");
	if (val == NULL) {
		g_spdk_iscsi.discovery_auth_group = 0;
	} else {
		ag_tag = val;
		if (strcasecmp(ag_tag, "None") == 0) {
			ag_tag_i = 0;
		} else {
			if (strncasecmp(ag_tag, "AuthGroup",
					strlen("AuthGroup")) != 0
			    || sscanf(ag_tag, "%*[^0-9]%d", &ag_tag_i) != 1) {
				SPDK_ERRLOG("auth group error\n");
				return -1;
			}
			if (ag_tag_i == 0) {
				SPDK_ERRLOG("invalid auth group %d\n", ag_tag_i);
				return -1;
			}
		}
		g_spdk_iscsi.discovery_auth_group = ag_tag_i;
	}
	if (g_spdk_iscsi.discovery_auth_group == 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "DiscoveryAuthGroup None\n");
	} else {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "DiscoveryAuthGroup AuthGroup%d\n",
			      g_spdk_iscsi.discovery_auth_group);
	}

	min_conn_per_core = spdk_conf_section_get_intval(sp, "MinConnectionsPerCore");
	if (min_conn_per_core >= 0)
		spdk_iscsi_conn_set_min_per_core(min_conn_per_core);

	conn_idle_interval = spdk_conf_section_get_intval(sp, "MinConnectionIdleInterval");
	if (conn_idle_interval > 0)
		spdk_iscsi_set_min_conn_idle_interval(conn_idle_interval);

	/* portal groups */
	rc = spdk_iscsi_portal_grp_array_create();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_portal_grp_array_create() failed\n");
		return -1;
	}

	/* initiator groups */
	rc = spdk_iscsi_init_grp_array_create();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_init_grp_array_create() failed\n");
		return -1;
	}

	rc = pthread_mutex_init(&g_spdk_iscsi.mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	return 0;
}

static void
spdk_iscsi_setup(void *arg1, void *arg2)
{
	int rc;

	/* open portals */
	rc = spdk_iscsi_portal_grp_open_all();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_portal_grp_open_all() failed\n");
		return;
	}

	spdk_iscsi_acceptor_start();
}

int
spdk_iscsi_init(void)
{
	int rc;

	rc = spdk_iscsi_app_read_parameters();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_app_read_parameters() failed\n");
		return -1;
	}

	rc = spdk_iscsi_initialize_all_pools();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_initialize_all_pools() failed\n");
		return -1;
	}

	rc = spdk_iscsi_init_tgt_nodes();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_init_tgt_nodes() failed\n");
		return -1;
	}

	rc = spdk_initialize_iscsi_conns();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_initialize_iscsi_conns() failed\n");
		return -1;
	}

	/*
	 * Defer creation of listening sockets until the reactor has started.
	 */
	spdk_event_call(spdk_event_allocate(spdk_env_get_current_core(), spdk_iscsi_setup, NULL, NULL));

	return 0;
}

int
spdk_iscsi_fini(void)
{
	int rc;

	rc = spdk_iscsi_check_pools();
	spdk_iscsi_free_pools();

	spdk_iscsi_shutdown_tgt_nodes();
	spdk_iscsi_init_grp_array_destroy();
	spdk_iscsi_portal_grp_array_destroy();
	free(g_spdk_iscsi.authfile);
	free(g_spdk_iscsi.nodebase);

	pthread_mutex_destroy(&g_spdk_iscsi.mutex);

	return rc;
}

void
spdk_iscsi_config_text(FILE *fp)
{
	spdk_iscsi_config_dump_section(fp);
	spdk_iscsi_config_dump_portal_groups(fp);
	spdk_iscsi_config_dump_initiator_groups(fp);
	spdk_iscsi_config_dump_target_nodes(fp);
}

SPDK_LOG_REGISTER_TRACE_FLAG("iscsi", SPDK_TRACE_ISCSI)
