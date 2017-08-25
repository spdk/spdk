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
#include "iscsi/portal_grp.h"

#define PORTNUMSTRLEN 32

static int
spdk_iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg);

/* Assumes caller allocated host and port strings on the heap */
struct spdk_iscsi_portal *
spdk_iscsi_portal_create(const char *host, const char *port, uint64_t cpumask)
{
	struct spdk_iscsi_portal *p = NULL;

	assert(host != NULL);
	assert(port != NULL);

	p = malloc(sizeof(*p));
	if (!p) {
		SPDK_ERRLOG("portal malloc error (%s, %s)\n", host, port);
		return NULL;
	}
	p->host = strdup(host);
	p->port = strdup(port);
	p->cpumask = cpumask;
	p->sock = -1;
	p->group = NULL; /* set at a later time by caller */

	return p;
}

void
spdk_iscsi_portal_destroy(struct spdk_iscsi_portal *p)
{
	assert(p != NULL);

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_portal_destroy\n");
	free(p->host);
	free(p->port);
	free(p);
}

static int
spdk_iscsi_portal_create_from_configline(const char *portalstring,
		struct spdk_iscsi_portal **ip,
		int dry_run)
{
	char *host = NULL, *port = NULL;
	const char *cpumask_str;
	uint64_t cpumask = 0;

	int n, len, rc = -1;
	const char *p, *q;

	if (portalstring == NULL) {
		SPDK_ERRLOG("portal error\n");
		goto error_out;
	}

	if (portalstring[0] == '[') {
		/* IPv6 */
		p = strchr(portalstring + 1, ']');
		if (p == NULL) {
			SPDK_ERRLOG("portal error\n");
			goto error_out;
		}
		p++;
		n = p - portalstring;
		if (!dry_run) {
			host = malloc(n + 1);
			if (!host) {
				perror("host");
				goto error_out;
			}
			memcpy(host, portalstring, n);
			host[n] = '\0';
		}
		if (p[0] == '\0') {
			if (!dry_run) {
				port = malloc(PORTNUMSTRLEN);
				if (!port) {
					perror("port");
					goto error_out;
				}
				snprintf(port, PORTNUMSTRLEN, "%d", DEFAULT_PORT);
			}
		} else {
			if (p[0] != ':') {
				SPDK_ERRLOG("portal error\n");
				goto error_out;
			}
			if (!dry_run) {
				q = strchr(portalstring, '@');
				if (q == NULL) {
					q = portalstring + strlen(portalstring);
				}
				len = q - p - 1;

				port = malloc(len + 1);
				if (!port) {
					perror("port");
					goto error_out;
				}
				memset(port, 0, len + 1);
				memcpy(port, p + 1, len);
			}
		}
	} else {
		/* IPv4 */
		p = strchr(portalstring, ':');
		if (p == NULL) {
			p = portalstring + strlen(portalstring);
		}
		n = p - portalstring;
		if (!dry_run) {
			host = malloc(n + 1);
			if (!host) {
				perror("host");
				goto error_out;
			}
			memcpy(host, portalstring, n);
			host[n] = '\0';
		}
		if (p[0] == '\0') {
			if (!dry_run) {
				port = malloc(PORTNUMSTRLEN);
				if (!port) {
					perror("port");
					goto error_out;
				}
				snprintf(port, PORTNUMSTRLEN, "%d", DEFAULT_PORT);
			}
		} else {
			if (p[0] != ':') {
				SPDK_ERRLOG("portal error\n");
				goto error_out;
			}
			if (!dry_run) {
				q = strchr(portalstring, '@');
				if (q == NULL) {
					q = portalstring + strlen(portalstring);
				}

				if (q == p) {
					SPDK_ERRLOG("no port specified\n");
					goto error_out;
				}

				len = q - p - 1;
				port = malloc(len + 1);
				if (!port) {
					perror("port");
					goto error_out;
				}
				memset(port, 0, len + 1);
				memcpy(port, p + 1, len);
			}

		}
	}

	p = strchr(portalstring, '@');
	if (p != NULL) {
		cpumask_str = p + 1;
		if (spdk_app_parse_core_mask(cpumask_str, &cpumask)) {
			SPDK_ERRLOG("invalid portal cpumask %s\n", cpumask_str);
			goto error_out;
		}
		if ((cpumask & spdk_app_get_core_mask()) != cpumask) {
			SPDK_ERRLOG("portal cpumask %s not a subset of "
				    "reactor mask %jx\n", cpumask_str,
				    spdk_app_get_core_mask());
			goto error_out;
		}
	} else {
		cpumask = spdk_app_get_core_mask();
	}

	if (!dry_run) {
		*ip = spdk_iscsi_portal_create(host, port, cpumask);
		if (!*ip) {
			goto error_out;
		}
	}

	rc = 0;
error_out:
	free(host);
	free(port);

	return rc;
}

