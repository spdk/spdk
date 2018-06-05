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

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"
#include "bdev_nvme.h"

#if defined(__linux__) && defined(SPDK_CONFIG_NVME_IOCTL)

#include <linux/fs.h>
#include <linux/nvme_ioctl.h>

#ifndef NVME_IOCTL_RESCAN
#define NVME_IOCTL_RESCAN	_IO('N', 0x46)
#endif

/*
 * Get cmd_len of blk ioctl from ioctl_cmd
 */
static uint32_t
blk_ioctl_cmd_size(uint32_t ioctl_cmd)
{
	uint32_t cmd_len = 0;

	switch (ioctl_cmd) {
	case BLKRRPART:
		break;
	case BLKGETSIZE:
		cmd_len = sizeof(long);
		break;
	case BLKSSZGET:
		cmd_len = sizeof(int);
		break;
	case BLKBSZGET:
		cmd_len = sizeof(unsigned int);
		break;
	case BLKGETSIZE64:
		cmd_len = sizeof(unsigned long long);
		break;
	case BLKPBSZGET:
		cmd_len = sizeof(unsigned int);
		break;
	default:
		SPDK_NOTICELOG("Unknown blk ioctl_cmd %d\n", ioctl_cmd);
	}

	return cmd_len;
}

static uint32_t
nvme_ioctl_cmd_size(uint32_t ioctl_cmd)
{
	return _IOC_SIZE(ioctl_cmd);
}

static uint32_t
nvme_ioctl_data_size(uint32_t ioctl_cmd, char *cmd_buf, struct spdk_nvme_ns *ns)
{
	struct nvme_user_io *io_cmd;
	struct nvme_passthru_cmd *adm_cmd;
	uint32_t data_size = 0;

	switch (ioctl_cmd) {
	case NVME_IOCTL_ID:
	case NVME_IOCTL_RESET:
	case NVME_IOCTL_SUBSYS_RESET:
	case NVME_IOCTL_RESCAN:
		break;
	case NVME_IOCTL_ADMIN_CMD:
	case NVME_IOCTL_IO_CMD:
		adm_cmd = (struct nvme_passthru_cmd *)cmd_buf;
		data_size = adm_cmd->data_len;
		break;
	case NVME_IOCTL_SUBMIT_IO:
		if (ns) {
			io_cmd = (struct nvme_user_io *)cmd_buf;
			if (spdk_nvme_ns_supports_extended_lba(ns)) {
				data_size = (io_cmd->nblocks + 1) * (spdk_nvme_ns_get_sector_size(ns)
								     + spdk_nvme_ns_get_md_size(ns));
			} else {
				data_size = (io_cmd->nblocks + 1) * spdk_nvme_ns_get_sector_size(ns);
			}
		} else {
			SPDK_NOTICELOG("NVME_IOCTL_SUBMIT_IO needs struct spdk_nvme_ns\n");
		}
		break;
	default:
		SPDK_NOTICELOG("Unknown nvme ioctl_cmd %d\n", ioctl_cmd);
	}

	return data_size;
}

static uint32_t
nvme_ioctl_metadata_size(uint32_t ioctl_cmd, char *cmd_buf, struct spdk_nvme_ns *ns)
{
	struct nvme_user_io *io_cmd;
	struct nvme_passthru_cmd *adm_cmd;
	uint32_t md_size = 0;

	switch (ioctl_cmd) {
	case NVME_IOCTL_ID:
	case NVME_IOCTL_RESET:
	case NVME_IOCTL_SUBSYS_RESET:
	case NVME_IOCTL_RESCAN:
		break;
	case NVME_IOCTL_ADMIN_CMD:
	case NVME_IOCTL_IO_CMD:
		adm_cmd = (struct nvme_passthru_cmd *)cmd_buf;
		md_size = adm_cmd->metadata_len;
		break;
	case NVME_IOCTL_SUBMIT_IO:
		if (ns) {
			io_cmd = (struct nvme_user_io *)cmd_buf;
			if (spdk_nvme_ns_supports_extended_lba(ns)) {
				md_size = 0;
			} else {
				md_size = (io_cmd->nblocks + 1) * spdk_nvme_ns_get_md_size(ns);
			}
		} else {
			SPDK_NOTICELOG("NVME_IOCTL_SUBMIT_IO needs struct spdk_nvme_ns\n");
		}
		break;
	default:
		SPDK_NOTICELOG("Unknown nvme ioctl_cmd %d\n", ioctl_cmd);
	}

	return md_size;
}

