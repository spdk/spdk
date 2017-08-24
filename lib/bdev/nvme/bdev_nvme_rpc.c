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

#include "bdev_nvme.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"
#include "spdk_internal/bdev.h"

struct open_descriptors {
	void *desc;
	TAILQ_ENTRY(open_descriptors) tqlst;
};
typedef TAILQ_HEAD(, open_descriptors) open_descriptors_t;

struct rpc_construct_nvme {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
};

static void
free_rpc_construct_nvme(struct rpc_construct_nvme *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->subnqn);
}

static const struct spdk_json_object_decoder rpc_construct_nvme_decoders[] = {
	{"name", offsetof(struct rpc_construct_nvme, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_construct_nvme, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_construct_nvme, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_construct_nvme, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_construct_nvme, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_construct_nvme, subnqn), spdk_json_decode_string, true},
};

#define NVME_MAX_BDEVS_PER_RPC 32

static void
spdk_rpc_construct_nvme_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_nvme req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nvme_transport_id trid = {};
	const char *names[NVME_MAX_BDEVS_PER_RPC];
	size_t count;
	size_t i;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_nvme_decoders,
				    SPDK_COUNTOF(rpc_construct_nvme_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
		goto invalid;
	}

	/* Parse traddr */
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", req.traddr);

	/* Parse adrfam */
	if (req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req.adrfam);
			goto invalid;
		}
	}

	/* Parse trsvcid */
	if (req.trsvcid) {
		snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", req.trsvcid);
	}

	/* Parse subnqn */
	if (req.subnqn) {
		snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", req.subnqn);
	}

	count = NVME_MAX_BDEVS_PER_RPC;
	if (spdk_bdev_nvme_create(&trid, req.name, names, &count)) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_construct_nvme(&req);
		return;
	}

	spdk_json_write_array_begin(w);
	for (i = 0; i < count; i++) {
		spdk_json_write_string(w, names[i]);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_construct_nvme(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_nvme(&req);
}
SPDK_RPC_REGISTER("construct_nvme_bdev", spdk_rpc_construct_nvme_bdev)

struct rpc_apply_firmware {
	char *filename;
	char *trtype;
	char *pci_address;
	char *adrfam;
	char *trsvcid;
	char *subnqn;
};

static void
free_rpc_apply_firmware(struct rpc_apply_firmware *req)
{
	free(req->filename);
	free(req->trtype);
	free(req->pci_address);
	free(req->adrfam);
	free(req->trsvcid);
	free(req->subnqn);
}

static const struct spdk_json_object_decoder rpc_apply_firmware_decoders[] = {
	{"filename", offsetof(struct rpc_apply_firmware, filename), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_apply_firmware, trtype), spdk_json_decode_string},
	{"pci_address", offsetof(struct rpc_apply_firmware, pci_address), spdk_json_decode_string},
	{"adrfam", offsetof(struct rpc_apply_firmware, adrfam), spdk_json_decode_string},
	{"trsvcid", offsetof(struct rpc_apply_firmware, trsvcid), spdk_json_decode_string},
	{"subnqn", offsetof(struct rpc_apply_firmware, subnqn), spdk_json_decode_string},
};

