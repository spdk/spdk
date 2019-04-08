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

#include "spdk/conf.h"
#include "spdk/cpuset.h"

//struct spdk_json_write_ctx;

struct spdk_memcached_tgt_node;

struct spdk_memcached_portal {
	struct spdk_memcached_portal_grp	*group;
	char				*host;
	char				*port;
	struct spdk_sock		*sock;
	struct spdk_cpuset		*cpumask;
	struct spdk_poller		*acceptor_poller;
	TAILQ_ENTRY(spdk_memcached_portal)	per_pg_tailq;
	TAILQ_ENTRY(spdk_memcached_portal)	g_tailq;
};

struct spdk_memcached_portal_grp {
	int ref;
	int tag;
	TAILQ_ENTRY(spdk_memcached_portal_grp)	tailq;
	TAILQ_HEAD(, spdk_memcached_portal)		head;

	struct spdk_memcached_tgt_node *target;
};

// calling spdk_memcached_acceptor_start(p) to wait for connection construct; calling spdk_memcached_conn_handle_nop and spdk_iscsi_poll_group_poll
/* Create portal group based on CONF file
 *
 */
int spdk_memcached_parse_portal_grps(void);


/* Portal GRP API with Memcached target*/
bool spdk_memcached_portal_grp_is_target_set(struct spdk_memcached_portal_grp *pg);
void spdk_memcached_portal_grp_clear_target(struct spdk_memcached_portal_grp *pg);
int spdk_memcached_portal_grp_set_target(struct spdk_memcached_portal_grp *pg,
		struct spdk_memcached_tgt_node *target);
struct spdk_memcached_tgt_node *spdk_memcached_portal_grp_get_target(struct
		spdk_memcached_portal_grp *pg);


/* SPDK memcached Portal Group management API */
struct spdk_memcached_portal *spdk_memcached_portal_create(const char *host, const char *port,
		const char *cpumask);
void spdk_memcached_portal_destroy(struct spdk_memcached_portal *p);

int spdk_memcached_parse_portal_grps(
	void); // calling spdk_memcached_acceptor_start(p) to wait for connection construct; calling spdk_memcached_conn_handle_nop and spdk_iscsi_poll_group_poll
void spdk_memcached_portal_grps_destroy(
	void); //called by subsystem triggered by conn's shutdown timer
struct spdk_memcached_portal_grp *spdk_memcached_portal_grp_find_by_tag(int tag);
struct spdk_memcached_portal_grp *spdk_memcached_portal_grp_create(int tag);
void spdk_memcached_portal_grp_add_portal(struct spdk_memcached_portal_grp *pg,
		struct spdk_memcached_portal *p);
void spdk_memcached_portal_grp_destroy(struct spdk_memcached_portal_grp *pg);
void spdk_memcached_portal_grp_release(struct spdk_memcached_portal_grp *pg);
int spdk_memcached_portal_grp_register(struct spdk_memcached_portal_grp *pg);
struct spdk_memcached_portal_grp *spdk_memcached_portal_grp_unregister(int tag);

int spdk_memcached_portal_grp_open(struct spdk_memcached_portal_grp *pg);

void spdk_memcached_portal_grp_close_all(
	void); // called by spdk_memcached_fini when spdk app going to close; calling spdk_memcached_acceptor_stop
//void spdk_memcached_portal_grps_config_text(FILE *fp);
//void spdk_memcached_portal_grps_info_json(struct spdk_json_write_ctx *w);
//void spdk_memcached_portal_grps_config_json(struct spdk_json_write_ctx *w);
#endif /* SPDK_PORTAL_GRP_H */
