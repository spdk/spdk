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
#include "spdk/opal.h"
#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#include "opal_internal.h"

typedef int (spdk_opal_cb)(struct spdk_opal_dev *dev);

static int spdk_end_opal_session_error(struct spdk_opal_dev *dev);

static const char *spdk_opal_error_to_human(int error)
{
	if (error == 0x3F) {
		return "FAILED";
	}

	if ((size_t)error >= ARRAY_SIZE(spdk_opal_errors) || error < 0) {
		return "UNKNOWN ERROR";
	}

	return spdk_opal_errors[error];
}

static int spdk_opal_send_cmd(struct spdk_opal_dev *dev)
{
	switch (dev->protocol) {
	case OPAL_NVME:
		return spdk_nvme_ctrlr_security_send(dev->data, SECP_TCG, dev->comid,
						     0, dev->cmd, IO_BUFFER_LENGTH);

	case OPAL_SCSI:
	case OPAL_ATA:
	default:
		SPDK_ERRLOG("Security Send Failed\n");
		return -1;
	}
}

static int spdk_opal_recv_cmd(struct spdk_opal_dev *dev)
{
	switch (dev->protocol) {
	case OPAL_NVME:
		return spdk_nvme_ctrlr_security_receive(dev->data, SECP_TCG, dev->comid,
							0, dev->resp, IO_BUFFER_LENGTH);

	case OPAL_SCSI:
	case OPAL_ATA:
	default:
		SPDK_ERRLOG("Security Receive Failed\n");
		return -1;
	}
}

static int spdk_opal_recv_check(struct spdk_opal_dev *dev)
{
	void *response = dev->resp;
	struct spdk_opal_header *header = response;
	int ret;

	do {
		SPDK_DEBUGLOG(SPDK_LOG_OPAL, "Sent OPAL command: outstanding=%d, minTransfer=%d\n",
			      header->com_packet.outstanding_data,
			      header->com_packet.min_transfer);

		if (header->com_packet.outstanding_data == 0 ||
		    header->com_packet.min_transfer != 0) {
			return 0;
		}

		memset(response, 0, IO_BUFFER_LENGTH);
		ret = spdk_opal_recv_cmd(dev);
	} while (!ret);

	return ret;
}

static int spdk_opal_send_recv(struct spdk_opal_dev *dev, spdk_opal_cb *cb)
{
	int ret;

	ret = spdk_opal_send_cmd(dev);
	if (ret) {
		return ret;
	}
	ret = spdk_opal_recv_cmd(dev);
	if (ret) {
		return ret;
	}
	ret = spdk_opal_recv_check(dev);
	if (ret) {
		return ret;
	}
	return cb(dev);
}

static void spdk_add_token_u8(int *err, struct spdk_opal_dev *cmd, uint8_t token)
{
	if (*err) {
		return;
	}
	if (cmd->pos >= IO_BUFFER_LENGTH - 1) {
		SPDK_ERRLOG("Error adding u8: end of buffer.\n");
		*err = -ERANGE;
		return;
	}
	cmd->cmd[cmd->pos++] = token;
}

static void spdk_add_short_atom_header(struct spdk_opal_dev *cmd, bool bytestring,
				       bool has_sign, size_t len)
{
	uint8_t atom;
	int err = 0;

	atom = SPDK_SHORT_ATOM_ID;
	atom |= bytestring ? SPDK_SHORT_ATOM_BYTESTRING_FLAG : 0;
	atom |= has_sign ? SPDK_SHORT_ATOM_SIGN_FLAG : 0;
	atom |= len & SPDK_SHORT_ATOM_LEN_MASK;

	spdk_add_token_u8(&err, cmd, atom);
}

static void spdk_add_medium_atom_header(struct spdk_opal_dev *cmd, bool bytestring,
					bool has_sign, size_t len)
{
	uint8_t header;

	header = SPDK_MEDIUM_ATOM_ID;
	header |= bytestring ? SPDK_MEDIUM_ATOM_BYTESTRING_FLAG : 0;
	header |= has_sign ? SPDK_MEDIUM_ATOM_SIGN_FLAG : 0;
	header |= (len >> 8) & SPDK_MEDIUM_ATOM_LEN_MASK;
	cmd->cmd[cmd->pos++] = header;
	cmd->cmd[cmd->pos++] = len;
}

