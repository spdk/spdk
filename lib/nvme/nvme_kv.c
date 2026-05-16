/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/nvme_kv.h"
#include "nvme_internal.h"

const struct spdk_nvme_kv_ns_data *
spdk_nvme_kv_ns_get_data(const struct spdk_nvme_ns *ns)
{
	return ns->nsdata_kv;
}

const struct spdk_nvme_kv_ctrlr_data *
spdk_nvme_kv_ctrlr_get_data(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata_kv;
}

static inline void
nvme_kv_cmd_set_key(struct spdk_nvme_cmd *cmd, const void *key, uint8_t key_len)
{
	assert(key != NULL && key_len >= SPDK_NVME_KV_KEY_MIN_LEN && key_len <= SPDK_NVME_KV_KEY_MAX_LEN);

	cmd->cdw11_bits.kv.kl = key_len;

	memcpy((uint8_t *)&cmd->cdw2, key, spdk_min(key_len, 8));

	if (key_len > 8) {
		memcpy((uint8_t *)&cmd->cdw14, (const uint8_t *)key + 8,
		       spdk_min(key_len - 8, 8));
	}
}

static int
nvme_kv_cmd_with_data(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      uint8_t opc, const void *key, uint8_t key_len,
		      void *data, uint32_t data_len, uint8_t options,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	if (key == NULL || key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN ||
	    data == NULL || data_len == 0) {
		return -EINVAL;
	}

	req = nvme_allocate_request_contig(qpair, data, data_len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

	/* CDW10: Value size (Store) or Host buffer size (Retrieve/List) */
	cmd->cdw10_bits.kv.vsize = data_len;

	/* CDW11: Key length and request options */
	cmd->cdw11_bits.kv.ro = options;

	nvme_kv_cmd_set_key(cmd, key, key_len);

	return nvme_qpair_submit_request(qpair, req);
}

static int
nvme_kv_cmd_without_data(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			 uint8_t opc, const void *key, uint8_t key_len,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	if (key == NULL || key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN) {
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

	/* CDW11: Key length */
	nvme_kv_cmd_set_key(cmd, key, key_len);

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_kv_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		   const void *key, uint8_t key_len,
		   const void *value, uint32_t value_len,
		   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		   uint8_t options)
{
	return nvme_kv_cmd_with_data(ns, qpair, SPDK_NVME_OPC_KV_STORE,
				     key, key_len, (void *)value, value_len, options,
				     cb_fn, cb_arg);
}

int
spdk_nvme_kv_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      const void *key, uint8_t key_len,
		      void *value, uint32_t value_len,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		      uint8_t options)
{
	return nvme_kv_cmd_with_data(ns, qpair, SPDK_NVME_OPC_KV_RETRIEVE,
				     key, key_len, value, value_len, options,
				     cb_fn, cb_arg);
}

int
spdk_nvme_kv_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		    const void *key, uint8_t key_len,
		    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_kv_cmd_without_data(ns, qpair, SPDK_NVME_OPC_KV_DELETE,
					key, key_len, cb_fn, cb_arg);
}

int
spdk_nvme_kv_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		   const void *key, uint8_t key_len,
		   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_kv_cmd_without_data(ns, qpair, SPDK_NVME_OPC_KV_EXIST,
					key, key_len, cb_fn, cb_arg);
}

int
spdk_nvme_kv_list(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		  const void *start_key, uint8_t start_key_len,
		  void *buffer, uint32_t buffer_len,
		  spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	const void *key;
	uint8_t key_len;

	if (buffer == NULL || buffer_len == 0) {
		return -EINVAL;
	}

	if (start_key == NULL) {
		if (start_key_len != 0) {
			return -EINVAL;
		}
		key = NULL;
		key_len = 0;
	} else {
		if (start_key_len < SPDK_NVME_KV_KEY_MIN_LEN || start_key_len > SPDK_NVME_KV_KEY_MAX_LEN) {
			return -EINVAL;
		}
		key = start_key;
		key_len = start_key_len;
	}

	req = nvme_allocate_request_contig(qpair, buffer, buffer_len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_LIST;
	cmd->nsid = ns->id;

	/* CDW10: Host buffer size */
	cmd->cdw10_bits.kv.vsize = buffer_len;

	if (key != NULL) {
		nvme_kv_cmd_set_key(cmd, key, key_len);
	}

	return nvme_qpair_submit_request(qpair, req);
}
