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

#ifndef _OPAL_H
#define _OPAL_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include <sys/queue.h>
#include "spdk/string.h"

#define IO_BUFFER_LENGTH 2048
#define MAX_TOKS         64
#define OPAL_KEY_MAX 256
#define OPAL_UID_LENGTH 8
#define OPAL_MAX_LRS 8 /* minimum 8 defined by spec */

#define GENERIC_HOST_SESSION_NUM 0x69

#define OPAL_INVAL_PARAM 12

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define SPDK_DTAERROR_NO_METHOD_STATUS 0x89

/*
 * SPC-4
 * Table-258 SECURITY PROTOCOL field in SECURITY PROTOCOL IN command
 */
#define SECP_INFO	0x00
#define SECP_TCG	0x01

/*
 * For further development
 */
enum spdk_if_protocol {
	OPAL_NVME = 0,
	OPAL_SCSI,
	OPAL_ATA,
};

enum spdk_opal_token_type {
	SPDK_OPAL_DTA_TOKENID_BYTESTRING = 0xE0,
	SPDK_OPAL_DTA_TOKENID_SINT = 0xE1,
	SPDK_OPAL_DTA_TOKENID_UINT = 0xE2,
	SPDK_OPAL_DTA_TOKENID_TOKEN = 0xE3, /* actual token is returned */
	SPDK_OPAL_DTA_TOKENID_INVALID = 0X0,
};

enum spdk_opal_atom_width {
	SPDK_OPAL_WIDTH_TINY, /* 1 byte in length */
	SPDK_OPAL_WIDTH_SHORT, /* a 1-byte header and contain up to 15 bytes of data */
	SPDK_OPAL_WIDTH_MEDIUM, /* a 2-byte header and contain up to 2047 bytes of data */
	SPDK_OPAL_WIDTH_LONG, /* a 4-byte header and which contain up to 16,777,215 bytes of data */
	SPDK_OPAL_WIDTH_TOKEN
};

enum spdk_opal_uid {
	/* users */
	UID_SMUID,
	UID_THISSP,
	UID_ADMINSP,
	UID_LOCKINGSP,
	UID_ANYBODY,
	UID_SID,
	UID_ADMIN1,
	UID_USER1,
	UID_USER2,

	/* tables */
	UID_LOCKINGRANGE_GLOBAL,
	UID_LOCKINGRANGE_ACE_RDLOCKED,
	UID_LOCKINGRANGE_ACE_WRLOCKED,
	UID_MBRCONTROL,
	UID_MBR,
	UID_AUTHORITY_TABLE,
	UID_C_PIN_TABLE,
	UID_LOCKING_INFO_TABLE,
	UID_PSID,

	/* C_PIN_TABLE object ID's */
	UID_C_PIN_MSID,
	UID_C_PIN_SID,
	UID_C_PIN_ADMIN1,
	UID_C_PIN_USER1,

	/* half UID's (only first 4 bytes used) */
	UID_HALF_AUTHORITY_OBJ_REF,
	UID_HALF_BOOLEAN_ACE,
};

/* enum for indexing the spdk_opal_method array */
enum spdk_opal_method {
	PROPERTIES_METHOD,
	STARTSESSION_METHOD,
	REVERT_METHOD,
	ACTIVATE_METHOD,
	NEXT_METHOD,
	GETACL_METHOD,
	GENKEY_METHOD,
	REVERTSP_METHOD,
	GET_METHOD,
	SET_METHOD,
	AUTHENTICATE_METHOD,
	RANDOM_METHOD,
};

enum spdk_opal_lock_state {
	OPAL_LS_DISALBELOCKING = 0x00,
	OPAL_LS_READLOCK_ENABLE = 0x01,
	OPAL_LS_WRITELOCK_ENABLE = 0x02,
	OPAL_LS_RWLOCK_ENABLE = 0x04,
};

/*
 * Response token
 */
struct spdk_opal_resp_token {
	const uint8_t *pos;
	uint8_t _padding[7];
	union {
		uint64_t unsigned_num;
		int64_t signed_num;
	} stored;
	size_t len;     /* header + data */
	enum spdk_opal_token_type type;
	enum spdk_opal_atom_width width;
};

/* Response */
struct spdk_opal_resp_parsed {
	int num;
	struct spdk_opal_resp_token resp_tokens[MAX_TOKS];
};

struct spdk_opal_dev {
	bool supported;
	void *data;
	enum spdk_if_protocol  protocol;

	const struct spdk_opal_step *steps;
	pthread_mutex_t	 mutex_lock;
	uint16_t comid;
	uint32_t hsn;
	uint32_t tsn;
	uint64_t align;
	uint64_t lowest_lba;

	size_t pos;
	uint8_t cmd[IO_BUFFER_LENGTH];
	uint8_t resp[IO_BUFFER_LENGTH];

