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
#include "spdk/sock.h"
#include "spdk/event.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"
#include "iscsi/portal_grp.h"
#include "iscsi/tgt_node.h"

#define PORTNUMSTRLEN 32
#define ACCEPT_TIMEOUT_US 1000 /* 1ms */

static int
iscsi_portal_accept(void *arg)
{
	struct spdk_iscsi_portal	*portal = arg;
	struct spdk_sock		*sock;
	int				rc;
	int				count = 0;

	if (portal->sock == NULL) {
		return -1;
	}

	while (1) {
		sock = spdk_sock_accept(portal->sock);
		if (sock != NULL) {
			rc = iscsi_conn_construct(portal, sock);
			if (rc < 0) {
				spdk_sock_close(&sock);
				SPDK_ERRLOG("spdk_iscsi_connection_construct() failed\n");
				break;
			}
			count++;
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("accept error(%d): %s\n", errno, spdk_strerror(errno));
			}
			break;
		}
	}

	return count;
}

static struct spdk_iscsi_portal *
iscsi_portal_find_by_addr(const char *host, const char *port)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &g_iscsi.portal_head, g_tailq) {
		if (!strcmp(p->host, host) && !strcmp(p->port, port)) {
			return p;
		}
	}

	return NULL;
}

/* Assumes caller allocated host and port strings on the heap */
struct spdk_iscsi_portal *
iscsi_portal_create(const char *host, const char *port)
{
	struct spdk_iscsi_portal *p = NULL, *tmp;

	assert(host != NULL);
	assert(port != NULL);

	if (strlen(host) > MAX_PORTAL_ADDR || strlen(port) > MAX_PORTAL_PORT) {
		return NULL;
	}

	p = calloc(1, sizeof(*p));
	if (!p) {
		SPDK_ERRLOG("calloc() failed for portal\n");
		return NULL;
	}

	/* check and overwrite abbreviation of wildcard */
	if (strcasecmp(host, "[*]") == 0) {
		SPDK_WARNLOG("Please use \"[::]\" as IPv6 wildcard\n");
		SPDK_WARNLOG("Convert \"[*]\" to \"[::]\" automatically\n");
		SPDK_WARNLOG("(Use of \"[*]\" will be deprecated in a future release)");
		snprintf(p->host, sizeof(p->host), "[::]");
	} else if (strcasecmp(host, "*") == 0) {
		SPDK_WARNLOG("Please use \"0.0.0.0\" as IPv4 wildcard\n");
		SPDK_WARNLOG("Convert \"*\" to \"0.0.0.0\" automatically\n");
		SPDK_WARNLOG("(Use of \"[*]\" will be deprecated in a future release)");
		snprintf(p->host, sizeof(p->host), "0.0.0.0");
	} else {
		memcpy(p->host, host, strlen(host));
	}

	memcpy(p->port, port, strlen(port));

	p->sock = NULL;
	p->group = NULL; /* set at a later time by caller */
	p->acceptor_poller = NULL;

	pthread_mutex_lock(&g_iscsi.mutex);
	tmp = iscsi_portal_find_by_addr(host, port);
	if (tmp != NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);
		SPDK_ERRLOG("portal (%s, %s) already exists\n", host, port);
		goto error_out;
	}

	TAILQ_INSERT_TAIL(&g_iscsi.portal_head, p, g_tailq);
	pthread_mutex_unlock(&g_iscsi.mutex);

	return p;

error_out:
	free(p);

	return NULL;
}

void
iscsi_portal_destroy(struct spdk_iscsi_portal *p)
{
	assert(p != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "iscsi_portal_destroy\n");

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_REMOVE(&g_iscsi.portal_head, p, g_tailq);
	pthread_mutex_unlock(&g_iscsi.mutex);

	free(p);

}

static int
iscsi_portal_open(struct spdk_iscsi_portal *p)
{
	struct spdk_sock *sock;
	int port;

	if (p->sock != NULL) {
		SPDK_ERRLOG("portal (%s, %s) is already opened\n",
			    p->host, p->port);
		return -1;
	}

	port = (int)strtol(p->port, NULL, 0);
	sock = spdk_sock_listen(p->host, port, NULL);
	if (sock == NULL) {
		SPDK_ERRLOG("listen error %.64s.%d\n", p->host, port);
		return -1;
	}

	p->sock = sock;

	/*
	 * When the portal is created by config file, incoming connection
	 * requests for the socket are pended to accept until reactors start.
	 * However the gap between listen() and accept() will be slight and
	 * the requests will be queued by the nonzero backlog of the socket
	 * or resend by TCP.
	 */
	p->acceptor_poller = SPDK_POLLER_REGISTER(iscsi_portal_accept, p, ACCEPT_TIMEOUT_US);

	return 0;
}

static void
iscsi_portal_close(struct spdk_iscsi_portal *p)
{
	if (p->sock) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "close portal (%s, %s)\n",
			      p->host, p->port);
		spdk_poller_unregister(&p->acceptor_poller);
		spdk_sock_close(&p->sock);
	}
}

