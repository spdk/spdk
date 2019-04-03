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
#include "spdk/likely.h"

#include "iscsi/iscsi.h"
#include "iscsi/init_grp.h"
#include "iscsi/portal_grp.h"
#include "iscsi/conn.h"
#include "iscsi/task.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

struct spdk_iscsi_opts *g_spdk_iscsi_opts = NULL;

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
"  %s %s\n" \
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
"  FirstBurstLength %d\n" \
"  ImmediateData %s\n" \
"  ErrorRecoveryLevel %d\n" \
"\n"

static void
iscsi_globals_config_text(FILE *fp)
{
	const char *authmethod = "None";
	char authgroup[32] = "None";

	if (NULL == fp) {
		return;
	}

	if (g_spdk_iscsi.require_chap) {
		authmethod = "CHAP";
	} else if (g_spdk_iscsi.mutual_chap) {
		authmethod = "CHAP Mutual";
	} else if (!g_spdk_iscsi.disable_chap) {
		authmethod = "Auto";
	}

	if (g_spdk_iscsi.chap_group) {
		snprintf(authgroup, sizeof(authgroup), "AuthGroup%d", g_spdk_iscsi.chap_group);
	}

	fprintf(fp, ISCSI_CONFIG_TMPL,
		g_spdk_iscsi.nodebase,
		g_spdk_iscsi.authfile ? "AuthFile" : "",
		g_spdk_iscsi.authfile ? g_spdk_iscsi.authfile : "",
		g_spdk_iscsi.timeout, authmethod, authgroup,
		g_spdk_iscsi.MaxSessions, g_spdk_iscsi.MaxConnectionsPerSession,
		g_spdk_iscsi.MaxConnections,
		g_spdk_iscsi.MaxQueueDepth,
		g_spdk_iscsi.DefaultTime2Wait, g_spdk_iscsi.DefaultTime2Retain,
		g_spdk_iscsi.FirstBurstLength,
		(g_spdk_iscsi.ImmediateData) ? "Yes" : "No",
		g_spdk_iscsi.ErrorRecoveryLevel);
}

#define ISCSI_DATA_BUFFER_ALIGNMENT	(0x1000)
#define ISCSI_DATA_BUFFER_MASK		(ISCSI_DATA_BUFFER_ALIGNMENT - 1)

static void
mobj_ctor(struct spdk_mempool *mp, __attribute__((unused)) void *arg,
	  void *_m, __attribute__((unused)) unsigned i)
{
	struct spdk_mobj *m = _m;

	m->mp = mp;
	m->buf = (uint8_t *)m + sizeof(struct spdk_mobj);
	m->buf = (void *)((unsigned long)((uint8_t *)m->buf + ISCSI_DATA_BUFFER_ALIGNMENT) &
			  ~ISCSI_DATA_BUFFER_MASK);
}

#define NUM_PDU_PER_CONNECTION(iscsi)	(2 * (iscsi->MaxQueueDepth + MAX_LARGE_DATAIN_PER_CONNECTION + 8))
#define PDU_POOL_SIZE(iscsi)		(iscsi->MaxConnections * NUM_PDU_PER_CONNECTION(iscsi))
#define IMMEDIATE_DATA_POOL_SIZE(iscsi)	(iscsi->MaxConnections * 128)
#define DATA_OUT_POOL_SIZE(iscsi)	(iscsi->MaxConnections * MAX_DATA_OUT_PER_CONNECTION)

static int
iscsi_initialize_pdu_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;
	int imm_mobj_size = SPDK_BDEV_BUF_SIZE_WITH_MD(spdk_get_max_immediate_data_size()) +
			    sizeof(struct spdk_mobj) + ISCSI_DATA_BUFFER_ALIGNMENT;
	int dout_mobj_size = SPDK_BDEV_BUF_SIZE_WITH_MD(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH) +
			     sizeof(struct spdk_mobj) + ISCSI_DATA_BUFFER_ALIGNMENT;

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
					 imm_mobj_size, 256,
					 SPDK_ENV_SOCKET_ID_ANY,
					 mobj_ctor, NULL);
	if (!iscsi->pdu_immediate_data_pool) {
		SPDK_ERRLOG("create PDU immediate data pool failed\n");
		return -1;
	}

	iscsi->pdu_data_out_pool = spdk_mempool_create_ctor("PDU_data_out_Pool",
				   DATA_OUT_POOL_SIZE(iscsi),
				   dout_mobj_size, 256,
				   SPDK_ENV_SOCKET_ID_ANY,
				   mobj_ctor, NULL);
	if (!iscsi->pdu_data_out_pool) {
		SPDK_ERRLOG("create PDU data out pool failed\n");
		return -1;
	}

	return 0;
}

static void
iscsi_sess_ctor(struct spdk_mempool *pool, void *arg, void *session_buf,
		unsigned index)
{
	struct spdk_iscsi_globals		*iscsi = arg;
	struct spdk_iscsi_sess	*sess = session_buf;

	iscsi->session[index] = sess;

	/* tsih 0 is reserved, so start tsih values at 1. */
	sess->tsih = index + 1;
}

#define DEFAULT_TASK_POOL_SIZE 32768

static int
iscsi_initialize_task_pool(void)
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
static int
iscsi_initialize_session_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	iscsi->session_pool = spdk_mempool_create_ctor("Session_Pool",
			      SESSION_POOL_SIZE(iscsi),
			      sizeof(struct spdk_iscsi_sess), 0,
			      SPDK_ENV_SOCKET_ID_ANY,
			      iscsi_sess_ctor, iscsi);
	if (!iscsi->session_pool) {
		SPDK_ERRLOG("create session pool failed\n");
		return -1;
	}

	return 0;
}

