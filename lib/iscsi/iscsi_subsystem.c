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
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/sock.h"

#include "iscsi/iscsi.h"
#include "iscsi/init_grp.h"
#include "iscsi/portal_grp.h"
#include "iscsi/conn.h"
#include "iscsi/task.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

static spdk_iscsi_init_cb g_init_cb_fn = NULL;
static void *g_init_cb_arg = NULL;

static spdk_iscsi_fini_cb g_fini_cb_fn;
static void *g_fini_cb_arg;

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
"  MaxQueueDepth %d\n" \
"\n" \
"  # iSCSI initial parameters negotiate with initiators\n" \
"  # NOTE: incorrect values might crash\n" \
"  DefaultTime2Wait %d\n" \
"  DefaultTime2Retain %d\n" \
"\n" \
"  ImmediateData %s\n" \
"  ErrorRecoveryLevel %d\n" \
"\n"

static void
spdk_iscsi_config_dump_section(FILE *fp)
{
	const char *authmethod = "None";
	char authgroup[32] = "None";

	if (NULL == fp) {
		return;
	}

	if (g_spdk_iscsi.req_discovery_auth) {
		authmethod = "CHAP";
	} else if (g_spdk_iscsi.req_discovery_auth_mutual) {
		authmethod = "CHAP Mutual";
	} else if (!g_spdk_iscsi.no_discovery_auth) {
		authmethod = "Auto";
	}

	if (g_spdk_iscsi.discovery_auth_group) {
		snprintf(authgroup, sizeof(authgroup), "AuthGroup%d", g_spdk_iscsi.discovery_auth_group);
	}

	fprintf(fp, ISCSI_CONFIG_TMPL,
		g_spdk_iscsi.nodebase, g_spdk_iscsi.authfile,
		g_spdk_iscsi.timeout, authmethod, authgroup,
		g_spdk_iscsi.MaxSessions, g_spdk_iscsi.MaxConnectionsPerSession,
		g_spdk_iscsi.MaxConnections,
		g_spdk_iscsi.MaxQueueDepth,
		g_spdk_iscsi.DefaultTime2Wait, g_spdk_iscsi.DefaultTime2Retain,
		(g_spdk_iscsi.ImmediateData == 1) ? "Yes" : "No",
		g_spdk_iscsi.ErrorRecoveryLevel);
}


/* Portal groups */
static const char *portal_group_section = \
		"\n"
		"# Users must change the PortalGroup section(s) to match the IP addresses\n"
		"#  for their environment.\n"
		"# PortalGroup sections define which network portals the iSCSI target\n"
		"# will use to listen for incoming connections.  These are also used to\n"
		"#  determine which targets are accessible over each portal group.\n"
		"# Up to 1024 Portal directives are allowed.  These define the network\n"
		"#  portals of the portal group. The user must specify a IP address\n"
		"#  for each network portal, and may optionally specify a port and\n"
		"#  a cpumask. If the port is omitted, 3260 will be used. Cpumask will\n"
		"#  be used to set the processor affinity of the iSCSI connection\n"
		"#  through the portal.  If the cpumask is omitted, cpumask will be\n"
		"#  set to all available processors.\n"
		"#  Syntax:\n"
		"#    Portal <Name> <IP address>[:<port>[@<cpumask>]]\n";

#define PORTAL_GROUP_TMPL \
"[PortalGroup%d]\n" \
"  Comment \"Portal%d\"\n"

#define PORTAL_TMPL \
"  Portal DA1 %s:%s@0x%s\n"

