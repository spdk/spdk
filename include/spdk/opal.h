/*-
 *   BSD LICENSE
 *
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

#ifndef SPDK_OPAL_H
#define SPDK_OPAL_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk/string.h"

#define SPDK_OPAL_NOT_SUPPORTED 0xFF

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * 5.1.5 Method Status Codes
 */
#define SPDK_OPAL_FAILED 0x3F

static const char *const spdk_opal_errors[] = {
	"SUCCESS",
	"NOT AUTHORIZED",
	"OBSOLETE/UNKNOWN ERROR",
	"SP BUSY",
	"SP FAILED",
	"SP DISABLED",
	"SP FROZEN",
	"NO SESSIONS AVAILABLE",
	"UNIQUENESS CONFLICT",
	"INSUFFICIENT SPACE",
	"INSUFFICIENT ROWS",
	"UNKNOWN ERROR",
	"INVALID PARAMETER",
	"OBSOLETE/UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"TPER MALFUNCTION",
	"TRANSACTION FAILURE",
	"RESPONSE OVERFLOW",
	"AUTHORITY LOCKED OUT",
};

enum spdk_opal_cmd {
	OPAL_CMD_SAVE,
	OPAL_CMD_LOCK_UNLOCK,
	OPAL_CMD_TAKE_OWNERSHIP,
	OPAL_CMD_ACTIVATE_LSP,	/* locking sp */
	OPAL_CMD_SET_NEW_PASSWD,
	OPAL_CMD_ACTIVATE_USER,
	OPAL_CMD_REVERT_TPER,
	OPAL_CMD_SETUP_LOCKING_RANGE,
	OPAL_CMD_ADD_USER_TO_LOCKING_RANGE,
	OPAL_CMD_ENABLE_DISABLE_SHADOW_MBR,
	OPAL_CMD_ERASE_LOCKING_RANGE,
	OPAL_CMD_SECURE_ERASE_LOCKING_RANGE,
	OPAL_CMD_INITIAL_SETUP,
};

struct spdk_opal_info {
	uint8_t tper : 1;
	uint8_t locking : 1;
	uint8_t geometry : 1;
	uint8_t single_user_mode : 1;
	uint8_t datastore : 1;
	uint8_t opal_v200 : 1;
	uint8_t opal_v100 : 1;
	uint8_t vendor_specific : 1;
	uint8_t opal_ssc_dev : 1;
	uint8_t tper_acknack : 1;
	uint8_t tper_async : 1;
	uint8_t tper_buffer_mgt : 1;
	uint8_t tper_comid_mgt : 1;
	uint8_t tper_streaming : 1;
	uint8_t tper_sync : 1;
	uint8_t locking_locked : 1;
	uint8_t locking_locking_enabled : 1;
	uint8_t locking_locking_supported : 1;
	uint8_t locking_mbr_done : 1;
	uint8_t locking_mbr_enabled : 1;
	uint8_t locking_media_encrypt : 1;
	uint8_t geometry_align : 1;
	uint64_t geometry_alignment_granularity;
	uint32_t geometry_logical_block_size;
	uint64_t geometry_lowest_aligned_lba;
	uint8_t single_user_any : 1;
	uint8_t single_user_all : 1;
	uint8_t single_user_policy : 1;
	uint32_t single_user_locking_objects;
	uint16_t datastore_max_tables;
	uint32_t datastore_max_table_size;
	uint32_t datastore_alignment;
	uint16_t opal_v100_base_comid;
	uint16_t opal_v100_num_comid;
	uint8_t opal_v100_range_crossing : 1;
	uint16_t opal_v200_base_comid;
	uint16_t opal_v200_num_comid;
	uint8_t opal_v200_initial_pin;
	uint8_t opal_v200_reverted_pin;
	uint16_t opal_v200_num_admin;
	uint16_t opal_v200_num_user;
	uint8_t opal_v200_range_crossing : 1;
	uint16_t vu_feature_code; /* vendor specific feature */
};

struct spdk_opal_dev;

struct spdk_opal_dev *spdk_opal_init_dev(void *dev_handler);

void spdk_opal_scan(struct spdk_opal_dev *dev);
void spdk_opal_close(struct spdk_opal_dev *dev);
struct spdk_opal_info *spdk_opal_get_info(struct spdk_opal_dev *dev);

bool spdk_opal_supported(struct spdk_opal_dev *dev);

#endif