static int
iscsi_parse_portal(const char *portalstring, struct spdk_iscsi_portal **ip)
{
	char *host = NULL, *port = NULL;
	int len, rc = -1;
	const char *p;

	if (portalstring == NULL) {
		SPDK_ERRLOG("portal error\n");
		goto error_out;
	}

	/* IP address */
	if (portalstring[0] == '[') {
		/* IPv6 */
		p = strchr(portalstring + 1, ']');
		if (p == NULL) {
			SPDK_ERRLOG("portal error\n");
			goto error_out;
		}
		p++;
	} else {
		/* IPv4 */
		p = strchr(portalstring, ':');
		if (p == NULL) {
			p = portalstring + strlen(portalstring);
		}
	}

	len = p - portalstring;
	host = malloc(len + 1);
	if (host == NULL) {
		SPDK_ERRLOG("malloc() failed for host\n");
		goto error_out;
	}
	memcpy(host, portalstring, len);
	host[len] = '\0';

	/* Port number (IPv4 and IPv6 are the same) */
	if (p[0] == '\0') {
		port = malloc(PORTNUMSTRLEN);
		if (!port) {
			SPDK_ERRLOG("malloc() failed for port\n");
			goto error_out;
		}
		snprintf(port, PORTNUMSTRLEN, "%d", DEFAULT_PORT);
	} else {
		p++;
		len = strlen(p);
		port = malloc(len + 1);
		if (port == NULL) {
			SPDK_ERRLOG("malloc() failed for port\n");
			goto error_out;
		}
		memcpy(port, p, len);
		port[len] = '\0';
	}

	*ip = iscsi_portal_create(host, port);
	if (!*ip) {
		goto error_out;
	}

	rc = 0;
error_out:
	free(host);
	free(port);

	return rc;
}

struct spdk_iscsi_portal_grp *
iscsi_portal_grp_create(int tag)
{
	struct spdk_iscsi_portal_grp *pg = malloc(sizeof(*pg));

	if (!pg) {
		SPDK_ERRLOG("malloc() failed for portal group\n");
		return NULL;
	}

	pg->ref = 0;
	pg->tag = tag;

	pthread_mutex_lock(&g_iscsi.mutex);
	pg->disable_chap = g_iscsi.disable_chap;
	pg->require_chap = g_iscsi.require_chap;
	pg->mutual_chap = g_iscsi.mutual_chap;
	pg->chap_group = g_iscsi.chap_group;
	pthread_mutex_unlock(&g_iscsi.mutex);

	TAILQ_INIT(&pg->head);

	return pg;
}

void
iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal	*p;

	assert(pg != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "iscsi_portal_grp_destroy\n");
	while (!TAILQ_EMPTY(&pg->head)) {
		p = TAILQ_FIRST(&pg->head);
		TAILQ_REMOVE(&pg->head, p, per_pg_tailq);
		iscsi_portal_destroy(p);
	}
	free(pg);
}

int
iscsi_portal_grp_register(struct spdk_iscsi_portal_grp *pg)
{
	int rc = -1;
	struct spdk_iscsi_portal_grp *tmp;

	assert(pg != NULL);

	pthread_mutex_lock(&g_iscsi.mutex);
	tmp = iscsi_portal_grp_find_by_tag(pg->tag);
	if (tmp == NULL) {
		TAILQ_INSERT_TAIL(&g_iscsi.pg_head, pg, tailq);
		rc = 0;
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return rc;
}

void
iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
			    struct spdk_iscsi_portal *p)
{
	assert(pg != NULL);
	assert(p != NULL);

	p->group = pg;
	TAILQ_INSERT_TAIL(&pg->head, p, per_pg_tailq);
}

int
iscsi_portal_grp_set_chap_params(struct spdk_iscsi_portal_grp *pg,
				 bool disable_chap, bool require_chap,
				 bool mutual_chap, int32_t chap_group)
{
	if (!iscsi_check_chap_params(disable_chap, require_chap,
				     mutual_chap, chap_group)) {
		return -EINVAL;
	}

	pg->disable_chap = disable_chap;
	pg->require_chap = require_chap;
	pg->mutual_chap = mutual_chap;
	pg->chap_group = chap_group;

	return 0;
}

static int
iscsi_parse_portal_grp(struct spdk_conf_section *sp)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_portal *p;
	const char *val;
	char *label, *portal;
	int i = 0, rc = 0;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "add portal group (from config file) %d\n",
		      spdk_conf_section_get_num(sp));

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Comment %s\n", val);
	}

	pg = iscsi_portal_grp_create(spdk_conf_section_get_num(sp));
	if (!pg) {
		SPDK_ERRLOG("portal group malloc error (%s)\n", spdk_conf_section_get_name(sp));
		return -1;
	}

	for (i = 0; ; i++) {
		label = spdk_conf_section_get_nmval(sp, "Portal", i, 0);
		portal = spdk_conf_section_get_nmval(sp, "Portal", i, 1);
		if (label == NULL || portal == NULL) {
			break;
		}

		rc = iscsi_parse_portal(portal, &p);
		if (rc < 0) {
			SPDK_ERRLOG("parse portal error (%s)\n", portal);
			goto error;
		}

		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "RIndex=%d, Host=%s, Port=%s, Tag=%d\n",
			      i, p->host, p->port, spdk_conf_section_get_num(sp));

		iscsi_portal_grp_add_portal(pg, p);
	}

	rc = iscsi_portal_grp_open(pg);
	if (rc != 0) {
		SPDK_ERRLOG("portal_grp_open failed\n");
		goto error;
	}

	/* Add portal group to the end of the pg list */
	rc = iscsi_portal_grp_register(pg);
	if (rc != 0) {
		SPDK_ERRLOG("register portal failed\n");
		goto error;
	}

	return 0;

