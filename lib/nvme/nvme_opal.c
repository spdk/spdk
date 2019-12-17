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
#include "spdk/util.h"

#include "nvme_opal_internal.h"

typedef int (*spdk_opal_cb)(struct spdk_opal_dev *dev, void *ctx);

static int opal_parse_and_check_status(struct spdk_opal_dev *dev, void *data);

static const char *
opal_error_to_human(int error)
{
	if (error == SPDK_OPAL_FAILED) {
		return "FAILED";
	}

	if ((size_t)error >= SPDK_COUNTOF(spdk_opal_errors) || error < 0) {
		return "UNKNOWN ERROR";
	}

	return spdk_opal_errors[error];
}

static int
opal_flush_response_buffer(struct spdk_opal_dev *dev)
{
	void *response = dev->resp;
	int ret = 0;

	ret = spdk_nvme_ctrlr_security_receive(dev->dev_handler, SPDK_SCSI_SECP_TCG, dev->comid,
					       0, response, IO_BUFFER_LENGTH);
	if (ret) {
		SPDK_ERRLOG("Security Receive Error on dev = %p\n", dev);
		return ret;
	}

	memset(response, 0, IO_BUFFER_LENGTH);
	return 0;
}

static int
opal_send_cmd(struct spdk_opal_dev *dev)
{
	return spdk_nvme_ctrlr_security_send(dev->dev_handler, SPDK_SCSI_SECP_TCG, dev->comid,
					     0, dev->cmd, IO_BUFFER_LENGTH);
}

static int
opal_recv_cmd(struct spdk_opal_dev *dev)
{
	void *response = dev->resp;
	struct spdk_opal_header *header = response;
	int ret = 0;
	uint64_t start = spdk_get_ticks();
	uint64_t now;

	do {
		ret = spdk_nvme_ctrlr_security_receive(dev->dev_handler, SPDK_SCSI_SECP_TCG, dev->comid,
						       0, dev->resp, IO_BUFFER_LENGTH);
		if (ret) {
			SPDK_ERRLOG("Security Receive Error on dev = %p\n", dev);
			return ret;
		}
		SPDK_DEBUGLOG(SPDK_LOG_OPAL, "outstanding_data=%d, minTransfer=%d\n",
			      header->com_packet.outstanding_data,
			      header->com_packet.min_transfer);

		if (header->com_packet.outstanding_data == 0 &&
		    header->com_packet.min_transfer == 0) {
			return 0;	/* return if all the response data are ready by tper and received by host */
		} else {	/* check timeout */
			now = spdk_get_ticks();
			if (now - start > dev->timeout * spdk_get_ticks_hz()) {
				SPDK_ERRLOG("Secutiy Receive Timeout on dev = %p\n", dev);
				return 0x0F; /* TPer Malfunction */
			}
		}

		memset(response, 0, IO_BUFFER_LENGTH);
	} while (!ret);

	return ret;
}

static int
opal_send_recv(struct spdk_opal_dev *dev, spdk_opal_cb cb, void *data)
{
	int ret;

	ret = opal_flush_response_buffer(dev);
	if (ret) {
		return ret;
	}

	ret = opal_send_cmd(dev);
	if (ret) {
		return ret;
	}
	ret = opal_recv_cmd(dev);
	if (ret) {
		return ret;
	}
	return cb(dev, data);
}

static void
opal_add_token_u8(int *err, struct spdk_opal_dev *dev, uint8_t token)
{
	if (*err) {
		return;
	}
	if (dev->cmd_pos >= IO_BUFFER_LENGTH - 1) {
		SPDK_ERRLOG("Error adding u8: end of buffer.\n");
		*err = -ERANGE;
		return;
	}
	dev->cmd[dev->cmd_pos++] = token;
}

static void
opal_add_short_atom_header(struct spdk_opal_dev *dev, bool bytestring,
			   bool has_sign, size_t len)
{
	uint8_t atom;
	int err = 0;

	atom = SPDK_SHORT_ATOM_ID;
	atom |= bytestring ? SPDK_SHORT_ATOM_BYTESTRING_FLAG : 0;
	atom |= has_sign ? SPDK_SHORT_ATOM_SIGN_FLAG : 0;
	atom |= len & SPDK_SHORT_ATOM_LEN_MASK;

	opal_add_token_u8(&err, dev, atom);
}

static void
opal_add_medium_atom_header(struct spdk_opal_dev *dev, bool bytestring,
			    bool has_sign, size_t len)
{
	uint8_t header;

	header = SPDK_MEDIUM_ATOM_ID;
	header |= bytestring ? SPDK_MEDIUM_ATOM_BYTESTRING_FLAG : 0;
	header |= has_sign ? SPDK_MEDIUM_ATOM_SIGN_FLAG : 0;
	header |= (len >> 8) & SPDK_MEDIUM_ATOM_LEN_MASK;
	dev->cmd[dev->cmd_pos++] = header;
	dev->cmd[dev->cmd_pos++] = len;
}

static void
opal_add_token_bytestring(int *err, struct spdk_opal_dev *dev,
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

	if (len >= IO_BUFFER_LENGTH - dev->cmd_pos - header_len) {
		SPDK_ERRLOG("Error adding bytestring: end of buffer.\n");
		*err = -ERANGE;
		return;
	}

	if (is_short_atom) {
		opal_add_short_atom_header(dev, true, false, len);
	} else {
		opal_add_medium_atom_header(dev, true, false, len);
	}

	memcpy(&dev->cmd[dev->cmd_pos], bytestring, len);
	dev->cmd_pos += len;
}

static void
opal_add_token_u64(int *err, struct spdk_opal_dev *dev, uint64_t number)
{
	int startat = 0;

	if (*err) {
		return;
	}

	/* add header first */
	if (number <= SPDK_TINY_ATOM_DATA_MASK) {
		dev->cmd[dev->cmd_pos++] = (uint8_t) number & SPDK_TINY_ATOM_DATA_MASK;
	} else {
		if (number < 0x100) {
			dev->cmd[dev->cmd_pos++] = 0x81; /* short atom, 1 byte length */
			startat = 0;
		} else if (number < 0x10000) {
			dev->cmd[dev->cmd_pos++] = 0x82; /* short atom, 2 byte length */
			startat = 1;
		} else if (number < 0x100000000) {
			dev->cmd[dev->cmd_pos++] = 0x84; /* short atom, 4 byte length */
			startat = 3;
		} else {
			dev->cmd[dev->cmd_pos++] = 0x88; /* short atom, 8 byte length */
			startat = 7;
		}

		/* add number value */
		for (int i = startat; i > -1; i--) {
			dev->cmd[dev->cmd_pos++] = (uint8_t)((number >> (i * 8)) & 0xff);
		}
	}
}

static void
opal_add_tokens(int *err, struct spdk_opal_dev *dev, int num, ...)
{
	int i;
	va_list args_ptr;
	enum spdk_opal_token tmp;

	va_start(args_ptr, num);

	for (i = 0; i < num; i++) {
		tmp = va_arg(args_ptr, enum spdk_opal_token);
		opal_add_token_u8(err, dev, tmp);
		if (*err != 0) { break; }
	}

	va_end(args_ptr);
}

