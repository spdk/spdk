/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_MLX5_H
#define SPDK_MLX5_H

#include <infiniband/mlx5dv.h>

#include "spdk/tree.h"

#define SPDK_MLX5_DEV_MAX_NAME_LEN 64

/* API for low level PRM based mlx5 driver implementation. Some terminology:
 * PRM - Programming Reference Manual
 * QP - Queue Pair
 * SQ - Submission Queue
 * CQ - Completion Queue
 * WQE - Work Queue Element
 * WQEBB - Work Queue Element Build Block (64 bytes)
 * CQE - Completion Queue Entry
 * BSF - Byte Stream Format - part of UMR WQ which describes specific data properties such as encryption or signature
 * UMR - User Memory Region
 * DEK - Data Encryption Key
 */

#define SPDK_MLX5_VENDOR_ID_MELLANOX 0x2c9

struct spdk_mlx5_crypto_dek_legacy;
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

struct spdk_mlx5_mkey_pool;

enum spdk_mlx5_mkey_pool_flags {
	SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO = 1 << 0,
	SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE = 1 << 1,
	/* Max number of pools of different types */
	SPDK_MLX5_MKEY_POOL_FLAG_COUNT = 2,
};

struct spdk_mlx5_mkey_pool_param {
	uint32_t mkey_count;
	uint32_t cache_per_thread;
	/* enum spdk_mlx5_mkey_pool_flags */
	uint32_t flags;
};

struct spdk_mlx5_mkey_pool_obj {
	uint32_t mkey;
	/* Determines which pool the mkey belongs to. See \ref spdk_mlx5_mkey_pool_flags */
	uint8_t pool_flag;
	RB_ENTRY(spdk_mlx5_mkey_pool_obj) node;
	struct {
		uint32_t sigerr_count;
		bool sigerr;
	} sig;
};

struct spdk_mlx5_umr_attr {
	struct ibv_sge *sge;
	uint32_t mkey; /* User Memory Region key to configure */
	uint32_t umr_len;
	uint16_t sge_count;
};

enum spdk_mlx5_encryption_order {
	SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_WIRE_SIGNATURE    = 0x0,
	SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_MEMORY_SIGNATURE  = 0x1,
	SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE          = 0x2,
	SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY        = 0x3,
};

enum spdk_mlx5_block_size_selector {
	SPDK_MLX5_BLOCK_SIZE_SELECTOR_RESERVED	= 0,
	SPDK_MLX5_BLOCK_SIZE_SELECTOR_512	= 1,
	SPDK_MLX5_BLOCK_SIZE_SELECTOR_520	= 2,
	SPDK_MLX5_BLOCK_SIZE_SELECTOR_4096	= 3,
	SPDK_MLX5_BLOCK_SIZE_SELECTOR_4160	= 4,
};

enum spdk_mlx5_crypto_key_tweak_mode {
	SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_BE	= 0,
	SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_LE	= 1,
};

struct spdk_mlx5_crypto_dek_data {
	/** low level devx obj id which represents the DEK */
	uint32_t dek_obj_id;
	/** Crypto key tweak mode */
	enum spdk_mlx5_crypto_key_tweak_mode tweak_mode;
};

struct spdk_mlx5_umr_crypto_attr {
	uint8_t enc_order; /* see \ref enum spdk_mlx5_encryption_order */
	uint8_t bs_selector; /* see \ref enum spdk_mlx5_block_size_selector */
	uint8_t tweak_mode; /* see \ref enum spdk_mlx5_crypto_key_tweak_mode */
	/* Low level ID of the Data Encryption Key */
	uint32_t dek_obj_id;
	uint64_t xts_iv;
	uint64_t keytag; /* Must match DEK's keytag or 0 */
};

/* Persistent Signature Value (PSV) is used to contain a calculated signature value for a single signature
 * along with some meta-data, such as error flags and status flags */
struct spdk_mlx5_psv {
	struct mlx5dv_devx_obj *devx_obj;
	uint32_t index;
};

enum spdk_mlx5_umr_sig_domain {
	SPDK_MLX5_UMR_SIG_DOMAIN_MEMORY,
	SPDK_MLX5_UMR_SIG_DOMAIN_WIRE
};

struct spdk_mlx5_umr_sig_attr {
	uint32_t seed;
	/* Index of the PSV used by this UMR */
	uint32_t psv_index;
	enum spdk_mlx5_umr_sig_domain domain;
	/* Number of sigerr completions received on the UMR */
	uint32_t sigerr_count;
	/* Number of bytes covered by this UMR */
	uint32_t raw_data_size;
	bool init; /* Set to true on the first UMR to initialize signature with its default values */
	bool check_gen; /* Set to true for the last UMR to generate signature */
};

struct spdk_mlx5_device_crypto_caps {
	bool wrapped_crypto_operational;
	bool wrapped_crypto_going_to_commissioning;
	bool wrapped_import_method_aes_xts;
	bool single_block_le_tweak;
	bool multi_block_be_tweak;
	bool multi_block_le_tweak;
};

