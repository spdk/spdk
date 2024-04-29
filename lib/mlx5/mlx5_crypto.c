/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma_utils.h"
#include "mlx5_ifc.h"

#define MLX5_VENDOR_ID_MELLANOX 0x2c9

/* Plaintext key sizes */
/* 64b keytag */
#define SPDK_MLX5_AES_XTS_KEYTAG_SIZE 8
/* key1_128b + key2_128b */
#define SPDK_MLX5_AES_XTS_128_DEK_BYTES 32
/* key1_256b + key2_256b */
#define SPDK_MLX5_AES_XTS_256_DEK_BYTES 64
/* key1_128b + key2_128b + 64b_keytag */
#define SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG (SPDK_MLX5_AES_XTS_128_DEK_BYTES + SPDK_MLX5_AES_XTS_KEYTAG_SIZE)
/* key1_256b + key2_256b + 64b_keytag */
#define SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG (SPDK_MLX5_AES_XTS_256_DEK_BYTES + SPDK_MLX5_AES_XTS_KEYTAG_SIZE)

static char **g_allowed_devices;
static size_t g_allowed_devices_count;

struct spdk_mlx5_crypto_dek {
	struct mlx5dv_dek *dek_obj;
	struct ibv_pd *pd;
	struct ibv_context *context;
};

struct spdk_mlx5_crypto_keytag {
	struct spdk_mlx5_crypto_dek *deks;
	uint32_t deks_num;
	bool has_keytag;
	char keytag[8];
};

static void
mlx5_crypto_devs_free(void)
{
	size_t i;

	if (!g_allowed_devices) {
		return;
	}

	for (i = 0; i < g_allowed_devices_count; i++) {
		free(g_allowed_devices[i]);
	}
	free(g_allowed_devices);
	g_allowed_devices = NULL;
	g_allowed_devices_count = 0;
}

static bool
mlx5_crypto_dev_allowed(const char *dev)
{
	size_t i;

	if (!g_allowed_devices || !g_allowed_devices_count) {
		return true;
	}

	for (i = 0; i < g_allowed_devices_count; i++) {
		if (strcmp(g_allowed_devices[i], dev) == 0) {
			return true;
		}
	}

	return false;
}

int
spdk_mlx5_crypto_devs_allow(const char *const dev_names[], size_t devs_count)
{
	size_t i;

	mlx5_crypto_devs_free();

	if (!dev_names || !devs_count) {
		return 0;
	}

	g_allowed_devices = calloc(devs_count, sizeof(char *));
	if (!g_allowed_devices) {
		return -ENOMEM;
	}
	for (i = 0; i < devs_count; i++) {
		g_allowed_devices[i] = strndup(dev_names[i], SPDK_MLX5_DEV_MAX_NAME_LEN);
		if (!g_allowed_devices[i]) {
			mlx5_crypto_devs_free();
			return -ENOMEM;
		}
		g_allowed_devices_count++;
	}

	return 0;
}