static int
opal_cmd_finalize(struct spdk_opal_dev *dev, uint32_t hsn, uint32_t tsn, bool eod)
{
	struct spdk_opal_header *hdr;
	int err = 0;

	if (eod) {
		opal_add_tokens(&err, dev, 6, SPDK_OPAL_ENDOFDATA,
				SPDK_OPAL_STARTLIST,
				0, 0, 0,
				SPDK_OPAL_ENDLIST);
	}

	if (err) {
		SPDK_ERRLOG("Error finalizing command.\n");
		return -EFAULT;
	}

	hdr = (struct spdk_opal_header *)dev->cmd;

	to_be32(&hdr->packet.session_tsn, tsn);
	to_be32(&hdr->packet.session_hsn, hsn);

	to_be32(&hdr->sub_packet.length, dev->cmd_pos - sizeof(*hdr));
	while (dev->cmd_pos % 4) {
		if (dev->cmd_pos >= IO_BUFFER_LENGTH) {
			SPDK_ERRLOG("Error: Buffer overrun\n");
			return -ERANGE;
		}
		dev->cmd[dev->cmd_pos++] = 0;
	}
	to_be32(&hdr->packet.length, dev->cmd_pos - sizeof(hdr->com_packet) -
		sizeof(hdr->packet));
	to_be32(&hdr->com_packet.length, dev->cmd_pos - sizeof(hdr->com_packet));

	return 0;
}

/**
 * synchronous function: send and then receive.
 *
 * Wait until response is received. And then call the callback functions.
 */
static int
opal_finalize_and_send(struct spdk_opal_dev *dev, bool eod, spdk_opal_cb cb, void *data)
{
	int ret;

	ret = opal_cmd_finalize(dev, dev->hsn, dev->tsn, eod);
	if (ret) {
		SPDK_ERRLOG("Error finalizing command buffer: %d\n", ret);
		return ret;
	}

	return opal_send_recv(dev, cb, data);
}

static size_t
opal_response_parse_tiny(struct spdk_opal_resp_token *token,
			 const uint8_t *pos)
{
	token->pos = pos;
	token->len = 1;
	token->width = OPAL_WIDTH_TINY;

	if (pos[0] & SPDK_TINY_ATOM_SIGN_FLAG) {
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = OPAL_DTA_TOKENID_UINT;
		token->stored.unsigned_num = pos[0] & SPDK_TINY_ATOM_DATA_MASK;
	}

	return token->len;
}