static int
iscsi_initialize_all_pools(void)
{
	if (iscsi_initialize_pdu_pool() != 0) {
		return -1;
	}

	if (iscsi_initialize_session_pool() != 0) {
		return -1;
	}

	if (iscsi_initialize_task_pool() != 0) {
		return -1;
	}

	return 0;
}

static void
iscsi_check_pool(struct spdk_mempool *pool, size_t count)
{
	if (pool && spdk_mempool_count(pool) != count) {
		SPDK_ERRLOG("spdk_mempool_count(%s) == %zu, should be %zu\n",
			    spdk_mempool_get_name(pool), spdk_mempool_count(pool), count);
	}
}

static void
iscsi_check_pools(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	iscsi_check_pool(iscsi->pdu_pool, PDU_POOL_SIZE(iscsi));
	iscsi_check_pool(iscsi->session_pool, SESSION_POOL_SIZE(iscsi));
	iscsi_check_pool(iscsi->pdu_immediate_data_pool, IMMEDIATE_DATA_POOL_SIZE(iscsi));
	iscsi_check_pool(iscsi->pdu_data_out_pool, DATA_OUT_POOL_SIZE(iscsi));
	iscsi_check_pool(iscsi->task_pool, DEFAULT_TASK_POOL_SIZE);
}

static void
iscsi_free_pools(void)
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
iscsi_log_globals(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthFile %s\n",
		      g_spdk_iscsi.authfile ? g_spdk_iscsi.authfile : "(none)");
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "NodeBase %s\n", g_spdk_iscsi.nodebase);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MaxSessions %d\n", g_spdk_iscsi.MaxSessions);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MaxConnectionsPerSession %d\n",
		      g_spdk_iscsi.MaxConnectionsPerSession);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MaxQueueDepth %d\n", g_spdk_iscsi.MaxQueueDepth);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "DefaultTime2Wait %d\n",
		      g_spdk_iscsi.DefaultTime2Wait);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "DefaultTime2Retain %d\n",
		      g_spdk_iscsi.DefaultTime2Retain);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "FirstBurstLength %d\n",
		      g_spdk_iscsi.FirstBurstLength);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "ImmediateData %s\n",
		      g_spdk_iscsi.ImmediateData ? "Yes" : "No");
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AllowDuplicateIsid %s\n",
		      g_spdk_iscsi.AllowDuplicateIsid ? "Yes" : "No");
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "ErrorRecoveryLevel %d\n",
		      g_spdk_iscsi.ErrorRecoveryLevel);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Timeout %d\n", g_spdk_iscsi.timeout);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "NopInInterval %d\n",
		      g_spdk_iscsi.nopininterval);
	if (g_spdk_iscsi.disable_chap) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthMethod None\n");
	} else if (!g_spdk_iscsi.require_chap) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthMethod Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthMethod %s %s\n",
			      g_spdk_iscsi.require_chap ? "CHAP" : "",
			      g_spdk_iscsi.mutual_chap ? "Mutual" : "");
	}

	if (g_spdk_iscsi.chap_group == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthGroup None\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "DiscoveryAuthGroup AuthGroup%d\n",
			      g_spdk_iscsi.chap_group);
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "MinConnectionsPerCore%d\n",
		      spdk_iscsi_conn_get_min_per_core());
}

static void
iscsi_opts_init(struct spdk_iscsi_opts *opts)
{
	opts->MaxSessions = DEFAULT_MAX_SESSIONS;
	opts->MaxConnectionsPerSession = DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	opts->MaxQueueDepth = DEFAULT_MAX_QUEUE_DEPTH;
	opts->DefaultTime2Wait = DEFAULT_DEFAULTTIME2WAIT;
	opts->DefaultTime2Retain = DEFAULT_DEFAULTTIME2RETAIN;
	opts->FirstBurstLength = SPDK_ISCSI_FIRST_BURST_LENGTH;
	opts->ImmediateData = DEFAULT_IMMEDIATEDATA;
	opts->AllowDuplicateIsid = false;
	opts->ErrorRecoveryLevel = DEFAULT_ERRORRECOVERYLEVEL;
	opts->timeout = DEFAULT_TIMEOUT;
	opts->nopininterval = DEFAULT_NOPININTERVAL;
	opts->disable_chap = false;
	opts->require_chap = false;
	opts->mutual_chap = false;
	opts->chap_group = 0;
	opts->authfile = NULL;
	opts->nodebase = NULL;
	opts->min_connections_per_core = DEFAULT_CONNECTIONS_PER_LCORE;
}

struct spdk_iscsi_opts *
spdk_iscsi_opts_alloc(void)
{
	struct spdk_iscsi_opts *opts;

	opts = calloc(1, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("calloc() failed for iscsi options\n");
		return NULL;
	}

	iscsi_opts_init(opts);

	return opts;
}

void
spdk_iscsi_opts_free(struct spdk_iscsi_opts *opts)
{
	free(opts->authfile);
	free(opts->nodebase);
	free(opts);
}