static void
spdk_iscsi_config_dump_portal_groups(FILE *fp)
{
	struct spdk_iscsi_portal *p = NULL;
	struct spdk_iscsi_portal_grp *pg = NULL;

	/* Create portal group section */
	fprintf(fp, "%s", portal_group_section);

	/* Dump portal groups */
	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		if (NULL == pg) { continue; }
		fprintf(fp, PORTAL_GROUP_TMPL, pg->tag, pg->tag);
		/* Dump portals */
		TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
			if (NULL == p) { continue; }
			fprintf(fp, PORTAL_TMPL, p->host, p->port,
				spdk_cpuset_fmt(p->cpumask));
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
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	struct spdk_iscsi_initiator_netmask *imask;

	/* Create initiator group section */
	fprintf(fp, "%s", initiator_group_section);

	/* Dump initiator groups */
	TAILQ_FOREACH(ig, &g_spdk_iscsi.ig_head, tailq) {
		if (NULL == ig) { continue; }
		fprintf(fp, INITIATOR_GROUP_TMPL, ig->tag, ig->tag);

		/* Dump initiators */
		fprintf(fp, INITIATOR_TMPL);
		TAILQ_FOREACH(iname, &ig->initiator_head, tailq) {
			fprintf(fp, "%s ", iname->name);
		}
		fprintf(fp, "\n");

		/* Dump netmasks */
		fprintf(fp, NETMASK_TMPL);
		TAILQ_FOREACH(imask, &ig->netmask_head, tailq) {
			fprintf(fp, "%s ", imask->mask);
		}
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
	int l = 0;
	struct spdk_scsi_dev *dev = NULL;
	struct spdk_iscsi_tgt_node *target = NULL;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;

	/* Create target nodes section */
	fprintf(fp, "%s", target_nodes_section);

	TAILQ_FOREACH(target, &g_spdk_iscsi.target_head, tailq) {
		int idx;
		const char *authmethod = "None";
		char authgroup[32] = "None";
		const char *usedigest = "Auto";

		dev = target->dev;
		if (NULL == dev) { continue; }

		idx = target->num;
		fprintf(fp, TARGET_NODE_TMPL, idx, idx, target->name, spdk_scsi_dev_get_name(dev));

		TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
			TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
				fprintf(fp, TARGET_NODE_PGIG_MAPPING_TMPL,
					pg_map->pg->tag,
					ig_map->ig->tag);
			}
		}

		if (target->auth_chap_disabled) {
			authmethod = "None";
		} else if (!target->auth_chap_required) {
			authmethod = "Auto";
		} else if (target->auth_chap_mutual) {
			authmethod = "CHAP Mutual";
		} else {
			authmethod = "CHAP";
		}

		if (target->auth_group > 0) {
			snprintf(authgroup, sizeof(authgroup), "AuthGroup%d", target->auth_group);
		}

		if (target->header_digest) {
			usedigest = "Header";
		} else if (target->data_digest) {
			usedigest = "Data";
		}

		fprintf(fp, TARGET_NODE_AUTH_TMPL,
			authmethod, authgroup, usedigest);

		for (l = 0; l < SPDK_SCSI_DEV_MAX_LUN; l++) {
			struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(dev, l);

			if (!lun) {
				continue;
			}

			fprintf(fp, TARGET_NODE_LUN_TMPL,
				spdk_scsi_lun_get_id(lun),
				spdk_scsi_lun_get_bdev_name(lun));
		}

		fprintf(fp, TARGET_NODE_QD_TMPL,
			target->queue_depth);
	}
}

static void
spdk_mobj_ctor(struct spdk_mempool *mp, __attribute__((unused)) void *arg,
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
	*phys_addr = spdk_vtophys(m) + off;
}

#define NUM_PDU_PER_CONNECTION(iscsi)	(2 * (iscsi->MaxQueueDepth + MAX_LARGE_DATAIN_PER_CONNECTION + 8))
#define PDU_POOL_SIZE(iscsi)	(iscsi->MaxConnections * NUM_PDU_PER_CONNECTION(iscsi))
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
	iscsi->pdu_pool = spdk_mempool_create("PDU_Pool",
					      PDU_POOL_SIZE(iscsi),
					      sizeof(struct spdk_iscsi_pdu),
					      256, SPDK_ENV_SOCKET_ID_ANY);
	if (!iscsi->pdu_pool) {
		SPDK_ERRLOG("create PDU pool failed\n");
		return -1;
	}

	iscsi->pdu_immediate_data_pool = spdk_mempool_create_ctor("PDU_immediate_data_Pool",
					 IMMEDIATE_DATA_POOL_SIZE(iscsi),
					 imm_mobj_size, 0,
					 spdk_env_get_socket_id(spdk_env_get_current_core()),
					 spdk_mobj_ctor, NULL);
	if (!iscsi->pdu_immediate_data_pool) {
		SPDK_ERRLOG("create PDU 8k pool failed\n");
		return -1;
	}

	iscsi->pdu_data_out_pool = spdk_mempool_create_ctor("PDU_data_out_Pool",
				   DATA_OUT_POOL_SIZE(iscsi),
				   dout_mobj_size, 0,
				   spdk_env_get_socket_id(spdk_env_get_current_core()),
				   spdk_mobj_ctor, NULL);
	if (!iscsi->pdu_data_out_pool) {
		SPDK_ERRLOG("create PDU 64k pool failed\n");
		return -1;
	}

	return 0;
}