static void spdk_add_token_bytestring(int *err, struct spdk_opal_dev *cmd,
				      const uint8_t *bytestring, size_t len)
{
	size_t header_len = 1;
	bool is_short_atom = true;

	if (*err) {
		return;
	}

	if (len & ~SPDK_SHORT_ATOM_LEN_MASK) {
		header_len = 2;
		is_short_atom = false;
	}

	if (len >= IO_BUFFER_LENGTH - cmd->pos - header_len) {
		SPDK_ERRLOG("Error adding bytestring: end of buffer.\n");
		*err = -ERANGE;
		return;
	}

	if (is_short_atom) {
		spdk_add_short_atom_header(cmd, true, false, len);
	} else {
		spdk_add_medium_atom_header(cmd, true, false, len);
	}

	memcpy(&cmd->cmd[cmd->pos], bytestring, len);
	cmd->pos += len;
}

static void spdk_add_token_u64(int *err, struct spdk_opal_dev *dev, uint64_t number)
{
	int startat = 0;

	/* add header first */
	if (number <= SPDK_TINY_ATOM_DATA_MASK) {
		dev->cmd[dev->pos++] = (uint8_t) number & SPDK_TINY_ATOM_DATA_MASK;
	} else {
		if (number < 0x100) {
			dev->cmd[dev->pos++] = 0x81; /* short atom, 1 byte length */
			startat = 0;
		} else if (number < 0x10000) {
			dev->cmd[dev->pos++] = 0x82; /* short atom, 2 byte length */
			startat = 1;
		} else if (number < 0x100000000) {
			dev->cmd[dev->pos++] = 0x84; /* short atom, 4 byte length */
			startat = 3;
		} else {
			dev->cmd[dev->pos++] = 0x88; /* short atom, 8 byte length */
			startat = 7;
		}

		/* add number value */
		for (int i = startat; i > -1; i--) {
			dev->cmd[dev->pos++] = (uint8_t)((number >> (i * 8)) & 0xff);
		}
	}
}

static int spdk_cmd_finalize(struct spdk_opal_dev *cmd, uint32_t hsn, uint32_t tsn, bool EOD)
{
	struct spdk_opal_header *hdr;
	int err = 0;

	if (EOD) {
		spdk_add_token_u8(&err, cmd, SPDK_OPAL_ENDOFDATA);
		spdk_add_token_u8(&err, cmd, SPDK_OPAL_STARTLIST);
		spdk_add_token_u8(&err, cmd, 0);
		spdk_add_token_u8(&err, cmd, 0);
		spdk_add_token_u8(&err, cmd, 0);
		spdk_add_token_u8(&err, cmd, SPDK_OPAL_ENDLIST);
	}

	if (err) {
		SPDK_ERRLOG("Error finalizing command.\n");
		return -EFAULT;
	}

	hdr = (struct spdk_opal_header *) cmd->cmd;

	to_be32(&hdr->packet.session_tsn, tsn);
	to_be32(&hdr->packet.session_hsn, hsn);

	to_be32(&hdr->sub_packet.length, cmd->pos - sizeof(*hdr));
	while (cmd->pos % 4) {
		if (cmd->pos >= IO_BUFFER_LENGTH) {
			SPDK_ERRLOG("Error: Buffer overrun\n");
			return -ERANGE;
		}
		cmd->cmd[cmd->pos++] = 0;
	}
	to_be32(&hdr->packet.length, cmd->pos - sizeof(hdr->com_packet) -
		sizeof(hdr->packet));
	to_be32(&hdr->com_packet.length, cmd->pos - sizeof(hdr->com_packet));

	return 0;
}

static int spdk_finalize_and_send(struct spdk_opal_dev *dev, bool EOD, spdk_opal_cb cb)
{
	int ret;

	ret = spdk_cmd_finalize(dev, dev->hsn, dev->tsn, EOD);
	if (ret) {
		SPDK_ERRLOG("Error finalizing command buffer: %d\n", ret);
		return ret;
	}

	return spdk_opal_send_recv(dev, cb);
}

static size_t spdk_response_parse_tiny(struct spdk_opal_resp_token *token,
				       const uint8_t *pos)
{
	token->pos = pos;
	token->len = 1;
	token->width = SPDK_OPAL_WIDTH_TINY;

	if (pos[0] & SPDK_TINY_ATOM_SIGN_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = SPDK_OPAL_DTA_TOKENID_UINT;
		token->stored.unsigned_num = pos[0] & 0x3f;
	}

	return token->len;
}