static int
opal_response_parse_short(struct spdk_opal_resp_token *token,
			  const uint8_t *pos)
{
	token->pos = pos;
	token->len = (pos[0] & SPDK_SHORT_ATOM_LEN_MASK) + 1; /* plus 1-byte header */
	token->width = OPAL_WIDTH_SHORT;

	if (pos[0] & SPDK_SHORT_ATOM_BYTESTRING_FLAG) {
		token->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_SHORT_ATOM_SIGN_FLAG) {
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		uint64_t u_integer = 0;
		size_t i, b = 0;

		token->type = OPAL_DTA_TOKENID_UINT;
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

static size_t
opal_response_parse_medium(struct spdk_opal_resp_token *token,
			   const uint8_t *pos)
{
	token->pos = pos;
	token->len = (((pos[0] & SPDK_MEDIUM_ATOM_LEN_MASK) << 8) | pos[1]) + 2; /* plus 2-byte header */
	token->width = OPAL_WIDTH_MEDIUM;

	if (pos[0] & SPDK_MEDIUM_ATOM_BYTESTRING_FLAG) {
		token->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_MEDIUM_ATOM_SIGN_FLAG) {
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = OPAL_DTA_TOKENID_UINT;
	}

	return token->len;
}

static size_t
opal_response_parse_long(struct spdk_opal_resp_token *token,
			 const uint8_t *pos)
{
	token->pos = pos;
	token->len = ((pos[1] << 16) | (pos[2] << 8) | pos[3]) + 4; /* plus 4-byte header */
	token->width = OPAL_WIDTH_LONG;

	if (pos[0] & SPDK_LONG_ATOM_BYTESTRING_FLAG) {
		token->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_LONG_ATOM_SIGN_FLAG) {
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = OPAL_DTA_TOKENID_UINT;
	}

	return token->len;
}

static size_t
opal_response_parse_token(struct spdk_opal_resp_token *token,
			  const uint8_t *pos)
{
	token->pos = pos;
	token->len = 1;
	token->type = OPAL_DTA_TOKENID_TOKEN;
	token->width = OPAL_WIDTH_TOKEN;

	return token->len;
}

static int
opal_response_parse(const uint8_t *buf, size_t length,
		    struct spdk_opal_resp_parsed *resp)
{
	const struct spdk_opal_header *hdr;
	struct spdk_opal_resp_token *token_iter;
	int num_entries = 0;
	int total;
	size_t token_length;
	const uint8_t *pos;
	uint32_t clen, plen, slen;

	if (!buf || !resp) {
		return -EINVAL;
	}

	hdr = (struct spdk_opal_header *)buf;
	pos = buf + sizeof(*hdr);

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
		SPDK_ERRLOG("Pointer out of range\n");
		return -EFAULT;
	}

	token_iter = resp->resp_tokens;
	total = slen;

	while (total > 0) {
		if (pos[0] <= SPDK_TINY_ATOM_TYPE_MAX) { /* tiny atom */
			token_length = opal_response_parse_tiny(token_iter, pos);
		} else if (pos[0] <= SPDK_SHORT_ATOM_TYPE_MAX) { /* short atom */
			token_length = opal_response_parse_short(token_iter, pos);
		} else if (pos[0] <= SPDK_MEDIUM_ATOM_TYPE_MAX) { /* medium atom */
			token_length = opal_response_parse_medium(token_iter, pos);
		} else if (pos[0] <= SPDK_LONG_ATOM_TYPE_MAX) { /* long atom */
			token_length = opal_response_parse_long(token_iter, pos);
		} else { /* TOKEN */
			token_length = opal_response_parse_token(token_iter, pos);
		}

		if (token_length <= 0) {
			SPDK_ERRLOG("Parse response failure.\n");
			return -EINVAL;
		}

		pos += token_length;
		total -= token_length;
		token_iter++;
		num_entries++;

		if (total < 0) {
			SPDK_ERRLOG("Length not matching.\n");
			return -EINVAL;
		}
	}

	if (num_entries == 0) {
		SPDK_ERRLOG("Couldn't parse response.\n");
		return -EINVAL;
	}
	resp->num = num_entries;

	return 0;
}

static inline bool
opal_response_token_matches(const struct spdk_opal_resp_token *token,
			    uint8_t match)
{
	if (!token ||
	    token->type != OPAL_DTA_TOKENID_TOKEN ||
	    token->pos[0] != match) {
		return false;
	}
	return true;
}

static const struct spdk_opal_resp_token *
opal_response_get_token(const struct spdk_opal_resp_parsed *resp, int index)
{
	const struct spdk_opal_resp_token *token;

	if (index >= resp->num) {
		SPDK_ERRLOG("Token number doesn't exist: %d, resp: %d\n",
			    index, resp->num);
		return NULL;
	}

	token = &resp->resp_tokens[index];
	if (token->len == 0) {
		SPDK_ERRLOG("Token length must be non-zero\n");
		return NULL;
	}

	return token;
}

static uint64_t
opal_response_get_u64(const struct spdk_opal_resp_parsed *resp, int index)
{
	if (!resp) {
		SPDK_ERRLOG("Response is NULL\n");
		return 0;
	}

	if (resp->resp_tokens[index].type != OPAL_DTA_TOKENID_UINT) {
		SPDK_ERRLOG("Token is not unsigned int: %d\n",
			    resp->resp_tokens[index].type);
		return 0;
	}

	if (!(resp->resp_tokens[index].width == OPAL_WIDTH_TINY ||
	      resp->resp_tokens[index].width == OPAL_WIDTH_SHORT)) {
		SPDK_ERRLOG("Atom is not short or tiny: %d\n",
			    resp->resp_tokens[index].width);
		return 0;
	}

	return resp->resp_tokens[index].stored.unsigned_num;
}

static uint16_t
opal_response_get_u16(const struct spdk_opal_resp_parsed *resp, int index)
{
	uint64_t i = opal_response_get_u64(resp, index);
	if (i > 0xffffull) {
		SPDK_ERRLOG("parse reponse u16 failed. Overflow\n");
		return 0;
	}
	return (uint16_t) i;
}

static uint8_t
opal_response_get_u8(const struct spdk_opal_resp_parsed *resp, int index)
{
	uint64_t i = opal_response_get_u64(resp, index);
	if (i > 0xffull) {
		SPDK_ERRLOG("parse reponse u8 failed. Overflow\n");
		return 0;
	}
	return (uint8_t) i;
}

static size_t
opal_response_get_string(const struct spdk_opal_resp_parsed *resp, int n,
			 const char **store)
{
	uint8_t header_len;
	struct spdk_opal_resp_token token = resp->resp_tokens[n];

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

	if (token.type != OPAL_DTA_TOKENID_BYTESTRING) {
		SPDK_ERRLOG("Token is not a byte string!\n");
		return 0;
	}

	switch (token.width) {
	case OPAL_WIDTH_SHORT:
		header_len = 1;
		break;
	case OPAL_WIDTH_MEDIUM:
		header_len = 2;
		break;
	case OPAL_WIDTH_LONG:
		header_len = 4;
		break;
	default:
		SPDK_ERRLOG("Can't get string from this Token\n");
		return 0;
	}

	*store = token.pos + header_len;
	return token.len - header_len;
}

static int
opal_response_status(const struct spdk_opal_resp_parsed *resp)
{
	const struct spdk_opal_resp_token *tok;

	/* if we get an EOS token, just return 0 */
	tok = opal_response_get_token(resp, 0);
	if (opal_response_token_matches(tok, SPDK_OPAL_ENDOFSESSION)) {
		return 0;
	}

	if (resp->num < 5) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	tok = opal_response_get_token(resp, resp->num - 5); /* the first token should be STARTLIST */
	if (!opal_response_token_matches(tok, SPDK_OPAL_STARTLIST)) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	tok = opal_response_get_token(resp, resp->num - 1); /* the last token should be ENDLIST */
	if (!opal_response_token_matches(tok, SPDK_OPAL_ENDLIST)) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	/* The second and third values in the status list are reserved, and are
	defined in core spec to be 0x00 and 0x00 and SHOULD be ignored by the host. */
	return (int)opal_response_get_u64(resp,
					  resp->num - 4); /* We only need the first value in the status list. */
}

static int
opal_parse_and_check_status(struct spdk_opal_dev *dev, void *data)
{
	int error;

	error = opal_response_parse(dev->resp, IO_BUFFER_LENGTH, &dev->parsed_resp);
	if (error) {
		SPDK_ERRLOG("Couldn't parse response.\n");
		return error;
	}
	return opal_response_status(&dev->parsed_resp);
}

static inline void
opal_clear_cmd(struct spdk_opal_dev *dev)
{
	dev->cmd_pos = sizeof(struct spdk_opal_header);
	memset(dev->cmd, 0, IO_BUFFER_LENGTH);
}

static inline void
opal_set_comid(struct spdk_opal_dev *dev, uint16_t comid)
{
	struct spdk_opal_header *hdr = (struct spdk_opal_header *)dev->cmd;

	hdr->com_packet.comid[0] = comid >> 8;
	hdr->com_packet.comid[1] = comid;
	hdr->com_packet.extended_comid[0] = 0;
	hdr->com_packet.extended_comid[1] = 0;
}

static inline int
opal_init_key(struct spdk_opal_key *opal_key, const char *passwd,
	      enum spdk_opal_locking_range locking_range)
{
	int len;

	if (passwd == NULL || passwd[0] == '\0') {
		SPDK_ERRLOG("Password is empty. Create key failed\n");
		return -EINVAL;
	}

	len = strlen(passwd);

	if (len >= OPAL_KEY_MAX) {
		SPDK_ERRLOG("Password too long. Create key failed\n");
		return -EINVAL;
	}

	memset(opal_key, 0, sizeof(struct spdk_opal_key));
	opal_key->key_len = len;
	memcpy(opal_key->key, passwd, opal_key->key_len);
	opal_key->locking_range = locking_range;

	return 0;
}

static int
opal_build_locking_user(uint8_t *buffer, size_t length, uint8_t user)
{
	if (length < OPAL_UID_LENGTH) {
		SPDK_ERRLOG("Can't build locking range user, buffer overflow\n");
		return -ERANGE;
	}

	memcpy(buffer, spdk_opal_uid[UID_USER1], OPAL_UID_LENGTH);

	buffer[7] = user;

	return 0;
}

static int
opal_build_locking_range(uint8_t *buffer, size_t length, uint8_t locking_range)
{
	if (length < OPAL_UID_LENGTH) {
		SPDK_ERRLOG("Can't build locking range. Buffer overflow\n");
		return -ERANGE;
	}

	memcpy(buffer, spdk_opal_uid[UID_LOCKINGRANGE_GLOBAL], OPAL_UID_LENGTH);

	/* global */
	if (locking_range == 0) {
		return 0;
	}

	/* non-global */
	buffer[5] = LOCKING_RANGE_NON_GLOBAL;
	buffer[7] = locking_range;
	return 0;
}

static void
opal_check_tper(struct spdk_opal_dev *dev, const void *data)
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
static bool
opal_check_sum(struct spdk_opal_dev *dev, const void *data)
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

static void
opal_check_lock(struct spdk_opal_dev *dev, const void *data)
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

static void
opal_check_geometry(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_geo_features *geo = data;
	struct spdk_opal_info *opal_info = dev->opal_info;
	uint64_t align = from_be64(&geo->alignment_granularity);
	uint64_t lowest_lba = from_be64(&geo->lowest_aligned_lba);

	dev->align = align;
	dev->lowest_lba = lowest_lba;

	opal_info->geometry = 1;
	opal_info->geometry_align = geo->align;
	opal_info->geometry_logical_block_size = from_be32(&geo->logical_block_size);
	opal_info->geometry_lowest_aligned_lba = lowest_lba;
	opal_info->geometry_alignment_granularity = align;
}

static void
opal_check_datastore(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_datastore_features *datastore = data;
	struct spdk_opal_info *opal_info = dev->opal_info;

	opal_info->datastore = 1;
	opal_info->datastore_max_tables = from_be16(&datastore->max_tables);
	opal_info->datastore_max_table_size = from_be32(&datastore->max_table_size);
	opal_info->datastore_alignment = from_be32(&datastore->alignment);
}

static uint16_t
opal_get_comid_v100(struct spdk_opal_dev *dev, const void *data)
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

static uint16_t
opal_get_comid_v200(struct spdk_opal_dev *dev, const void *data)
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

static int
opal_discovery0_end(struct spdk_opal_dev *dev)
{
	bool found_com_id = false, supported = false, single_user = false;
	const struct spdk_d0_header *hdr = (struct spdk_d0_header *)dev->resp;
	const uint8_t *epos = dev->resp, *cpos = dev->resp;
	uint16_t comid = 0;
	uint32_t hlen = from_be32(&(hdr->length));

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
			opal_check_tper(dev, body);
			break;
		case FEATURECODE_SINGLEUSER:
			single_user = opal_check_sum(dev, body);
			break;
		case FEATURECODE_GEOMETRY:
			opal_check_geometry(dev, body);
			break;
		case FEATURECODE_LOCKING:
			opal_check_lock(dev, body);
			break;
		case FEATURECODE_DATASTORE:
			opal_check_datastore(dev, body);
			break;
		case FEATURECODE_OPALV100:
			comid = opal_get_comid_v100(dev, body);
			found_com_id = true;
			supported = true;
			break;
		case FEATURECODE_OPALV200:
			comid = opal_get_comid_v200(dev, body);
			found_com_id = true;
			supported = true;
			break;
		default:
			SPDK_INFOLOG(SPDK_LOG_OPAL, "Unknow feature code: %d\n", feature_code);
		}
		cpos += body->tper.length + 4;
	}

	if (supported == false) {
		SPDK_ERRLOG("Opal Not Supported.\n");
		return SPDK_OPAL_NOT_SUPPORTED;
	}

	if (single_user == false) {
		SPDK_INFOLOG(SPDK_LOG_OPAL, "Single User Mode Not Supported\n");
	}

	if (found_com_id == false) {
		SPDK_ERRLOG("Could not find OPAL comid for device. Returning early\n");
		return -EINVAL;
	}

	dev->comid = comid;
	return 0;
}

