/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_PORTAL_GRP_H
#define SPDK_PORTAL_GRP_H

#include "spdk/conf.h"
#include "spdk/cpuset.h"
#include "iscsi/iscsi.h"

struct spdk_json_write_ctx;

struct spdk_iscsi_portal {
	struct spdk_iscsi_portal_grp	*group;
	char				host[MAX_PORTAL_ADDR + 1];
	char				port[MAX_PORTAL_PORT + 1];
	struct spdk_sock		*sock;
	struct spdk_poller		*acceptor_poller;
	TAILQ_ENTRY(spdk_iscsi_portal)	per_pg_tailq;
	TAILQ_ENTRY(spdk_iscsi_portal)	g_tailq;
};

struct spdk_iscsi_portal_grp {
	int					ref;
	int					tag;

	/* For login redirection, there are two types of portal groups, public and
	 * private portal groups. Public portal groups have their portals returned
	 * by a discovery session. Private portal groups do not have their portals
	 * returned by a discovery session. A public portal group may optionally
	 * specify a redirect portal for non-discovery logins. This redirect portal
	 * must be from a private portal group.
	 */
	bool					is_private;

	bool					disable_chap;
	bool					require_chap;
	bool					mutual_chap;
	int32_t					chap_group;
	TAILQ_ENTRY(spdk_iscsi_portal_grp)	tailq;
	TAILQ_HEAD(, spdk_iscsi_portal)		head;
};

/* SPDK iSCSI Portal Group management API */

struct spdk_iscsi_portal *iscsi_portal_create(const char *host, const char *port);
void iscsi_portal_destroy(struct spdk_iscsi_portal *p);

struct spdk_iscsi_portal_grp *iscsi_portal_grp_create(int tag, bool is_private);
void iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
				 struct spdk_iscsi_portal *p);
struct spdk_iscsi_portal *iscsi_portal_grp_find_portal_by_addr(
	struct spdk_iscsi_portal_grp *pg, const char *host, const char *port);

void iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg);
void iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg);
int iscsi_parse_portal_grps(void);
void iscsi_portal_grps_destroy(void);
int iscsi_portal_grp_register(struct spdk_iscsi_portal_grp *pg);
struct spdk_iscsi_portal_grp *iscsi_portal_grp_unregister(int tag);
struct spdk_iscsi_portal_grp *iscsi_portal_grp_find_by_tag(int tag);
int iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg, bool pause);
void iscsi_portal_grp_resume(struct spdk_iscsi_portal_grp *pg);
int iscsi_portal_grp_set_chap_params(struct spdk_iscsi_portal_grp *pg,
				     bool disable_chap, bool require_chap,
				     bool mutual_chap, int32_t chap_group);

void iscsi_portal_grp_close_all(void);
void iscsi_portal_grps_info_json(struct spdk_json_write_ctx *w);
void iscsi_portal_grps_config_json(struct spdk_json_write_ctx *w);

int iscsi_parse_redirect_addr(struct sockaddr_storage *sa,
			      const char *host, const char *port);

#endif /* SPDK_PORTAL_GRP_H */
