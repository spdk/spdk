/*-
 *
 *   Copyright(c) 2015-2017 Intel Corporation. All rights reserved.
 *   Copyright 2014 6WIND S.A.
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

#ifndef _RTE_CRYPTODEV_H_
#define _RTE_CRYPTODEV_H_

#ifdef __cplusplus
extern "C" {
#endif

/* In order to mock some DPDK functions, we place headers here with the name name as the DPDK headers
 * so these definitions wil be picked up.  Only what's mocked is included.
 */

uint8_t dummy[16];
#define rte_crypto_op_ctod_offset(c, t, o) &dummy[0]

#define	RTE_CRYPTODEV_FF_MBUF_SCATTER_GATHER	(1ULL << 9)

struct rte_cryptodev_info {
	const char *driver_name;
	uint8_t driver_id;
	struct rte_pci_device *pci_dev;
	uint64_t feature_flags;
	const struct rte_cryptodev_capabilities *capabilities;
	unsigned max_nb_queue_pairs;
	struct {
		unsigned max_nb_sessions;
		unsigned int max_nb_sessions_per_qp;
	} sym;
};

enum rte_cryptodev_event_type {
	RTE_CRYPTODEV_EVENT_UNKNOWN,
	RTE_CRYPTODEV_EVENT_ERROR,
	RTE_CRYPTODEV_EVENT_MAX
};

struct rte_cryptodev_qp_conf {
	uint32_t nb_descriptors;
};

struct rte_cryptodev_stats {
	uint64_t enqueued_count;
	uint64_t dequeued_count;
	uint64_t enqueue_err_count;
	uint64_t dequeue_err_count;
};

#define RTE_CRYPTODEV_NAME_MAX_LEN	(64)

extern uint8_t
rte_cryptodev_count(void);

extern uint8_t
rte_cryptodev_device_count_by_driver(uint8_t driver_id);

extern int
rte_cryptodev_socket_id(uint8_t dev_id);

struct rte_cryptodev_config {
	int socket_id;
	uint16_t nb_queue_pairs;
};

extern int
rte_cryptodev_configure(uint8_t dev_id, struct rte_cryptodev_config *config);

extern int
rte_cryptodev_start(uint8_t dev_id);

extern void
rte_cryptodev_stop(uint8_t dev_id);

extern int
rte_cryptodev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
			       const struct rte_cryptodev_qp_conf *qp_conf, int socket_id,
			       struct rte_mempool *session_pool);

extern void
rte_cryptodev_info_get(uint8_t dev_id, struct rte_cryptodev_info *dev_info);

static inline uint16_t
rte_cryptodev_dequeue_burst(uint8_t dev_id, uint16_t qp_id,
			    struct rte_crypto_op **ops, uint16_t nb_ops);

static inline uint16_t
rte_cryptodev_enqueue_burst(uint8_t dev_id, uint16_t qp_id,
			    struct rte_crypto_op **ops, uint16_t nb_ops);

struct rte_cryptodev_sym_session {
	__extension__ void *sess_private_data[0];
};

struct rte_cryptodev_asym_session {
	__extension__ void *sess_private_data[0];
};

struct rte_crypto_asym_xform;

struct rte_cryptodev_sym_session *
rte_cryptodev_sym_session_create(struct rte_mempool *mempool);

int
rte_cryptodev_sym_session_free(struct rte_cryptodev_sym_session *sess);

int
rte_cryptodev_sym_session_init(uint8_t dev_id,
			       struct rte_cryptodev_sym_session *sess,
			       struct rte_crypto_sym_xform *xforms,
			       struct rte_mempool *mempool);

int
rte_cryptodev_sym_session_clear(uint8_t dev_id,
				struct rte_cryptodev_sym_session *sess);

unsigned int
rte_cryptodev_sym_get_private_session_size(uint8_t dev_id);

#ifdef __cplusplus
}
#endif

#endif