static int
opal_discovery0(struct spdk_opal_dev *dev)
{
	int ret;

	memset(dev->resp, 0, IO_BUFFER_LENGTH);
	dev->comid = LV0_DISCOVERY_COMID;
	ret = opal_recv_cmd(dev);
	if (ret) {
		return ret;
	}

	return opal_discovery0_end(dev);
}

static inline void
opal_setup_dev(struct spdk_opal_dev *dev)
{
	dev->tsn = 0;
	dev->hsn = 0;
	dev->prev_data = NULL;
	dev->timeout = SPDK_OPAL_TPER_TIMEOUT;
}

static int
opal_end_session_cb(struct spdk_opal_dev *dev, void *data)
{
	dev->hsn = 0;
	dev->tsn = 0;
	return opal_parse_and_check_status(dev, NULL);
}

static int
opal_end_session(struct spdk_opal_dev *dev)
{
	int err = 0;
	bool eod = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);
	opal_add_token_u8(&err, dev, SPDK_OPAL_ENDOFSESSION);

	if (err < 0) {
		return err;
	}
	return opal_finalize_and_send(dev, eod, opal_end_session_cb, NULL);
}

static int
opal_check_support(struct spdk_opal_dev *dev)
{
	int ret;

	opal_setup_dev(dev);
	ret = opal_discovery0(dev);

	dev->supported = (ret == 0 ? true : false);

	return ret;
}

void
spdk_opal_close(struct spdk_opal_dev *dev)
{
	pthread_mutex_destroy(&dev->mutex_lock);
	if (dev->max_ranges > 0) {
		for (int i = 0; i < dev->max_ranges; i++) {
			spdk_opal_free_locking_range_info(dev, i);
		}
	}
	free(dev->opal_info);
	free(dev);
}

static int
opal_start_session_cb(struct spdk_opal_dev *dev, void *data)
{
	uint32_t hsn, tsn;
	int error = 0;

	error = opal_parse_and_check_status(dev, NULL);
	if (error) {
		return error;
	}

	hsn = opal_response_get_u64(&dev->parsed_resp, 4);
	tsn = opal_response_get_u64(&dev->parsed_resp, 5);

	if (hsn == 0 && tsn == 0) {
		SPDK_ERRLOG("Couldn't authenticate session\n");
		return -EPERM;
	}

	dev->hsn = hsn;
	dev->tsn = tsn;
	return 0;
}

static int
opal_start_generic_session(struct spdk_opal_dev *dev,
			   enum opal_uid_enum auth,
			   enum opal_uid_enum sp_type,
			   const char *key,
			   uint8_t key_len)
{
	uint32_t hsn;
	int err = 0;

	if (key == NULL && auth != UID_ANYBODY) {
		return OPAL_INVAL_PARAM;
	}

	opal_clear_cmd(dev);

	opal_set_comid(dev, dev->comid);
	hsn = GENERIC_HOST_SESSION_NUM;

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_SMUID],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[STARTSESSION_METHOD],
				  OPAL_UID_LENGTH);
	opal_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	opal_add_token_u64(&err, dev, hsn);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[sp_type], OPAL_UID_LENGTH);
	opal_add_token_u8(&err, dev, SPDK_OPAL_TRUE); /* Write */

	switch (auth) {
	case UID_ANYBODY:
		opal_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
		break;
	case UID_ADMIN1:
	case UID_SID:
		opal_add_token_u8(&err, dev, SPDK_OPAL_STARTNAME);
		opal_add_token_u8(&err, dev, 0); /* HostChallenge */
		opal_add_token_bytestring(&err, dev, key, key_len);
		opal_add_tokens(&err, dev, 3,    /* number of token */
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				3);/* HostSignAuth */
		opal_add_token_bytestring(&err, dev, spdk_opal_uid[auth],
					  OPAL_UID_LENGTH);
		opal_add_token_u8(&err, dev, SPDK_OPAL_ENDNAME);
		opal_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
		break;
	default:
		SPDK_ERRLOG("Cannot start Admin SP session with auth %d\n", auth);
		return -EINVAL;
	}

	if (err) {
		SPDK_ERRLOG("Error building start adminsp session command.\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_start_session_cb, NULL);
}

static int
opal_start_anybody_adminsp_session(struct spdk_opal_dev *dev)
{
	return opal_start_generic_session(dev, UID_ANYBODY,
					  UID_ADMINSP, NULL, 0);
}

static int
opal_start_admin_session(struct spdk_opal_dev *dev, void *data)
{
	struct spdk_opal_key *opal_key = data;

	if (opal_key == NULL) {
		SPDK_ERRLOG("No key found for auth session\n");
		return -EINVAL;
	}
	return opal_start_generic_session(dev, UID_ADMIN1, UID_LOCKINGSP,
					  opal_key->key, opal_key->key_len);
}

static int
opal_get_msid_cpin_pin_cb(struct spdk_opal_dev *dev, void *data)
{
	const char *msid_pin;
	size_t strlen;
	int error = 0;

	error = opal_parse_and_check_status(dev, NULL);
	if (error) {
		return error;
	}

	strlen = opal_response_get_string(&dev->parsed_resp, 4, &msid_pin);
	if (!msid_pin) {
		SPDK_ERRLOG("Couldn't extract PIN from response\n");
		return -EINVAL;
	}

	dev->prev_d_len = strlen;
	dev->prev_data = calloc(1, strlen);
	if (!dev->prev_data) {
		SPDK_ERRLOG("memory allocation error\n");
		return -ENOMEM;
	}
	memcpy(dev->prev_data, msid_pin, strlen);

	SPDK_DEBUGLOG(SPDK_LOG_OPAL, "MSID = %p\n", dev->prev_data);
	return 0;
}

static int
opal_get_msid_cpin_pin(struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_C_PIN_MSID],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_PIN,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_PIN,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building Get MSID CPIN PIN command.\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_get_msid_cpin_pin_cb, NULL);
}

