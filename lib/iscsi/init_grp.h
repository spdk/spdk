/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INIT_GRP_H
#define SPDK_INIT_GRP_H

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"

struct spdk_iscsi_initiator_name {
	char name[MAX_INITIATOR_NAME + 1];
	TAILQ_ENTRY(spdk_iscsi_initiator_name) tailq;
};

struct spdk_iscsi_initiator_netmask {
	char mask[MAX_INITIATOR_ADDR + 1];
	TAILQ_ENTRY(spdk_iscsi_initiator_netmask) tailq;
};

struct spdk_iscsi_init_grp {
	int ninitiators;
	TAILQ_HEAD(, spdk_iscsi_initiator_name) initiator_head;
	int nnetmasks;
	TAILQ_HEAD(, spdk_iscsi_initiator_netmask) netmask_head;
	int ref;
	int tag;
	TAILQ_ENTRY(spdk_iscsi_init_grp)	tailq;
};

/* SPDK iSCSI Initiator Group management API */
int iscsi_init_grp_create_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);
int iscsi_init_grp_add_initiators_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);
int iscsi_init_grp_delete_initiators_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);
int iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig);
struct spdk_iscsi_init_grp *iscsi_init_grp_unregister(int tag);
struct spdk_iscsi_init_grp *iscsi_init_grp_find_by_tag(int tag);
void iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig);
int iscsi_parse_init_grps(void);
void iscsi_init_grps_destroy(void);
void iscsi_init_grps_info_json(struct spdk_json_write_ctx *w);
void iscsi_init_grps_config_json(struct spdk_json_write_ctx *w);
#endif /* SPDK_INIT_GRP_H */