/* Deep copy of spdk_iscsi_opts */
struct spdk_iscsi_opts *
spdk_iscsi_opts_copy(struct spdk_iscsi_opts *src)
{
	struct spdk_iscsi_opts *dst;

	dst = calloc(1, sizeof(*dst));
	if (!dst) {
		SPDK_ERRLOG("calloc() failed for iscsi options\n");
		return NULL;
	}

	if (src->authfile) {
		dst->authfile = strdup(src->authfile);
		if (!dst->authfile) {
			free(dst);
			SPDK_ERRLOG("failed to strdup for auth file %s\n", src->authfile);
			return NULL;
		}
	}

	if (src->nodebase) {
		dst->nodebase = strdup(src->nodebase);
		if (!dst->nodebase) {
			free(dst->authfile);
			free(dst);
			SPDK_ERRLOG("failed to strdup for nodebase %s\n", src->nodebase);
			return NULL;
		}
	}

	dst->MaxSessions = src->MaxSessions;
	dst->MaxConnectionsPerSession = src->MaxConnectionsPerSession;
	dst->MaxQueueDepth = src->MaxQueueDepth;
	dst->DefaultTime2Wait = src->DefaultTime2Wait;
	dst->DefaultTime2Retain = src->DefaultTime2Retain;
	dst->FirstBurstLength = src->FirstBurstLength;
	dst->ImmediateData = src->ImmediateData;
	dst->AllowDuplicateIsid = src->AllowDuplicateIsid;
	dst->ErrorRecoveryLevel = src->ErrorRecoveryLevel;
	dst->timeout = src->timeout;
	dst->nopininterval = src->nopininterval;
	dst->disable_chap = src->disable_chap;
	dst->require_chap = src->require_chap;
	dst->mutual_chap = src->mutual_chap;
	dst->chap_group = src->chap_group;
	dst->min_connections_per_core = src->min_connections_per_core;

	return dst;
}

static int
iscsi_read_config_file_params(struct spdk_conf_section *sp,
			      struct spdk_iscsi_opts *opts)
{
	const char *val;
	int MaxSessions;
	int MaxConnectionsPerSession;
	int MaxQueueDepth;
	int DefaultTime2Wait;
	int DefaultTime2Retain;
	int FirstBurstLength;
	int ErrorRecoveryLevel;
	int timeout;
	int nopininterval;
	int min_conn_per_core = 0;
	const char *ag_tag;
	int ag_tag_i;
	int i;

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Comment %s\n", val);
	}

	val = spdk_conf_section_get_val(sp, "AuthFile");
	if (val != NULL) {
		opts->authfile = strdup(val);
		if (!opts->authfile) {
			SPDK_ERRLOG("strdup() failed for AuthFile\n");
			return -ENOMEM;
		}
	}

	val = spdk_conf_section_get_val(sp, "NodeBase");
	if (val != NULL) {
		opts->nodebase = strdup(val);
		if (!opts->nodebase) {
			free(opts->authfile);
			SPDK_ERRLOG("strdup() failed for NodeBase\n");
			return -ENOMEM;
		}
	}

	MaxSessions = spdk_conf_section_get_intval(sp, "MaxSessions");
	if (MaxSessions >= 0) {
		opts->MaxSessions = MaxSessions;
	}

	MaxConnectionsPerSession = spdk_conf_section_get_intval(sp, "MaxConnectionsPerSession");
	if (MaxConnectionsPerSession >= 0) {
		opts->MaxConnectionsPerSession = MaxConnectionsPerSession;
	}

	MaxQueueDepth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (MaxQueueDepth >= 0) {
		opts->MaxQueueDepth = MaxQueueDepth;
	}

	DefaultTime2Wait = spdk_conf_section_get_intval(sp, "DefaultTime2Wait");
	if (DefaultTime2Wait >= 0) {
		opts->DefaultTime2Wait = DefaultTime2Wait;
	}

	DefaultTime2Retain = spdk_conf_section_get_intval(sp, "DefaultTime2Retain");
	if (DefaultTime2Retain >= 0) {
		opts->DefaultTime2Retain = DefaultTime2Retain;
	}

	FirstBurstLength = spdk_conf_section_get_intval(sp, "FirstBurstLength");
	if (FirstBurstLength >= 0) {
		opts->FirstBurstLength = FirstBurstLength;
	}

	opts->ImmediateData = spdk_conf_section_get_boolval(sp, "ImmediateData",
			      opts->ImmediateData);

	/* This option is only for test.
	 * If AllowDuplicateIsid is enabled, it allows different connections carrying
	 * TSIH=0 login the target within the same session.
	 */
	opts->AllowDuplicateIsid = spdk_conf_section_get_boolval(sp, "AllowDuplicateIsid",
				   opts->AllowDuplicateIsid);

	ErrorRecoveryLevel = spdk_conf_section_get_intval(sp, "ErrorRecoveryLevel");
	if (ErrorRecoveryLevel >= 0) {
		opts->ErrorRecoveryLevel = ErrorRecoveryLevel;
	}
	timeout = spdk_conf_section_get_intval(sp, "Timeout");
	if (timeout >= 0) {
		opts->timeout = timeout;
	}
	nopininterval = spdk_conf_section_get_intval(sp, "NopInInterval");
	if (nopininterval >= 0) {
		opts->nopininterval = nopininterval;
	}
	val = spdk_conf_section_get_val(sp, "DiscoveryAuthMethod");
	if (val != NULL) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "DiscoveryAuthMethod", 0, i);
			if (val == NULL) {
				break;
			}
			if (strcasecmp(val, "CHAP") == 0) {
				opts->require_chap = true;
			} else if (strcasecmp(val, "Mutual") == 0) {
				opts->require_chap = true;
				opts->mutual_chap = true;
			} else if (strcasecmp(val, "Auto") == 0) {
				opts->disable_chap = false;
				opts->require_chap = false;
				opts->mutual_chap = false;
			} else if (strcasecmp(val, "None") == 0) {
				opts->disable_chap = true;
				opts->require_chap = false;
				opts->mutual_chap = false;
			} else {
				SPDK_ERRLOG("unknown CHAP mode %s\n", val);
			}
		}
		if (opts->mutual_chap && !opts->require_chap) {
			SPDK_ERRLOG("CHAP must set to be required when using mutual CHAP.\n");
			return -EINVAL;
		}
	}
	val = spdk_conf_section_get_val(sp, "DiscoveryAuthGroup");
	if (val != NULL) {
		ag_tag = val;
		if (strcasecmp(ag_tag, "None") == 0) {
			opts->chap_group = 0;
		} else {
			if (strncasecmp(ag_tag, "AuthGroup",
					strlen("AuthGroup")) != 0
			    || sscanf(ag_tag, "%*[^0-9]%d", &ag_tag_i) != 1
			    || ag_tag_i == 0) {
				SPDK_ERRLOG("invalid auth group %s, ignoring\n", ag_tag);
			} else {
				opts->chap_group = ag_tag_i;
			}
		}
	}
	min_conn_per_core = spdk_conf_section_get_intval(sp, "MinConnectionsPerCore");
	if (min_conn_per_core >= 0) {
		opts->min_connections_per_core = min_conn_per_core;
	}

	return 0;
}