static void
spdk_rpc_apply_nvme_firmware(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	int                                     rc;
	int                                     fd = -1;
	int                                     slot = 0;
	unsigned int                            size;
	struct stat                             fw_stat;
	void                                    *fw_image;
	struct rpc_apply_firmware req           = {};
	struct spdk_json_write_ctx              *w;
	struct spdk_nvme_transport_id           trid;
	struct spdk_nvme_ctrlr                  *ctrlr;
	struct spdk_nvme_ctrlr                  *ctrlr_tmp;
	char                                    msg[1024];
	struct spdk_nvme_status                 status;
	struct spdk_bdev			*bdev;
	struct spdk_bdev_desc			*desc;
	struct open_descriptors			*opt;
	open_descriptors_t			desc_head;

	if (spdk_json_decode_object(params, rpc_apply_firmware_decoders,
				    SPDK_COUNTOF(rpc_apply_firmware_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, req.trtype);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "failed to parse trtype.");
		free_rpc_apply_firmware(&req);
		return;
	}

	/* Parse traddr */
	if (req.pci_address) {
		snprintf(trid.traddr, sizeof(trid.traddr), "%s", req.pci_address);
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "PCI is too long.");
		free_rpc_apply_firmware(&req);
		return;
	}


	if (req.adrfam && strcmp(req.adrfam, "null") != 0) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, req.adrfam);
		if (rc < 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "failed to parse adrfam.");
			free_rpc_apply_firmware(&req);
			return;
		}
	}

	if (req.trsvcid && strcmp(req.trsvcid, "null") != 0) {
		snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", req.trsvcid);
	}

	if (req.subnqn && strcmp(req.subnqn, "null") != 0) {
		snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", req.subnqn);
	}

	if ((ctrlr = spdk_nvme_ctrlr_get(&trid)) == NULL) {
		sprintf(msg, "Device %s was not found", req.pci_address);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		free_rpc_apply_firmware(&req);
		return;
	}

	TAILQ_INIT(&desc_head);
	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {

		ctrlr_tmp = spdk_bdev_get_ctrlr(bdev);
		if (ctrlr_tmp != ctrlr) {
			continue;
		}

		if ((rc = spdk_bdev_open(bdev, true, NULL, NULL, &desc)) != 0) {
			/*
			 * If a bdev is already opened, do not allow firmware update.
			 */

			TAILQ_FOREACH(opt, &desc_head, tqlst) {
				spdk_bdev_close(opt->desc);
			}

			sprintf(msg, "Device %s is in use.", req.pci_address);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
			free_rpc_apply_firmware(&req);
			return;

		} else {
			opt = malloc(sizeof(struct open_descriptors));
			opt->desc = desc;
			TAILQ_INSERT_TAIL(&desc_head, opt, tqlst);
		}
	}

	fd = open(req.filename, O_RDONLY);
	if (fd < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "open file failed.");
		free_rpc_apply_firmware(&req);
		TAILQ_FOREACH(opt, &desc_head, tqlst) {
			spdk_bdev_close(opt->desc);
		}
		return;
	}

	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "fstat failed.");
		free_rpc_apply_firmware(&req);
		TAILQ_FOREACH(opt, &desc_head, tqlst) {
			spdk_bdev_close(opt->desc);
		}
		return;
	}

	if (fw_stat.st_size % 4) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Firmware image size is not multiple of 4.");
		free_rpc_apply_firmware(&req);
		TAILQ_FOREACH(opt, &desc_head, tqlst) {
			spdk_bdev_close(opt->desc);
		}
		return;
	}

	size = fw_stat.st_size;
	fw_image = spdk_dma_zmalloc(size, 4096, NULL);
	if (!fw_image) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		free_rpc_apply_firmware(&req);
		TAILQ_FOREACH(opt, &desc_head, tqlst) {
			spdk_bdev_close(opt->desc);
		}
		return;
	}

	if (read(fd, fw_image, size) != ((ssize_t)(size))) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Read firmware image failed!");
		spdk_dma_free(fw_image);
		free_rpc_apply_firmware(&req);
		TAILQ_FOREACH(opt, &desc_head, tqlst) {
			spdk_bdev_close(opt->desc);
		}
		return;
	}
	close(fd);

	rc = spdk_nvme_ctrlr_update_firmware(ctrlr, fw_image, size, slot,
					     SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG, &status);

	TAILQ_FOREACH(opt, &desc_head, tqlst) {
		spdk_bdev_close(opt->desc);
	}
	spdk_dma_free(fw_image);
	free_rpc_apply_firmware(&req);

	if (rc == -ENXIO && status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC &&
	    status.sc == SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET) {
		snprintf(msg, sizeof(msg), "conventional reset is needed to enable firmware.");
	} else if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_nvme_ctrlr_update_firmware failed..");
		return;
	} else {
		snprintf(msg, sizeof(msg), "spdk_nvme_ctrlr_update_firmware success.");
	}

	if (!(w = spdk_jsonrpc_begin_result(request))) {
		return;
	}
	spdk_json_write_string(w, msg);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("apply_nvme_firmware", spdk_rpc_apply_nvme_firmware)