static enum spdk_nvme_data_transfer
spdk_nvme_cmd_get_data_transfer(uint32_t ioctl_cmd, char *cmd_buf) {
	uint8_t opc = 0;
	struct nvme_user_io *io_cmd;
	struct nvme_passthru_cmd *adm_cmd;

	switch (ioctl_cmd)
	{
	case NVME_IOCTL_ID:
	case NVME_IOCTL_RESET:
	case NVME_IOCTL_SUBSYS_RESET:
	case NVME_IOCTL_RESCAN:
		break;
	case NVME_IOCTL_ADMIN_CMD:
	case NVME_IOCTL_IO_CMD:
		adm_cmd = (struct nvme_passthru_cmd *)cmd_buf;
		opc =  adm_cmd->opcode;
		break;
	case NVME_IOCTL_SUBMIT_IO:
		io_cmd = (struct nvme_user_io *)cmd_buf;
		opc = io_cmd->opcode;
		break;
	default:
		SPDK_NOTICELOG("Unknown nvme ioctl_cmd %d\n", ioctl_cmd);
	}

	return (enum spdk_nvme_data_transfer)(opc & 3);
}

/*
 * Determine next recv state for blk ioctl
 * Get cmd_len of blk ioctl from ioctl_cmd
 */
static uint32_t
blk_ioctl_cmd_recv_check(uint32_t ioctl_cmd, enum ioctl_conn_state_t *_conn_state)
{
	*_conn_state = IOCTL_CONN_STATE_PROC;

	return blk_ioctl_cmd_size(ioctl_cmd);
}

/*
 * Set cmd_len and determine next recv state
 */
int
nvme_ioctl_cmd_recv_check(struct spdk_nvme_ioctl_req *req, enum ioctl_conn_state_t *_conn_state)
{
	uint32_t ioctl_cmd;
	char ioctl_magic;

	ioctl_cmd = req->ioctl_cmd;
	ioctl_magic = _IOC_TYPE(ioctl_cmd);

	switch (ioctl_magic) {
	case BLK_IOCTL_MAGIC:
		req->cmd_len = blk_ioctl_cmd_recv_check(ioctl_cmd, _conn_state);
		break;
	case NVME_IOCTL_MAGIC:
		req->cmd_len = nvme_ioctl_cmd_size(ioctl_cmd);
		if (req->cmd_len) {
			*_conn_state = IOCTL_CONN_STATE_RECV_CMD;
		} else {
			*_conn_state = IOCTL_CONN_STATE_PROC;
		}
		break;
	default:
		SPDK_NOTICELOG("Unknown ioctl_magic %d\n", ioctl_magic);
		req->cmd_len = 0;
		*_conn_state = IOCTL_CONN_STATE_PROC;
	}

	if (req->cmd_len) {
		req->cmd_buf = calloc(1, req->cmd_len);
		if (!req->cmd_buf) {
			SPDK_ERRLOG("Failed to allocate memory for req->cmd_buf\n");
			return -ENOMEM;
		}
	}

	return 0;
}

/*
 * Set data_len and md_len; determine next recv state.
 * Notes: this func need to know ioctl_conn type and spdk_nvme_ns from ioctl_conn
 */
int
nvme_ioctl_cmdbuf_recv_check(struct spdk_nvme_ioctl_req *req, enum ioctl_conn_state_t *_conn_state,
			     struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ns *ns = NULL;

	/* data_len & md_len of NVME_IOCTL_SUBMIT_IO is also determined by info of ns */
	if (req->ioctl_cmd == NVME_IOCTL_SUBMIT_IO && ioctl_conn->type == IOCTL_CONN_TYPE_BLK) {
		ns = ((struct nvme_bdev *)ioctl_conn->device)->ns;
	}

	req->data_len = nvme_ioctl_data_size(req->ioctl_cmd, (char *)req->cmd_buf, ns);
	req->md_len = nvme_ioctl_metadata_size(req->ioctl_cmd, (char *)req->cmd_buf, ns);
	if (req->data_len) {
		req->data = spdk_dma_zmalloc(req->data_len, 0, NULL);
		if (!req->data) {
			SPDK_ERRLOG("Failed to allocate memory for req->data\n");
			return -ENOMEM;
		}
	}
	if (req->md_len) {
		req->metadata = spdk_dma_zmalloc(req->md_len, 0, NULL);
		if (!req->metadata) {
			SPDK_ERRLOG("Failed to allocate memory for req->metadata\n");
			return -ENOMEM;
		}
	}

	/* determine next recv state based on data transfer direction */
	enum spdk_nvme_data_transfer xfer = spdk_nvme_cmd_get_data_transfer(req->ioctl_cmd, req->cmd_buf);
	if (xfer != SPDK_NVME_DATA_HOST_TO_CONTROLLER && xfer != SPDK_NVME_DATA_BIDIRECTIONAL) {
		/* no need to receiving data and metadata */
		*_conn_state = IOCTL_CONN_STATE_PROC;
	} else if (req->data_len) {
		*_conn_state = IOCTL_CONN_STATE_RECV_DATA;
	} else if (req->md_len) {
		*_conn_state = IOCTL_CONN_STATE_RECV_METADATA;
	} else {
		*_conn_state = IOCTL_CONN_STATE_PROC;
	}

	return 0;
}