static int
iscsi_opts_verify(struct spdk_iscsi_opts *opts)
{
	if (!opts->nodebase) {
		opts->nodebase = strdup(SPDK_ISCSI_DEFAULT_NODEBASE);
		if (opts->nodebase == NULL) {
			SPDK_ERRLOG("strdup() failed for default nodebase\n");
			return -ENOMEM;
		}
	}

	if (opts->MaxSessions == 0 || opts->MaxSessions > 65535) {
		SPDK_ERRLOG("%d is invalid. MaxSessions must be more than 0 and no more than 65535\n",
			    opts->MaxSessions);
		return -EINVAL;
	}

	if (opts->MaxConnectionsPerSession == 0 || opts->MaxConnectionsPerSession > 65535) {
		SPDK_ERRLOG("%d is invalid. MaxConnectionsPerSession must be more than 0 and no more than 65535\n",
			    opts->MaxConnectionsPerSession);
		return -EINVAL;
	}

	if (opts->MaxQueueDepth == 0 || opts->MaxQueueDepth > 256) {
		SPDK_ERRLOG("%d is invalid. MaxQueueDepth must be more than 0 and no more than 256\n",
			    opts->MaxQueueDepth);
		return -EINVAL;
	}

	if (opts->DefaultTime2Wait > 3600) {
		SPDK_ERRLOG("%d is invalid. DefaultTime2Wait must be no more than 3600\n",
			    opts->DefaultTime2Wait);
		return -EINVAL;
	}

	if (opts->DefaultTime2Retain > 3600) {
		SPDK_ERRLOG("%d is invalid. DefaultTime2Retain must be no more than 3600\n",
			    opts->DefaultTime2Retain);
		return -EINVAL;
	}

	if (opts->FirstBurstLength >= SPDK_ISCSI_MIN_FIRST_BURST_LENGTH) {
		if (opts->FirstBurstLength > SPDK_ISCSI_MAX_BURST_LENGTH) {
			SPDK_ERRLOG("FirstBurstLength %d shall not exceed MaxBurstLength %d\n",
				    opts->FirstBurstLength, SPDK_ISCSI_MAX_BURST_LENGTH);
			return -EINVAL;
		}
	} else {
		SPDK_ERRLOG("FirstBurstLength %d shall be no less than %d\n",
			    opts->FirstBurstLength, SPDK_ISCSI_MIN_FIRST_BURST_LENGTH);
		return -EINVAL;
	}

	if (opts->ErrorRecoveryLevel > 2) {
		SPDK_ERRLOG("ErrorRecoveryLevel %d is not supported.\n", opts->ErrorRecoveryLevel);
		return -EINVAL;
	}

	if (opts->timeout < 0) {
		SPDK_ERRLOG("%d is invalid. timeout must not be less than 0\n", opts->timeout);
		return -EINVAL;
	}

	if (opts->nopininterval < 0 || opts->nopininterval > MAX_NOPININTERVAL) {
		SPDK_ERRLOG("%d is invalid. nopinterval must be between 0 and %d\n",
			    opts->nopininterval, MAX_NOPININTERVAL);
		return -EINVAL;
	}

	if (!spdk_iscsi_check_chap_params(opts->disable_chap, opts->require_chap,
					  opts->mutual_chap, opts->chap_group)) {
		SPDK_ERRLOG("CHAP params in opts are illegal combination\n");
		return -EINVAL;
	}

	return 0;
}

static int
iscsi_parse_options(struct spdk_iscsi_opts **popts)
{
	struct spdk_iscsi_opts *opts;
	struct spdk_conf_section *sp;
	int rc;

	opts = spdk_iscsi_opts_alloc();
	if (!opts) {
		SPDK_ERRLOG("spdk_iscsi_opts_alloc_failed() failed\n");
		return -ENOMEM;
	}

	/* Process parameters */
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "iscsi_read_config_file_parmas\n");
	sp = spdk_conf_find_section(NULL, "iSCSI");
	if (sp != NULL) {
		rc = iscsi_read_config_file_params(sp, opts);
		if (rc != 0) {
			free(opts);
			SPDK_ERRLOG("iscsi_read_config_file_params() failed\n");
			return rc;
		}
	}

	*popts = opts;

	return 0;
}