static size_t spdk_response_parse_short(struct spdk_opal_resp_token *token,
					const uint8_t *pos)
{
	token->pos = pos;
	token->len = (pos[0] & SPDK_SHORT_ATOM_LEN_MASK) + 1; /* plus 1-byte header */
	token->width = SPDK_OPAL_WIDTH_SHORT;

	if (pos[0] & SPDK_SHORT_ATOM_BYTESTRING_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_SHORT_ATOM_SIGN_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_SINT;
	} else {
		uint64_t u_integer = 0;
		size_t i, b = 0;

		token->type = SPDK_OPAL_DTA_TOKENID_UINT;
		if (token->len > 9) {
			SPDK_ERRLOG("uint64 with more than 8 bytes\n");
			return -EINVAL;
		}
		for (i = token->len - 1; i > 0; i--) {
			u_integer |= ((uint64_t)pos[i] << (8 * b));
			b++;
		}
		token->stored.unsigned_num = u_integer;
	}

	return token->len;
}

static size_t spdk_response_parse_medium(struct spdk_opal_resp_token *token,
		const uint8_t *pos)
{
	token->pos = pos;
	token->len = (((pos[0] & SPDK_MEDIUM_ATOM_LEN_MASK) << 8) | pos[1]) + 2; /* plus 2-byte header */
	token->width = SPDK_OPAL_WIDTH_MEDIUM;

	if (pos[0] & SPDK_MEDIUM_ATOM_BYTESTRING_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_MEDIUM_ATOM_SIGN_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = SPDK_OPAL_DTA_TOKENID_UINT;
	}

	return token->len;
}

static size_t spdk_response_parse_long(struct spdk_opal_resp_token *token,
				       const uint8_t *pos)
{
	token->pos = pos;
	token->len = ((pos[1] << 16) | (pos[2] << 8) | pos[3]) + 4; /* plus 4-byte header */
	token->width = SPDK_OPAL_WIDTH_LONG;

	if (pos[0] & SPDK_LONG_ATOM_BYTESTRING_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_LONG_ATOM_SIGN_FLAG) {
		token->type = SPDK_OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = SPDK_OPAL_DTA_TOKENID_UINT;
	}

	return token->len;
}

static size_t spdk_response_parse_token(struct spdk_opal_resp_token *token,
					const uint8_t *pos)
{
	token->pos = pos;
	token->len = 1;
	token->type = SPDK_OPAL_DTA_TOKENID_TOKEN;
	token->width = SPDK_OPAL_WIDTH_TOKEN;

	return token->len;
}

static int spdk_response_parse(const uint8_t *buf, size_t length,
			       struct spdk_opal_resp_parsed *resp)
{
	const struct spdk_opal_header *hdr;
	struct spdk_opal_resp_token *token_iter;
	int num_entries = 0;
	int total;
	size_t token_length;
	const uint8_t *pos;
	uint32_t clen, plen, slen;

	if (!buf) {
		return -EFAULT;
	}

	if (!resp) {
		return -EFAULT;
	}

	hdr = (struct spdk_opal_header *)buf;
	pos = buf;
	pos += sizeof(*hdr);

	clen = from_be32(&hdr->com_packet.length);
	plen = from_be32(&hdr->packet.length);
	slen = from_be32(&hdr->sub_packet.length);
	SPDK_DEBUGLOG(SPDK_LOG_OPAL, "Response size: cp: %u, pkt: %u, subpkt: %u\n",
		      clen, plen, slen);

	if (clen == 0 || plen == 0 || slen == 0 ||
	    slen > IO_BUFFER_LENGTH - sizeof(*hdr)) {
		SPDK_ERRLOG("Bad header length. cp: %u, pkt: %u, subpkt: %u\n",
			    clen, plen, slen);
		return -EINVAL;
	}

	if (pos > buf + length) {
		return -EFAULT;
	}

	token_iter = resp->resp_tokens;
	total = slen;

	while (total > 0) {
		if (pos[0] <= SPDK_TINY_ATOM_TYPE_MAX) { /* tiny atom */
			token_length = spdk_response_parse_tiny(token_iter, pos);
		} else if (pos[0] <= SPDK_SHORT_ATOM_TYPE_MAX) { /* short atom */
			token_length = spdk_response_parse_short(token_iter, pos);
		} else if (pos[0] <= SPDK_MEDIUM_ATOM_TYPE_MAX) { /* medium atom */
			token_length = spdk_response_parse_medium(token_iter, pos);
		} else if (pos[0] <= SPDK_LONG_ATOM_TYPE_MAX) { /* long atom */
			token_length = spdk_response_parse_long(token_iter, pos);
		} else { /* TOKEN */
			token_length = spdk_response_parse_token(token_iter, pos);
		}

		pos += token_length;
		total -= token_length;
		token_iter++;
		num_entries++;
	}

	if (num_entries == 0) {
		SPDK_ERRLOG("Couldn't parse response.\n");
		return -EINVAL;
	}
	resp->num = num_entries;

	return 0;
}