struct spdk_mlx5_device_caps {
	/* Content of this structure is valid only if crypto_supported is true */
	struct spdk_mlx5_device_crypto_caps crypto;
	bool crypto_supported;
	bool crc32c_supported;
};

/**
 * Query device capabilities
 *
 * \param context Context of a device to query
 * \param caps Device capabilities
 * \return 0 on success, negated errno on failure.
 */
int spdk_mlx5_device_query_caps(struct ibv_context *context, struct spdk_mlx5_device_caps *caps);

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
 * Get original verbs qp
 *
 * \param qp mlx5 qp
 * \return Pointer to the underlying ibv_verbs qp
 */
struct ibv_qp *spdk_mlx5_qp_get_verbs_qp(struct spdk_mlx5_qp *qp);

/**
 * Destroy qpair
 *
 * \param qp QP created with \ref spdk_mlx5_qp_create
 */
void spdk_mlx5_qp_destroy(struct spdk_mlx5_qp *qp);

/**
 * Poll Completion Queue, save up to \b max_completions into \b comp array
 *
 * \param cq Completion Queue
 * \param comp Array of completions to be filled by this function
 * \param max_completions
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_cq_poll_completions(struct spdk_mlx5_cq *cq,
				  struct spdk_mlx5_cq_completion *comp, int max_completions);

/**
 * Ring Send Queue doorbell, submits all previously posted WQEs to HW
 *
 * \param qp qpair pointer
 */
void spdk_mlx5_qp_complete_send(struct spdk_mlx5_qp *qp);

/**
 * Submit RDMA_WRITE operations on the qpair
 *
 * \param qp qpair pointer
 * \param sge Memory layout of the local data to be written
 * \param sge_count Number of \b sge entries
 * \param dstaddr Remote address to write \b sge to
 * \param rkey Remote memory key
 * \param wrid wrid which is returned in the CQE
 * \param flags SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE to have a signaled completion; Any of SPDK_MLX5_WQE_CTRL_FENCE* or 0
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_qp_rdma_write(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
			    uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags);

/**
 * Submit RDMA_WRITE operations on the qpair
 *
 * \param qp qpair pointer
 * \param sge Memory layout of the local buffers for reading remote data
 * \param sge_count Number of \b sge entries
 * \param dstaddr Remote address to read into \b sge
 * \param rkey Remote memory key
 * \param wrid wrid which is returned in the CQE
 * \param flags SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE to have a signaled completion; Any of SPDK_MLX5_WQE_CTRL_FENCE* or 0
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_qp_rdma_read(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
			   uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags);

/**
 * Configure User Memory Region obtained using \ref spdk_mlx5_mkey_pool_get_bulk with crypto capabilities.
 *
 * Besides crypto capabilities, it allows to gather memory chunks into virtually contig (from the NIC point of view)
 * memory space with start address 0. The user must ensure that \b qp's capacity is enough to perform this operation.
 * It only works if the UMR pool was created with crypto capabilities.
 *
 * \param qp Qpair to be used for UMR configuration. If RDMA operation which references this UMR is used on the same \b qp
 * then it is not necessary to wait for the UMR configuration to complete. Instead, first RDMA operation after UMR
 * configuration must have flag SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE set to 1
 * \param umr_attr Common UMR attributes, describe memory layout
 * \param crypto_attr Crypto UMR attributes
 * \param wr_id wrid which is returned in the CQE
 * \param flags SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE to have a signaled completion; Any of SPDK_MLX5_WQE_CTRL_FENCE* or 0
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_umr_configure_crypto(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
				   struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id, uint32_t flags);

/**
 * Configure User Memory Region obtained using \ref spdk_mlx5_mkey_pool_get_bulk.
 *
 * It allows to gather memory chunks into virtually contig (from the NIC point of view) memory space with
 * start address 0. The user must ensure that \b qp's capacity is enough to perform this operation.
 * It only works if the UMR pool was created without crypto capabilities.
 *
 * \param qp Qpair to be used for UMR configuration. If RDMA operation which references this UMR is used on the same \b qp
 * then it is not necessary to wait for the UMR configuration to complete. Instead, first RDMA operation after UMR
 * configuration must have flag SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE set to 1
 * \param umr_attr Common UMR attributes, describe memory layout
 * \param wr_id wrid which is returned in the CQE
 * \param flags SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE to have a signaled completion; Any of SPDK_MLX5_WQE_CTRL_FENCE* or 0
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_umr_configure(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			    uint64_t wr_id, uint32_t flags);

/**
 * Create a PSV to be used for signature operations
 *
 * \param pd Protection Domain PSV belongs to
 * \return 0 on success, negated errno on failure
 */
struct spdk_mlx5_psv *spdk_mlx5_create_psv(struct ibv_pd *pd);