struct ibv_context **
spdk_mlx5_crypto_devs_get(int *dev_num)
{
	struct ibv_context **rdma_devs, **rdma_devs_out = NULL, *dev;
	struct ibv_device_attr dev_attr;
	struct ibv_port_attr port_attr;
	struct spdk_mlx5_device_caps dev_caps;
	uint8_t in[DEVX_ST_SZ_BYTES(query_nic_vport_context_in)];
	uint8_t out[DEVX_ST_SZ_BYTES(query_nic_vport_context_out)];
	uint8_t devx_v;
	int num_rdma_devs = 0, i, rc;
	int num_crypto_devs = 0;

	/* query all devices, save mlx5 with crypto support */
	rdma_devs = rdma_get_devices(&num_rdma_devs);
	if (!rdma_devs || !num_rdma_devs) {
		*dev_num = 0;
		return NULL;
	}

	rdma_devs_out = calloc(num_rdma_devs + 1, sizeof(*rdma_devs_out));
	if (!rdma_devs_out) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	for (i = 0; i < num_rdma_devs; i++) {
		dev = rdma_devs[i];
		rc = ibv_query_device(dev, &dev_attr);
		if (rc) {
			SPDK_ERRLOG("Failed to query dev %s, skipping\n", dev->device->name);
			continue;
		}
		if (dev_attr.vendor_id != MLX5_VENDOR_ID_MELLANOX) {
			SPDK_DEBUGLOG(mlx5, "dev %s is not Mellanox device, skipping\n", dev->device->name);
			continue;
		}

		if (!mlx5_crypto_dev_allowed(dev->device->name)) {
			continue;
		}

		rc = ibv_query_port(dev, 1, &port_attr);
		if (rc) {
			SPDK_ERRLOG("Failed to query port attributes for device %s, rc %d\n", dev->device->name, rc);
			continue;
		}

		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
			/* Port may be ethernet but roce is still disabled */
			memset(in, 0, sizeof(in));
			memset(out, 0, sizeof(out));
			DEVX_SET(query_nic_vport_context_in, in, opcode, MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
			rc = mlx5dv_devx_general_cmd(dev, in, sizeof(in), out, sizeof(out));
			if (rc) {
				SPDK_ERRLOG("Failed to get VPORT context for device %s. Assuming ROCE is disabled\n",
					    dev->device->name);
				continue;
			}

			devx_v = DEVX_GET(query_nic_vport_context_out, out, nic_vport_context.roce_en);
			if (!devx_v) {
				SPDK_ERRLOG("Device %s, RoCE disabled\n", dev->device->name);
				continue;
			}
		}

		memset(&dev_caps, 0, sizeof(dev_caps));
		rc = spdk_mlx5_device_query_caps(dev, &dev_caps);
		if (rc) {
			SPDK_ERRLOG("Failed to query mlx5 dev %s, skipping\n", dev->device->name);
			continue;
		}
		if (!dev_caps.crypto_supported) {
			SPDK_WARNLOG("dev %s crypto engine doesn't support crypto\n", dev->device->name);
			continue;
		}
		if (!(dev_caps.crypto.single_block_le_tweak || dev_caps.crypto.multi_block_le_tweak ||
		      dev_caps.crypto.multi_block_be_tweak)) {
			SPDK_WARNLOG("dev %s crypto engine doesn't support AES_XTS\n", dev->device->name);
			continue;
		}
		if (dev_caps.crypto.wrapped_import_method_aes_xts) {
			SPDK_WARNLOG("dev %s uses wrapped import method which is not supported by mlx5 lib\n",
				     dev->device->name);
			continue;
		}

		rdma_devs_out[num_crypto_devs++] = dev;
	}

	if (!num_crypto_devs) {
		SPDK_DEBUGLOG(mlx5, "Found no mlx5 crypto devices\n");
		goto err_out;
	}

	rdma_free_devices(rdma_devs);
	*dev_num = num_crypto_devs;

	return rdma_devs_out;

err_out:
	free(rdma_devs_out);
	rdma_free_devices(rdma_devs);
	*dev_num = 0;
	return NULL;
}

void
spdk_mlx5_crypto_devs_release(struct ibv_context **rdma_devs)
{
	if (rdma_devs) {
		free(rdma_devs);
	}
}

int
spdk_mlx5_device_query_caps(struct ibv_context *context, struct spdk_mlx5_device_caps *caps)
{
	uint16_t opmod = MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE |
			 HCA_CAP_OPMOD_GET_CUR;
	uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {};
	int rc;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, opmod);

	rc = mlx5dv_devx_general_cmd(context, in, sizeof(in), out, sizeof(out));
	if (rc) {
		return rc;
	}

	caps->crypto_supported = DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.crypto);
	if (!caps->crypto_supported) {
		return 0;
	}

	caps->crypto.single_block_le_tweak = DEVX_GET(query_hca_cap_out,
					     out, capability.cmd_hca_cap.aes_xts_single_block_le_tweak);
	caps->crypto.multi_block_be_tweak = DEVX_GET(query_hca_cap_out, out,
					    capability.cmd_hca_cap.aes_xts_multi_block_be_tweak);
	caps->crypto.multi_block_le_tweak = DEVX_GET(query_hca_cap_out, out,
					    capability.cmd_hca_cap.aes_xts_multi_block_le_tweak);

	opmod = MLX5_SET_HCA_CAP_OP_MOD_CRYPTO | HCA_CAP_OPMOD_GET_CUR;
	memset(&out, 0, sizeof(out));
	memset(&in, 0, sizeof(in));

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, opmod);

	rc = mlx5dv_devx_general_cmd(context, in, sizeof(in), out, sizeof(out));
	if (rc) {
		return rc;
	}

	caps->crypto.wrapped_crypto_operational = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_caps.wrapped_crypto_operational);
	caps->crypto.wrapped_crypto_going_to_commissioning = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_caps .wrapped_crypto_going_to_commissioning);
	caps->crypto.wrapped_import_method_aes_xts = (DEVX_GET(query_hca_cap_out, out,
			capability.crypto_caps.wrapped_import_method) &
			MLX5_CRYPTO_CAPS_WRAPPED_IMPORT_METHOD_AES) != 0;

	return 0;
}