static void spdk_iscsi_sess_ctor(struct spdk_mempool *pool, void *arg,
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
	iscsi->task_pool = spdk_mempool_create("SCSI_TASK_Pool",
					       DEFAULT_TASK_POOL_SIZE,
					       sizeof(struct spdk_iscsi_task),
					       128, SPDK_ENV_SOCKET_ID_ANY);
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

	iscsi->session_pool = spdk_mempool_create_ctor("Session_Pool",
			      SESSION_POOL_SIZE(iscsi),
			      sizeof(struct spdk_iscsi_sess), 0,
			      SPDK_ENV_SOCKET_ID_ANY,
			      spdk_iscsi_sess_ctor, iscsi);
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

static void
spdk_iscsi_check_pool(struct spdk_mempool *pool, size_t count)
{
	if (spdk_mempool_count(pool) != count) {
		SPDK_ERRLOG("spdk_mempool_count(%s) == %zu, should be %zu\n",
			    spdk_mempool_get_name(pool), spdk_mempool_count(pool), count);
	}
}

static void
spdk_iscsi_check_pools(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	spdk_iscsi_check_pool(iscsi->pdu_pool, PDU_POOL_SIZE(iscsi));
	spdk_iscsi_check_pool(iscsi->session_pool, SESSION_POOL_SIZE(iscsi));
	spdk_iscsi_check_pool(iscsi->pdu_immediate_data_pool, IMMEDIATE_DATA_POOL_SIZE(iscsi));
	spdk_iscsi_check_pool(iscsi->pdu_data_out_pool, DATA_OUT_POOL_SIZE(iscsi));
	/* TODO: check the task_pool on exit */
}

static void
spdk_iscsi_free_pools(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	spdk_mempool_free(iscsi->pdu_pool);
	spdk_mempool_free(iscsi->session_pool);
	spdk_mempool_free(iscsi->pdu_immediate_data_pool);
	spdk_mempool_free(iscsi->pdu_data_out_pool);
	spdk_mempool_free(iscsi->task_pool);
}

void spdk_put_pdu(struct spdk_iscsi_pdu *pdu)
{
	if (!pdu) {
		return;
	}

	pdu->ref--;

	if (pdu->ref < 0) {
		SPDK_ERRLOG("Negative PDU refcount: %p\n", pdu);
		pdu->ref = 0;
	}

	if (pdu->ref == 0) {
		if (pdu->mobj) {
			spdk_mempool_put(pdu->mobj->mp, (void *)pdu->mobj);
		}

		if (pdu->data && !pdu->data_from_mempool) {
			free(pdu->data);
		}

		spdk_mempool_put(g_spdk_iscsi.pdu_pool, (void *)pdu);
	}
}

struct spdk_iscsi_pdu *spdk_get_pdu(void)
{
	struct spdk_iscsi_pdu *pdu;

	pdu = spdk_mempool_get(g_spdk_iscsi.pdu_pool);
	if (!pdu) {
		SPDK_ERRLOG("Unable to get PDU\n");
		abort();
	}

	/* we do not want to zero out the last part of the structure reserved for AHS and sense data */
	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	pdu->ref = 1;

	return pdu;
}

static void
spdk_iscsi_log_globals(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthFile %s\n", g_spdk_iscsi.authfile);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "NodeBase %s\n", g_spdk_iscsi.nodebase);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MaxSessions %d\n", g_spdk_iscsi.MaxSessions);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MaxConnectionsPerSession %d\n",
		      g_spdk_iscsi.MaxConnectionsPerSession);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MaxQueueDepth %d\n", g_spdk_iscsi.MaxQueueDepth);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "DefaultTime2Wait %d\n",
		      g_spdk_iscsi.DefaultTime2Wait);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "DefaultTime2Retain %d\n",
		      g_spdk_iscsi.DefaultTime2Retain);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "ImmediateData %s\n",
		      g_spdk_iscsi.ImmediateData ? "Yes" : "No");
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AllowDuplicateIsid %s\n",
		      g_spdk_iscsi.AllowDuplicateIsid ? "Yes" : "No");
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "ErrorRecoveryLevel %d\n",
		      g_spdk_iscsi.ErrorRecoveryLevel);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Timeout %d\n", g_spdk_iscsi.timeout);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "NopInInterval %d\n",
		      g_spdk_iscsi.nopininterval);
	if (g_spdk_iscsi.no_discovery_auth != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthMethod None\n");
	} else if (g_spdk_iscsi.req_discovery_auth == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthMethod Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthMethod %s %s\n",
			      g_spdk_iscsi.req_discovery_auth ? "CHAP" : "",
			      g_spdk_iscsi.req_discovery_auth_mutual ? "Mutual" : "");
	}

	if (g_spdk_iscsi.discovery_auth_group == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthGroup None\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthGroup AuthGroup%d\n",
			      g_spdk_iscsi.discovery_auth_group);
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MinConnectionsPerCore%d\n",
		      spdk_iscsi_conn_get_min_per_core());
}

