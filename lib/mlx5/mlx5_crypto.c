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
#include "mlx5_priv.h"

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

struct mlx5_crypto_dek_init_attr {
	char *dek;
	uint64_t opaque;
	uint32_t key_size_bytes;
	uint8_t key_size; /* driver representation of \b key_size_bytes */
	uint8_t keytag;
};

struct mlx5_crypto_dek_query_attr {
	/* state either MLX5_ENCRYPTION_KEY_OBJ_STATE_READY or MLX5_ENCRYPTION_KEY_OBJ_STATE_ERROR */
	uint8_t state;
	uint64_t opaque;
};

struct mlx5_crypto_dek {
	struct mlx5dv_devx_obj *devx_obj;
	struct ibv_pd *pd;
	struct ibv_context *context;
	/* Cached dek_obj_id */
	uint32_t dek_obj_id;
	enum spdk_mlx5_crypto_key_tweak_mode tweak_mode;
};

struct spdk_mlx5_crypto_keytag {
	struct mlx5_crypto_dek *deks;
	uint32_t deks_num;
	bool has_keytag;
	char keytag[8];
};

static char **g_allowed_devices;
static size_t g_allowed_devices_count;

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
		if (dev_attr.vendor_id != SPDK_MLX5_VENDOR_ID_MELLANOX) {
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

	caps->crc32c_supported = DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.sho) &&
				 DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.sig_crc32c);

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

static void
mlx5_crypto_dek_deinit(struct mlx5_crypto_dek *dek)
{
	int rc;

	rc = mlx5dv_devx_obj_destroy(dek->devx_obj);
	if (rc) {
		SPDK_ERRLOG("Failed to destroy crypto obj:%p, rc %d\n", dek->devx_obj, rc);
	}
}

void
spdk_mlx5_crypto_keytag_destroy(struct spdk_mlx5_crypto_keytag *keytag)
{
	struct mlx5_crypto_dek *dek;
	uint32_t i;

	if (!keytag) {
		return;
	}

	for (i = 0; i < keytag->deks_num; i++) {
		dek = &keytag->deks[i];
		if (dek->devx_obj) {
			mlx5_crypto_dek_deinit(dek);
		}
		if (dek->pd) {
			spdk_rdma_utils_put_pd(dek->pd);
		}
	}
	spdk_memset_s(keytag->keytag, sizeof(keytag->keytag), 0, sizeof(keytag->keytag));
	free(keytag->deks);
	free(keytag);
}

static int
mlx5_crypto_dek_init(struct ibv_pd *pd, struct mlx5_crypto_dek_init_attr *attr,
		     struct mlx5_crypto_dek *dek)
{
	uint32_t in[DEVX_ST_SZ_DW(create_encryption_key_obj_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(general_obj_out_cmd_hdr)] = {};
	uint8_t *dek_in;
	uint32_t pdn;
	int rc;

	rc = mlx5_get_pd_id(pd, &pdn);
	if (rc) {
		return rc;
	}

	dek_in = DEVX_ADDR_OF(create_encryption_key_obj_in, in, hdr);
	DEVX_SET(general_obj_in_cmd_hdr, dek_in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, dek_in, obj_type, MLX5_OBJ_TYPE_DEK);
	dek_in = DEVX_ADDR_OF(create_encryption_key_obj_in, in, key_obj);
	DEVX_SET(encryption_key_obj, dek_in, key_size, attr->key_size);
	DEVX_SET(encryption_key_obj, dek_in, has_keytag, attr->keytag);
	DEVX_SET(encryption_key_obj, dek_in, key_purpose, MLX5_ENCRYPTION_KEY_OBJ_KEY_PURPOSE_AES_XTS);
	DEVX_SET(encryption_key_obj, dek_in, pd, pdn);
	memcpy(DEVX_ADDR_OF(encryption_key_obj, dek_in, opaque), &attr->opaque, sizeof(attr->opaque));
	memcpy(DEVX_ADDR_OF(encryption_key_obj, dek_in, key), attr->dek, attr->key_size_bytes);

	dek->devx_obj = mlx5dv_devx_obj_create(pd->context, in, sizeof(in), out, sizeof(out));
	spdk_memset_s(DEVX_ADDR_OF(encryption_key_obj, dek_in, key), attr->key_size_bytes, 0,
		      attr->key_size_bytes);
	if (!dek->devx_obj) {
		return -errno;
	}
	dek->dek_obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);

	return 0;
}

static int
mlx5_crypto_dek_query(struct mlx5_crypto_dek *dek, struct mlx5_crypto_dek_query_attr *attr)
{
	uint32_t out[DEVX_ST_SZ_DW(query_encryption_key_obj_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	uint8_t *dek_out;
	int rc;

	assert(attr);
	DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_DEK);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, dek->dek_obj_id);

	rc = mlx5dv_devx_obj_query(dek->devx_obj, in, sizeof(in), out, sizeof(out));
	if (rc) {
		return rc;
	}

	dek_out = DEVX_ADDR_OF(query_encryption_key_obj_out, out, obj);
	attr->state = DEVX_GET(encryption_key_obj, dek_out, state);
	memcpy(&attr->opaque, DEVX_ADDR_OF(encryption_key_obj, dek_out, opaque), sizeof(attr->opaque));

	return 0;
}