void
spdk_mlx5_crypto_keytag_destroy(struct spdk_mlx5_crypto_keytag *keytag)
{
	struct spdk_mlx5_crypto_dek *dek;
	uint32_t i;

	if (!keytag) {
		return;
	}

	for (i = 0; i < keytag->deks_num; i++) {
		dek = &keytag->deks[i];
		if (dek->dek_obj) {
			mlx5dv_dek_destroy(dek->dek_obj);
		}
		if (dek->pd) {
			spdk_rdma_utils_put_pd(dek->pd);
		}
	}
	spdk_memset_s(keytag->keytag, sizeof(keytag->keytag), 0, sizeof(keytag->keytag));
	free(keytag->deks);
	free(keytag);
}

int
spdk_mlx5_crypto_keytag_create(struct spdk_mlx5_crypto_dek_create_attr *attr,
			       struct spdk_mlx5_crypto_keytag **out)
{
	struct spdk_mlx5_crypto_dek *dek;
	struct spdk_mlx5_crypto_keytag *keytag;
	struct ibv_context **devs;
	struct mlx5dv_dek_init_attr init_attr = {};
	struct mlx5dv_dek_attr query_attr;
	int num_devs = 0, i, rc;
	bool has_keytag;


	if (!attr || !attr->dek) {
		return -EINVAL;
	}
	switch (attr->dek_len) {
	case SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG:
		init_attr.key_size = MLX5DV_CRYPTO_KEY_SIZE_128;
		has_keytag = true;
		SPDK_DEBUGLOG(mlx5, "128b AES_XTS with keytag\n");
		break;
	case SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG:
		init_attr.key_size = MLX5DV_CRYPTO_KEY_SIZE_256;
		has_keytag = true;
		SPDK_DEBUGLOG(mlx5, "256b AES_XTS with keytag\n");
		break;
	case SPDK_MLX5_AES_XTS_128_DEK_BYTES:
		init_attr.key_size = MLX5DV_CRYPTO_KEY_SIZE_128;
		has_keytag = false;
		SPDK_DEBUGLOG(mlx5, "128b AES_XTS\n");
		break;
	case SPDK_MLX5_AES_XTS_256_DEK_BYTES:
		init_attr.key_size = MLX5DV_CRYPTO_KEY_SIZE_256;
		has_keytag = false;
		SPDK_DEBUGLOG(mlx5, "256b AES_XTS\n");
		break;
	default:
		SPDK_ERRLOG("Invalid key length %zu. The following keys are supported:\n"
			    "128b key + key2, %u bytes;\n"
			    "256b key + key2, %u bytes\n"
			    "128b key + key2 + keytag, %u bytes\n"
			    "256b lye + key2 + keytag, %u bytes\n",
			    attr->dek_len, SPDK_MLX5_AES_XTS_128_DEK_BYTES, MLX5DV_CRYPTO_KEY_SIZE_256,
			    SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG, SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG);
		return -EINVAL;
	}

	devs = spdk_mlx5_crypto_devs_get(&num_devs);
	if (!devs || !num_devs) {
		SPDK_DEBUGLOG(mlx5, "No crypto devices found\n");
		return -ENOTSUP;
	}