int
nvme_ioctl_cmd_xmit_check(struct spdk_nvme_ioctl_resp *resp, enum ioctl_conn_state_t *_conn_state)
{
	/* Stop xmit if ioctl_ret shows error or cmd_len==0 */
	if ((int)resp->ioctl_ret < 0 || !resp->cmd_len) {
		*_conn_state = IOCTL_CONN_STATE_RECV_HEAD;
	} else {
		*_conn_state = IOCTL_CONN_STATE_XMIT_CMD;
	}

	return 0;
}

/*
 * Only determine next recv state.
 * When xmit resp, all kinds of lens already have been set.
 */
int
nvme_ioctl_cmdbuf_xmit_check(struct spdk_nvme_ioctl_resp *resp,
			     enum ioctl_conn_state_t *_conn_state)
{
	/* check data transfer direction */

	char ioctl_magic;
	enum spdk_nvme_data_transfer xfer = SPDK_NVME_DATA_NONE;

	ioctl_magic = _IOC_TYPE(resp->ioctl_cmd);
	if (ioctl_magic ==  NVME_IOCTL_MAGIC) {
		xfer = spdk_nvme_cmd_get_data_transfer(resp->ioctl_cmd, resp->cmd_buf);
	}

	if (xfer != SPDK_NVME_DATA_CONTROLLER_TO_HOST && xfer != SPDK_NVME_DATA_BIDIRECTIONAL) {
		/* no need to transmit data and metadata */
		*_conn_state = IOCTL_CONN_STATE_RECV_HEAD;
	} else if (resp->data_len) {
		*_conn_state = IOCTL_CONN_STATE_XMIT_DATA;
	} else if (resp->md_len) {
		*_conn_state = IOCTL_CONN_STATE_XMIT_METADATA;
	} else {
		*_conn_state = IOCTL_CONN_STATE_RECV_HEAD;
	}

	return 0;
}

static void
nvme_ioctl_resp_get_lens(struct spdk_nvme_ioctl_resp *resp, struct spdk_nvme_ioctl_req *req)
{
	char ioctl_magic;

	resp->total_len += IOCTL_HEAD_SIZE + sizeof(int);

	ioctl_magic = _IOC_TYPE(resp->ioctl_cmd);
	if (ioctl_magic != NVME_IOCTL_MAGIC) {
		resp->cmd_len = blk_ioctl_cmd_size(resp->ioctl_cmd);
		resp->total_len += resp->cmd_len;
		return;
	}

	resp->cmd_len = req->cmd_len;
	enum spdk_nvme_data_transfer xfer = spdk_nvme_cmd_get_data_transfer(resp->ioctl_cmd, resp->cmd_buf);
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST || xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		/* xmit data and metadata */
		resp->data_len = req->data_len;
		resp->md_len = req->md_len;
	}

	resp->total_len += resp->cmd_len + resp->data_len + resp->md_len;
	return;
}

static void
nvme_ioctl_construct_resp(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ioctl_req *req;
	struct spdk_nvme_ioctl_resp *resp;

	resp = &ioctl_conn->resp;
	req = &ioctl_conn->req;

	/* Assign allocated resources from request to response */
	resp->ioctl_cmd = req->ioctl_cmd;
	resp->cmd_buf = req->cmd_buf;
	resp->data = req->data;
	resp->metadata = req->metadata;

	resp->handle = req->handle;
	resp->resp_magic = IOCTL_RESP_MAGIC;
	nvme_ioctl_resp_get_lens(resp, req);
}

static int
spdk_nvme_ioctl_proc_done(struct spdk_nvme_ioctl_conn *ioctl_conn, int ioctl_ret)
{
	struct spdk_nvme_ioctl_resp *resp;
	int ret = 0;

	nvme_ioctl_construct_resp(ioctl_conn);

	resp = &ioctl_conn->resp;
	/*
	 * resp->ioctl_ret may be already set by previous steps,
	 * like checking elements of request after receiving them.
	 */
	if (!resp->ioctl_ret) {
		resp->ioctl_ret = ioctl_ret;
	}

	/* check whether ioctl-conn should stop and release */
	if (ioctl_conn->state == IOCTL_CONN_STATE_CLOSE) {
		spdk_nvme_ioctl_conn_free(ioctl_conn);
	} else {
		ioctl_conn->state = IOCTL_CONN_STATE_XMIT_HEAD;
		ret = spdk_nvme_ioctl_conn_xmit(ioctl_conn);
	}

	return ret;
}