	struct spdk_opal_resp_parsed parsed_resp;
	size_t prev_d_len;
	void *prev_data;

	struct spdk_opal_key *dev_key;

	struct spdk_disk_info *opal_info;
};

struct spdk_opal_step {
	int (*opal_fn)(struct spdk_opal_dev *dev, void *data);
	void *data;
};

struct spdk_opal_key {
	uint8_t locking_range;
	uint8_t key_len;
	uint8_t _padding[6];
	uint8_t key[OPAL_KEY_MAX];
};


struct spdk_opal_session {
	uint32_t sum; /* single user mode */
	uint32_t who;
	struct spdk_opal_key *opal_key;
};

struct spdk_opal_lock_unlock {
	struct spdk_opal_session session;
	uint32_t l_state;
};

struct spdk_opal_new_pw {
	struct spdk_opal_session session_start;

	/* When we're not operating in sum, and we first set
	 * passwords we need to set them via ADMIN authority.
	 * After passwords are changed, we can set them via,
	 * User authorities.
	 */
	struct spdk_opal_session new_user_pw;
};

static const uint8_t spdk_opal_uid[][OPAL_UID_LENGTH] = {
	/* users */
	[UID_SMUID] = /* Session Manager UID */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },
	[UID_THISSP] =
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
	[UID_ADMINSP] =
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x01 },
	[UID_LOCKINGSP] =
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x02 },
	[UID_ANYBODY] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01 },
	[UID_SID] = /* Security Identifier UID */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06 },
	[UID_ADMIN1] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01 },
	[UID_USER1] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x01 },
	[UID_USER2] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x02 },

	/* tables */
	[UID_LOCKINGRANGE_GLOBAL] =
	{ 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x01 },
	[UID_LOCKINGRANGE_ACE_RDLOCKED] =
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE0, 0x01 },
	[UID_LOCKINGRANGE_ACE_WRLOCKED] =
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE8, 0x01 },
	[UID_MBRCONTROL] =
	{ 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x01 },
	[UID_MBR] =
	{ 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 },
	[UID_AUTHORITY_TABLE] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00},
	[UID_C_PIN_TABLE] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x00},
	[UID_LOCKING_INFO_TABLE] =
	{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x01 },
	[UID_PSID] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0xff, 0x01 },

	/* C_PIN_TABLE object ID's */
	[UID_C_PIN_MSID] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x84, 0x02},
	[UID_C_PIN_SID] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x01},
	[UID_C_PIN_ADMIN1] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x01},
	[UID_C_PIN_USER1] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x03, 0x00, 0x01},

	/* half UID's (only first 4 bytes used) */
	[UID_HALF_AUTHORITY_OBJ_REF] =
	{ 0x00, 0x00, 0x0C, 0x05, 0xff, 0xff, 0xff, 0xff },
	[UID_HALF_BOOLEAN_ACE] =
	{ 0x00, 0x00, 0x04, 0x0E, 0xff, 0xff, 0xff, 0xff },
};

/*
 * TCG Storage SSC Methods.
 */
static const uint8_t spdk_opal_method[][OPAL_UID_LENGTH] = {
	[PROPERTIES_METHOD] =
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01 },
	[STARTSESSION_METHOD] =
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x02 },
	[REVERT_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x02 },
	[ACTIVATE_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x03 },
	[NEXT_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08 },
	[GETACL_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0d },
	[GENKEY_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10 },
	[REVERTSP_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11 },
	[GET_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x16 },
	[SET_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x17 },
	[AUTHENTICATE_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1c },
	[RANDOM_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x01 },
};

enum spdk_opal_user {
	OPAL_ADMIN1 = 0x0,
	OPAL_USER1 = 0x01,
	OPAL_USER2 = 0x02,
	OPAL_USER3 = 0x03,
	OPAL_USER4 = 0x04,
	OPAL_USER5 = 0x05,
	OPAL_USER6 = 0x06,
	OPAL_USER7 = 0x07,
	OPAL_USER8 = 0x08,
	OPAL_USER9 = 0x09,
};

struct spdk_opal_locking_range_activate {
	struct spdk_opal_key key;
	uint32_t sum;		/* single user mode */
	uint8_t lockingrange_num;
	uint8_t lockingrange[OPAL_MAX_LRS];
};

struct spdk_opal_locking_range_setup {
	uint8_t id;
	uint8_t _padding[7];
	uint64_t range_start;
	uint64_t range_length;
	bool RLE; /* Read Lock enabled */
	bool WLE; /* Write Lock Enabled */
	struct spdk_opal_session session;
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

struct spdk_disk_info {
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

struct spdk_opal_dev *spdk_init_opal_dev(void *data, enum spdk_if_protocol protocol);

void spdk_opal_scan(struct spdk_opal_dev *dev);

#endif