static inline bool spdk_response_token_matches(const struct spdk_opal_resp_token *token,
		uint8_t match)
{
	if (!token ||
	    token->type != SPDK_OPAL_DTA_TOKENID_TOKEN ||
	    token->pos[0] != match) {
		return false;
	}
	return true;
}

static const struct spdk_opal_resp_token *spdk_response_get_token(
	const struct spdk_opal_resp_parsed *resp,
	int n)
{
	const struct spdk_opal_resp_token *token;

	if (n >= resp->num) {
		SPDK_ERRLOG("Token number doesn't exist: %d, resp: %d\n",
			    n, resp->num);
		return NULL;
	}

	token = &resp->resp_tokens[n];
	if (token->len == 0) {
		SPDK_ERRLOG("Token length must be non-zero\n");
		return NULL;
	}

	return token;
}

static uint64_t spdk_response_get_u64(const struct spdk_opal_resp_parsed *resp, int n)
{
	if (!resp) {
		SPDK_ERRLOG("Response is NULL\n");
		return 0;
	}

	if (resp->resp_tokens[n].type != SPDK_OPAL_DTA_TOKENID_UINT) {
		SPDK_ERRLOG("Token is not unsigned int: %d\n",
			    resp->resp_tokens[n].type);
		return 0;
	}

	if (!(resp->resp_tokens[n].width == SPDK_OPAL_WIDTH_TINY ||
	      resp->resp_tokens[n].width == SPDK_OPAL_WIDTH_SHORT)) {
		SPDK_ERRLOG("Atom is not short or tiny: %d\n",
			    resp->resp_tokens[n].width);
		return 0;
	}

	return resp->resp_tokens[n].stored.unsigned_num;
}

static size_t spdk_response_get_string(const struct spdk_opal_resp_parsed *resp, int n,
				       const char **store)
{
	*store = NULL;
	if (!resp) {
		SPDK_ERRLOG("Response is NULL\n");
		return 0;
	}

	if (n > resp->num) {
		SPDK_ERRLOG("Response has %d tokens. Can't access %d\n",
			    resp->num, n);
		return 0;
	}

	if (resp->resp_tokens[n].type != SPDK_OPAL_DTA_TOKENID_BYTESTRING) {
		SPDK_ERRLOG("Token is not a byte string!\n");
		return 0;
	}

	*store = resp->resp_tokens[n].pos + 1;
	return resp->resp_tokens[n].len - 1;
}

static int spdk_response_status(const struct spdk_opal_resp_parsed *resp)
{
	int ret;
	const struct spdk_opal_resp_token *tok;

	/* if we get an EOS token, just return 0 */
	tok = spdk_response_get_token(resp, 0);
	if (spdk_response_token_matches(tok, SPDK_OPAL_ENDOFSESSION)) {
		return 0;
	}

	/* if we receive a status code, return it */
	if (resp->num < 5) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	tok = spdk_response_get_token(resp, resp->num - 5);
	if (!spdk_response_token_matches(tok, SPDK_OPAL_STARTLIST)) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	tok = spdk_response_get_token(resp, resp->num - 1);
	if (!spdk_response_token_matches(tok, SPDK_OPAL_ENDLIST)) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	ret = (int)spdk_response_get_u64(resp, resp->num - 4); /* status code 0x00-0x3f */
	return ret;
}

static int spdk_parse_and_check_status(struct spdk_opal_dev *dev)
{
	int error;

	error = spdk_response_parse(dev->resp, IO_BUFFER_LENGTH, &dev->parsed_resp);
	if (error) {
		SPDK_ERRLOG("Couldn't parse response.\n");
		return error;
	}
	return spdk_response_status(&dev->parsed_resp);
}

static inline void spdk_clear_opal_cmd(struct spdk_opal_dev *dev)
{
	dev->pos = sizeof(struct spdk_opal_header);
	memset(dev->cmd, 0, IO_BUFFER_LENGTH);
}

static inline void spdk_set_comid(struct spdk_opal_dev *cmd, uint16_t comid)
{
	struct spdk_opal_header *hdr = (struct spdk_opal_header *)cmd->cmd;

	hdr->com_packet.comid[0] = comid >> 8;
	hdr->com_packet.comid[1] = comid;
	hdr->com_packet.extended_comid[0] = 0;
	hdr->com_packet.extended_comid[1] = 0;
}

static int spdk_opal_next(struct spdk_opal_dev *dev)
{
	const struct spdk_opal_step *step;
	int state = 0, error = 0;

	do {
		step = &dev->steps[state];
		if (!step->opal_fn) {
			break;
		}

		error = step->opal_fn(dev, step->data);
		if (error) {
			SPDK_ERRLOG("Error on step function: %d with error %d: %s\n",
				    state, error,
				    spdk_opal_error_to_human(error));
			if (state > 1) {
				spdk_end_opal_session_error(dev);
				return error;
			}
		}
		state++;
	} while (!error);

	return error;
}