static int
opal_start_adminsp_session(struct spdk_opal_dev *dev, void *data)
{
	int ret;
	uint8_t *key = dev->prev_data;

	if (!key) {
		const struct spdk_opal_key *okey = data;
		if (okey == NULL) {
			SPDK_ERRLOG("No key found for auth session\n");
			return -EINVAL;
		}
		ret = opal_start_generic_session(dev, UID_SID,
						 UID_ADMINSP,
						 okey->key,
						 okey->key_len);
	} else {
		ret = opal_start_generic_session(dev, UID_SID,
						 UID_ADMINSP,
						 key, dev->prev_d_len);
		free(key);
		dev->prev_data = NULL;
	}

	return ret;
}

static int
opal_generic_pw_cmd(uint8_t *key, size_t key_len, uint8_t *cpin_uid,
		    struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, cpin_uid, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD],
				  OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 6,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_PIN);
	opal_add_token_bytestring(&err, dev, key, key_len);
	opal_add_tokens(&err, dev, 4,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	return err;
}

static int
opal_get_locking_sp_lifecycle_cb(struct spdk_opal_dev *dev, void *data)
{
	uint8_t lifecycle;
	int error = 0;

	error = opal_parse_and_check_status(dev, NULL);
	if (error) {
		return error;
	}

	lifecycle = opal_response_get_u64(&dev->parsed_resp, 4);
	if (lifecycle != OPAL_MANUFACTURED_INACTIVE) { /* status before activate */
		SPDK_ERRLOG("Couldn't determine the status of the Lifecycle state\n");
		return -EINVAL;
	}

	return 0;
}

static int
opal_get_locking_sp_lifecycle(struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_LOCKINGSP],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_LIFECYCLE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_LIFECYCLE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error Building GET Lifecycle Status command\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_get_locking_sp_lifecycle_cb, NULL);
}

static int
opal_activate(struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_LOCKINGSP],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[ACTIVATE_METHOD],
				  OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 2, SPDK_OPAL_STARTLIST, SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building Activate LockingSP command.\n");
		return err;
	}

	/* TODO: Single User Mode for activatation */

	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int
opal_start_auth_session(struct spdk_opal_dev *dev, struct opal_common_session *session)
{
	uint8_t uid_user[OPAL_UID_LENGTH];
	size_t keylen = session->opal_key->key_len;
	int err = 0;
	uint8_t *key = session->opal_key->key;
	uint32_t hsn = GENERIC_HOST_SESSION_NUM;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	if (session->who != OPAL_ADMIN1) {
		err = opal_build_locking_user(uid_user, sizeof(uid_user),
					      session->who);
		if (err) {
			return err;
		}
	} else {
		memcpy(uid_user, spdk_opal_uid[UID_ADMIN1], OPAL_UID_LENGTH);
	}

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_SMUID],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[STARTSESSION_METHOD],
				  OPAL_UID_LENGTH);

	opal_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	opal_add_token_u64(&err, dev, hsn);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_LOCKINGSP],
				  OPAL_UID_LENGTH);
	opal_add_tokens(&err, dev, 3, SPDK_OPAL_TRUE, SPDK_OPAL_STARTNAME,
			0); /* True for a Read-Write session  */
	opal_add_token_bytestring(&err, dev, key, keylen);
	opal_add_tokens(&err, dev, 3, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME, 3); /* HostSignAuth */
	opal_add_token_bytestring(&err, dev, uid_user, OPAL_UID_LENGTH);
	opal_add_tokens(&err, dev, 2, SPDK_OPAL_ENDNAME, SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building STARTSESSION command.\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_start_session_cb, NULL);
}

static int
opal_lock_unlock_range(struct spdk_opal_dev *dev, struct spdk_opal_locking_session *locking_session)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	uint8_t read_locked, write_locked;
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	if (opal_build_locking_range(uid_locking_range, sizeof(uid_locking_range),
				     locking_session->session.opal_key->locking_range) < 0) {
		return -ERANGE;
	}

	switch (locking_session->l_state) {
	case OPAL_READONLY:
		read_locked = 0;
		write_locked = 1;
		break;
	case OPAL_READWRITE:
		read_locked = 0;
		write_locked = 0;
		break;
	case OPAL_RWLOCK:
		read_locked = 1;
		write_locked = 1;
		break;
	default:
		SPDK_ERRLOG("Tried to set an invalid locking state.\n");
		return -EINVAL;
	}

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid_locking_range, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 15, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_READLOCKED,
			read_locked,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_WRITELOCKED,
			write_locked,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building SET command.\n");
		return err;
	}
	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int opal_generic_locking_range_enable_disable(struct spdk_opal_dev *dev,
		uint8_t *uid, bool read_lock_enabled, bool write_lock_enabled,
		bool read_locked, bool write_locked)
{
	int err = 0;

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 23, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_READLOCKENABLED,
			read_lock_enabled,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_WRITELOCKENABLED,
			write_lock_enabled,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_READLOCKED,
			read_locked,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_WRITELOCKED,
			write_locked,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	if (err) {
		SPDK_ERRLOG("Error building locking range enable/disable command.\n");
	}
	return err;
}

static int
spdk_enable_global_locking_range(struct spdk_opal_dev *dev, uint8_t *locking_range,
				 struct opal_locking_range_setup_session *setup_session)
{
	int err;

	err = opal_generic_locking_range_enable_disable(dev, locking_range,
			setup_session->read_lock_enabled,
			setup_session->write_lock_enabled,
			0, 0);
	if (err) {
		SPDK_ERRLOG("Failed to create enable global locking range command\n");
	}
	return err;
}

static int
opal_setup_locking_range(struct spdk_opal_dev *dev,
			 struct opal_locking_range_setup_session *setup_session)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	uint8_t locking_range_id;
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	locking_range_id = setup_session->session.opal_key->locking_range;
	err = opal_build_locking_range(uid_locking_range, OPAL_UID_LENGTH, locking_range_id);
	if (err) {
		return err;
	}

	if (locking_range_id == 0) {
		err = spdk_enable_global_locking_range(dev, uid_locking_range, setup_session);
	} else {
		opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
		opal_add_token_bytestring(&err, dev, uid_locking_range, OPAL_UID_LENGTH);
		opal_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD],
					  OPAL_UID_LENGTH);

		opal_add_tokens(&err, dev, 6,
				SPDK_OPAL_STARTLIST,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_VALUES,
				SPDK_OPAL_STARTLIST,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_RANGESTART);
		opal_add_token_u64(&err, dev, setup_session->range_start);
		opal_add_tokens(&err, dev, 3,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_RANGELENGTH);
		opal_add_token_u64(&err, dev, setup_session->range_length);
		opal_add_tokens(&err, dev, 3,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_READLOCKENABLED);
		opal_add_token_u64(&err, dev, setup_session->read_lock_enabled);
		opal_add_tokens(&err, dev, 3,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_WRITELOCKENABLED);
		opal_add_token_u64(&err, dev, setup_session->write_lock_enabled);
		opal_add_tokens(&err, dev, 4,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_ENDLIST,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_ENDLIST);
	}
	if (err) {
		SPDK_ERRLOG("Error building Setup Locking range command.\n");
		return err;

	}

	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int
opal_get_max_ranges_cb(struct spdk_opal_dev *dev, void *data)
{
	uint8_t max_ranges;
	int error = 0;

	error = opal_parse_and_check_status(dev, NULL);
	if (error) {
		return error;
	}

	max_ranges = opal_response_get_u16(&dev->parsed_resp, 4); /* "MaxRanges" is token 4 of response */
	dev->max_ranges = max_ranges;
	return 0;
}

static int
opal_get_max_ranges(struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_LOCKING_INFO_TABLE],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_MAXRANGES,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_MAXRANGES,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error Building GET Lifecycle Status command\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_get_max_ranges_cb, NULL);
}

