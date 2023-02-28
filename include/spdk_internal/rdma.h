/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_RDMA_H
#define SPDK_RDMA_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

/* Contains hooks definition */
#include "spdk/nvme.h"

/* rxe driver vendor_id has been changed from 0 to 0XFFFFFF in 0184afd15a141d7ce24c32c0d86a1e3ba6bc0eb3 */
#define SPDK_RDMA_RXE_VENDOR_ID_OLD 0
#define SPDK_RDMA_RXE_VENDOR_ID_NEW 0XFFFFFF

struct spdk_rdma_wr_stats {
	/* Total number of submitted requests */
	uint64_t num_submitted_wrs;
	/* Total number of doorbell updates */
	uint64_t doorbell_updates;
};

struct spdk_rdma_qp_stats {
	struct spdk_rdma_wr_stats send;
	struct spdk_rdma_wr_stats recv;
};

struct spdk_rdma_qp_init_attr {
	void		       *qp_context;
	struct ibv_cq	       *send_cq;
	struct ibv_cq	       *recv_cq;
	struct ibv_srq	       *srq;
	struct ibv_qp_cap	cap;
	struct ibv_pd	       *pd;
	struct spdk_rdma_qp_stats *stats;
};

struct spdk_rdma_send_wr_list {
	struct ibv_send_wr	*first;
	struct ibv_send_wr	*last;
};

struct spdk_rdma_recv_wr_list {
	struct ibv_recv_wr	*first;
	struct ibv_recv_wr	*last;
};

struct spdk_rdma_qp {
	struct ibv_qp *qp;
	struct rdma_cm_id *cm_id;
	struct spdk_rdma_send_wr_list send_wrs;
	struct spdk_rdma_recv_wr_list recv_wrs;
	struct spdk_rdma_qp_stats *stats;
	bool shared_stats;
};

struct spdk_rdma_mem_map;

union spdk_rdma_mr {
	struct ibv_mr	*mr;
	uint64_t	key;
};

enum SPDK_RDMA_TRANSLATION_TYPE {
	SPDK_RDMA_TRANSLATION_MR = 0,
	SPDK_RDMA_TRANSLATION_KEY
};

struct spdk_rdma_memory_translation {
	union spdk_rdma_mr mr_or_key;
	uint8_t translation_type;
};
struct spdk_rdma_srq_init_attr {
	struct ibv_pd *pd;
	struct spdk_rdma_wr_stats *stats;
	struct ibv_srq_init_attr srq_init_attr;
};

struct spdk_rdma_srq {
	struct ibv_srq *srq;
	struct spdk_rdma_recv_wr_list recv_wrs;
	struct spdk_rdma_wr_stats *stats;
	bool shared_stats;
};

enum spdk_rdma_memory_map_role {
	SPDK_RDMA_MEMORY_MAP_ROLE_TARGET,
	SPDK_RDMA_MEMORY_MAP_ROLE_INITIATOR
};

/**
 * Create RDMA SRQ
 *
 * \param init_attr Pointer to SRQ init attr
 * \return pointer to srq on success or NULL on failure. errno is updated in failure case.
 */
struct spdk_rdma_srq *spdk_rdma_srq_create(struct spdk_rdma_srq_init_attr *init_attr);

/**
 * Destroy RDMA SRQ
 *
 * \param rdma_srq Pointer to SRQ
 * \return 0 on succes, errno on failure
 */
int spdk_rdma_srq_destroy(struct spdk_rdma_srq *rdma_srq);

/**
 * Append the given recv wr structure to the SRQ's outstanding recv list.
 * This function accepts either a single Work Request or the first WR in a linked list.
 *
 * \param rdma_srq Pointer to SRQ
 * \param first pointer to the first Work Request
 * \return true if there were no outstanding WRs before, false otherwise
 */
bool spdk_rdma_srq_queue_recv_wrs(struct spdk_rdma_srq *rdma_srq, struct ibv_recv_wr *first);

/**
 * Submit all queued receive Work Request
 *
 * \param rdma_srq Pointer to SRQ
 * \param bad_wr Stores a pointer to the first failed WR if this function return nonzero value
 * \return 0 on succes, errno on failure
 */
int spdk_rdma_srq_flush_recv_wrs(struct spdk_rdma_srq *rdma_srq, struct ibv_recv_wr **bad_wr);

/**
 * Create RDMA provider specific qpair
 *
 * \param cm_id Pointer to RDMA_CM cm_id
 * \param qp_attr Pointer to qpair init attributes
 * \return Pointer to a newly created qpair on success or NULL on failure
 */
struct spdk_rdma_qp *spdk_rdma_qp_create(struct rdma_cm_id *cm_id,
		struct spdk_rdma_qp_init_attr *qp_attr);