struct spdk_iscsi_portal_grp *
spdk_iscsi_portal_grp_create(int tag)
{
	struct spdk_iscsi_portal_grp *pg = malloc(sizeof(*pg));

	if (!pg) {
		SPDK_ERRLOG("portal group malloc error (%d)\n", tag);
		return NULL;
	}

	/* Make sure there are no duplicate portal group tags */
	if (spdk_iscsi_portal_grp_find_by_tag(tag)) {
		SPDK_ERRLOG("portal group creation failed.  duplicate portal group tag (%d)\n", tag);
		free(pg);
		return NULL;
	}

	pg->state = GROUP_INIT;
	pg->ref = 0;
	pg->tag = tag;

	TAILQ_INIT(&pg->head);

	return pg;
}

void
spdk_iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal	*p;

	assert(pg != NULL);

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_portal_grp_destroy\n");
	while (!TAILQ_EMPTY(&pg->head)) {
		p = TAILQ_FIRST(&pg->head);
		TAILQ_REMOVE(&pg->head, p, tailq);
		spdk_iscsi_portal_destroy(p);
	}
	free(pg);
}

static void
spdk_iscsi_portal_grp_register(struct spdk_iscsi_portal_grp *pg)
{
	assert(pg != NULL);
	assert(!TAILQ_EMPTY(&pg->head));

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	pg->state = GROUP_READY;
	TAILQ_INSERT_TAIL(&g_spdk_iscsi.pg_head, pg, tailq);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

/**
 * If all portals are valid, this function will take their ownership.
 */
int
spdk_iscsi_portal_grp_create_from_portal_list(int tag,
		struct spdk_iscsi_portal **portal_list,
		int num_portals)
{
	int i = 0, rc = 0, port;
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "add portal group (from portal list) %d\n", tag);

	if (num_portals > MAX_PORTAL) {
		SPDK_ERRLOG("%d > MAX_PORTAL\n", num_portals);
		return -1;
	}

	pg = spdk_iscsi_portal_grp_create(tag);
	if (!pg) {
		SPDK_ERRLOG("portal group creation error (%d)\n", tag);
		return -1;
	}

	for (i = 0; i < num_portals; i++) {
		struct spdk_iscsi_portal *p = portal_list[i];

		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "RIndex=%d, Host=%s, Port=%s, Tag=%d\n",
			      i, p->host, p->port, tag);

		port = (int)strtol(p->port, NULL, 0);
		p->sock = spdk_sock_listen(p->host, port);

		if (p->sock < 0) {
			/* if listening failed on any port, do not register the portal group
			 * and close any previously opened. */
			SPDK_ERRLOG("listen error %.64s:%d\n", p->host, port);
			rc = -1;

			for (--i; i >= 0; --i) {
				spdk_sock_close(portal_list[i]->sock);
				portal_list[i]->sock = -1;
			}

			break;
		}
	}

	if (rc < 0) {
		spdk_iscsi_portal_grp_destroy(pg);
	} else {
		/* Add portals to portal group */
		for (i = 0; i < num_portals; i++) {
			spdk_iscsi_portal_grp_add_portal(pg, portal_list[i]);
		}

		/* Add portal group to the end of the pg list */
		spdk_iscsi_portal_grp_register(pg);
	}

	return rc;
}

int
spdk_iscsi_portal_grp_create_from_configfile(struct spdk_conf_section *sp)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_portal	*p;
	const char *val;
	char *label, *portal;
	int portals = 0, i = 0, rc = 0;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "add portal group (from config file) %d\n",
		      spdk_conf_section_get_num(sp));

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "Comment %s\n", val);
	}

	/* counts number of definitions */
	for (i = 0; ; i++) {
		/*
		 * label is no longer used, but we keep it in the config
		 *  file definition so that we do not break existing config
		 *  files.
		 */
		label = spdk_conf_section_get_nmval(sp, "Portal", i, 0);
		portal = spdk_conf_section_get_nmval(sp, "Portal", i, 1);
		if (label == NULL || portal == NULL)
			break;
		rc = spdk_iscsi_portal_create_from_configline(portal, &p, 1);
		if (rc < 0) {
			SPDK_ERRLOG("parse portal error (%s)\n", portal);
			goto error_out;
		}
	}

	portals = i;
	if (portals > MAX_PORTAL) {
		SPDK_ERRLOG("%d > MAX_PORTAL\n", portals);
		goto error_out;
	}

	pg = spdk_iscsi_portal_grp_create(spdk_conf_section_get_num(sp));
	if (!pg) {
		SPDK_ERRLOG("portal group malloc error (%s)\n", spdk_conf_section_get_name(sp));
		goto error_out;
	}

	for (i = 0; i < portals; i++) {
		label = spdk_conf_section_get_nmval(sp, "Portal", i, 0);
		portal = spdk_conf_section_get_nmval(sp, "Portal", i, 1);
		if (label == NULL || portal == NULL) {
			spdk_iscsi_portal_grp_destroy(pg);
			SPDK_ERRLOG("portal error\n");
			goto error_out;
		}

		rc = spdk_iscsi_portal_create_from_configline(portal, &p, 0);
		if (rc < 0) {
			spdk_iscsi_portal_grp_destroy(pg);
			SPDK_ERRLOG("parse portal error (%s)\n", portal);
			goto error_out;
		}

		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
			      "RIndex=%d, Host=%s, Port=%s, Tag=%d\n",
			      i, p->host, p->port, spdk_conf_section_get_num(sp));

		spdk_iscsi_portal_grp_add_portal(pg, p);
	}

	/* Add portal group to the end of the pg list */
	spdk_iscsi_portal_grp_register(pg);

	return 0;