/**
 * Destroy PSV
 *
 * \param psv PSV pointer
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_destroy_psv(struct spdk_mlx5_psv *psv);

/**
 * Once a signature error happens on PSV, it's state can be re-initialized via a special SET_PSV WQE
 *
 * \param qp qp to be used to re-initialize PSV after error
 * \param psv_index index of the PSV object
 * \param crc_seed CRC32C seed to be used for initialization
 * \param wrid wrid which is returned in the CQE
 * \param flags SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE to have a signaled completion or 0
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_qp_set_psv(struct spdk_mlx5_qp *qp, uint32_t psv_index, uint32_t crc_seed,
			 uint64_t wr_id, uint32_t flags);

/**
 * Configure User Memory Region obtained using \ref spdk_mlx5_mkey_pool_get_bulk with CRC32C capabilities.
 *
 * Besides signature capabilities, it allows to gather memory chunks into virtually contig (from the NIC point of view)
 * memory space with start address 0. The user must ensure that \b qp's capacity is enough to perform this operation.
 *
 * \param qp Qpair to be used for UMR configuration. If RDMA operation which references this UMR is used on the same \b qp
 * then it is not necessary to wait for the UMR configuration to complete. Instead, first RDMA operation after UMR
 * configuration must have flag SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE set to 1
 * \param umr_attr Common UMR attributes, describe memory layout
 * \param sig_attr Signature UMR attributes
 * \param wr_id wrid which is returned in the CQE
 * \param flags SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE to have a signaled completion; Any of SPDK_MLX5_WQE_CTRL_FENCE* or 0
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_umr_configure_sig(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
				struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id, uint32_t flags);

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
 * Get Data Encryption Key data
 *
 * \param keytag Keytag with DEKs
 * \param pd Protection Domain which is going to be used to register UMR.
 * \param dek_obj_id Low level DEK ID, can be used to configure crypto UMR
 * \param data DEK data to be filled by this function
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_get_dek_data(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
				  struct spdk_mlx5_crypto_dek_data *data);

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

/**
 * Creates a pool of memory keys for a given \b PD. If params::flags has SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO enabled,
 * then a device associated with PD must support crypto operations.
 *
 * Can be called several times for different PDs. Has no effect if a pool for \b PD with the same \b flags already exists
 *
 * \param params Parameter of the memory pool
 * \param pd Protection Domain
 * \return 0 on success, errno on failure
 */
int spdk_mlx5_mkey_pool_init(struct spdk_mlx5_mkey_pool_param *params, struct ibv_pd *pd);

/**
 * Destroy mkey pools with the given \b flags and \b pd which was created by \ref spdk_mlx5_mkey_pool_init.
 *
 * The pool reference must be released by \ref spdk_mlx5_mkey_pool_put_ref before calling this function.
 *
 * \param pd Protection Domain
 * \param flags Specifies type of the pool to delete.
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_mkey_pool_destroy(uint32_t flags, struct ibv_pd *pd);

/**
 * Get a reference to mkey pool specified by PD, increment internal reference counter.
 *
 * \param pd PD to get a mkey pool for
 * \param flags Required mkey pool flags, see \ref enum spdk_mlx5_mkey_pool_flags
 * \return Pointer to the mkey pool on success or NULL on error
 */
struct spdk_mlx5_mkey_pool *spdk_mlx5_mkey_pool_get_ref(struct ibv_pd *pd, uint32_t flags);

/**
 * Put the mkey pool reference.
 *
 * The pool is NOT destroyed if even reference counter reaches 0
 *
 * \param pool Mkey pool pointer
 */
void spdk_mlx5_mkey_pool_put_ref(struct spdk_mlx5_mkey_pool *pool);

/**
 * Get several mkeys from the pool
 *
 * \param pool mkey pool
 * \param mkeys array of mkey pointers to be filled by this function
 * \param mkeys_count number of mkeys to get from the pool
 * \return 0 on success, errno on failure
 */
int spdk_mlx5_mkey_pool_get_bulk(struct spdk_mlx5_mkey_pool *pool,
				 struct spdk_mlx5_mkey_pool_obj **mkeys, uint32_t mkeys_count);

/**
 * Return mkeys to the pool
 *
 * \param pool mkey pool
 * \param mkeys array of mkey pointers to be returned to the pool
 * \param mkeys_count number of mkeys to be returned to the pool
 */
void spdk_mlx5_mkey_pool_put_bulk(struct spdk_mlx5_mkey_pool *pool,
				  struct spdk_mlx5_mkey_pool_obj **mkeys, uint32_t mkeys_count);

/**
 * Notify the mlx5 library that a module which can handle UMR configuration is registered or unregistered
 *
 * \param registered True if the module is registered, false otherwise
 */
void spdk_mlx5_umr_implementer_register(bool registered);

/**
 * Check whether a module which can handle UMR configuration is registered or not
 *
 * \return True of the UMR implementer is registered, false otherwise
 */
bool spdk_mlx5_umr_implementer_is_registered(void);

#endif /* SPDK_MLX5_H */
