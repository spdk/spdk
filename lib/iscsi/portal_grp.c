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

#include "spdk/sock.h"
#include "spdk/string.h"

#include "spdk/log.h"

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

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_destroy\n");

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
		SPDK_DEBUGLOG(iscsi, "close portal (%s, %s)\n",
			      p->host, p->port);
		spdk_poller_unregister(&p->acceptor_poller);
		spdk_sock_close(&p->sock);
	}
}

static void
iscsi_portal_pause(struct spdk_iscsi_portal *p)
{
	assert(p->acceptor_poller != NULL);

	spdk_poller_pause(p->acceptor_poller);
}

static void
iscsi_portal_resume(struct spdk_iscsi_portal *p)
{
	assert(p->acceptor_poller != NULL);

	spdk_poller_resume(p->acceptor_poller);
}

int
iscsi_parse_redirect_addr(struct sockaddr_storage *sa,
			  const char *host, const char *port)
{
	struct addrinfo hints, *res;
	int rc;

	if (host == NULL || port == NULL) {
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = getaddrinfo(host, port, &hints, &res);
	if (rc != 0) {
		SPDK_ERRLOG("getaddinrfo failed: %s (%d)\n", gai_strerror(rc), rc);
		return -EINVAL;
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n",
			    (size_t)res->ai_addrlen);
		rc = -EINVAL;
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	return rc;
}

struct spdk_iscsi_portal_grp *
iscsi_portal_grp_create(int tag, bool is_private)
{
	struct spdk_iscsi_portal_grp *pg = malloc(sizeof(*pg));

	if (!pg) {
		SPDK_ERRLOG("malloc() failed for portal group\n");
		return NULL;
	}

	pg->ref = 0;
	pg->tag = tag;
	pg->is_private = is_private;

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

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_grp_destroy\n");
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

struct spdk_iscsi_portal *
iscsi_portal_grp_find_portal_by_addr(struct spdk_iscsi_portal_grp *pg,
				     const char *host, const char *port)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		if (!strcmp(p->host, host) && !strcmp(p->port, port)) {
			return p;
		}
	}

	return NULL;
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

void
iscsi_portal_grps_destroy(void)
{
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_grps_destroy\n");
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
iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg, bool pause)
{
	struct spdk_iscsi_portal *p;
	int rc;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		rc = iscsi_portal_open(p);
		if (rc < 0) {
			return rc;
		}

		if (pause) {
			iscsi_portal_pause(p);
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
iscsi_portal_grp_resume(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		iscsi_portal_resume(p);
	}
}

void
iscsi_portal_grp_close_all(void)
{
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_grp_close_all\n");
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

	spdk_json_write_named_bool(w, "private", pg->is_private);

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