/**
 * Accept a connection request. Called by the passive side (NVMEoF target)
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param conn_param Optional information needed to establish the connection
 * \return 0 on success, errno on failure
 */
int spdk_rdma_qp_accept(struct spdk_rdma_qp *spdk_rdma_qp, struct rdma_conn_param *conn_param);

/**
 * Complete the connection process, must be called by the active
 * side (NVMEoF initiator) upon receipt RDMA_CM_EVENT_CONNECT_RESPONSE
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \return 0 on success, errno on failure
 */
int spdk_rdma_qp_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Destroy RDMA provider specific qpair
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair to be destroyed
 */
void spdk_rdma_qp_destroy(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Disconnect a connection and transition associated qpair to error state.
 * Generates RDMA_CM_EVENT_DISCONNECTED on both connection sides
 *
 * \param spdk_rdma_qp Pointer to qpair to be disconnected
 */
int spdk_rdma_qp_disconnect(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Append the given send wr structure to the qpair's outstanding sends list.
 * This function accepts either a single Work Request or the first WR in a linked list.
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param first Pointer to the first Work Request
 * \return true if there were no outstanding WRs before, false otherwise
 */
bool spdk_rdma_qp_queue_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr *first);

/**
 * Submit all queued send Work Request
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param bad_wr Stores a pointer to the first failed WR if this function return nonzero value
 * \return 0 on succes, errno on failure
 */
int spdk_rdma_qp_flush_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr **bad_wr);

/**
 * Append the given recv wr structure to the qpair's outstanding recv list.
 * This function accepts either a single Work Request or the first WR in a linked list.
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param first Pointer to the first Work Request
 * \return true if there were no outstanding WRs before, false otherwise
 */
bool spdk_rdma_qp_queue_recv_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_recv_wr *first);

/**
 * Submit all queued recv Work Request
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param bad_wr Stores a pointer to the first failed WR if this function return nonzero value
 * \return 0 on succes, errno on failure
 */
int spdk_rdma_qp_flush_recv_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_recv_wr **bad_wr);

/**
 * Create a memory map which is used to register Memory Regions and perform address -> memory
 * key translations
 *
 * \param pd Protection Domain which will be used to create Memory Regions
 * \param hooks Optional hooks which are used to create Protection Domain or ger RKey
 * \param role Specifies whether this map is used by RDMA target or initiator, determines access flags of registered MRs
 * \return Pointer to memory map or NULL on failure
 */
struct spdk_rdma_mem_map *
spdk_rdma_create_mem_map(struct ibv_pd *pd, struct spdk_nvme_rdma_hooks *hooks,
			 enum spdk_rdma_memory_map_role role);

/**
 * Free previously allocated memory map
 *
 * \param map Pointer to memory map to free
 */
void spdk_rdma_free_mem_map(struct spdk_rdma_mem_map **map);

/**
 * Get a translation for the given address and length.
 *
 * Note: the user of this function should use address returned in \b translation structure
 *
 * \param map Pointer to translation map
 * \param address Memory address for translation
 * \param length Length of the memory address
 * \param[in,out] translation Pointer to translation result to be filled by this function
 * \retval -EINVAL if translation is not found
 * \retval 0 translation succeed
 */
int spdk_rdma_get_translation(struct spdk_rdma_mem_map *map, void *address,
			      size_t length, struct spdk_rdma_memory_translation *translation);

/**
 * Helper function for retrieving Local Memory Key. Should be applied to a translation
 * returned by \b spdk_rdma_get_translation
 *
 * \param translation Memory translation
 * \return Local Memory Key
 */
static inline uint32_t
spdk_rdma_memory_translation_get_lkey(struct spdk_rdma_memory_translation
				      *translation)
{
	return translation->translation_type == SPDK_RDMA_TRANSLATION_MR ?
	       translation->mr_or_key.mr->lkey : (uint32_t)translation->mr_or_key.key;
}

/**
 * Helper function for retrieving Remote Memory Key. Should be applied to a translation
 * returned by \b spdk_rdma_get_translation
 *
 * \param translation Memory translation
 * \return Remote Memory Key
 */
static inline uint32_t
spdk_rdma_memory_translation_get_rkey(struct spdk_rdma_memory_translation
				      *translation)
{
	return translation->translation_type == SPDK_RDMA_TRANSLATION_MR ?
	       translation->mr_or_key.mr->rkey : (uint32_t)translation->mr_or_key.key;
}

/**
 * Get a Protection Domain for an RDMA device context.
 *
 * \param context RDMA device context
 * \return Pointer to the allocated Protection Domain
 */
struct ibv_pd *
spdk_rdma_get_pd(struct ibv_context *context);

/**
 * Return a Protection Domain.
 *
 * \param pd Pointer to the Protection Domain
 */
void spdk_rdma_put_pd(struct ibv_pd *pd);

#endif /* SPDK_RDMA_H */