static int
iscsi_set_global_params(struct spdk_iscsi_opts *opts)
{
	int rc;

	rc = iscsi_opts_verify(opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_iscsi_opts_verify() failed\n");
		return rc;
	}

	if (opts->authfile != NULL) {
		g_spdk_iscsi.authfile = strdup(opts->authfile);
		if (!g_spdk_iscsi.authfile) {
			SPDK_ERRLOG("failed to strdup for auth file %s\n", opts->authfile);
			return -ENOMEM;
		}
	}

	g_spdk_iscsi.nodebase = strdup(opts->nodebase);
	if (!g_spdk_iscsi.nodebase) {
		SPDK_ERRLOG("failed to strdup for nodebase %s\n", opts->nodebase);
		return -ENOMEM;
	}

	g_spdk_iscsi.MaxSessions = opts->MaxSessions;
	g_spdk_iscsi.MaxConnectionsPerSession = opts->MaxConnectionsPerSession;
	g_spdk_iscsi.MaxQueueDepth = opts->MaxQueueDepth;
	g_spdk_iscsi.DefaultTime2Wait = opts->DefaultTime2Wait;
	g_spdk_iscsi.DefaultTime2Retain = opts->DefaultTime2Retain;
	g_spdk_iscsi.FirstBurstLength = opts->FirstBurstLength;
	g_spdk_iscsi.ImmediateData = opts->ImmediateData;
	g_spdk_iscsi.AllowDuplicateIsid = opts->AllowDuplicateIsid;
	g_spdk_iscsi.ErrorRecoveryLevel = opts->ErrorRecoveryLevel;
	g_spdk_iscsi.timeout = opts->timeout;
	g_spdk_iscsi.nopininterval = opts->nopininterval;
	g_spdk_iscsi.disable_chap = opts->disable_chap;
	g_spdk_iscsi.require_chap = opts->require_chap;
	g_spdk_iscsi.mutual_chap = opts->mutual_chap;
	g_spdk_iscsi.chap_group = opts->chap_group;

	spdk_iscsi_conn_set_min_per_core(opts->min_connections_per_core);

	iscsi_log_globals();

	return 0;
}

int
spdk_iscsi_set_discovery_auth(bool disable_chap, bool require_chap, bool mutual_chap,
			      int32_t chap_group)
{
	if (!spdk_iscsi_check_chap_params(disable_chap, require_chap, mutual_chap,
					  chap_group)) {
		SPDK_ERRLOG("CHAP params are illegal combination\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	g_spdk_iscsi.disable_chap = disable_chap;
	g_spdk_iscsi.require_chap = require_chap;
	g_spdk_iscsi.mutual_chap = mutual_chap;
	g_spdk_iscsi.chap_group = chap_group;
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	return 0;
}

int
spdk_iscsi_auth_group_add_secret(struct spdk_iscsi_auth_group *group,
				 const char *user, const char *secret,
				 const char *muser, const char *msecret)
{
	struct spdk_iscsi_auth_secret *_secret;
	size_t len;

	if (user == NULL || secret == NULL) {
		SPDK_ERRLOG("user and secret must be specified\n");
		return -EINVAL;
	}

	if (muser != NULL && msecret == NULL) {
		SPDK_ERRLOG("msecret must be specified with muser\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
		if (strcmp(_secret->user, user) == 0) {
			SPDK_ERRLOG("user for secret is duplicated\n");
			return -EEXIST;
		}
	}

	_secret = calloc(1, sizeof(*_secret));
	if (_secret == NULL) {
		SPDK_ERRLOG("calloc() failed for CHAP secret\n");
		return -ENOMEM;
	}

	len = strnlen(user, sizeof(_secret->user));
	if (len > sizeof(_secret->user) - 1) {
		SPDK_ERRLOG("CHAP user longer than %zu characters: %s\n",
			    sizeof(_secret->user) - 1, user);
		free(_secret);
		return -EINVAL;
	}
	memcpy(_secret->user, user, len);

	len = strnlen(secret, sizeof(_secret->secret));
	if (len > sizeof(_secret->secret) - 1) {
		SPDK_ERRLOG("CHAP secret longer than %zu characters: %s\n",
			    sizeof(_secret->secret) - 1, secret);
		free(_secret);
		return -EINVAL;
	}
	memcpy(_secret->secret, secret, len);

	if (muser != NULL) {
		len = strnlen(muser, sizeof(_secret->muser));
		if (len > sizeof(_secret->muser) - 1) {
			SPDK_ERRLOG("Mutual CHAP user longer than %zu characters: %s\n",
				    sizeof(_secret->muser) - 1, muser);
			free(_secret);
			return -EINVAL;
		}
		memcpy(_secret->muser, muser, len);

		len = strnlen(msecret, sizeof(_secret->msecret));
		if (len > sizeof(_secret->msecret) - 1) {
			SPDK_ERRLOG("Mutual CHAP secret longer than %zu characters: %s\n",
				    sizeof(_secret->msecret) - 1, msecret);
			free(_secret);
			return -EINVAL;
		}
		memcpy(_secret->msecret, msecret, len);
	}

	TAILQ_INSERT_TAIL(&group->secret_head, _secret, tailq);
	return 0;
}

int
spdk_iscsi_auth_group_delete_secret(struct spdk_iscsi_auth_group *group,
				    const char *user)
{
	struct spdk_iscsi_auth_secret *_secret;

	if (user == NULL) {
		SPDK_ERRLOG("user must be specified\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
		if (strcmp(_secret->user, user) == 0) {
			break;
		}
	}

	if (_secret == NULL) {
		SPDK_ERRLOG("secret is not found\n");
		return -ENODEV;
	}

	TAILQ_REMOVE(&group->secret_head, _secret, tailq);
	free(_secret);

	return 0;
}

int
spdk_iscsi_add_auth_group(int32_t tag, struct spdk_iscsi_auth_group **_group)
{
	struct spdk_iscsi_auth_group *group;

	TAILQ_FOREACH(group, &g_spdk_iscsi.auth_group_head, tailq) {
		if (group->tag == tag) {
			SPDK_ERRLOG("Auth group (%d) already exists\n", tag);
			return -EEXIST;
		}
	}

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		SPDK_ERRLOG("calloc() failed for auth group\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&group->secret_head);
	group->tag = tag;

	TAILQ_INSERT_TAIL(&g_spdk_iscsi.auth_group_head, group, tailq);

	*_group = group;
	return 0;
}

void
spdk_iscsi_delete_auth_group(struct spdk_iscsi_auth_group *group)
{
	struct spdk_iscsi_auth_secret *_secret, *tmp;

	TAILQ_REMOVE(&g_spdk_iscsi.auth_group_head, group, tailq);

	TAILQ_FOREACH_SAFE(_secret, &group->secret_head, tailq, tmp) {
		TAILQ_REMOVE(&group->secret_head, _secret, tailq);
		free(_secret);
	}
	free(group);
}

struct spdk_iscsi_auth_group *
spdk_iscsi_find_auth_group_by_tag(int32_t tag)
{
	struct spdk_iscsi_auth_group *group;

	TAILQ_FOREACH(group, &g_spdk_iscsi.auth_group_head, tailq) {
		if (group->tag == tag) {
			return group;
		}
	}

	return NULL;
}

static void
iscsi_auth_groups_destroy(void)
{
	struct spdk_iscsi_auth_group *group, *tmp;

	TAILQ_FOREACH_SAFE(group, &g_spdk_iscsi.auth_group_head, tailq, tmp) {
		spdk_iscsi_delete_auth_group(group);
	}
}

static int
iscsi_parse_auth_group(struct spdk_conf_section *sp)
{
	int rc;
	int i;
	int tag;
	const char *val, *user, *secret, *muser, *msecret;
	struct spdk_iscsi_auth_group *group = NULL;

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Comment %s\n", val);
	}

	tag = spdk_conf_section_get_num(sp);

	rc = spdk_iscsi_add_auth_group(tag, &group);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add auth group\n");
		return rc;
	}

	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "Auth", i);
		if (val == NULL) {
			break;
		}

		user = spdk_conf_section_get_nmval(sp, "Auth", i, 0);
		secret = spdk_conf_section_get_nmval(sp, "Auth", i, 1);
		muser = spdk_conf_section_get_nmval(sp, "Auth", i, 2);
		msecret = spdk_conf_section_get_nmval(sp, "Auth", i, 3);

		rc = spdk_iscsi_auth_group_add_secret(group, user, secret, muser, msecret);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to add secret to auth group\n");
			spdk_iscsi_delete_auth_group(group);
			return rc;
		}
	}

	return 0;
}