static int
spdk_nvme_ioctl_blk_proc(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ioctl_req *req;
	int ioctl_ret = 0;
	uint32_t ioctl_cmd;
	struct spdk_nvme_ns *ns;
	unsigned int val_uint;
	long val_l;
	unsigned long long val_ull;

	if (ioctl_conn->type != IOCTL_CONN_TYPE_BLK) {
		ioctl_ret = -ENOTBLK;
		goto end;
	}

	req = &ioctl_conn->req;
	ioctl_cmd = req->ioctl_cmd;
	ns = ((struct nvme_bdev *)ioctl_conn->device)->ns;
	switch (ioctl_cmd) {
	case BLKRRPART:
		break;
	case BLKGETSIZE:
		val_l = spdk_nvme_ns_get_num_sectors(ns);
		memcpy(req->cmd_buf, &val_l, sizeof(val_l));
		break;
	case BLKGETSIZE64:
		val_ull = spdk_nvme_ns_get_size(ns);
		memcpy(req->cmd_buf, &val_ull, sizeof(val_ull));
		break;
	case BLKBSZGET:
	case BLKSSZGET:
	case BLKPBSZGET:
		val_uint = spdk_nvme_ns_get_sector_size(ns);
		memcpy(req->cmd_buf, &val_uint, sizeof(val_uint));
		break;
	default:
		SPDK_NOTICELOG("Unknown blk ioctl_cmd %d\n", ioctl_cmd);
		ioctl_ret = -1;
	}

end:
	return spdk_nvme_ioctl_proc_done(ioctl_conn, ioctl_ret);
}

static int
spdk_nvme_ioctl_id_proc(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ns *ns = NULL;
	int ret;

	/* ioctl(fd, NVME_IOCTL_ID), return value is id */
	if (ioctl_conn->type == IOCTL_CONN_TYPE_BLK) {
		ns = ((struct nvme_bdev *)ioctl_conn->device)->ns;
		ret = spdk_nvme_ns_get_id(ns);
	} else {
		ret = -ENOTBLK;
	}

	ret = spdk_nvme_ioctl_proc_done(ioctl_conn, ret);
	return ret;
}

static inline int
nvme_spdk_get_error_code(const struct spdk_nvme_cpl *cpl)
{
	return (cpl->status.sct << 8) | cpl->status.sc;
}

static void
spdk_nvme_ioctl_psthr_cmd_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ioctl_conn *ioctl_conn = arg;
	struct nvme_passthru_cmd *adm_cmd;
	int ioctl_ret;

	adm_cmd = (struct nvme_passthru_cmd *)ioctl_conn->req.cmd_buf;
	adm_cmd->result = cpl->cdw0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_NOTICELOG("passthru command error: SC %x SCT %x\n", cpl->status.sc, cpl->status.sct);
		/* keep consistency with kernel driver, return <sct<<8|sc> */
		ioctl_ret = nvme_spdk_get_error_code(cpl);
	} else {
		ioctl_ret = 0;
	}

	spdk_nvme_ioctl_proc_done(ioctl_conn, ioctl_ret);
}

static int
spdk_nvme_ioctl_admin_cmd_proc(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ioctl_req *req;
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	struct nvme_passthru_cmd *cmd;
	int ret;

	req = &ioctl_conn->req;
	cmd = (struct nvme_passthru_cmd *)req->cmd_buf;
	cmd->addr = (__u64)req->data;
	cmd->metadata = (__u64)req->metadata;

	if (ioctl_conn->type == IOCTL_CONN_TYPE_BLK) {
		ctrlr = ((struct nvme_bdev *)ioctl_conn->device)->nvme_ctrlr->ctrlr;
	} else {
		ctrlr = ((struct nvme_ctrlr *)ioctl_conn->device)->ctrlr;
	}

	ret = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr,
					    (struct spdk_nvme_cmd *)cmd,
					    (void *)cmd->addr,
					    cmd->data_len,
					    spdk_nvme_ioctl_psthr_cmd_done, ioctl_conn);

	if (ret < 0) {
		ret = spdk_nvme_ioctl_proc_done(ioctl_conn, ret);
	}

	return ret;
}

