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

#ifndef SPDK_ISCSI_TGT_NODE_H_
#define SPDK_ISCSI_TGT_NODE_H_

#include "spdk/stdinc.h"

#include "spdk/scsi.h"

struct spdk_iscsi_conn;

#define SPDK_ISCSI_MAX_QUEUE_DEPTH	64
#define MAX_TARGET_MAP			256
#define SPDK_TN_TAG_MAX 0x0000ffff

struct spdk_iscsi_tgt_node_map {
	struct spdk_iscsi_portal_grp	*pg;
	struct spdk_iscsi_init_grp	*ig;
};

struct spdk_iscsi_tgt_node {
	int num;
	char *name;
	char *alias;

	pthread_mutex_t mutex;

	int auth_chap_disabled;
	int auth_chap_required;
	int auth_chap_mutual;
	int auth_group;
	int header_digest;
	int data_digest;
	int queue_depth;

	struct spdk_scsi_dev *dev;
	/**
	 * Counts number of active iSCSI connections associated with this
	 *  target node.
	 */
	uint32_t num_active_conns;
	int lcore;

	int maxmap;
	struct spdk_iscsi_tgt_node_map map[MAX_TARGET_MAP];
};

int spdk_iscsi_init_tgt_nodes(void);

int spdk_iscsi_shutdown_tgt_nodes(void);
int spdk_iscsi_shutdown_tgt_node_by_name(const char *target_name);
int spdk_iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
			 const char *iaddr, const char *tiqn, uint8_t *data, int alloc_len,
			 int data_len);

struct spdk_iscsi_init_grp *
spdk_iscsi_find_init_grp(int tag);

/* This typedef exists to work around an astyle 2.05 bug.
 * Remove it when astyle is fixed.
 */
typedef struct spdk_iscsi_tgt_node _spdk_iscsi_tgt_node;

_spdk_iscsi_tgt_node *
spdk_iscsi_tgt_node_construct(int target_index,
			      const char *name, const char *alias,
			      int *pg_tag_list, int *ig_tag_list, uint16_t num_maps,
			      char *lun_name_list[], int *lun_id_list, int num_luns,
			      int queue_depth,
			      int no_auth_chap, int auth_chap, int auth_chap_mutual, int auth_group,
			      int header_digest, int data_digest);

int spdk_iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_tgt_node *target, const char *iqn,
			       const char *addr);
struct spdk_iscsi_tgt_node *spdk_iscsi_find_tgt_node(const char *target_name);
int spdk_iscsi_tgt_node_reset(struct spdk_iscsi_tgt_node *target,
			      uint64_t lun);
int spdk_iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
				     struct spdk_iscsi_tgt_node *target);
void spdk_iscsi_tgt_node_delete_map(struct spdk_iscsi_portal_grp *portal_group,
				    struct spdk_iscsi_init_grp *initiator_group);
#endif /* SPDK_ISCSI_TGT_NODE_H_ */