static void
spdk_iscsi_read_parameters_from_config_file(struct spdk_conf_section *sp)
{
	const char *val;
	char *authfile, *nodebase;
	int MaxSessions;
	int MaxConnectionsPerSession;
	int MaxQueueDepth;
	int DefaultTime2Wait;
	int DefaultTime2Retain;
	int ErrorRecoveryLevel;
	int timeout;
	int nopininterval;
	int min_conn_per_core = 0;
	const char *ag_tag;
	int ag_tag_i;

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Comment %s\n", val);
	}

	val = spdk_conf_section_get_val(sp, "AuthFile");
	if (val != NULL) {
		authfile = strdup(val);
		if (authfile) {
			free(g_spdk_iscsi.authfile);
			g_spdk_iscsi.authfile = authfile;
		} else {
			SPDK_ERRLOG("could not strdup for authfile %s,"
				    "keeping %s instead\n", val, g_spdk_iscsi.authfile);
		}
	}

	val = spdk_conf_section_get_val(sp, "NodeBase");
	if (val != NULL) {
		nodebase = strdup(val);
		if (nodebase) {
			free(g_spdk_iscsi.nodebase);
			g_spdk_iscsi.nodebase = nodebase;
		} else {
			SPDK_ERRLOG("could not strdup for nodebase %s,"
				    "keeping %s instead\n", val, g_spdk_iscsi.nodebase);
		}
	}

	MaxSessions = spdk_conf_section_get_intval(sp, "MaxSessions");
	if (MaxSessions >= 0) {
		if (MaxSessions == 0) {
			SPDK_ERRLOG("MaxSessions == 0 invalid, ignoring\n");
		} else if (MaxSessions > 65535) {
			SPDK_ERRLOG("MaxSessions == %d invalid, ignoring\n", MaxSessions);
		} else {
			g_spdk_iscsi.MaxSessions = MaxSessions;
		}
	}

	MaxConnectionsPerSession = spdk_conf_section_get_intval(sp, "MaxConnectionsPerSession");
	if (MaxConnectionsPerSession >= 0) {
		if (MaxConnectionsPerSession == 0) {
			SPDK_ERRLOG("MaxConnectionsPerSession == 0 invalid, ignoring\n");
		} else if (MaxConnectionsPerSession > 65535) {
			SPDK_ERRLOG("MaxConnectionsPerSession == %d invalid, ignoring\n",
				    MaxConnectionsPerSession);
		} else {
			g_spdk_iscsi.MaxConnectionsPerSession = MaxConnectionsPerSession;
		}
	}

	MaxQueueDepth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (MaxQueueDepth >= 0) {
		if (MaxQueueDepth == 0) {
			SPDK_ERRLOG("MaxQueueDepth == 0 invalid, ignoring\n");
		} else if (MaxQueueDepth > 256) {
			SPDK_ERRLOG("MaxQueueDepth == %d invalid, ignoring\n", MaxQueueDepth);
		} else {
			g_spdk_iscsi.MaxQueueDepth = MaxQueueDepth;
		}
	}

	DefaultTime2Wait = spdk_conf_section_get_intval(sp, "DefaultTime2Wait");
	if (DefaultTime2Wait >= 0) {
		if (DefaultTime2Wait > 3600) {
			SPDK_ERRLOG("DefaultTime2Wait == %d invalid, ignoring\n", DefaultTime2Wait);
		} else {
			g_spdk_iscsi.DefaultTime2Wait = DefaultTime2Wait;
		}
	}
	DefaultTime2Retain = spdk_conf_section_get_intval(sp, "DefaultTime2Retain");
	if (DefaultTime2Retain >= 0) {
		if (DefaultTime2Retain > 3600) {
			SPDK_ERRLOG("DefaultTime2Retain == %d invalid, ignoring\n", DefaultTime2Retain);
		} else {
			g_spdk_iscsi.DefaultTime2Retain = DEFAULT_DEFAULTTIME2RETAIN;
		}
	}
	g_spdk_iscsi.ImmediateData = spdk_conf_section_get_boolval(sp, "ImmediateData",
				     g_spdk_iscsi.ImmediateData);

	/* This option is only for test.
	 * If AllowDuplicateIsid is enabled, it allows different connections carrying
	 * TSIH=0 login the target within the same session.
	 */
	g_spdk_iscsi.AllowDuplicateIsid = spdk_conf_section_get_boolval(sp, "AllowDuplicateIsid",
					  g_spdk_iscsi.AllowDuplicateIsid);

	ErrorRecoveryLevel = spdk_conf_section_get_intval(sp, "ErrorRecoveryLevel");
	if (ErrorRecoveryLevel >= 0) {
		if (ErrorRecoveryLevel > 2) {
			SPDK_ERRLOG("ErrorRecoveryLevel %d not supported, keeping existing %d\n",
				    ErrorRecoveryLevel, g_spdk_iscsi.ErrorRecoveryLevel);
		} else {
			g_spdk_iscsi.ErrorRecoveryLevel = ErrorRecoveryLevel;
		}
	}
	timeout = spdk_conf_section_get_intval(sp, "Timeout");
	if (timeout >= 0) {
		g_spdk_iscsi.timeout = timeout;
	}
	nopininterval = spdk_conf_section_get_intval(sp, "NopInInterval");
	if (nopininterval >= 0) {
		if (nopininterval > MAX_NOPININTERVAL) {
			SPDK_ERRLOG("NopInInterval == %d invalid, ignoring\n", nopininterval);
		} else {
			g_spdk_iscsi.nopininterval = nopininterval;
		}
	}
	val = spdk_conf_section_get_val(sp, "DiscoveryAuthMethod");
	if (val != NULL) {
		if (strcasecmp(val, "CHAP") == 0) {
			g_spdk_iscsi.no_discovery_auth = 0;
			g_spdk_iscsi.req_discovery_auth = 1;
			g_spdk_iscsi.req_discovery_auth_mutual = 0;
		} else if (strcasecmp(val, "Mutual") == 0) {
			g_spdk_iscsi.no_discovery_auth = 0;
			g_spdk_iscsi.req_discovery_auth = 1;
			g_spdk_iscsi.req_discovery_auth_mutual = 1;
		} else if (strcasecmp(val, "Auto") == 0) {
			g_spdk_iscsi.no_discovery_auth = 0;
			g_spdk_iscsi.req_discovery_auth = 0;
			g_spdk_iscsi.req_discovery_auth_mutual = 0;
		} else if (strcasecmp(val, "None") == 0) {
			g_spdk_iscsi.no_discovery_auth = 1;
			g_spdk_iscsi.req_discovery_auth = 0;
			g_spdk_iscsi.req_discovery_auth_mutual = 0;
		} else {
			SPDK_ERRLOG("unknown auth %s, ignoring\n", val);
		}
	}
	val = spdk_conf_section_get_val(sp, "DiscoveryAuthGroup");
	if (val != NULL) {
		ag_tag = val;
		if (strcasecmp(ag_tag, "None") == 0) {
			g_spdk_iscsi.discovery_auth_group = 0;
		} else {
			if (strncasecmp(ag_tag, "AuthGroup",
					strlen("AuthGroup")) != 0
			    || sscanf(ag_tag, "%*[^0-9]%d", &ag_tag_i) != 1
			    || ag_tag_i == 0) {
				SPDK_ERRLOG("invalid auth group %s, ignoring\n", ag_tag);
			} else {
				g_spdk_iscsi.discovery_auth_group = ag_tag_i;
			}
		}
	}
	min_conn_per_core = spdk_conf_section_get_intval(sp, "MinConnectionsPerCore");
	if (min_conn_per_core >= 0) {
		spdk_iscsi_conn_set_min_per_core(min_conn_per_core);
	}
}

