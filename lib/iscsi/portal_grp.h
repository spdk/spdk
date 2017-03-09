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

#ifndef SPDK_PORTAL_GRP_H
#define SPDK_PORTAL_GRP_H

#include "iscsi/init_grp.h"

struct spdk_iscsi_portal {
	struct spdk_iscsi_portal_grp	*group;
	char				*host;
	char				*port;
	int				sock;
	uint64_t			cpumask;
	TAILQ_ENTRY(spdk_iscsi_portal)	tailq;
};

struct spdk_iscsi_portal_grp {
	int ref;
	int tag;
	enum group_state state;
	TAILQ_ENTRY(spdk_iscsi_portal_grp)	tailq;
	TAILQ_HEAD(, spdk_iscsi_portal)		head;
};

/* SPDK iSCSI Portal Group management API */

struct spdk_iscsi_portal *spdk_iscsi_portal_create(const char *host, const char *port,
		uint64_t cpumask);
void spdk_iscsi_portal_destroy(struct spdk_iscsi_portal *p);

struct spdk_iscsi_portal_grp *spdk_iscsi_portal_grp_create(int tag);
int spdk_iscsi_portal_grp_create_from_configfile(struct spdk_conf_section *sp);
int spdk_iscsi_portal_grp_create_from_portal_list(int tag,
		struct spdk_iscsi_portal **portal_list,
		int num_portals);
void spdk_iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg);
void spdk_iscsi_portal_grp_destroy_by_tag(int tag);
void spdk_iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg);

void spdk_iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
				      struct spdk_iscsi_portal *p);
void spdk_iscsi_portal_grp_delete_portal(struct spdk_iscsi_portal_grp *pg,
		struct spdk_iscsi_portal *p);


int spdk_iscsi_portal_grp_array_create(void);
void spdk_iscsi_portal_grp_array_destroy(void);

struct spdk_iscsi_portal_grp *spdk_iscsi_portal_grp_find_by_tag(int tag);

int spdk_iscsi_portal_grp_open_all(void);
int spdk_iscsi_portal_grp_close_all(void);

int spdk_iscsi_portal_grp_deletable(int tag);
#endif // SPDK_PORTAL_GRP_H