static int
opal_get_locking_range_info_cb(struct spdk_opal_dev *dev, void *data)
{
	int error = 0;
	uint8_t id = *(uint8_t *)data;

	error = opal_parse_and_check_status(dev, NULL);
	if (error) {
		return error;
	}

	if (dev->max_ranges != 0 && id > dev->max_ranges) {
		SPDK_ERRLOG("Locking range ID not valid\n");
		return -EINVAL;
	}

	dev->locking_range_info[id]->range_start = opal_response_get_u64(&dev->parsed_resp, 4);
	dev->locking_range_info[id]->range_length = opal_response_get_u64(&dev->parsed_resp, 8);
	dev->locking_range_info[id]->read_lock_enabled = opal_response_get_u8(&dev->parsed_resp, 12);
	dev->locking_range_info[id]->write_lock_enabled = opal_response_get_u8(&dev->parsed_resp, 16);
	dev->locking_range_info[id]->read_locked = opal_response_get_u8(&dev->parsed_resp, 20);
	dev->locking_range_info[id]->write_locked = opal_response_get_u8(&dev->parsed_resp, 24);

	return 0;
}

static int
opal_get_locking_range_info(struct spdk_opal_dev *dev,
			    enum spdk_opal_locking_range locking_range_id)
{
	int err = 0;
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	struct spdk_opal_locking_range_info *info;

	err = opal_build_locking_range(uid_locking_range, OPAL_UID_LENGTH, locking_range_id);
	if (err) {
		return err;
	}

	if (dev->locking_range_info[locking_range_id] == NULL) {
		info = calloc(1, sizeof(struct spdk_opal_locking_range_info));
		if (info == NULL) {
			SPDK_ERRLOG("Memory allocation failed for spdk_opal_locking_range_info\n");
			return -ENOMEM;
		}
		info->locking_range_id = locking_range_id;
		dev->locking_range_info[locking_range_id] = info;
	} else {
		info = dev->locking_range_info[locking_range_id];
		memset(info, 0, sizeof(*info));
		info->locking_range_id = locking_range_id;
	}

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid_locking_range, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);


	opal_add_tokens(&err, dev, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_RANGESTART,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_WRITELOCKED,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error Building get locking range info command\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_get_locking_range_info_cb, &locking_range_id);
}