static int
spdk_iscsi_app_read_parameters(void)
{
	struct spdk_conf_section *sp;
	int rc;

	g_spdk_iscsi.MaxSessions = DEFAULT_MAX_SESSIONS;
	g_spdk_iscsi.MaxConnectionsPerSession = DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	g_spdk_iscsi.MaxQueueDepth = DEFAULT_MAX_QUEUE_DEPTH;
	g_spdk_iscsi.DefaultTime2Wait = DEFAULT_DEFAULTTIME2WAIT;
	g_spdk_iscsi.DefaultTime2Retain = DEFAULT_DEFAULTTIME2RETAIN;
	g_spdk_iscsi.ImmediateData = DEFAULT_IMMEDIATEDATA;
	g_spdk_iscsi.AllowDuplicateIsid = 0;
	g_spdk_iscsi.ErrorRecoveryLevel = DEFAULT_ERRORRECOVERYLEVEL;
	g_spdk_iscsi.timeout = DEFAULT_TIMEOUT;
	g_spdk_iscsi.nopininterval = DEFAULT_NOPININTERVAL;
	g_spdk_iscsi.no_discovery_auth = 0;
	g_spdk_iscsi.req_discovery_auth = 0;
	g_spdk_iscsi.req_discovery_auth_mutual = 0;
	g_spdk_iscsi.discovery_auth_group = 0;
	g_spdk_iscsi.authfile = strdup(SPDK_ISCSI_DEFAULT_AUTHFILE);
	if (!g_spdk_iscsi.authfile) {
		SPDK_ERRLOG("could not strdup() default authfile name\n");
		return -ENOMEM;
	}
	g_spdk_iscsi.nodebase = strdup(SPDK_ISCSI_DEFAULT_NODEBASE);
	if (!g_spdk_iscsi.nodebase) {
		SPDK_ERRLOG("could not strdup() default nodebase\n");
		return -ENOMEM;
	}

	/* Process parameters */
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_app_read_parameters\n");
	sp = spdk_conf_find_section(NULL, "iSCSI");
	if (sp != NULL) {
		spdk_iscsi_read_parameters_from_config_file(sp);
	}

	g_spdk_iscsi.session = spdk_dma_zmalloc(sizeof(void *) * g_spdk_iscsi.MaxSessions, 0, NULL);
	if (!g_spdk_iscsi.session) {
		SPDK_ERRLOG("spdk_dma_zmalloc() failed for session array\n");
		return -1;
	}

	/*
	 * For now, just support same number of total connections, rather
	 *  than MaxSessions * MaxConnectionsPerSession.  After we add better
	 *  handling for low resource conditions from our various buffer
	 *  pools, we can bump this up to support more connections.
	 */
	g_spdk_iscsi.MaxConnections = g_spdk_iscsi.MaxSessions;

	spdk_iscsi_log_globals();

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
spdk_iscsi_init_complete(int rc)
{
	spdk_iscsi_init_cb cb_fn = g_init_cb_fn;
	void *cb_arg = g_init_cb_arg;

	g_init_cb_fn = NULL;
	g_init_cb_arg = NULL;

	cb_fn(cb_arg, rc);
}

static void
spdk_iscsi_poll_group_poll(void *ctx)
{
	struct spdk_iscsi_poll_group *group = ctx;
	struct spdk_iscsi_conn *conn, *tmp;
	int rc;

	STAILQ_FOREACH_SAFE(conn, &group->connections, link, tmp) {
		conn->fn(conn);
	}

	rc = spdk_sock_group_poll(group->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", group->sock_group);
	}
}

static void
iscsi_create_poll_group_done(void *ctx)
{
	spdk_iscsi_init_complete(0);
}

static void
iscsi_create_poll_group(void *ctx)
{
	struct spdk_iscsi_poll_group *pg;

	pg = &g_spdk_iscsi.poll_group[spdk_env_get_current_core()];
	pg->core = spdk_env_get_current_core();
	assert(pg != NULL);

	STAILQ_INIT(&pg->connections);
	pg->sock_group = spdk_sock_group_create();
	assert(pg->sock_group != NULL);

	pg->poller = spdk_poller_register(spdk_iscsi_poll_group_poll, pg, 0);
}

static void
spdk_initialize_iscsi_poll_group(void)
{
	size_t g_num_poll_groups = spdk_env_get_last_core() + 1;

	g_spdk_iscsi.poll_group = calloc(g_num_poll_groups, sizeof(struct spdk_iscsi_poll_group));
	if (!g_spdk_iscsi.poll_group) {
		SPDK_ERRLOG("Failed to allocated iscsi poll group\n");
		spdk_iscsi_init_complete(-1);
		return;
	}

	/* Send a message to each thread and create a poll group */
	spdk_for_each_thread(iscsi_create_poll_group, NULL, iscsi_create_poll_group_done);
}

void
spdk_iscsi_init(spdk_iscsi_init_cb cb_fn, void *cb_arg)
{
	int rc;

	assert(cb_fn != NULL);
	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;

	rc = spdk_iscsi_app_read_parameters();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_app_read_parameters() failed\n");
		spdk_iscsi_init_complete(-1);
		return;
	}

	rc = spdk_iscsi_initialize_all_pools();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_initialize_all_pools() failed\n");
		spdk_iscsi_init_complete(-1);
		return;
	}

	rc = spdk_iscsi_init_tgt_nodes();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_init_tgt_nodes() failed\n");
		spdk_iscsi_init_complete(-1);
		return;
	}

	rc = spdk_initialize_iscsi_conns();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_initialize_iscsi_conns() failed\n");
		spdk_iscsi_init_complete(-1);
		return;
	}

	spdk_initialize_iscsi_poll_group();
}