static int
iscsi_parse_auth_info(void)
{
	struct spdk_conf *config;
	struct spdk_conf_section *sp;
	int rc;

	config = spdk_conf_allocate();
	if (!config) {
		SPDK_ERRLOG("Failed to allocate config file\n");
		return -ENOMEM;
	}

	rc = spdk_conf_read(config, g_spdk_iscsi.authfile);
	if (rc != 0) {
		SPDK_INFOLOG(SPDK_LOG_ISCSI, "Failed to load auth file\n");
		spdk_conf_free(config);
		return rc;
	}

	sp = spdk_conf_first_section(config);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "AuthGroup")) {
			if (spdk_conf_section_get_num(sp) == 0) {
				SPDK_ERRLOG("Group 0 is invalid\n");
				iscsi_auth_groups_destroy();
				spdk_conf_free(config);
				return -EINVAL;
			}

			rc = iscsi_parse_auth_group(sp);
			if (rc != 0) {
				SPDK_ERRLOG("parse_auth_group() failed\n");
				iscsi_auth_groups_destroy();
				spdk_conf_free(config);
				return rc;
			}
		}
		sp = spdk_conf_next_section(sp);
	}

	spdk_conf_free(config);
	return 0;
}

static struct spdk_iscsi_auth_secret *
iscsi_find_auth_secret(const char *authuser, int ag_tag)
{
	struct spdk_iscsi_auth_group *group;
	struct spdk_iscsi_auth_secret *_secret;

	TAILQ_FOREACH(group, &g_spdk_iscsi.auth_group_head, tailq) {
		if (group->tag == ag_tag) {
			TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
				if (strcmp(_secret->user, authuser) == 0) {
					return _secret;
				}
			}
		}
	}

	return NULL;
}

int
spdk_iscsi_chap_get_authinfo(struct iscsi_chap_auth *auth, const char *authuser,
			     int ag_tag)
{
	struct spdk_iscsi_auth_secret *_secret;

	if (authuser == NULL) {
		return -EINVAL;
	}

	if (auth->user[0] != '\0') {
		memset(auth->user, 0, sizeof(auth->user));
		memset(auth->secret, 0, sizeof(auth->secret));
		memset(auth->muser, 0, sizeof(auth->muser));
		memset(auth->msecret, 0, sizeof(auth->msecret));
	}

	pthread_mutex_lock(&g_spdk_iscsi.mutex);

	_secret = iscsi_find_auth_secret(authuser, ag_tag);
	if (_secret == NULL) {
		pthread_mutex_unlock(&g_spdk_iscsi.mutex);

		SPDK_ERRLOG("CHAP secret is not found: user:%s, tag:%d\n",
			    authuser, ag_tag);
		return -ENOENT;
	}

	memcpy(auth->user, _secret->user, sizeof(auth->user));
	memcpy(auth->secret, _secret->secret, sizeof(auth->secret));

	if (_secret->muser[0] != '\0') {
		memcpy(auth->muser, _secret->muser, sizeof(auth->muser));
		memcpy(auth->msecret, _secret->msecret, sizeof(auth->msecret));
	}

	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;
}