static void spdk_check_tper(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_tper_features *tper = data;
	struct spdk_opal_info *opal_info = dev->opal_info;

	opal_info->opal_ssc_dev = 1;
	opal_info->tper = 1;
	opal_info->tper_acknack = tper->acknack;
	opal_info->tper_async = tper->async;
	opal_info->tper_buffer_mgt = tper->buffer_management;
	opal_info->tper_comid_mgt = tper->comid_management;
	opal_info->tper_streaming = tper->streaming;
	opal_info->tper_sync = tper->sync;
}

/*
 * check single user mode
 */
static bool spdk_check_sum(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_sum *sum = data;
	uint32_t num_locking_objects = from_be32(&sum->num_locking_objects);
	struct spdk_opal_info *opal_info = dev->opal_info;

	if (num_locking_objects == 0) {
		SPDK_NOTICELOG("Need at least one locking object.\n");
		return false;
	}

	opal_info->single_user_mode = 1;
	opal_info->single_user_locking_objects = num_locking_objects;
	opal_info->single_user_any = sum->any;
	opal_info->single_user_all = sum->all;
	opal_info->single_user_policy = sum->policy;

	return true;
}

static void spdk_check_lock(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_locking_features *lock = data;
	struct spdk_opal_info *opal_info = dev->opal_info;

	opal_info->locking = 1;
	opal_info->locking_locked = lock->locked;
	opal_info->locking_locking_enabled = lock->locking_enabled;
	opal_info->locking_locking_supported = lock->locking_supported;
	opal_info->locking_mbr_done = lock->mbr_done;
	opal_info->locking_mbr_enabled = lock->mbr_enabled;
	opal_info->locking_media_encrypt = lock->media_encryption;
}

static void spdk_check_geometry(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_geo_features *geo = data;
	struct spdk_opal_info *opal_info = dev->opal_info;
	uint64_t align = from_be64(&geo->alignment_granularity);
	uint64_t lowest_lba = from_be64(&geo->lowest_aligned_lba);

	dev->align = align;
	dev->lowest_lba = lowest_lba;

	opal_info->geometry = 1;
	opal_info->geometry_align = geo->align;
	opal_info->geometry_logical_block_size = from_be64(&geo->logical_block_size);
	opal_info->geometry_lowest_aligned_lba = lowest_lba;
	opal_info->geometry_alignment_granularity = align;
}

static void spdk_check_datastore(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_datastore_features *datastore = data;
	struct spdk_opal_info *opal_info = dev->opal_info;

	opal_info->datastore = 1;
	opal_info->datastore_max_tables = from_be16(&datastore->max_tables);
	opal_info->datastore_max_table_size = from_be32(&datastore->max_table_size);
	opal_info->datastore_alignment = from_be32(&datastore->alignment);
}

static uint16_t spdk_get_comid_v100(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_opal_v100 *v100 = data;
	struct spdk_opal_info *opal_info = dev->opal_info;
	uint16_t base_comid = from_be16(&v100->base_comid);

	opal_info->opal_v100 = 1;
	opal_info->opal_v100_base_comid = base_comid;
	opal_info->opal_v100_num_comid = from_be16(&v100->number_comids);
	opal_info->opal_v100_range_crossing = v100->range_crossing;

	return base_comid;
}

static uint16_t spdk_get_comid_v200(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_opal_v200 *v200 = data;
	struct spdk_opal_info *opal_info = dev->opal_info;
	uint16_t base_comid = from_be16(&v200->base_comid);

	opal_info->opal_v200 = 1;
	opal_info->opal_v200_base_comid = base_comid;
	opal_info->opal_v200_num_comid = from_be16(&v200->num_comids);
	opal_info->opal_v200_range_crossing = v200->range_crossing;
	opal_info->opal_v200_num_admin = from_be16(&v200->num_locking_admin_auth);
	opal_info->opal_v200_num_user = from_be16(&v200->num_locking_user_auth);

	opal_info->opal_v200_initial_pin = v200->initial_pin;
	opal_info->opal_v200_reverted_pin = v200->reverted_pin;

	return base_comid;
}