error_out:
	return -1;
}

void
spdk_iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
				 struct spdk_iscsi_portal *p)
{
	assert(pg != NULL);
	assert(p != NULL);

	p->group = pg;
	TAILQ_INSERT_TAIL(&pg->head, p, tailq);
}

struct spdk_iscsi_portal_grp *
spdk_iscsi_portal_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_portal_grp *pg;

	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		if (pg->tag == tag) {
			return pg;
		}
	}

	return NULL;
}

int
spdk_iscsi_portal_grp_array_create(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	TAILQ_INIT(&g_spdk_iscsi.pg_head);
	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "PortalGroup")) {
			if (spdk_conf_section_get_num(sp) == 0) {
				SPDK_ERRLOG("Group 0 is invalid\n");
				return -1;
			}

			/* Build portal group from cfg section PortalGroup */
			rc = spdk_iscsi_portal_grp_create_from_configfile(sp);
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
spdk_iscsi_portal_grp_array_destroy(void)
{
	struct spdk_iscsi_portal_grp *pg, *tmp;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_portal_grp_array_destroy\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(pg, &g_spdk_iscsi.pg_head, tailq, tmp) {
		pg->state = GROUP_DESTROY;
		TAILQ_REMOVE(&g_spdk_iscsi.pg_head, pg, tailq);
		spdk_iscsi_portal_grp_destroy(pg);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

static int
spdk_iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;
	int port;
	int sock;

	TAILQ_FOREACH(p, &pg->head, tailq) {
		if (p->sock < 0) {
			SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "open host %s, port %s, tag %d\n",
				      p->host, p->port, pg->tag);
			port = (int)strtol(p->port, NULL, 0);
			sock = spdk_sock_listen(p->host, port);
			if (sock < 0) {
				SPDK_ERRLOG("listen error %.64s:%d\n", p->host, port);
				return -1;
			}
			p->sock = sock;
		}
	}
	return 0;
}

int
spdk_iscsi_portal_grp_open_all(void)
{
	struct spdk_iscsi_portal_grp *pg;
	int rc;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_portal_grp_open_all\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		rc = spdk_iscsi_portal_grp_open(pg);
		if (rc < 0) {
			pthread_mutex_unlock(&g_spdk_iscsi.mutex);
			return -1;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;
}

static int
spdk_iscsi_portal_grp_close(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &pg->head, tailq) {
		if (p->sock >= 0) {
			SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "close host %s, port %s, tag %d\n",
				      p->host, p->port, pg->tag);
			close(p->sock);
			p->sock = -1;
		}
	}
	return 0;
}

int
spdk_iscsi_portal_grp_close_all(void)
{
	struct spdk_iscsi_portal_grp *pg;
	int rc;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_portal_grp_close_all\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		rc = spdk_iscsi_portal_grp_close(pg);
		if (rc < 0) {
			pthread_mutex_unlock(&g_spdk_iscsi.mutex);
			return -1;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;
}

static inline void
spdk_iscsi_portal_grp_unregister(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal_grp *portal_group;
	struct spdk_iscsi_portal_grp *portal_group_tmp;

	assert(pg != NULL);
	assert(!TAILQ_EMPTY(&pg->head));

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(portal_group, &g_spdk_iscsi.pg_head, tailq, portal_group_tmp) {
		if (portal_group->tag == pg->tag)
			TAILQ_REMOVE(&g_spdk_iscsi.pg_head, portal_group, tailq);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

int
spdk_iscsi_portal_grp_deletable(int tag)
{
	int ret = 0;
	struct spdk_iscsi_portal_grp *pg;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	pg = spdk_iscsi_portal_grp_find_by_tag(tag);
	if (pg == NULL) {
		ret = -1;
		goto out;
	}

	if (pg->state != GROUP_READY) {
		ret = -1;
		goto out;
	}

	if (pg->ref == 0) {
		ret = 0;
		goto out;
	}

out:
	if (ret == 0)
		pg->state = GROUP_DESTROY;
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return ret;
}

void
spdk_iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg)
{
	spdk_iscsi_portal_grp_close(pg);
	spdk_iscsi_portal_grp_unregister(pg);
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	spdk_iscsi_portal_grp_destroy(pg);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}
