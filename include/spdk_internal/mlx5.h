/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_MLX5_H
#define SPDK_MLX5_H

#include <infiniband/mlx5dv.h>

#define SPDK_MLX5_DEV_MAX_NAME_LEN 64

/* API for low level PRM based mlx5 driver implementation. Some terminology:
 * PRM - Programming Reference Manual
 * QP - Queue Pair
 * SQ - Submission Queue
 * CQ - Completion Queue
 * WQE - Work Queue Element
 * WQEBB - Work Queue Element Build Block (64 bytes)
 * CQE - Completion Queue Entry
 */

struct spdk_mlx5_crypto_dek;
struct spdk_mlx5_crypto_keytag;

enum {
	/** Error Completion Event - generate CQE on error for every CTRL segment, even one without CQ_UPDATE bit.
	 * Don't generate CQE in other cases. Default behaviour */
	SPDK_MLX5_WQE_CTRL_CE_CQ_ECE			= 3 << 2,
	/** Do not generate IBV_WC_WR_FLUSH_ERR for non-signaled CTRL segments. Completions are generated only for
	 * signaled (CQ_UPDATE) CTRL segments and the first error */
	SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR		= 1 << 2,
	/** Always generate CQE for CTRL segment WQE */
	SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE			= MLX5_WQE_CTRL_CQ_UPDATE,
	SPDK_MLX5_WQE_CTRL_CE_MASK			= 3 << 2,
	SPDK_MLX5_WQE_CTRL_SOLICITED			= MLX5_WQE_CTRL_SOLICITED,
	/** WQE starts execution only after all previous Read/Atomic WQEs complete */
	SPDK_MLX5_WQE_CTRL_FENCE			= MLX5_WQE_CTRL_FENCE,
	/** WQE starts execution after all local WQEs (memory operation, gather) complete */
	SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE	= MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE,
	/** WQE starts execution only after all previous WQEs complete */
	SPDK_MLX5_WQE_CTRL_STRONG_ORDERING		= 3 << 5,
};

struct spdk_mlx5_crypto_dek_create_attr {
	/* Data Encryption Key in binary form */
	char *dek;
	/* Length of the dek */
	size_t dek_len;
};

struct spdk_mlx5_cq;
struct spdk_mlx5_qp;

struct spdk_mlx5_cq_attr {
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	void *cq_context;
	struct ibv_comp_channel *comp_channel;
	int comp_vector;
};

struct spdk_mlx5_qp_attr {
	struct ibv_qp_cap cap;
	bool sigall;
	/* If set then CQ_UPDATE will be cleared for every ctrl WQE and only last ctrl WQE before ringing the doorbell
	 * will be updated with CQ_UPDATE flag */
	bool siglast;
};

struct spdk_mlx5_cq_completion {
	union {
		uint64_t wr_id;
		uint32_t mkey; /* applicable if status == MLX5_CQE_SYNDROME_SIGERR */
	};
	int status;
};

/**
 * Create Completion Queue
 *
 * \note: CQ and all associated qpairs must be accessed in scope of a single thread
 * \note: CQ size must be enough to hold completions of all connected qpairs
 *
 * \param pd Protection Domain
 * \param cq_attr Attributes to be used to create CQ
 * \param cq_out Pointer created CQ
 * \return 0 on success, negated errno on failure. \b cq_out is set only on success result
 */
int spdk_mlx5_cq_create(struct ibv_pd *pd, struct spdk_mlx5_cq_attr *cq_attr,
			struct spdk_mlx5_cq **cq_out);

/**
 * Destroy Completion Queue
 *
 * \param cq CQ created with \ref spdk_mlx5_cq_create
 */
int spdk_mlx5_cq_destroy(struct spdk_mlx5_cq *cq);

/**
 * Create loopback qpair suitable for RDMA operations
 *
 * \param pd Protection Domain
 * \param cq Completion Queue to bind QP to
 * \param qp_attr Attributes to be used to create QP
 * \param qp_out Pointer created QP
 * \return 0 on success, negated errno on failure. \b qp_out is set only on success result
 */
int spdk_mlx5_qp_create(struct ibv_pd *pd, struct spdk_mlx5_cq *cq,
			struct spdk_mlx5_qp_attr *qp_attr, struct spdk_mlx5_qp **qp_out);

/**
 * Changes internal qpair state to error causing all unprocessed Work Requests to be completed with IBV_WC_WR_FLUSH_ERR
 * status code.
 *
 * \param qp qpair pointer
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_qp_set_error_state(struct spdk_mlx5_qp *qp);

/**
 * Destroy qpair
 *
 * \param qp QP created with \ref spdk_mlx5_qp_create
 */
void spdk_mlx5_qp_destroy(struct spdk_mlx5_qp *qp);

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

/**
 * Specify which devices are allowed to be used for crypto operation.
 *
 * If the user doesn't call this function then all devices which support crypto will be used.
 * This function copies devices names. In order to free allocated memory, the user must call
 * this function with either NULL \b dev_names or with \b devs_count equal 0. This way can also
 * be used to allow all devices.
 *
 * Subsequent calls with non-NULL \b dev_names and non-zero \b devs_count current copied dev_names array.
 *
 * This function is not thread safe.
 *
 * \param dev_names Array of devices names which are allowed to be used for crypto operations
 * \param devs_count Size of \b devs_count array
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_devs_allow(const char *const dev_names[], size_t devs_count);

#endif /* SPDK_MLX5_H */
