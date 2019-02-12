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

typedef int (spdk_opal_cb)(struct spdk_opal_dev *dev);

static int opal_end_session_error(struct spdk_opal_dev *dev);

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
opal_send_recv(struct spdk_opal_dev *dev, spdk_opal_cb *cb)
{
	int ret;

	ret = opal_send_cmd(dev);
	if (ret) {
		return ret;
	}
	ret = opal_recv_cmd(dev);
	if (ret) {
		return ret;
	}
	return cb(dev);
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

static int
opal_cmd_finalize(struct spdk_opal_dev *dev, uint32_t hsn, uint32_t tsn, bool eod)
{
	struct spdk_opal_header *hdr;
	int err = 0;

	if (eod) {
		opal_add_token_u8(&err, dev, SPDK_OPAL_ENDOFDATA);
		opal_add_token_u8(&err, dev, SPDK_OPAL_STARTLIST);
		opal_add_token_u8(&err, dev, 0);
		opal_add_token_u8(&err, dev, 0);
		opal_add_token_u8(&err, dev, 0);
		opal_add_token_u8(&err, dev, SPDK_OPAL_ENDLIST);
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

static int
opal_finalize_and_send(struct spdk_opal_dev *dev, bool eod, spdk_opal_cb cb)
{
	int ret;

	ret = opal_cmd_finalize(dev, dev->hsn, dev->tsn, eod);
	if (ret) {
		SPDK_ERRLOG("Error finalizing command buffer: %d\n", ret);
		return ret;
	}

	return opal_send_recv(dev, cb);
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

static size_t
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

static inline bool
opal_response_token_matches(const struct spdk_opal_resp_token *token,
			    uint8_t match)
{
	if (!token ||
	    token->type != OPAL_DTA_TOKENID_TOKEN ||
	    token->pos[0] != match) {
		SPDK_ERRLOG("Token not matching\n");
		return false;
	}
	return true;
}

static const struct spdk_opal_resp_token *
opal_response_get_token(const struct spdk_opal_resp_parsed *resp, int n)
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

static uint64_t
opal_response_get_u64(const struct spdk_opal_resp_parsed *resp, int n)
{
	if (!resp) {
		SPDK_ERRLOG("Response is NULL\n");
		return 0;
	}

	if (resp->resp_tokens[n].type != OPAL_DTA_TOKENID_UINT) {
		SPDK_ERRLOG("Token is not unsigned int: %d\n",
			    resp->resp_tokens[n].type);
		return 0;
	}

	if (!(resp->resp_tokens[n].width == OPAL_WIDTH_TINY ||
	      resp->resp_tokens[n].width == OPAL_WIDTH_SHORT)) {
		SPDK_ERRLOG("Atom is not short or tiny: %d\n",
			    resp->resp_tokens[n].width);
		return 0;
	}

	return resp->resp_tokens[n].stored.unsigned_num;
}

static uint64_t
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

	tok = opal_response_get_token(resp, resp->num - 5);
	if (!opal_response_token_matches(tok, SPDK_OPAL_STARTLIST)) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	tok = opal_response_get_token(resp, resp->num - 1);
	if (!opal_response_token_matches(tok, SPDK_OPAL_ENDLIST)) {
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	/* The second and third values in the status list are reserved, and are
	defined in core spec to be 0x00 and 0x00 and SHOULD be ignored by the TPer. */
	return opal_response_get_u64(resp, resp->num - 4);
}

static uint64_t
opal_parse_and_check_status(struct spdk_opal_dev *dev)
{
	uint64_t error;

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

static int
opal_next(struct spdk_opal_dev *dev)
{
	const struct spdk_opal_step *step;
	int state = 0, error = 0;

	do {
		step = &dev->steps[state];
		if (!step->opal_fn) {
			if (state != 0) {
				break;
			} else {
				SPDK_ERRLOG("First step is NULL\n");
				return -1;
			}
		}

		error = step->opal_fn(dev, step->data);
		if (error) {
			SPDK_ERRLOG("Error on step function: %d with error %d: %s\n",
				    state, error,
				    opal_error_to_human(error));
			if (state > 1) {
				opal_end_session_error(dev);
				return error;
			}
		}
		state++;
	} while (!error);

	return error;
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
	opal_info->geometry_logical_block_size = from_be64(&geo->logical_block_size);
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
			SPDK_NOTICELOG("Unknow feature code: %d\n", feature_code);
		}
		cpos += body->tper.length + 4;
	}

	if (supported == false) {
		SPDK_ERRLOG("Opal Not Supported.\n");
		return SPDK_OPAL_NOT_SUPPORTED;
	}

	if (single_user == false) {
		SPDK_NOTICELOG("Single User Mode Not Supported\n");
	}

	if (found_com_id == false) {
		SPDK_ERRLOG("Could not find OPAL comid for device. Returning early\n");
		return -EINVAL;
	}

	dev->comid = comid;
	return 0;
}

static int
opal_discovery0(struct spdk_opal_dev *dev, void *data)
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
opal_setup_dev(struct spdk_opal_dev *dev,
	       const struct spdk_opal_step *steps)
{
	dev->steps = steps;
	dev->tsn = 0;
	dev->hsn = 0;
	dev->prev_data = NULL;
	dev->timeout = SPDK_OPAL_TPER_TIMEOUT;
}

static int
opal_end_session_cb(struct spdk_opal_dev *dev)
{
	dev->hsn = 0;
	dev->tsn = 0;
	return opal_parse_and_check_status(dev);
}

static int
opal_end_session(struct spdk_opal_dev *dev, void *data)
{
	int err = 0;
	bool eod = 0;

	opal_clear_cmd(dev);
	opal_set_comid(dev, dev->comid);
	opal_add_token_u8(&err, dev, SPDK_OPAL_ENDOFSESSION);

	if (err < 0) {
		return err;
	}
	return opal_finalize_and_send(dev, eod, opal_end_session_cb);
}

static int
opal_end_session_error(struct spdk_opal_dev *dev)
{
	const struct spdk_opal_step error_end_session[] = {
		{ opal_end_session, },
		{ NULL, }
	};
	dev->steps = error_end_session;
	return opal_next(dev);
}

static int
opal_check_support(struct spdk_opal_dev *dev)
{
	const struct spdk_opal_step steps[] = {
		{ opal_discovery0, },
		{ NULL, }
	};
	int ret;

	opal_setup_dev(dev, steps);
	ret = opal_next(dev);
	if (ret == 0) {
		dev->supported = true;
	} else {
		dev->supported = false;
	}

	return ret;
}

void
spdk_opal_close(struct spdk_opal_dev *dev)
{
	free(dev->opal_info);
	free(dev);
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
	return dev;
}

void
spdk_opal_scan(struct spdk_opal_dev *dev)
{
	int ret;

	ret = opal_check_support(dev);
	if (ret) {
		SPDK_ERRLOG("check opal support failed: %d\n", ret);
		spdk_opal_close(dev);
		return;
	}
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

/* Log component for opal submodule */
SPDK_LOG_REGISTER_COMPONENT("opal", SPDK_LOG_OPAL)