static int
iscsi_initialize_global_params(void)
{
	int rc;

	if (!g_spdk_iscsi_opts) {
		rc = iscsi_parse_options(&g_spdk_iscsi_opts);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_iscsi_parse_options() failed\n");
			return rc;
		}
	}

	rc = iscsi_set_global_params(g_spdk_iscsi_opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_iscsi_set_global_params() failed\n");
	}

	spdk_iscsi_opts_free(g_spdk_iscsi_opts);
	g_spdk_iscsi_opts = NULL;

	return rc;
}

static void
iscsi_init_complete(int rc)
{
	spdk_iscsi_init_cb cb_fn = g_init_cb_fn;
	void *cb_arg = g_init_cb_arg;

	g_init_cb_fn = NULL;
	g_init_cb_arg = NULL;

	cb_fn(cb_arg, rc);
}

static int
iscsi_poll_group_poll(void *ctx)
{
	struct spdk_iscsi_poll_group *group = ctx;
	struct spdk_iscsi_conn *conn, *tmp;
	int rc;

	if (spdk_unlikely(STAILQ_EMPTY(&group->connections))) {
		return 0;
	}

	rc = spdk_sock_group_poll(group->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", group->sock_group);
	}

	STAILQ_FOREACH_SAFE(conn, &group->connections, link, tmp) {
		if (conn->state == ISCSI_CONN_STATE_EXITING) {
			spdk_iscsi_conn_destruct(conn);
		}
	}

	return -1;
}

static int
iscsi_poll_group_handle_nop(void *ctx)
{
	struct spdk_iscsi_poll_group *group = ctx;
	struct spdk_iscsi_conn *conn, *tmp;

	STAILQ_FOREACH_SAFE(conn, &group->connections, link, tmp) {
		spdk_iscsi_conn_handle_nop(conn);
	}

	return -1;
}

static void
iscsi_create_poll_group(void *ctx)
{
	struct spdk_iscsi_poll_group *pg;

	assert(g_spdk_iscsi.poll_group != NULL);
	pg = &g_spdk_iscsi.poll_group[spdk_env_get_current_core()];
	pg->core = spdk_env_get_current_core();

	STAILQ_INIT(&pg->connections);
	pg->sock_group = spdk_sock_group_create();
	assert(pg->sock_group != NULL);

	pg->poller = spdk_poller_register(iscsi_poll_group_poll, pg, 0);
	/* set the period to 1 sec */
	pg->nop_poller = spdk_poller_register(iscsi_poll_group_handle_nop, pg, 1000000);
}

static void
iscsi_unregister_poll_group(void *ctx)
{
	struct spdk_iscsi_poll_group *pg;

	assert(g_spdk_iscsi.poll_group != NULL);
	pg = &g_spdk_iscsi.poll_group[spdk_env_get_current_core()];
	assert(pg->poller != NULL);
	assert(pg->sock_group != NULL);

	spdk_sock_group_close(&pg->sock_group);
	spdk_poller_unregister(&pg->poller);
	spdk_poller_unregister(&pg->nop_poller);
}

static void
initialize_iscsi_poll_group(spdk_msg_fn cpl)
{
	size_t g_num_poll_groups = spdk_env_get_last_core() + 1;

	g_spdk_iscsi.poll_group = calloc(g_num_poll_groups, sizeof(struct spdk_iscsi_poll_group));
	if (!g_spdk_iscsi.poll_group) {
		SPDK_ERRLOG("Failed to allocated iscsi poll group\n");
		iscsi_init_complete(-1);
		return;
	}

	/* Send a message to each thread and create a poll group */
	spdk_for_each_thread(iscsi_create_poll_group, NULL, cpl);
}

static void
iscsi_parse_configuration(void *ctx)
{
	int rc;

	rc = spdk_iscsi_parse_portal_grps();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_parse_portal_grps() failed\n");
		goto end;
	}

	rc = spdk_iscsi_parse_init_grps();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_parse_init_grps() failed\n");
		goto end;
	}

	rc = spdk_iscsi_parse_tgt_nodes();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_parse_tgt_nodes() failed\n");
	}

	if (g_spdk_iscsi.authfile != NULL) {
		if (access(g_spdk_iscsi.authfile, R_OK) == 0) {
			rc = iscsi_parse_auth_info();
			if (rc < 0) {
				SPDK_ERRLOG("spdk_iscsi_parse_auth_info() failed\n");
			}
		} else {
			SPDK_INFOLOG(SPDK_LOG_ISCSI, "CHAP secret file is not found in the path %s\n",
				     g_spdk_iscsi.authfile);
		}
	}

end:
	iscsi_init_complete(rc);
}

static int
iscsi_parse_globals(void)
{
	int rc;

	rc = iscsi_initialize_global_params();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_iscsi_initialize_iscsi_global_params() failed\n");
		return rc;
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

	rc = iscsi_initialize_all_pools();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_initialize_all_pools() failed\n");
		return -1;
	}

	rc = spdk_initialize_iscsi_conns();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_initialize_iscsi_conns() failed\n");
		return rc;
	}

	initialize_iscsi_poll_group(iscsi_parse_configuration);
	return 0;
}