error:
	iscsi_portal_grp_release(pg);
	return -1;
}

struct spdk_iscsi_portal_grp *
iscsi_portal_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_portal_grp *pg;

	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		if (pg->tag == tag) {
			return pg;
		}
	}

	return NULL;
}

int
iscsi_parse_portal_grps(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "PortalGroup")) {
			if (spdk_conf_section_get_num(sp) == 0) {
				SPDK_ERRLOG("Group 0 is invalid\n");
				return -1;
			}

			/* Build portal group from cfg section PortalGroup */
			rc = iscsi_parse_portal_grp(sp);
			if (rc < 0) {
				SPDK_ERRLOG("parse_portal_group() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

void
iscsi_portal_grps_destroy(void)
{
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "iscsi_portal_grps_destroy\n");
	pthread_mutex_lock(&g_iscsi.mutex);
	while (!TAILQ_EMPTY(&g_iscsi.pg_head)) {
		pg = TAILQ_FIRST(&g_iscsi.pg_head);
		TAILQ_REMOVE(&g_iscsi.pg_head, pg, tailq);
		pthread_mutex_unlock(&g_iscsi.mutex);
		iscsi_portal_grp_destroy(pg);
		pthread_mutex_lock(&g_iscsi.mutex);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

int
iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;
	int rc;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		rc = iscsi_portal_open(p);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static void
iscsi_portal_grp_close(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		iscsi_portal_close(p);
	}
}

void
iscsi_portal_grp_close_all(void)
{
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "iscsi_portal_grp_close_all\n");
	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		iscsi_portal_grp_close(pg);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

struct spdk_iscsi_portal_grp *
iscsi_portal_grp_unregister(int tag)
{
	struct spdk_iscsi_portal_grp *pg;

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		if (pg->tag == tag) {
			TAILQ_REMOVE(&g_iscsi.pg_head, pg, tailq);
			pthread_mutex_unlock(&g_iscsi.mutex);
			return pg;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return NULL;
}

void
iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg)
{
	iscsi_portal_grp_close(pg);
	iscsi_portal_grp_destroy(pg);
}

static const char *portal_group_section = \
		"\n"
		"# Users must change the PortalGroup section(s) to match the IP addresses\n"
		"#  for their environment.\n"
		"# PortalGroup sections define which network portals the iSCSI target\n"
		"# will use to listen for incoming connections.  These are also used to\n"
		"#  determine which targets are accessible over each portal group.\n"
		"# Up to 1024 Portal directives are allowed.  These define the network\n"
		"#  portals of the portal group. The user must specify a IP address\n"
		"#  for each network portal, and may optionally specify a port.\n"
		"# If the port is omitted, 3260 will be used\n"
		"#  Syntax:\n"
		"#    Portal <Name> <IP address>[:<port>]\n";

#define PORTAL_GROUP_TMPL \
"[PortalGroup%d]\n" \
"  Comment \"Portal%d\"\n"

#define PORTAL_TMPL \
"  Portal DA1 %s:%s\n"

void
iscsi_portal_grps_config_text(FILE *fp)
{
	struct spdk_iscsi_portal *p = NULL;
	struct spdk_iscsi_portal_grp *pg = NULL;

	/* Create portal group section */
	fprintf(fp, "%s", portal_group_section);

	/* Dump portal groups */
	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		if (NULL == pg) { continue; }
		fprintf(fp, PORTAL_GROUP_TMPL, pg->tag, pg->tag);
		/* Dump portals */
		TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
			if (NULL == p) { continue; }
			fprintf(fp, PORTAL_TMPL, p->host, p->port);
		}
	}
}

static void
iscsi_portal_grp_info_json(struct spdk_iscsi_portal_grp *pg,
			   struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_portal *portal;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_int32(w, "tag", pg->tag);

	spdk_json_write_named_array_begin(w, "portals");
	TAILQ_FOREACH(portal, &pg->head, per_pg_tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "host", portal->host);
		spdk_json_write_named_string(w, "port", portal->port);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

static void
iscsi_portal_grp_config_json(struct spdk_iscsi_portal_grp *pg,
			     struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "iscsi_create_portal_group");

	spdk_json_write_name(w, "params");
	iscsi_portal_grp_info_json(pg, w);

	spdk_json_write_object_end(w);
}

void
iscsi_portal_grps_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_portal_grp *pg;

	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		iscsi_portal_grp_info_json(pg, w);
	}
}

void
iscsi_portal_grps_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_portal_grp *pg;

	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		iscsi_portal_grp_config_json(pg, w);
	}
}
