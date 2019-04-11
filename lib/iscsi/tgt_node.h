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
struct spdk_iscsi_init_grp;
struct spdk_iscsi_portal_grp;
struct spdk_iscsi_portal;
struct spdk_json_write_ctx;

#define MAX_TARGET_MAP			256
#define SPDK_TN_TAG_MAX			0x0000ffff

typedef void (*iscsi_tgt_node_destruct_cb)(void *cb_arg, int rc);

struct spdk_iscsi_ig_map {
	struct spdk_iscsi_init_grp *ig;
	TAILQ_ENTRY(spdk_iscsi_ig_map) tailq;
};

struct spdk_iscsi_pg_map {
	struct spdk_iscsi_portal_grp *pg;
	int num_ig_maps;
	TAILQ_HEAD(, spdk_iscsi_ig_map) ig_map_head;
	TAILQ_ENTRY(spdk_iscsi_pg_map) tailq ;
};

struct spdk_iscsi_tgt_node {
	int num;
	char *name;
	char *alias;

	pthread_mutex_t mutex;

	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int chap_group;
	bool header_digest;
	bool data_digest;
	int queue_depth;

	struct spdk_scsi_dev *dev;
	/**
	 * Counts number of active iSCSI connections associated with this
	 *  target node.
	 */
	uint32_t num_active_conns;
	int lcore;

	int num_pg_maps;
	TAILQ_HEAD(, spdk_iscsi_pg_map) pg_map_head;
	TAILQ_ENTRY(spdk_iscsi_tgt_node) tailq;

	bool destructed;
	struct spdk_poller *destruct_poller;
	iscsi_tgt_node_destruct_cb destruct_cb_fn;
	void *destruct_cb_arg;
};

int spdk_iscsi_parse_tgt_nodes(void);

void spdk_iscsi_shutdown_tgt_nodes(void);
void spdk_iscsi_shutdown_tgt_node_by_name(const char *target_name,
		iscsi_tgt_node_destruct_cb cb_fn, void *cb_arg);
int spdk_iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
			 const char *iaddr, const char *tiqn, uint8_t *data, int alloc_len,
			 int data_len);

/* This typedef exists to work around an astyle 2.05 bug.
 * Remove it when astyle is fixed.
 */
typedef struct spdk_iscsi_tgt_node _spdk_iscsi_tgt_node;

/*
 * bdev_name_list and lun_id_list are equal sized arrays of size num_luns.
 * bdev_name_list refers to the names of the bdevs that will be used for the LUNs on the
 *  new target node.
 * lun_id_list refers to the LUN IDs that will be used for the LUNs on the target node.
 */
_spdk_iscsi_tgt_node *
spdk_iscsi_tgt_node_construct(int target_index,
			      const char *name, const char *alias,
			      int *pg_tag_list, int *ig_tag_list, uint16_t num_maps,
			      const char *bdev_name_list[], int *lun_id_list, int num_luns,
			      int queue_depth,
			      bool disable_chap, bool require_chap, bool mutual_chap, int chap_group,
			      bool header_digest, bool data_digest);

bool spdk_iscsi_check_chap_params(bool disable, bool require, bool mutual, int group);

int spdk_iscsi_tgt_node_add_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
				       int *pg_tag_list, int *ig_tag_list,
				       uint16_t num_maps);
int spdk_iscsi_tgt_node_delete_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
		int *pg_tag_list, int *ig_tag_list,
		uint16_t num_maps);

bool spdk_iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
				struct spdk_iscsi_tgt_node *target, const char *iqn,
				const char *addr);
struct spdk_iscsi_tgt_node *spdk_iscsi_find_tgt_node(const char *target_name);
int spdk_iscsi_tgt_node_reset(struct spdk_iscsi_tgt_node *target,
			      uint64_t lun);
int spdk_iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
				     struct spdk_iscsi_tgt_node *target);
void spdk_iscsi_tgt_node_delete_map(struct spdk_iscsi_portal_grp *portal_group,
				    struct spdk_iscsi_init_grp *initiator_group);
int spdk_iscsi_tgt_node_add_lun(struct spdk_iscsi_tgt_node *target,
				const char *bdev_name, int lun_id);
int spdk_iscsi_tgt_node_set_chap_params(struct spdk_iscsi_tgt_node *target,
					bool disable_chap, bool require_chap,
					bool mutual_chap, int32_t chap_group);
void spdk_iscsi_tgt_nodes_config_text(FILE *fp);
void spdk_iscsi_tgt_nodes_info_json(struct spdk_json_write_ctx *w);
void spdk_iscsi_tgt_nodes_config_json(struct spdk_json_write_ctx *w);
#endif /* SPDK_ISCSI_TGT_NODE_H_ */