void
spdk_iscsi_init(spdk_iscsi_init_cb cb_fn, void *cb_arg)
{
	int rc;

	assert(cb_fn != NULL);
	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;

	rc = iscsi_parse_globals();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_parse_globals() failed\n");
		iscsi_init_complete(-1);
	}

	/*
	 * spdk_iscsi_parse_configuration() will be called as the callback to
	 * spdk_initialize_iscsi_poll_group() and will complete iSCSI
	 * subsystem initialization.
	 */
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
iscsi_fini_done(void *arg)
{
	iscsi_check_pools();
	iscsi_free_pools();

	spdk_iscsi_shutdown_tgt_nodes();
	spdk_iscsi_init_grps_destroy();
	spdk_iscsi_portal_grps_destroy();
	iscsi_auth_groups_destroy();
	free(g_spdk_iscsi.authfile);
	free(g_spdk_iscsi.nodebase);
	free(g_spdk_iscsi.poll_group);

	pthread_mutex_destroy(&g_spdk_iscsi.mutex);
	g_fini_cb_fn(g_fini_cb_arg);
}

void
spdk_shutdown_iscsi_conns_done(void)
{
	if (g_spdk_iscsi.poll_group) {
		spdk_for_each_thread(iscsi_unregister_poll_group, NULL, iscsi_fini_done);
	} else {
		iscsi_fini_done(NULL);
	}
}

void
spdk_iscsi_config_text(FILE *fp)
{
	iscsi_globals_config_text(fp);
	spdk_iscsi_portal_grps_config_text(fp);
	spdk_iscsi_init_grps_config_text(fp);
	spdk_iscsi_tgt_nodes_config_text(fp);
}

void
spdk_iscsi_opts_info_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	if (g_spdk_iscsi.authfile != NULL) {
		spdk_json_write_named_string(w, "auth_file", g_spdk_iscsi.authfile);
	}
	spdk_json_write_named_string(w, "node_base", g_spdk_iscsi.nodebase);

	spdk_json_write_named_uint32(w, "max_sessions", g_spdk_iscsi.MaxSessions);
	spdk_json_write_named_uint32(w, "max_connections_per_session",
				     g_spdk_iscsi.MaxConnectionsPerSession);

	spdk_json_write_named_uint32(w, "max_queue_depth", g_spdk_iscsi.MaxQueueDepth);

	spdk_json_write_named_uint32(w, "default_time2wait", g_spdk_iscsi.DefaultTime2Wait);
	spdk_json_write_named_uint32(w, "default_time2retain", g_spdk_iscsi.DefaultTime2Retain);

	spdk_json_write_named_uint32(w, "first_burst_length", g_spdk_iscsi.FirstBurstLength);

	spdk_json_write_named_bool(w, "immediate_data", g_spdk_iscsi.ImmediateData);

	spdk_json_write_named_bool(w, "allow_duplicated_isid", g_spdk_iscsi.AllowDuplicateIsid);

	spdk_json_write_named_uint32(w, "error_recovery_level", g_spdk_iscsi.ErrorRecoveryLevel);

	spdk_json_write_named_int32(w, "nop_timeout", g_spdk_iscsi.timeout);
	spdk_json_write_named_int32(w, "nop_in_interval", g_spdk_iscsi.nopininterval);

	spdk_json_write_named_bool(w, "disable_chap", g_spdk_iscsi.disable_chap);
	spdk_json_write_named_bool(w, "require_chap", g_spdk_iscsi.require_chap);
	spdk_json_write_named_bool(w, "mutual_chap", g_spdk_iscsi.mutual_chap);
	spdk_json_write_named_int32(w, "chap_group", g_spdk_iscsi.chap_group);

	spdk_json_write_named_uint32(w, "min_connections_per_core",
				     spdk_iscsi_conn_get_min_per_core());

	spdk_json_write_object_end(w);
}

static void
iscsi_auth_group_info_json(struct spdk_iscsi_auth_group *group,
			   struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_auth_secret *_secret;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_int32(w, "tag", group->tag);

	spdk_json_write_named_array_begin(w, "secrets");
	TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "user", _secret->user);
		spdk_json_write_named_string(w, "secret", _secret->secret);

		if (_secret->muser[0] != '\0') {
			spdk_json_write_named_string(w, "muser", _secret->muser);
			spdk_json_write_named_string(w, "msecret", _secret->msecret);
		}

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

static void
iscsi_auth_group_config_json(struct spdk_iscsi_auth_group *group,
			     struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "add_iscsi_auth_group");

	spdk_json_write_name(w, "params");
	iscsi_auth_group_info_json(group, w);

	spdk_json_write_object_end(w);
}

void
spdk_iscsi_auth_groups_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_auth_group *group;

	TAILQ_FOREACH(group, &g_spdk_iscsi.auth_group_head, tailq) {
		iscsi_auth_group_info_json(group, w);
	}
}

static void
iscsi_auth_groups_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_auth_group *group;

	TAILQ_FOREACH(group, &g_spdk_iscsi.auth_group_head, tailq) {
		iscsi_auth_group_config_json(group, w);
	}
}

static void
iscsi_opts_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "set_iscsi_options");

	spdk_json_write_name(w, "params");
	spdk_iscsi_opts_info_json(w);

	spdk_json_write_object_end(w);
}

void
spdk_iscsi_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);
	iscsi_opts_config_json(w);
	spdk_iscsi_portal_grps_config_json(w);
	spdk_iscsi_init_grps_config_json(w);
	spdk_iscsi_tgt_nodes_config_json(w);
	iscsi_auth_groups_config_json(w);
	spdk_json_write_array_end(w);
}

SPDK_LOG_REGISTER_COMPONENT("iscsi", SPDK_LOG_ISCSI)
