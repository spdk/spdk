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

#ifndef SPDK_NVMF_SPEC_H
#define SPDK_NVMF_SPEC_H

#include "spdk/stdinc.h"

#include "spdk/assert.h"
#include "spdk/nvme_spec.h"

/**
 * \file
 * NVMe over Fabrics specification definitions
 */

#pragma pack(push, 1)

/* Minimum number of admin queue entries defined by NVMe over Fabrics spec */
#define SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES	32

struct spdk_nvmf_capsule_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype;
	uint8_t		reserved2[35];
	uint8_t		fabric_specific[24];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_capsule_cmd) == 64, "Incorrect size");

/* Fabric Command Set */
#define SPDK_NVME_OPC_FABRIC 0x7f

enum spdk_nvmf_fabric_cmd_types {
	SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET			= 0x00,
	SPDK_NVMF_FABRIC_COMMAND_CONNECT			= 0x01,
	SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET			= 0x04,
	SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND		= 0x05,
	SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV		= 0x06,
	SPDK_NVMF_FABRIC_COMMAND_START_VENDOR_SPECIFIC		= 0xC0,
};

enum spdk_nvmf_fabric_cmd_status_code {
	SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT		= 0x80,
	SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY		= 0x81,
	SPDK_NVMF_FABRIC_SC_INVALID_PARAM		= 0x82,
	SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY		= 0x83,
	SPDK_NVMF_FABRIC_SC_INVALID_HOST		= 0x84,
	SPDK_NVMF_FABRIC_SC_LOG_RESTART_DISCOVERY	= 0x90,
	SPDK_NVMF_FABRIC_SC_AUTH_REQUIRED		= 0x91,
};

/**
 * RDMA Queue Pair service types
 */
enum spdk_nvmf_rdma_qptype {
	/** Reliable connected */
	SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED	= 0x1,

	/** Reliable datagram */
	SPDK_NVMF_RDMA_QPTYPE_RELIABLE_DATAGRAM		= 0x2,
};

/**
 * RDMA provider types
 */
enum spdk_nvmf_rdma_prtype {
	/** No provider specified */
	SPDK_NVMF_RDMA_PRTYPE_NONE	= 0x1,

	/** InfiniBand */
	SPDK_NVMF_RDMA_PRTYPE_IB	= 0x2,

	/** RoCE v1 */
	SPDK_NVMF_RDMA_PRTYPE_ROCE	= 0x3,

	/** RoCE v2 */
	SPDK_NVMF_RDMA_PRTYPE_ROCE2	= 0x4,

	/** iWARP */
	SPDK_NVMF_RDMA_PRTYPE_IWARP	= 0x5,
};

/**
 * RDMA connection management service types
 */
enum spdk_nvmf_rdma_cms {
	/** Sockets based endpoint addressing */
	SPDK_NVMF_RDMA_CMS_RDMA_CM	= 0x1,
};

/**
 * NVMe over Fabrics transport types
 */
enum spdk_nvmf_trtype {
	/** RDMA */
	SPDK_NVMF_TRTYPE_RDMA		= 0x1,

	/** Fibre Channel */
	SPDK_NVMF_TRTYPE_FC		= 0x2,

	/** Intra-host transport (loopback) */
	SPDK_NVMF_TRTYPE_INTRA_HOST	= 0xfe,
};

/**
 * Address family types
 */
enum spdk_nvmf_adrfam {
	/** IPv4 (AF_INET) */
	SPDK_NVMF_ADRFAM_IPV4		= 0x1,

	/** IPv6 (AF_INET6) */
	SPDK_NVMF_ADRFAM_IPV6		= 0x2,

	/** InfiniBand (AF_IB) */
	SPDK_NVMF_ADRFAM_IB		= 0x3,

	/** Fibre Channel address family */
	SPDK_NVMF_ADRFAM_FC		= 0x4,

	/** Intra-host transport (loopback) */
	SPDK_NVMF_ADRFAM_INTRA_HOST	= 0xfe,
};

/**
 * NVM subsystem types
 */
enum spdk_nvmf_subtype {
	/** Discovery type for NVM subsystem */
	SPDK_NVMF_SUBTYPE_DISCOVERY		= 0x1,

	/** NVMe type for NVM subsystem */
	SPDK_NVMF_SUBTYPE_NVME		= 0x2,
};

/**
 * Connections shall be made over a fabric secure channel
 */
enum spdk_nvmf_treq_secure_channel {
	/** Not specified */
	SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED	= 0x0,

