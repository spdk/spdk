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

#ifndef _RTE_MBUF_H_
#define _RTE_MBUF_H_

#include "rte_mempool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In order to mock some DPDK functions, we place headers here with the name name as the DPDK headers
 * so these definitions wil be picked up.  Only what's mocked is included.
 */

__extension__
typedef void    *MARKER[0];
__extension__
typedef uint8_t  MARKER8[0];
__extension__
typedef uint64_t MARKER64[0];

struct rte_mbuf {
	MARKER cacheline0;
	void *buf_addr;
	RTE_STD_C11
	union {
		rte_iova_t buf_iova;
		rte_iova_t buf_physaddr;
	} __rte_aligned(sizeof(rte_iova_t));
	MARKER64 rearm_data;
	uint16_t data_off;
	RTE_STD_C11
	union {
		rte_atomic16_t refcnt_atomic;
		uint16_t refcnt;
	};
	uint16_t nb_segs;
	uint16_t port;
	uint64_t ol_flags;
	MARKER rx_descriptor_fields1;
	RTE_STD_C11
	union {
		uint32_t packet_type;
		struct {
			uint32_t l2_type: 4;
			uint32_t l3_type: 4;
			uint32_t l4_type: 4;
			uint32_t tun_type: 4;
			RTE_STD_C11
			union {
				uint8_t inner_esp_next_proto;
				__extension__
				struct {
					uint8_t inner_l2_type: 4;
					uint8_t inner_l3_type: 4;
				};
			};
			uint32_t inner_l4_type: 4;
		};
	};
	uint32_t pkt_len;
	uint16_t data_len;
	uint16_t vlan_tci;
	union {
		uint32_t rss;
		struct {
			RTE_STD_C11
			union {
				struct {
					uint16_t hash;
					uint16_t id;
				};
				uint32_t lo;
			};
			uint32_t hi;
		} fdir;
		struct {
			uint32_t lo;
			uint32_t hi;
		} sched;
		uint32_t usr;
	} hash;
	uint16_t vlan_tci_outer;
	uint16_t buf_len;
	uint64_t timestamp;
	MARKER cacheline1 __rte_cache_min_aligned;
	RTE_STD_C11
	union {
		void *userdata;
		uint64_t udata64;
	};
	struct rte_mempool *pool;
	struct rte_mbuf *next;
	RTE_STD_C11
	union {
		uint64_t tx_offload;
		__extension__
		struct {
			uint64_t l2_len: 7;
			uint64_t l3_len: 9;
			uint64_t l4_len: 8;
			uint64_t tso_segsz: 16;
			uint64_t outer_l3_len: 9;
			uint64_t outer_l2_len: 7;
		};
	};
	uint16_t priv_size;
	uint16_t timesync;
	uint32_t seqn;

} __rte_cache_aligned;

#ifdef __cplusplus
}
#endif

#endif