	keytag = calloc(1, sizeof(*keytag));
	if (!keytag) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_mlx5_crypto_devs_release(devs);
		return -ENOMEM;
	}
	keytag->deks = calloc(num_devs, sizeof(struct spdk_mlx5_crypto_dek));
	if (!keytag->deks) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_mlx5_crypto_devs_release(devs);
		free(keytag);
		return -ENOMEM;
	}

	for (i = 0; i < num_devs; i++) {
		keytag->deks_num++;
		dek = &keytag->deks[i];
		dek->pd = spdk_rdma_utils_get_pd(devs[i]);
		if (!dek->pd) {
			SPDK_ERRLOG("Failed to get PD on device %s\n", devs[i]->device->name);
			rc = -EINVAL;
			goto err_out;
		}
		dek->context = devs[i];

		init_attr.pd = dek->pd;
		init_attr.has_keytag = has_keytag;
		init_attr.key_purpose = MLX5DV_CRYPTO_KEY_PURPOSE_AES_XTS;
		init_attr.comp_mask = MLX5DV_DEK_INIT_ATTR_CRYPTO_LOGIN;
		init_attr.crypto_login = NULL;
		memcpy(init_attr.key, attr->dek, attr->dek_len);

		dek->dek_obj = mlx5dv_dek_create(dek->context, &init_attr);
		spdk_memset_s(init_attr.key, sizeof(init_attr.key), 0, sizeof(init_attr.key));
		if (!dek->dek_obj) {
			SPDK_ERRLOG("mlx5dv_dek_create failed on dev %s, errno %d\n", dek->context->device->name, errno);
			rc = -EINVAL;
			goto err_out;
		}

		memset(&query_attr, 0, sizeof(query_attr));
		rc = mlx5dv_dek_query(dek->dek_obj, &query_attr);
		if (rc) {
			SPDK_ERRLOG("Failed to query DEK on dev %s, rc %d\n", dek->context->device->name, rc);
			goto err_out;
		}
		if (query_attr.state != MLX5DV_DEK_STATE_READY) {
			SPDK_ERRLOG("DEK on dev %s state %d\n", dek->context->device->name, query_attr.state);
			rc = -EINVAL;
			goto err_out;
		}
	}

	if (has_keytag) {
		/* Save keytag, it will be used to configure crypto MKEY */
		keytag->has_keytag = true;
		memcpy(keytag->keytag, attr->dek + attr->dek_len - SPDK_MLX5_AES_XTS_KEYTAG_SIZE,
		       SPDK_MLX5_AES_XTS_KEYTAG_SIZE);
	}

	spdk_mlx5_crypto_devs_release(devs);
	*out = keytag;

	return 0;

err_out:
	spdk_mlx5_crypto_keytag_destroy(keytag);
	spdk_mlx5_crypto_devs_release(devs);

	return rc;
}

static inline struct spdk_mlx5_crypto_dek *
mlx5_crypto_get_dek_by_pd(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd)
{
	struct spdk_mlx5_crypto_dek *dek;
	uint32_t i;

	for (i = 0; i < keytag->deks_num; i++) {
		dek = &keytag->deks[i];
		if (dek->pd == pd) {
			return dek;
		}
	}

	return NULL;
}

int
spdk_mlx5_crypto_set_attr(struct mlx5dv_crypto_attr *attr_out,
			  struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
			  uint32_t block_size, uint64_t iv, bool encrypt_on_tx)
{
	struct spdk_mlx5_crypto_dek *dek;
	enum mlx5dv_block_size bs;

	dek = mlx5_crypto_get_dek_by_pd(keytag, pd);
	if (spdk_unlikely(!dek)) {
		SPDK_ERRLOG("No DEK for pd %p (dev %s)\n", pd, pd->context->device->name);
		return -EINVAL;
	}

	switch (block_size) {
	case 512:
		bs = MLX5DV_BLOCK_SIZE_512;
		break;
	case 520:
		bs = MLX5DV_BLOCK_SIZE_520;
		break;
	case 4048:
		bs = MLX5DV_BLOCK_SIZE_4048;
		break;
	case 4096:
		bs = MLX5DV_BLOCK_SIZE_4096;
		break;
	case 4160:
		bs = MLX5DV_BLOCK_SIZE_4160;
		break;
	default:
		SPDK_ERRLOG("Unsupported block size %u\n", block_size);
		return -EINVAL;
	}

	memset(attr_out, 0, sizeof(*attr_out));
	attr_out->dek = dek->dek_obj;
	attr_out->crypto_standard = MLX5DV_CRYPTO_STANDARD_AES_XTS;
	attr_out->data_unit_size = bs;
	attr_out->encrypt_on_tx = encrypt_on_tx;
	memcpy(attr_out->initial_tweak, &iv, sizeof(iv));
	if (keytag->has_keytag) {
		memcpy(attr_out->keytag, keytag->keytag, sizeof(keytag->keytag));
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(mlx5)