	/** Required */
	SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED		= 0x1,

	/** Not required */
	SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED	= 0x2,
};

struct spdk_nvmf_fabric_auth_recv_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype; /* NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV (0x06) */
	uint8_t		reserved2[19];
	struct spdk_nvme_sgl_descriptor sgl1;
	uint8_t		reserved3;
	uint8_t		spsp0;
	uint8_t		spsp1;
	uint8_t		secp;
	uint32_t	al;
	uint8_t		reserved4[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_auth_recv_cmd) == 64, "Incorrect size");

struct spdk_nvmf_fabric_auth_send_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype; /* NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND (0x05) */
	uint8_t		reserved2[19];
	struct spdk_nvme_sgl_descriptor sgl1;
	uint8_t		reserved3;
	uint8_t		spsp0;
	uint8_t		spsp1;
	uint8_t		secp;
	uint32_t	tl;
	uint8_t		reserved4[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_auth_send_cmd) == 64, "Incorrect size");

struct spdk_nvmf_fabric_connect_data {
	uint8_t		hostid[16];
	uint16_t	cntlid;
	uint8_t		reserved5[238];
	uint8_t		subnqn[256];
	uint8_t		hostnqn[256];
	uint8_t		reserved6[256];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_data) == 1024, "Incorrect size");

struct spdk_nvmf_fabric_connect_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype;
	uint8_t		reserved2[19];
	struct spdk_nvme_sgl_descriptor sgl1;
	uint16_t	recfmt; /* Connect Record Format */
	uint16_t	qid; /* Queue Identifier */
	uint16_t	sqsize; /* Submission Queue Size */
	uint8_t		cattr; /* queue attributes */
	uint8_t		reserved3;
	uint32_t	kato; /* keep alive timeout */
	uint8_t		reserved4[12];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_cmd) == 64, "Incorrect size");

struct spdk_nvmf_fabric_connect_rsp {
	union {
		struct {
			uint16_t cntlid;
			uint16_t authreq;
		} success;

		struct {
			uint16_t	ipo;
			uint8_t		iattr;
			uint8_t		reserved;
		} invalid;

		uint32_t raw;
	} status_code_specific;

	uint32_t	reserved0;
	uint16_t	sqhd;
	uint16_t	reserved1;
	uint16_t	cid;
	struct spdk_nvme_status status;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_rsp) == 16, "Incorrect size");

#define SPDK_NVMF_PROP_SIZE_4	0
#define SPDK_NVMF_PROP_SIZE_8	1

struct spdk_nvmf_fabric_prop_get_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype;
	uint8_t		reserved2[35];
	struct {
		uint8_t size		: 2;
		uint8_t reserved	: 6;
	} attrib;
	uint8_t		reserved3[3];
	uint32_t	ofst;
	uint8_t		reserved4[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_get_cmd) == 64, "Incorrect size");

struct spdk_nvmf_fabric_prop_get_rsp {
	union {
		uint64_t u64;
		struct {
			uint32_t low;
			uint32_t high;
		} u32;
	} value;

	uint16_t	sqhd;
	uint16_t	reserved0;
	uint16_t	cid;
	struct spdk_nvme_status status;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_get_rsp) == 16, "Incorrect size");

struct spdk_nvmf_fabric_prop_set_cmd {
	uint8_t		opcode;
	uint8_t		reserved0;
	uint16_t	cid;
	uint8_t		fctype;
	uint8_t		reserved1[35];
	struct {
		uint8_t size		: 2;
		uint8_t reserved	: 6;
	} attrib;
	uint8_t		reserved2[3];
	uint32_t	ofst;

	union {
		uint64_t u64;
		struct {
			uint32_t low;
			uint32_t high;
		} u32;
	} value;

	uint8_t		reserved4[8];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_set_cmd) == 64, "Incorrect size");

#define SPDK_NVMF_NQN_MAX_LEN 223
#define SPDK_NVMF_DISCOVERY_NQN "nqn.2014-08.org.nvmexpress.discovery"

#define SPDK_NVMF_TRADDR_MAX_LEN 256
#define SPDK_NVMF_TRSVCID_MAX_LEN 32

/** RDMA transport-specific address subtype */
struct spdk_nvmf_rdma_transport_specific_address_subtype {
	/** RDMA QP service type (\ref spdk_nvmf_rdma_qptype) */
	uint8_t		rdma_qptype;