void
spdk_iscsi_fini(spdk_iscsi_fini_cb cb_fn, void *cb_arg)
{
	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_iscsi_portal_grp_close_all();
	spdk_shutdown_iscsi_conns();
}

static void
iscsi_destroy_poll_group_done(void *ctx)
{
	free(g_spdk_iscsi.poll_group);
	pthread_mutex_destroy(&g_spdk_iscsi.mutex);
	g_fini_cb_fn(g_fini_cb_arg);
}

static void
iscsi_destroy_poll_group(void *ctx)
{
	struct spdk_iscsi_poll_group *pg;

	pg = &g_spdk_iscsi.poll_group[spdk_env_get_current_core()];
	assert(pg != NULL);

	free(pg->sock_group);
}

static void
spdk_iscsi_destroy_poll_group(void)
{
	if (g_spdk_iscsi.poll_group) {
		/* Send a message to each thread and destroy a poll group */
		spdk_for_each_thread(iscsi_destroy_poll_group, NULL, iscsi_destroy_poll_group_done);
	} else {
		pthread_mutex_destroy(&g_spdk_iscsi.mutex);
		g_fini_cb_fn(g_fini_cb_arg);
	}
}

void
spdk_iscsi_fini_done(void)
{
	spdk_iscsi_check_pools();
	spdk_iscsi_free_pools();

	spdk_iscsi_shutdown_tgt_nodes();
	spdk_iscsi_init_grp_array_destroy();
	spdk_iscsi_portal_grp_array_destroy();
	free(g_spdk_iscsi.authfile);
	free(g_spdk_iscsi.nodebase);
	spdk_iscsi_destroy_poll_group();
}

void
spdk_iscsi_config_text(FILE *fp)
{
	spdk_iscsi_config_dump_section(fp);
	spdk_iscsi_config_dump_portal_groups(fp);
	spdk_iscsi_config_dump_initiator_groups(fp);
	spdk_iscsi_config_dump_target_nodes(fp);
}

SPDK_LOG_REGISTER_COMPONENT("iscsi", SPDK_LOG_ISCSI)