static int spdk_opal_discovery0_end(struct spdk_opal_dev *dev)
{
	bool found_com_id = false, supported = false, single_user = false;
	const struct spdk_d0_header *hdr = (struct spdk_d0_header *)dev->resp;
	const uint8_t *epos = dev->resp, *cpos = dev->resp;
	uint16_t comid = 0;
	uint32_t hlen = from_be32(&(hdr->length));
	struct spdk_opal_info *info = calloc(1, sizeof(struct spdk_opal_info));
	if (info == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}

	dev->opal_info = info;
	if (hlen > IO_BUFFER_LENGTH - sizeof(*hdr)) {
		SPDK_ERRLOG("Discovery length overflows buffer (%zu+%u)/%u\n",
			    sizeof(*hdr), hlen, IO_BUFFER_LENGTH);
		return -EFAULT;
	}

	epos += hlen; /* end of buffer */
	cpos += sizeof(*hdr); /* current position on buffer */

	while (cpos < epos) {
		const union spdk_discovery0_features *body =
				(const union spdk_discovery0_features *)cpos;
		uint16_t feature_code = from_be16(&(body->tper.feature_code));

		switch (feature_code) {
		case FEATURECODE_TPER:
			spdk_check_tper(dev, body);
			break;
		case FEATURECODE_SINGLEUSER:
			single_user = spdk_check_sum(dev, body);
			break;
		case FEATURECODE_GEOMETRY:
			spdk_check_geometry(dev, body);
			break;
		case FEATURECODE_LOCKING:
			spdk_check_lock(dev, body);
			break;
		case FEATURECODE_DATASTORE:
			spdk_check_datastore(dev, body);
			break;
		case FEATURECODE_OPALV100:
			comid = spdk_get_comid_v100(dev, body);
			found_com_id = true;
			supported = true;
			break;
		case FEATURECODE_OPALV200:
			comid = spdk_get_comid_v200(dev, body);
			found_com_id = true;
			supported = true;
			break;
		default:
			SPDK_NOTICELOG("Unknow feature code: %d\n", feature_code);
		}
		cpos += body->tper.length + 4;
	}

	if (!supported) {
		SPDK_ERRLOG("This device is not Opal enabled. Not Supported!\n");
		return -EOPNOTSUPP;
	}

	if (!single_user) {
		SPDK_NOTICELOG("Device doesn't support single user mode\n");
	}


	if (!found_com_id) {
		SPDK_ERRLOG("Could not find OPAL comid for device. Returning early\n");
		return -EOPNOTSUPP;;
	}

	dev->comid = comid;
	return 0;
}

static int spdk_opal_discovery0(struct spdk_opal_dev *dev, void *data)
{
	int ret;

	memset(dev->resp, 0, IO_BUFFER_LENGTH);
	dev->comid = LV0_DISCOVERY_COMID;
	ret = spdk_opal_recv_cmd(dev);
	if (ret) {
		return ret;
	}
	return spdk_opal_discovery0_end(dev);
}

static inline void spdk_setup_opal_dev(struct spdk_opal_dev *dev,
				       const struct spdk_opal_step *steps)
{
	dev->steps = steps;
	dev->tsn = 0;
	dev->hsn = 0;
	dev->prev_data = NULL;
}

static int spdk_end_session_cb(struct spdk_opal_dev *dev)
{
	dev->hsn = 0;
	dev->tsn = 0;
	return spdk_parse_and_check_status(dev);
}

static int spdk_end_opal_session(struct spdk_opal_dev *dev, void *data)
{
	int err = 0;
	bool EOD = 0;

	spdk_clear_opal_cmd(dev);
	spdk_set_comid(dev, dev->comid);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDOFSESSION);

	if (err < 0) {
		return err;
	}
	return spdk_finalize_and_send(dev, EOD, spdk_end_session_cb);
}

static int spdk_end_opal_session_error(struct spdk_opal_dev *dev)
{
	const struct spdk_opal_step error_end_session[] = {
		{ spdk_end_opal_session, },
		{ NULL, }
	};
	dev->steps = error_end_session;
	return spdk_opal_next(dev);
}