	/** RDMA provider type (\ref spdk_nvmf_rdma_prtype) */
	uint8_t		rdma_prtype;

	/** RDMA connection management service (\ref spdk_nvmf_rdma_cms) */
	uint8_t		rdma_cms;

	uint8_t		reserved0[5];

	/** RDMA partition key for AF_IB */
	uint16_t	rdma_pkey;

	uint8_t		reserved2[246];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_transport_specific_address_subtype) == 256,
		   "Incorrect size");

/** Transport-specific address subtype */
union spdk_nvmf_transport_specific_address_subtype {
	uint8_t raw[256];

	/** RDMA */
	struct spdk_nvmf_rdma_transport_specific_address_subtype rdma;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_transport_specific_address_subtype) == 256,
		   "Incorrect size");

/**
 * Discovery Log Page entry
 */
struct spdk_nvmf_discovery_log_page_entry {
	/** Transport type (\ref spdk_nvmf_trtype) */
	uint8_t		trtype;

	/** Address family (\ref spdk_nvmf_adrfam) */
	uint8_t		adrfam;

	/** Subsystem type (\ref spdk_nvmf_subtype) */
	uint8_t		subtype;

	/** Transport requirements */
	struct {
		/** Secure channel requirements (\ref spdk_nvmf_treq_secure_channel) */
		uint8_t secure_channel : 2;

		uint8_t reserved : 6;
	} treq;

	/** NVM subsystem port ID */
	uint16_t	portid;

	/** Controller ID */
	uint16_t	cntlid;

	/** Admin max SQ size */
	uint16_t	asqsz;

	uint8_t		reserved0[22];

	/** Transport service identifier */
	uint8_t		trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN];

	uint8_t		reserved1[192];

	/** NVM subsystem qualified name */
	uint8_t		subnqn[256];

	/** Transport address */
	uint8_t		traddr[SPDK_NVMF_TRADDR_MAX_LEN];

	/** Transport-specific address subtype */
	union spdk_nvmf_transport_specific_address_subtype tsas;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_discovery_log_page_entry) == 1024, "Incorrect size");

struct spdk_nvmf_discovery_log_page {
	uint64_t	genctr;
	uint64_t	numrec;
	uint16_t	recfmt;
	uint8_t		reserved0[1006];
	struct spdk_nvmf_discovery_log_page_entry entries[0];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_discovery_log_page) == 1024, "Incorrect size");

/* RDMA Fabric specific definitions below */

#define SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY	0xF

struct spdk_nvmf_rdma_request_private_data {
	uint16_t	recfmt; /* record format */
	uint16_t	qid;	/* queue id */
	uint16_t	hrqsize;	/* host receive queue size */
	uint16_t	hsqsize;	/* host send queue size */
	uint16_t 	cntlid;		/* controller id */
	uint8_t 	reserved[22];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_request_private_data) == 32, "Incorrect size");

struct spdk_nvmf_rdma_accept_private_data {
	uint16_t	recfmt; /* record format */
	uint16_t	crqsize;	/* controller receive queue size */
	uint8_t		reserved[28];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_accept_private_data) == 32, "Incorrect size");

struct spdk_nvmf_rdma_reject_private_data {
	uint16_t	recfmt; /* record format */
	struct spdk_nvme_status status;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_reject_private_data) == 4, "Incorrect size");

union spdk_nvmf_rdma_private_data {
	struct spdk_nvmf_rdma_request_private_data	pd_request;
	struct spdk_nvmf_rdma_accept_private_data	pd_accept;
	struct spdk_nvmf_rdma_reject_private_data	pd_reject;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_rdma_private_data) == 32, "Incorrect size");

enum spdk_nvmf_rdma_transport_errors {
	SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH	= 0x1,
	SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT			= 0x2,
	SPDK_NVMF_RDMA_ERROR_INVALID_QID			= 0x3,
	SPDK_NVMF_RDMA_ERROR_INVALID_HSQSIZE			= 0x4,
	SPDK_NVMF_RDMA_ERROR_INVALID_HRQSIZE			= 0x5,
	SPDK_NVMF_RDMA_ERROR_NO_RESOURCES			= 0x6,
	SPDK_NVMF_RDMA_ERROR_INVALID_IRD			= 0x7,
	SPDK_NVMF_RDMA_ERROR_INVALID_ORD			= 0x8,
	SPDK_NVMF_RDMA_ERROR_INVALID_CNTLID			= 0x9,
};

#pragma pack(pop)

#endif /* __NVMF_SPEC_H__ */
