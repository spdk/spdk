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

#ifndef SPDK_INIT_GRP_H
#define SPDK_INIT_GRP_H

#include "spdk/conf.h"

enum group_state {
	GROUP_INIT = 0x0,
	GROUP_READY = 0x1,
	GROUP_DESTROY = 0x2,
};

struct spdk_iscsi_init_grp {
	int ninitiators;
	char **initiators;
	int nnetmasks;
	char **netmasks;
	int ref;
	int tag;
	enum group_state state;
	TAILQ_ENTRY(spdk_iscsi_init_grp)	tailq;
};

/* SPDK iSCSI Initiator Group management API */
int spdk_iscsi_init_grp_create_from_configfile(struct spdk_conf_section *sp);

int spdk_iscsi_init_grp_create_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);

void spdk_iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig);
void spdk_iscsi_init_grp_destroy_by_tag(int tag);
void spdk_iscsi_init_grp_release(struct spdk_iscsi_init_grp *ig);

struct spdk_iscsi_init_grp *spdk_iscsi_init_grp_find_by_tag(int tag);

void spdk_iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig);

int spdk_iscsi_init_grp_array_create(void);
void spdk_iscsi_init_grp_array_destroy(void);
int spdk_iscsi_init_grp_deletable(int tag);

#endif // SPDK_INIT_GRP_H