static int spdk_check_opal_support(struct spdk_opal_dev *dev)
{
	const struct spdk_opal_step steps[] = {
		{ spdk_opal_discovery0, },
		{ NULL, }
	};
	int ret;

	pthread_mutex_lock(&dev->mutex_lock);
	spdk_setup_opal_dev(dev, steps);
	ret = spdk_opal_next(dev);
	dev->supported = !ret;
	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

void spdk_opal_close(struct spdk_opal_dev *dev)
{
	free(dev->opal_info);
	free(dev);
}

static int spdk_start_opal_session_cb(struct spdk_opal_dev *dev)
{
	uint32_t hsn, tsn;
	int error = 0;

	error = spdk_parse_and_check_status(dev);
	if (error) {
		return error;
	}

	hsn = spdk_response_get_u64(&dev->parsed_resp, 4);
	tsn = spdk_response_get_u64(&dev->parsed_resp, 5);

	if (hsn == 0 && tsn == 0) {
		SPDK_ERRLOG("Couldn't authenticate session\n");
		return -EPERM;
	}

	dev->hsn = hsn;
	dev->tsn = tsn;
	return 0;
}

static int spdk_start_generic_opal_session(struct spdk_opal_dev *dev,
		enum spdk_opal_uid auth,
		enum spdk_opal_uid sp_type,
		const char *key,
		uint8_t key_len)
{
	uint32_t hsn;
	int err = 0;

	if (key == NULL && auth != UID_ANYBODY) {
		return OPAL_INVAL_PARAM;
	}

	spdk_clear_opal_cmd(dev);

	spdk_set_comid(dev, dev->comid);
	hsn = GENERIC_HOST_SESSION_NUM;

	spdk_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	spdk_add_token_bytestring(&err, dev, spdk_opal_uid[UID_SMUID],
				  OPAL_UID_LENGTH);
	spdk_add_token_bytestring(&err, dev, spdk_opal_method[STARTSESSION_METHOD],
				  OPAL_UID_LENGTH);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	spdk_add_token_u64(&err, dev, hsn);
	spdk_add_token_bytestring(&err, dev, spdk_opal_uid[sp_type], OPAL_UID_LENGTH);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_TRUE); /* Write */

	switch (auth) {
	case UID_ANYBODY:
		spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
		break;
	case UID_ADMIN1:
	case UID_SID:
		spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
		spdk_add_token_u8(&err, dev, 0); /* HostChallenge */
		spdk_add_token_bytestring(&err, dev, key, key_len);
		spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);
		spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
		spdk_add_token_u8(&err, dev, 3); /* HostSignAuth */
		spdk_add_token_bytestring(&err, dev, spdk_opal_uid[auth],
					  OPAL_UID_LENGTH);
		spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);
		spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
		break;
	default:
		SPDK_ERRLOG("Cannot start Admin SP session with auth %d\n", auth);
		return -EINVAL;
	}

	if (err) {
		SPDK_ERRLOG("Error building start adminsp session command.\n");
		return err;
	}

	return spdk_finalize_and_send(dev, 1, spdk_start_opal_session_cb);
}


static int spdk_start_anybody_adminsp_opal_session(struct spdk_opal_dev *dev, void *data)
{
	return spdk_start_generic_opal_session(dev, UID_ANYBODY,
					       UID_ADMINSP, NULL, 0);
}

static int spdk_get_msid_cpin_pin_cb(struct spdk_opal_dev *dev)
{
	const char *msid_pin;
	size_t strlen;
	int error = 0;

	error = spdk_parse_and_check_status(dev);
	if (error) {
		return error;
	}

	strlen = spdk_response_get_string(&dev->parsed_resp, 4, &msid_pin);
	if (!msid_pin) {
		SPDK_ERRLOG("Couldn't extract PIN from response\n");
		return -EINVAL;
	}

	dev->prev_d_len = strlen;
	dev->prev_data = calloc(0, strlen);
	if (!dev->prev_data) {
		SPDK_ERRLOG("memory allocation error\n");
		return -ENOMEM;
	}
	memcpy(dev->prev_data, msid_pin, strlen);

	SPDK_DEBUGLOG(SPDK_LOG_OPAL, "MSID = %p\n", dev->prev_data);
	return 0;
}

static int spdk_get_msid_cpin_pin(struct spdk_opal_dev *dev, void *data)
{
	int err = 0;

	spdk_clear_opal_cmd(dev);
	spdk_set_comid(dev, dev->comid);

	spdk_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	spdk_add_token_bytestring(&err, dev, spdk_opal_uid[UID_C_PIN_MSID],
				  OPAL_UID_LENGTH);
	spdk_add_token_bytestring(&err, dev, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);

	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);

	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTCOLUMN);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_PIN);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);

	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDCOLUMN);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_PIN);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);

	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building Get MSID CPIN PIN command.\n");
		return err;
	}

	return spdk_finalize_and_send(dev, 1, spdk_get_msid_cpin_pin_cb);
}

static int spdk_start_adminsp_opal_session(struct spdk_opal_dev *dev, void *data)
{
	int ret;
	uint8_t *key = dev->prev_data;

	if (!key) {
		const struct spdk_opal_key *okey = data;
		if (okey == NULL) {
			SPDK_ERRLOG("No key found for auth session\n");
			return -EINVAL;
		}
		ret = spdk_start_generic_opal_session(dev, UID_SID,
						      UID_ADMINSP,
						      okey->key,
						      okey->key_len);
	} else {
		ret = spdk_start_generic_opal_session(dev, UID_SID,
						      UID_ADMINSP,
						      key, dev->prev_d_len);
		free(key);
		dev->prev_data = NULL;
	}

	return ret;
}

