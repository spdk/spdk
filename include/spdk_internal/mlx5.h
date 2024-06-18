/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_MLX5_H
#define SPDK_MLX5_H

#include <infiniband/mlx5dv.h>

struct spdk_mlx5_crypto_dek;
struct spdk_mlx5_crypto_keytag;

struct spdk_mlx5_crypto_dek_create_attr {
	/* Data Encryption Key in binary form */
	char *dek;
	/* Length of the dek */
	size_t dek_len;
};

/**
 * Return a NULL terminated array of devices which support crypto operation on Nvidia NICs
 *
 * \param dev_num The size of the array or 0
 * \return Array of contexts. This array must be released with \b spdk_mlx5_crypto_devs_release
 */
struct ibv_context **spdk_mlx5_crypto_devs_get(int *dev_num);

/**
 * Releases array of devices allocated by \b spdk_mlx5_crypto_devs_get
 *
 * \param rdma_devs Array of device to be released
 */
void spdk_mlx5_crypto_devs_release(struct ibv_context **rdma_devs);

/**
 * Create a keytag which contains DEKs per each crypto device in the system
 *
 * \param attr Crypto attributes
 * \param out Keytag
 * \return 0 on success, negated errno of failure
 */
int spdk_mlx5_crypto_keytag_create(struct spdk_mlx5_crypto_dek_create_attr *attr,
				   struct spdk_mlx5_crypto_keytag **out);

/**
 * Destroy a keytag created using \b spdk_mlx5_crypto_keytag_create
 *
 * \param keytag Keytag pointer
 */
void spdk_mlx5_crypto_keytag_destroy(struct spdk_mlx5_crypto_keytag *keytag);

/**
 * Fills attributes used to register UMR with crypto operation
 *
 * \param attr_out Configured UMR attributes
 * \param keytag Keytag with DEKs
 * \param pd Protection Domain which is going to be used to register UMR. This function will find a DEK in \b keytag with the same PD
 * \param block_size Logical block size
 * \param iv Initialization vector or tweak. Usually that is logical block address
 * \param encrypt_on_tx If set, memory data will be encrypted during TX and wire data will be decrypted during RX. If not set, memory data will be decrypted during TX and wire data will be encrypted during RX.
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_set_attr(struct mlx5dv_crypto_attr *attr_out,
			      struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
			      uint32_t block_size, uint64_t iv, bool encrypt_on_tx);


#endif /* SPDK_MLX5_H */
