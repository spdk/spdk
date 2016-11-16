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

#include "blockdev_nvme.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/nvme.h"

struct rpc_construct_nvme {
	char *pci_address;
};

static void
free_rpc_construct_nvme(struct rpc_construct_nvme *req)
{
	free(req->pci_address);
}

static const struct spdk_json_object_decoder rpc_construct_nvme_decoders[] = {
	{"pci_address", offsetof(struct rpc_construct_nvme, pci_address), spdk_json_decode_string},
};

static void
spdk_rpc_construct_nvme_bdev(struct spdk_jsonrpc_server_conn *conn,
			     const struct spdk_json_val *params,
			     const struct spdk_json_val *id)
{
	struct rpc_construct_nvme req = {};
	struct spdk_json_write_ctx *w;
	struct nvme_probe_ctx ctx = {};
	int i;

	if (spdk_json_decode_object(params, rpc_construct_nvme_decoders,
				    sizeof(rpc_construct_nvme_decoders) / sizeof(*rpc_construct_nvme_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	ctx.controllers_remaining = 1;
	ctx.num_whitelist_controllers = 1;

	if (spdk_pci_addr_parse(&ctx.whitelist[0], req.pci_address) < 0) {
		goto invalid;
	}

	if (spdk_bdev_nvme_create(&ctx)) {
		goto invalid;
	}

	free_rpc_construct_nvme(&req);

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	for (i = 0; i < ctx.num_created_bdevs; i++) {
		spdk_json_write_string(w, ctx.created_bdevs[i]->name);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_nvme(&req);
}
SPDK_RPC_REGISTER("construct_nvme_bdev", spdk_rpc_construct_nvme_bdev)

static void
spdk_rpc_get_nvme_devices(struct spdk_jsonrpc_server_conn *conn,
			  const struct spdk_json_val *params,
			  const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct nvme_device *dev;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	union spdk_nvme_vs_register vs;
	char buf[128];
	int i, num_ns;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_nvme_devices requires no parameters");
		return;
	}

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);

	for (dev = blockdev_nvme_device_first(); dev != NULL;
	     dev = blockdev_nvme_device_next(dev)) {
		spdk_json_write_object_begin(w);

		ctrlr = dev->ctrlr;
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		vs = spdk_nvme_ctrlr_get_regs_vs(ctrlr);

		snprintf(buf, sizeof(buf), "%04x:%02x:%02x.%x", dev->pci_addr.domain,
			 dev->pci_addr.bus, dev->pci_addr.dev, dev->pci_addr.func);
		spdk_json_write_name(w, "PCI Address");
		spdk_json_write_string(w, buf);

		snprintf(buf, sizeof(buf), "%#04x", cdata->vid);
		spdk_json_write_name(w, "Vendor ID");
		spdk_json_write_string(w, buf);

		snprintf(buf, sizeof(cdata->mn) + 1, "%s", cdata->mn);
		spdk_str_trim(buf);
		spdk_json_write_name(w, "Model Number");
		spdk_json_write_string(w, buf);

		snprintf(buf, sizeof(cdata->sn) + 1, "%s", cdata->sn);
		spdk_str_trim(buf);
		spdk_json_write_name(w, "Serial Number");
		spdk_json_write_string(w, buf);

		snprintf(buf, sizeof(cdata->fr) + 1, "%s", cdata->fr);
		spdk_str_trim(buf);
		spdk_json_write_name(w, "Firmware Revision");
		spdk_json_write_string(w, buf);

		snprintf(buf, sizeof(buf), "%u.%u", vs.bits.mjr, vs.bits.mnr);
		if (vs.bits.ter) {
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
				 ".%u", vs.bits.ter);
		}
		spdk_json_write_name(w, "NVMe Specification Version");
		spdk_json_write_string(w, buf);

		spdk_json_write_name(w, "Namespaces");
		spdk_json_write_array_begin(w);

		num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
		for (i = 1; i <= num_ns; i++) {
			spdk_json_write_object_begin(w);

			ns = spdk_nvme_ctrlr_get_ns(ctrlr, i);
			spdk_nvme_ns_get_data(ns);

			spdk_json_write_name(w, "Namespace ID");
			spdk_json_write_uint32(w, spdk_nvme_ns_get_id(ns));

			spdk_json_write_name(w, "Total Size (in bytes)");
			spdk_json_write_uint64(w, spdk_nvme_ns_get_size(ns));

			spdk_json_write_name(w, "Sector Size (in bytes)");
			spdk_json_write_uint32(w, spdk_nvme_ns_get_sector_size(ns));

			spdk_json_write_object_end(w);
		}

		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
}
SPDK_RPC_REGISTER("get_nvme_devices", spdk_rpc_get_nvme_devices)