static int spdk_generic_pw_cmd(uint8_t *key, size_t key_len, uint8_t *cpin_uid,
			       struct spdk_opal_dev *dev)
{
	int err = 0;

	spdk_clear_opal_cmd(dev);
	spdk_set_comid(dev, dev->comid);

	spdk_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	spdk_add_token_bytestring(&err, dev, cpin_uid, OPAL_UID_LENGTH);
	spdk_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD],
				  OPAL_UID_LENGTH);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_VALUES);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_PIN);
	spdk_add_token_bytestring(&err, dev, key, key_len);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);
	spdk_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);

	return err;
}

static int spdk_set_sid_cpin_pin(struct spdk_opal_dev *dev, void *data)
{
	uint8_t cpin_uid[OPAL_UID_LENGTH];
	const char *new_passwd = data;
	struct spdk_opal_key *opal_key = calloc(1, sizeof(struct spdk_opal_key));
	if (!opal_key) {
		SPDK_ERRLOG("Memory allocation failed for spdk_opal_key\n");
		return -ENOMEM;
	}

	opal_key->key_len = strlen(new_passwd);
	memcpy(opal_key->key, new_passwd, opal_key->key_len);
	dev->dev_key = opal_key;

	memcpy(cpin_uid, spdk_opal_uid[UID_C_PIN_SID], OPAL_UID_LENGTH);

	if (spdk_generic_pw_cmd(opal_key->key, opal_key->key_len, cpin_uid, dev)) {
		SPDK_ERRLOG("Error building Set SID cpin\n");
		return -ERANGE;
	}
	return spdk_finalize_and_send(dev, 1, spdk_parse_and_check_status);
}

static int spdk_opal_take_ownership(struct spdk_opal_dev *dev, char *new_passwd)
{
	const struct spdk_opal_step owner_steps[] = {
		{ spdk_opal_discovery0, },
		{ spdk_start_anybody_adminsp_opal_session, },
		{ spdk_get_msid_cpin_pin, },
		{ spdk_end_opal_session, },
		{ spdk_start_adminsp_opal_session, },
		{ spdk_set_sid_cpin_pin, new_passwd },
		{ spdk_end_opal_session, },
		{ NULL, }
	};
	int ret;

	if (!dev) {
		return -ENODEV;
	}

	pthread_mutex_lock(&dev->mutex_lock);
	spdk_setup_opal_dev(dev, owner_steps);
	ret = spdk_opal_next(dev);
	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

struct spdk_opal_dev *spdk_init_opal_dev(void *data, enum spdk_if_protocol protocol)
{
	struct spdk_opal_dev *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}
	dev->data = data;
	dev->protocol = protocol;
	if (spdk_check_opal_support(dev) != 0) {
		SPDK_ERRLOG("Opal is not supported on this device\n");
		dev->supported = false;
	}
	return dev;
}

int spdk_opal_scan(struct spdk_opal_dev *dev)
{
	int ret;

	ret = spdk_check_opal_support(dev);
	if (ret) {
		SPDK_ERRLOG("check opal support failed: %d\n", ret);
		spdk_opal_close(dev);
		return ret;
	}
	return 0;
}

struct spdk_opal_info *spdk_get_opal_info(struct spdk_opal_dev *dev)
{
	return dev->opal_info;
}

bool spdk_get_opal_support(struct spdk_opal_dev *dev)
{
	return dev->supported;
}

int spdk_opal_cmd(struct spdk_opal_dev *dev, unsigned int cmd, void *arg)
{
	if (!dev) {
		SPDK_ERRLOG("Device null\n");
		return -ENODEV;
	}
	if (!dev->supported) {
		SPDK_ERRLOG("Device not supported\n");
		return -EINVAL;
	}

	switch (cmd) {
	case OPAL_CMD_SCAN:
		return spdk_opal_scan(dev);
	case OPAL_CMD_TAKE_OWNERSHIP:
		return spdk_opal_take_ownership(dev, arg);
	case OPAL_CMD_LOCK_UNLOCK:
	case OPAL_CMD_ACTIVATE_LSP:
	case OPAL_CMD_REVERT_TPER:
	case OPAL_CMD_SETUP_LOCKING_RANGE:

	default:
		SPDK_ERRLOG("NOT SUPPORTED\n");
		return -EINVAL;
	}
}

/* Log component for opal submodule */
SPDK_LOG_REGISTER_COMPONENT("opal", SPDK_LOG_OPAL)