int
spdk_mlx5_crypto_keytag_create(struct spdk_mlx5_crypto_dek_create_attr *attr,
			       struct spdk_mlx5_crypto_keytag **out)
{
	struct mlx5_crypto_dek *dek;
	struct spdk_mlx5_crypto_keytag *keytag;
	struct ibv_context **devs;
	struct ibv_pd *pd;
	struct mlx5_crypto_dek_init_attr dek_attr = {};
	struct mlx5_crypto_dek_query_attr query_attr;
	struct spdk_mlx5_device_caps dev_caps;
	int num_devs = 0, i, rc;

	dek_attr.dek = attr->dek;
	dek_attr.key_size_bytes = attr->dek_len;
	dek_attr.opaque = 0;
	switch (dek_attr.key_size_bytes) {
	case SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_128;
		dek_attr.keytag = 1;
		SPDK_DEBUGLOG(mlx5, "128b AES_XTS with keytag\n");
		break;
	case SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_256;
		dek_attr.keytag = 1;
		SPDK_DEBUGLOG(mlx5, "256b AES_XTS with keytag\n");
		break;
	case SPDK_MLX5_AES_XTS_128_DEK_BYTES:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_128;
		dek_attr.keytag = 0;
		SPDK_DEBUGLOG(mlx5, "128b AES_XTS\n");
		break;
	case SPDK_MLX5_AES_XTS_256_DEK_BYTES:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_256;
		dek_attr.keytag = 0;
		SPDK_DEBUGLOG(mlx5, "256b AES_XTS\n");
		break;
	default:
		SPDK_ERRLOG("Invalid key length %zu. The following keys are supported:\n"
			    "128b key + key2, %u bytes;\n"
			    "256b key + key2, %u bytes\n"
			    "128b key + key2 + keytag, %u bytes\n"
			    "256b lye + key2 + keytag, %u bytes\n",
			    attr->dek_len, SPDK_MLX5_AES_XTS_128_DEK_BYTES, SPDK_MLX5_AES_XTS_256_DEK_BYTES,
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
	keytag->deks = calloc(num_devs, sizeof(struct mlx5_crypto_dek));
	if (!keytag->deks) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_mlx5_crypto_devs_release(devs);
		free(keytag);
		return -ENOMEM;
	}

	for (i = 0; i < num_devs; i++) {
		keytag->deks_num++;
		dek = &keytag->deks[i];
		pd = spdk_rdma_utils_get_pd(devs[i]);
		if (!pd) {
			SPDK_ERRLOG("Failed to get PD on device %s\n", devs[i]->device->name);
			rc = -EINVAL;
			goto err_out;
		}

		memset(&dev_caps, 0, sizeof(dev_caps));
		rc =  spdk_mlx5_device_query_caps(devs[i], &dev_caps);
		if (rc) {
			SPDK_ERRLOG("Failed to get device %s crypto caps\n", devs[i]->device->name);
			goto err_out;
		}
		rc = mlx5_crypto_dek_init(pd, &dek_attr, dek);
		if (rc) {
			SPDK_ERRLOG("Failed to create DEK on dev %s, rc %d\n", pd->context->device->name, rc);
			goto err_out;
		}
		memset(&query_attr, 0, sizeof(query_attr));
		rc = mlx5_crypto_dek_query(dek, &query_attr);
		if (rc) {
			SPDK_ERRLOG("Failed to query DEK on dev %s, rc %d\n", pd->context->device->name, rc);
			goto err_out;
		}
		if (query_attr.opaque != 0 || query_attr.state != MLX5_ENCRYPTION_KEY_OBJ_STATE_READY) {
			SPDK_ERRLOG("DEK on dev %s in bad state %d, oapque %"PRIu64"\n", pd->context->device->name,
				    query_attr.state, query_attr.opaque);
			rc = -EINVAL;
			goto err_out;
		}

		dek->pd = pd;
		dek->context = devs[i];
		dek->tweak_mode = dev_caps.crypto.multi_block_be_tweak ?
				  SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_BE : SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_LE;
	}

	if (dek_attr.keytag) {
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

static inline struct mlx5_crypto_dek *
mlx5_crypto_get_dek_by_pd(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd)
{
	struct mlx5_crypto_dek *dek;
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
spdk_mlx5_crypto_get_dek_data(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
			      struct spdk_mlx5_crypto_dek_data *data)
{
	struct mlx5_crypto_dek *dek;

	dek = mlx5_crypto_get_dek_by_pd(keytag, pd);
	if (spdk_unlikely(!dek)) {
		SPDK_ERRLOG("No DEK for pd %p (dev %s)\n", pd, pd->context->device->name);
		return -EINVAL;
	}
	data->dek_obj_id = dek->dek_obj_id;
	data->tweak_mode = dek->tweak_mode;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(mlx5)