static int
opal_enable_user(struct spdk_opal_dev *dev, struct opal_common_session *session)
{
	int err = 0;
	uint8_t uid_user[OPAL_UID_LENGTH];

	err = opal_build_locking_user(uid_user, OPAL_UID_LENGTH, session->who);

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid_user, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 11,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_AUTH_ENABLE,
			SPDK_OPAL_TRUE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error Building enable user command\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int
opal_add_user_to_locking_range(struct spdk_opal_dev *dev,
			       struct spdk_opal_locking_session *locking_session)
{
	int err = 0;
	uint8_t uid_user[OPAL_UID_LENGTH];
	uint8_t uid_locking_range[OPAL_UID_LENGTH];

	err = opal_build_locking_user(uid_user, OPAL_UID_LENGTH, locking_session->session.who);
	if (err) {
		return err;
	}

	switch (locking_session->l_state) {
	case OPAL_READONLY:
		memcpy(uid_locking_range, spdk_opal_uid[UID_LOCKINGRANGE_ACE_RDLOCKED], OPAL_UID_LENGTH);
		break;
	case OPAL_READWRITE:
		memcpy(uid_locking_range, spdk_opal_uid[UID_LOCKINGRANGE_ACE_WRLOCKED], OPAL_UID_LENGTH);
		break;
	default:
		SPDK_ERRLOG("locking state should only be OPAL_READONLY or OPAL_READWRITE\n");
		return -EINVAL;
	}

	uid_locking_range[7] = locking_session->session.opal_key->locking_range;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid_locking_range, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 8,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_BOOLEAN_EXPR,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_HALF_AUTHORITY_OBJ_REF],
				  OPAL_UID_LENGTH / 2);
	opal_add_token_bytestring(&err, dev, uid_user, OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 2, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_HALF_AUTHORITY_OBJ_REF],
				  OPAL_UID_LENGTH / 2);
	opal_add_token_bytestring(&err, dev, uid_user, OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 2, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_HALF_BOOLEAN_ACE], OPAL_UID_LENGTH / 2);
	opal_add_tokens(&err, dev, 7,
			SPDK_OPAL_TRUE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	if (err) {
		SPDK_ERRLOG("Error building add user to locking range command\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int
opal_new_user_passwd(struct spdk_opal_dev *dev, struct opal_common_session *session)
{
	uint8_t uid_cpin[OPAL_UID_LENGTH];
	int ret;

	if (session->who == OPAL_ADMIN1) {
		memcpy(uid_cpin, spdk_opal_uid[UID_C_PIN_ADMIN1], OPAL_UID_LENGTH);
	} else {
		memcpy(uid_cpin, spdk_opal_uid[UID_C_PIN_USER1], OPAL_UID_LENGTH);
		uid_cpin[7] = session->who;
	}

	ret = opal_generic_pw_cmd(session->opal_key->key, session->opal_key->key_len, uid_cpin, dev);
	if (ret != 0) {
		SPDK_ERRLOG("Error building set password command\n");
		return ret;
	}

	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int
opal_set_sid_cpin_pin(struct spdk_opal_dev *dev, void *data)
{
	uint8_t cpin_uid[OPAL_UID_LENGTH];
	const char *new_passwd = data;
	struct spdk_opal_key opal_key;
	int ret;

	ret = opal_init_key(&opal_key, new_passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret != 0) {
		return ret;
	}

	memcpy(cpin_uid, spdk_opal_uid[UID_C_PIN_SID], OPAL_UID_LENGTH);

	if (opal_generic_pw_cmd(opal_key.key, opal_key.key_len, cpin_uid, dev)) {
		SPDK_ERRLOG("Error building Set SID cpin\n");
		return -ERANGE;
	}
	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

int
spdk_opal_cmd_take_ownership(struct spdk_opal_dev *dev, char *new_passwd)
{
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	pthread_mutex_lock(&dev->mutex_lock);
	opal_setup_dev(dev);
	ret = opal_start_anybody_adminsp_session(dev);
	if (ret) {
		SPDK_ERRLOG("start admin SP session error %d: %s\n", ret,
			    opal_error_to_human(ret));
		opal_end_session(dev);
		goto end;
	}

	ret = opal_get_msid_cpin_pin(dev);
	if (ret) {
		SPDK_ERRLOG("get msid error %d: %s\n", ret,
			    opal_error_to_human(ret));
		opal_end_session(dev);
		goto end;
	}

	ret = opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

	ret = opal_start_adminsp_session(dev, NULL); /* key stored in dev->prev_data */
	if (ret) {
		SPDK_ERRLOG("start admin SP session error %d: %s\n", ret,
			    opal_error_to_human(ret));
		opal_end_session(dev);
		goto end;
	}

	ret = opal_set_sid_cpin_pin(dev, new_passwd);
	if (ret) {
		SPDK_ERRLOG("set cpin error %d: %s\n", ret,
			    opal_error_to_human(ret));
		opal_end_session(dev);
		goto end;
	}

	ret = opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

end:
	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

struct spdk_opal_dev *
spdk_opal_init_dev(void *dev_handler)
{
	struct spdk_opal_dev *dev;
	struct spdk_opal_info *info;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	dev->dev_handler = dev_handler;

	info = calloc(1, sizeof(struct spdk_opal_info));
	if (info == NULL) {
		free(dev);
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	dev->opal_info = info;
	if (opal_check_support(dev) != 0) {
		SPDK_INFOLOG(SPDK_LOG_OPAL, "Opal is not supported on this device\n");
		dev->supported = false;
	}

	if (pthread_mutex_init(&dev->mutex_lock, NULL)) {
		SPDK_ERRLOG("Mutex init failed\n");
		free(dev->opal_info);
		free(dev);
		return NULL;
	}

	return dev;
}

int
spdk_opal_cmd_scan(struct spdk_opal_dev *dev)
{
	int ret;

	ret = opal_check_support(dev);
	if (ret) {
		SPDK_ERRLOG("check opal support failed: %d\n", ret);
		spdk_opal_close(dev);
	}
	return ret;
}

static int
opal_revert_tper(struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, spdk_opal_uid[UID_ADMINSP],
				  OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[REVERT_METHOD],
				  OPAL_UID_LENGTH);
	opal_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
	opal_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
	if (err) {
		SPDK_ERRLOG("Error building REVERT TPER command.\n");
	}

	return err;
}

static int
opal_gen_new_active_key(struct spdk_opal_dev *dev)
{
	uint8_t uid_data[OPAL_UID_LENGTH] = {0};
	int err = 0;
	int length;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	if (dev->prev_data == NULL || dev->prev_d_len == 0) {
		SPDK_ERRLOG("Error finding previous data to generate new active key\n");
		return -EINVAL;
	}

	length = spdk_min(dev->prev_d_len, OPAL_UID_LENGTH);
	memcpy(uid_data, dev->prev_data, length);
	free(dev->prev_data);
	dev->prev_data = NULL;

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid_data, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[GENKEY_METHOD],
				  OPAL_UID_LENGTH);

	opal_add_tokens(&err, dev, 2, SPDK_OPAL_STARTLIST, SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building new key generation command.\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
}

static int
opal_get_active_key_cb(struct spdk_opal_dev *dev, void *data)
{
	const char *active_key;
	size_t str_len;
	int error = 0;

	error = opal_parse_and_check_status(dev, NULL);
	if (error) {
		return error;
	}

	str_len = opal_response_get_string(&dev->parsed_resp, 4, &active_key);
	if (!active_key) {
		SPDK_ERRLOG("Couldn't extract active key from response\n");
		return -EINVAL;
	}

	dev->prev_d_len = str_len;
	dev->prev_data = calloc(1, str_len);
	if (!dev->prev_data) {
		SPDK_ERRLOG("memory allocation error\n");
		return -ENOMEM;
	}
	memcpy(dev->prev_data, active_key, str_len);

	SPDK_DEBUGLOG(SPDK_LOG_OPAL, "active key = %p\n", dev->prev_data);
	return 0;
}

static int
opal_get_active_key(struct spdk_opal_dev *dev, struct opal_common_session *session)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	uint8_t locking_range_id;
	int err = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);

	locking_range_id = session->opal_key->locking_range;
	err = opal_build_locking_range(uid_locking_range, OPAL_UID_LENGTH, locking_range_id);
	if (err) {
		return err;
	}

	opal_add_token_u8(&err, dev, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, dev, uid_locking_range, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, dev, spdk_opal_method[GET_METHOD],
				  OPAL_UID_LENGTH);
	opal_add_tokens(&err, dev, 12,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_ACTIVEKEY,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_ACTIVEKEY,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);

	if (err) {
		SPDK_ERRLOG("Error building get active key command.\n");
		return err;
	}

	return opal_finalize_and_send(dev, 1, opal_get_active_key_cb, NULL);
}

int
spdk_opal_cmd_revert_tper(struct spdk_opal_dev *dev, const char *passwd)
{
	int ret;
	struct spdk_opal_key opal_key;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret) {
		SPDK_ERRLOG("Init key failed\n");
		return ret;
	}

	pthread_mutex_lock(&dev->mutex_lock);
	opal_setup_dev(dev);

	ret = opal_start_adminsp_session(dev, &opal_key);
	if (ret) {
		opal_end_session(dev);
		SPDK_ERRLOG("Error on starting admin SP session with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

	ret = opal_revert_tper(dev);
	if (ret) {
		opal_end_session(dev);
		SPDK_ERRLOG("Error on reverting TPer with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

	ret = opal_finalize_and_send(dev, 1, opal_parse_and_check_status, NULL);
	if (ret) {
		opal_end_session(dev);
		SPDK_ERRLOG("Error on reverting TPer with error %d: %s\n", ret,
			    opal_error_to_human(ret));
	}

	/* Controller will terminate session. No "end session" here needed. */

end:
	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_revert_poll(struct spdk_opal_dev *dev)
{
	void *response = dev->resp;
	struct spdk_opal_header *header = response;
	int ret;

	assert(dev->revert_cb_fn);

	ret = spdk_nvme_ctrlr_security_receive(dev->dev_handler, SPDK_SCSI_SECP_TCG, dev->comid,
					       0, dev->resp, IO_BUFFER_LENGTH);
	if (ret) {
		SPDK_ERRLOG("Security Receive Error on dev = %p\n", dev);
		dev->revert_cb_fn(dev, dev->ctx, ret);
		return 0;
	}

	if (header->com_packet.outstanding_data == 0 &&
	    header->com_packet.min_transfer == 0) {
		ret = opal_parse_and_check_status(dev, NULL);
		dev->revert_cb_fn(dev, dev->ctx, ret);
		return 0;
	} else {
		memset(response, 0, IO_BUFFER_LENGTH);
	}

	return -EAGAIN;
}

int
spdk_opal_cmd_revert_tper_async(struct spdk_opal_dev *dev, const char *passwd,
				spdk_opal_revert_cb cb_fn, void *cb_ctx)
{
	int ret;
	struct spdk_opal_key opal_key;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	if (cb_fn == NULL) {
		SPDK_ERRLOG("No revert callback function specified.\n");
		return -EFAULT;
	}

	dev->revert_cb_fn = cb_fn;
	dev->ctx = cb_ctx;

	ret = opal_init_key(&opal_key, passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret) {
		SPDK_ERRLOG("Init key failed\n");
		return ret;
	}

	pthread_mutex_lock(&dev->mutex_lock);
	opal_setup_dev(dev);

	ret = opal_start_adminsp_session(dev, &opal_key);
	if (ret) {
		opal_end_session(dev);
		SPDK_ERRLOG("Error on starting admin SP session with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

	ret = opal_revert_tper(dev);
	if (ret) {
		opal_end_session(dev);
		SPDK_ERRLOG("Error on reverting TPer with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

	ret = opal_cmd_finalize(dev, dev->hsn, dev->tsn, true);    /* true: end of data */
	if (ret) {
		SPDK_ERRLOG("Error finalizing command buffer: %d\n", ret);
		goto end;
	}

	ret = opal_send_cmd(dev);
	if (ret) {
		SPDK_ERRLOG("Error sending opal command: %d\n", ret);
	}

	/* Controller will terminate session. No "end session" here needed. */

end:
	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_activate_locking_sp(struct spdk_opal_dev *dev, const char *passwd)
{
	struct spdk_opal_key opal_key;
	int ret;

	ret = opal_init_key(&opal_key, passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret != 0) {
		return ret;
	}

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_adminsp_session(dev, &opal_key);
	if (ret) {
		SPDK_ERRLOG("Error on starting admin SP session with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_get_locking_sp_lifecycle(dev);
	if (ret) {
		SPDK_ERRLOG("Error on getting SP lifecycle with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

	ret = opal_activate(dev);
	if (ret) {
		SPDK_ERRLOG("Error on activation with error %d: %s\n", ret,
			    opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("Error on ending session with error %d: %s\n", ret,
			    opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_lock_unlock(struct spdk_opal_dev *dev, enum spdk_opal_user user,
			  enum spdk_opal_lock_state flag, enum spdk_opal_locking_range locking_range,
			  const char *passwd)
{
	struct spdk_opal_locking_session locking_session;
	struct spdk_opal_key opal_key;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, locking_range);
	if (ret != 0) {
		return ret;
	}

	memset(&locking_session, 0, sizeof(struct spdk_opal_locking_session));
	locking_session.l_state = flag;
	locking_session.session.who = user;
	locking_session.session.opal_key = &opal_key;

	pthread_mutex_lock(&dev->mutex_lock);

	ret = opal_start_auth_session(dev, &locking_session.session);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_lock_unlock_range(dev, &locking_session);
	if (ret) {
		SPDK_ERRLOG("lock unlock range error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_setup_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user,
				  enum spdk_opal_locking_range locking_range_id, uint64_t range_start,
				  uint64_t range_length, const char *passwd)
{
	struct opal_locking_range_setup_session setup_session;
	struct spdk_opal_key opal_key;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, locking_range_id);
	if (ret != 0) {
		return ret;
	}

	memset(&setup_session, 0, sizeof(struct opal_locking_range_setup_session));
	setup_session.session.opal_key = &opal_key;
	setup_session.session.who = user;
	setup_session.id = locking_range_id;
	setup_session.range_length = range_length;
	setup_session.range_start = range_start;
	setup_session.read_lock_enabled = true;
	setup_session.write_lock_enabled = true;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_auth_session(dev, &setup_session.session);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_setup_locking_range(dev, &setup_session);
	if (ret) {
		SPDK_ERRLOG("setup locking range error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_get_max_ranges(struct spdk_opal_dev *dev, const char *passwd)
{
	struct spdk_opal_key opal_key;
	struct opal_common_session session;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret != 0) {
		return ret;
	}

	memset(&session, 0, sizeof(struct opal_common_session));
	session.opal_key = &opal_key;
	session.who = OPAL_ADMIN1;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_auth_session(dev, &session);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_get_max_ranges(dev);
	if (ret) {
		SPDK_ERRLOG("get max ranges error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_get_locking_range_info(struct spdk_opal_dev *dev, const char *passwd,
				     enum spdk_opal_user user_id,
				     enum spdk_opal_locking_range locking_range_id)
{
	struct spdk_opal_key opal_key;
	struct opal_common_session session;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, locking_range_id);
	if (ret != 0) {
		return ret;
	}

	memset(&session, 0, sizeof(struct opal_common_session));
	session.opal_key = &opal_key;
	session.who = user_id;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_auth_session(dev, &session);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_get_locking_range_info(dev, locking_range_id);
	if (ret) {
		SPDK_ERRLOG("get locking range info error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_enable_user(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
			  const char *passwd)
{
	struct spdk_opal_key opal_key;
	struct opal_common_session session;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret != 0) {
		return ret;
	}

	memset(&session, 0, sizeof(struct opal_common_session));
	session.opal_key = &opal_key;
	session.who = user_id;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_admin_session(dev, session.opal_key);
	if (ret) {
		SPDK_ERRLOG("start locking SP session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_enable_user(dev, &session);
	if (ret) {
		SPDK_ERRLOG("enable user error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_add_user_to_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
					enum spdk_opal_locking_range locking_range_id,
					enum spdk_opal_lock_state lock_flag, const char *passwd)
{
	struct spdk_opal_key opal_key;
	struct spdk_opal_locking_session locking_session;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, passwd, locking_range_id);
	if (ret != 0) {
		return ret;
	}

	memset(&locking_session, 0, sizeof(struct spdk_opal_locking_session));
	locking_session.session.opal_key = &opal_key;
	locking_session.session.who = user_id;
	locking_session.l_state = lock_flag;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_admin_session(dev, locking_session.session.opal_key);
	if (ret) {
		SPDK_ERRLOG("start locking SP session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_add_user_to_locking_range(dev, &locking_session);
	if (ret) {
		SPDK_ERRLOG("add user to locking range error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_set_new_passwd(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
			     const char *new_passwd, const char *old_passwd, bool new_user)
{
	struct spdk_opal_key old_key;
	struct spdk_opal_key new_key;
	struct spdk_opal_new_pw_session session;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&old_key, old_passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret != 0) {
		return ret;
	}

	ret = opal_init_key(&new_key, new_passwd, OPAL_LOCKING_RANGE_GLOBAL);
	if (ret != 0) {
		return ret;
	}

	memset(&session, 0, sizeof(struct spdk_opal_new_pw_session));
	session.new_session.who = user_id;
	session.new_session.opal_key = &new_key;
	session.old_session.who = new_user ? OPAL_ADMIN1 : user_id;
	session.old_session.opal_key = &old_key;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_auth_session(dev, &session.old_session);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_new_user_passwd(dev, &session.new_session);
	if (ret) {
		SPDK_ERRLOG("set new passwd error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

int
spdk_opal_cmd_erase_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
				  enum spdk_opal_locking_range locking_range_id, const char *password)
{
	struct opal_common_session session = {};
	struct spdk_opal_key opal_key;
	int ret;

	if (!dev || dev->supported == false) {
		return -ENODEV;
	}

	ret = opal_init_key(&opal_key, password, locking_range_id);
	if (ret != 0) {
		return ret;
	}

	session.opal_key = &opal_key;
	session.who = user_id;

	pthread_mutex_lock(&dev->mutex_lock);
	ret = opal_start_auth_session(dev, &session);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d: %s\n", ret, opal_error_to_human(ret));
		pthread_mutex_unlock(&dev->mutex_lock);
		return ret;
	}

	ret = opal_get_active_key(dev, &session);
	if (ret) {
		SPDK_ERRLOG("get active key error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

	ret = opal_gen_new_active_key(dev);
	if (ret) {
		SPDK_ERRLOG("generate new active key error %d: %s\n", ret, opal_error_to_human(ret));
		goto end;
	}

end:
	ret += opal_end_session(dev);
	if (ret) {
		SPDK_ERRLOG("end session error %d: %s\n", ret, opal_error_to_human(ret));
	}

	pthread_mutex_unlock(&dev->mutex_lock);
	return ret;
}

struct spdk_opal_info *
spdk_opal_get_info(struct spdk_opal_dev *dev)
{
	return dev->opal_info;
}

bool
spdk_opal_supported(struct spdk_opal_dev *dev)
{
	return dev->supported;
}

struct spdk_opal_locking_range_info *
spdk_opal_get_locking_range_info(struct spdk_opal_dev *dev, enum spdk_opal_locking_range id)
{
	return dev->locking_range_info[id];
}

void
spdk_opal_free_locking_range_info(struct spdk_opal_dev *dev, enum spdk_opal_locking_range id)
{
	struct spdk_opal_locking_range_info *info = dev->locking_range_info[id];

	free(info);
	dev->locking_range_info[id] = NULL;
}

uint8_t
spdk_opal_get_max_locking_ranges(struct spdk_opal_dev *dev)
{
	return dev->max_ranges;
}

/* Log component for opal submodule */
SPDK_LOG_REGISTER_COMPONENT("opal", SPDK_LOG_OPAL)