static void
spdk_nvme_ioctl_bdev_io_done(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_nvme_ioctl_conn *ioctl_conn = arg;
	int ioctl_ret;
	int sct, sc;

	if (success) {
		ioctl_ret = 0;
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
		ioctl_ret = sct << 8 | sc;
		SPDK_NOTICELOG("submit_io command error: SC %x SCT %x\n", sc, sct);
	}

	spdk_nvme_ioctl_proc_done(ioctl_conn, ioctl_ret);
	spdk_bdev_free_io(bdev_io);
}

static int
spdk_nvme_ioctl_submit_io_proc(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ioctl_req *req;
	struct nvme_bdev *bdev = NULL;
	struct nvme_user_io *io_cmd;
	int ret = 0;

	if (ioctl_conn->type != IOCTL_CONN_TYPE_BLK) {
		return spdk_nvme_ioctl_proc_done(ioctl_conn, -ENOTBLK);
	}

	bdev = (struct nvme_bdev *)ioctl_conn->device;
	req = &ioctl_conn->req;
	io_cmd = (struct nvme_user_io *)req->cmd_buf;

	switch (io_cmd->opcode) {
	case SPDK_NVME_OPC_READ:
		ret = spdk_bdev_read_blocks(bdev->bdev_desc, bdev->bdev_ch,
					    req->data, io_cmd->slba, io_cmd->nblocks + 1,
					    spdk_nvme_ioctl_bdev_io_done, ioctl_conn);
		break;
	case SPDK_NVME_OPC_WRITE:
		ret = spdk_bdev_write_blocks(bdev->bdev_desc, bdev->bdev_ch,
					     req->data, io_cmd->slba, io_cmd->nblocks + 1,
					     spdk_nvme_ioctl_bdev_io_done, ioctl_conn);
		break;
	case SPDK_NVME_OPC_COMPARE:
	default:
		SPDK_NOTICELOG("unsupported io_cmd opcode %d\n", io_cmd->opcode);
		break;
	}

	if (ret < 0) {
		ret = spdk_nvme_ioctl_proc_done(ioctl_conn, ret);
	}

	return ret;
}

int
spdk_nvme_ioctl_proc(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	uint32_t ioctl_cmd;
	char ioctl_magic;
	int ret = 0;

	ioctl_cmd = ioctl_conn->req.ioctl_cmd;

	/* filter out blk ioctl */
	ioctl_magic = _IOC_TYPE(ioctl_cmd);
	if (ioctl_magic == BLK_IOCTL_MAGIC) {
		return spdk_nvme_ioctl_blk_proc(ioctl_conn);
	}

	switch (ioctl_cmd) {
	case NVME_IOCTL_ID:
		ret = spdk_nvme_ioctl_id_proc(ioctl_conn);
		break;
	case NVME_IOCTL_ADMIN_CMD:
		ret = spdk_nvme_ioctl_admin_cmd_proc(ioctl_conn);
		break;
	case NVME_IOCTL_SUBMIT_IO:
		ret = spdk_nvme_ioctl_submit_io_proc(ioctl_conn);
		break;
	case NVME_IOCTL_IO_CMD:
	case NVME_IOCTL_RESET:
	case NVME_IOCTL_SUBSYS_RESET:
	case NVME_IOCTL_RESCAN:
		SPDK_NOTICELOG("Unsupported nvme ioctl_cmd %d\n", ioctl_cmd);
		ret = spdk_nvme_ioctl_proc_done(ioctl_conn, -EINVAL);
		break;
	default:
		SPDK_NOTICELOG("Unknown nvme ioctl_cmd %d\n", ioctl_cmd);
		ret = spdk_nvme_ioctl_proc_done(ioctl_conn, -EINVAL);
	}

	return ret;
}

#else

int
nvme_ioctl_cmd_recv_check(struct spdk_nvme_ioctl_req *req,
			  enum ioctl_conn_state_t *_conn_state)
{
	return 0;
}

int
nvme_ioctl_cmdbuf_recv_check(struct spdk_nvme_ioctl_req *req,
			     enum ioctl_conn_state_t *_conn_state,
			     struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	return 0;
}

int
nvme_ioctl_cmd_xmit_check(struct spdk_nvme_ioctl_resp *resp,
			  enum ioctl_conn_state_t *_conn_state)
{
	return 0;
}

int
nvme_ioctl_cmdbuf_xmit_check(struct spdk_nvme_ioctl_resp *resp,
			     enum ioctl_conn_state_t *_conn_state)
{
	return 0;
}

int
spdk_nvme_ioctl_proc(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	return 0;
}

#endif
