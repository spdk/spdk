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

static void spdk_opal_print_buffer(uint8_t *buf, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_OPAL, "Opal printf buffer [%lu]:%d\n", i, buf[i]);
	}
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
	struct spdk_disk_info *opal_info = dev->opal_info;

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
	struct spdk_disk_info *opal_info = dev->opal_info;

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
	struct spdk_disk_info *opal_info = dev->opal_info;

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
	struct spdk_disk_info *opal_info = dev->opal_info;
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
	struct spdk_disk_info *opal_info = dev->opal_info;

	opal_info->datastore = 1;
	opal_info->datastore_max_tables = from_be16(&datastore->max_tables);
	opal_info->datastore_max_table_size = from_be32(&datastore->max_table_size);
	opal_info->datastore_alignment = from_be32(&datastore->alignment);
}

static uint16_t spdk_get_comid_v100(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_d0_opal_v100 *v100 = data;
	struct spdk_disk_info *opal_info = dev->opal_info;
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
	struct spdk_disk_info *opal_info = dev->opal_info;
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
	struct spdk_disk_info *info = calloc(1, sizeof(struct spdk_disk_info));
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
	return 0;
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

static void spdk_opal_close(struct spdk_opal_dev *dev)
{
	free(dev->opal_info);
	free(dev);
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

static void spdk_dump_opal_info(struct spdk_disk_info *opal)
{
	if (!opal->opal_ssc_dev) {
		SPDK_ERRLOG("This device is not Opal enabled. Not Supported!\n");
		return;
	}

	if (opal->tper) {
		printf("\nOpal TPer feature:\n");
		printf("ACKNACK = %s", (opal->tper_acknack ? "Y, " : "N, "));
		printf("ASYNC = %s", (opal->tper_async ? "Y, " : "N, "));
		printf("BufferManagement = %s\n", (opal->tper_buffer_mgt ? "Y, " : "N, "));
		printf("ComIDManagement = %s", (opal->tper_comid_mgt ? "Y, " : "N, "));
		printf("Streaming = %s", (opal->tper_streaming ? "Y, " : "N, "));
		printf("Sync = %s\n", (opal->tper_sync ? "Y" : "N"));
		printf("\n");
	}

	if (opal->locking) {
		printf("Opal Locking feature:\n");
		printf("Locked = %s", (opal->locking_locked ? "Y, " : "N, "));
		printf("Locking Enabled = %s", (opal->locking_locking_enabled ? "Y, " : "N, "));
		printf("Locking supported = %s\n", (opal->locking_locking_supported ? "Y" : "N"));


		printf("MBR done = %s", (opal->locking_mbr_done ? "Y, " : "N, "));
		printf("MBR enabled = %s", (opal->locking_mbr_enabled ? "Y, " : "N, "));
		printf("Media encrypt = %s\n", (opal->locking_media_encrypt ? "Y" : "N"));
		printf("\n");
	}

	if (opal->geometry) {
		printf("Opal Geometry feature:\n");
		printf("Align = %s", (opal->geometry_align ? "Y, " : "N, "));
		printf("Logical block size = %d, ", opal->geometry_logical_block_size);
		printf("Lowest aligned LBA = %ld\n", opal->geometry_lowest_aligned_lba);
		printf("\n");
	}

	if (opal->single_user_mode) {
		printf("Opal Single User Mode feature:\n");
		printf("Any in SUM = %s", (opal->single_user_any ? "Y, " : "N, "));
		printf("All in SUM = %s", (opal->single_user_all ? "Y, " : "N, "));
		printf("Policy: %s Authority,\n", (opal->single_user_policy ? "Admin" : "Users"));
		printf("Number of locking objects = %d\n ", opal->single_user_locking_objects);
		printf("\n");
	}

	if (opal->datastore) {
		printf("Opal DataStore feature:\n");
		printf("Table alignment = %d, ", opal->datastore_alignment);
		printf("Max number of tables = %d, ", opal->datastore_max_tables);
		printf("Max size of tables = %d\n", opal->datastore_max_table_size);
		printf("\n");
	}

	if (opal->opal_v100) {
		printf("Opal V100 feature:\n");
		printf("Base comID = %d, ", opal->opal_v100_base_comid);
		printf("Number of comIDs = %d, ", opal->opal_v100_num_comid);
		printf("Range crossing = %s\n", (opal->opal_v100_range_crossing ? "N" : "Y"));
		printf("\n");
	}

	if (opal->opal_v200) {
		printf("Opal V200 feature:\n");
		printf("Base comID = %d, ", opal->opal_v200_base_comid);
		printf("Number of comIDs = %d, ", opal->opal_v200_num_comid);
		printf("Initial PIN = %d,\n", opal->opal_v200_initial_pin);
		printf("Reverted PIN = %d, ", opal->opal_v200_reverted_pin);
		printf("Number of admins = %d, ", opal->opal_v200_num_admin);
		printf("Number of users = %d\n", opal->opal_v200_num_user);
		printf("\n\n");
	}
}

void spdk_opal_scan(struct spdk_opal_dev *dev)
{
	int ret;

	ret = spdk_check_opal_support(dev);
	if (ret) {
		SPDK_ERRLOG("check opal support failed: %d\n", ret);
		spdk_opal_close(dev);
		return;
	}

	spdk_dump_opal_info(dev->opal_info);
	spdk_opal_close(dev);
}

/* Log component for opal submodule */
SPDK_LOG_REGISTER_COMPONENT("opal", SPDK_LOG_OPAL)
