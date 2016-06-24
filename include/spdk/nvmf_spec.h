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

#include <stdint.h>

#include "spdk/assert.h"
#include "spdk/nvme_spec.h"

/**
 * \file
 *
 */

#pragma pack(push, 1)

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
enum spdk_nvmf_rdma_qp_service_types {
	/** Reliable connected */
	SPDK_NVMF_QP_TYPE_RELIABLE_CONNECTED	= 0x1,
	/** Reliable datagram */
	SPDK_NVMF_OQ_TYPE_RELIABLE_DATAGRAM	= 0x2,
};

/**
 * RDMA provider types
 */
enum spdk_nvmf_rdma_provider_types {
	SPDK_NVMF_RDMA_NO_PROVIDER	= 0x1,
	SPDK_NVMF_RDMA_PRTYPE_IB	= 0x2,
	SPDK_NVMF_RDMA_PRTYPE_ROCE	= 0x3,
	SPDK_NVMF_RDMA_PRTYPE_ROCE2	= 0x4,
	SPDK_NVMF_RDMA_PRTYPE_IWARP	= 0x5,
};

/**
 * RDMA connection management service types
 */
enum spdk_nvmf_rdma_connection_mgmt_service {
	/** Sockets based endpoint addressing */
	SPDK_NVMF_RDMA_CMS_RDMA_CM	= 0x1,
};

/**
 * NVMe over Fabrics transport types
 */
enum spdk_nvmf_transport_types {
	SPDK_NVMF_TRANS_RDMA		= 0x1,
	SPDK_NVMF_TRANS_FC		= 0x2,
	SPDK_NVMF_TRANS_INTRA_HOST	= 0xfe,
};

/**
 * Address family types
 */
enum spdk_nvmf_address_family_types {
	SPDK_NVMF_ADDR_FAMILY_IPV4		= 0x1,
	SPDK_NVMF_ADDR_FAMILY_IPV6		= 0x2,
	SPDK_NVMF_ADDR_FAMILY_IB		= 0x3,
	SPDK_NVMF_ADDR_FAMILY_FC		= 0x4,
	SPDK_NVMF_ADDR_FAMILY_INTRA_HOST	= 0xfe,
};

/**
 * NVM subsystem types
 */
enum spdk_nvmf_subsystem_types {
	/** Discovery type for NVM subsystem */
	SPDK_NVMF_SUB_DISCOVERY		= 0x1,
	/** NVMe type for NVM subsystem */
	SPDK_NVMF_SUB_NVME		= 0x2,
};

/**
 * Connections shall be made over a fabric secure channel
 */
enum spdk_nvmf_transport_requirements {
	SPDK_NVMF_TREQ_NOT_SPECIFIED	= 0x0,
	SPDK_NVMF_TREQ_REQUIRED		= 0x1,
	SPDK_NVMF_TREQ_NOT_REQUIRED	= 0x2,
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

struct spdk_nvmf_fabric_auth_recv_rsp {
	uint8_t		reserved0[8];
	uint16_t	sqhd;
	uint8_t		reserved1[2];
	uint16_t	cid;
	struct spdk_nvme_status status;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_auth_recv_rsp) == 16, "Incorrect size");

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

struct spdk_nvmf_fabric_auth_send_rsp {
	uint8_t		reserved0[8];
	uint16_t	sqhd;
	uint8_t		reserved1[2];
	uint16_t	cid;
	struct spdk_nvme_status status;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_auth_send_rsp) == 16, "Incorrect size");

struct spdk_nvmf_fabric_connect_data {
	uint8_t		hostid[16];
	uint16_t	cntlid;
	uint8_t		reserved5[238];
	uint8_t		subnqn[256];
	uint8_t		hostnqn[256];
	uint8_t		reserved6[256];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_data) == 1024, "Incorrect size");

#define SPDK_NVMF_CONNECT_ATTR_PRIORITY_URGENT	0x00
#define SPDK_NVMF_CONNECT_ATTR_PRIORITY_HIGH	0x01
#define SPDK_NVMF_CONNECT_ATTR_PRIORITY_MEDIUM	0x02
#define SPDK_NVMF_CONNECT_ATTR_PRIORITY_LOW	0x03
#define SPDK_NVMF_CONNECT_ATTR_RESERVED		0xFC

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

#define SPDK_NVMF_PROP_CAP_OFST		0x0
#define SPDK_NVMF_PROP_VS_OFST		0x8
#define SPDK_NVMF_PROP_INTMS_OFST	0xC
#define SPDK_NVMF_PROP_INTMC_OFST	0x10
#define SPDK_NVMF_PROP_CC_OFST		0x14
#define SPDK_NVMF_PROP_CSTS_OFST	0x1C
#define SPDK_NVMF_PROP_NSSR_OFST	0x20
#define SPDK_NVMF_PROP_AQA_OFST		0x24
#define SPDK_NVMF_PROP_ASQ_OFST		0x28
#define SPDK_NVMF_PROP_ACQ_OFST		0x30
#define SPDK_NVMF_PROP_CMBLOC_OFST	0x38
#define SPDK_NVMF_PROP_CMBSZ_OFST	0x3C

#define SPDK_NVMF_PROP_CAP_LEN		0x8
#define SPDK_NVMF_PROP_VS_LEN		0x4
#define SPDK_NVMF_PROP_INTMS_LEN	0x4
#define SPDK_NVMF_PROP_INTMC_LEN	0x4
#define SPDK_NVMF_PROP_CC_LEN		0x4
#define SPDK_NVMF_PROP_CSTS_LEN		0x4
#define SPDK_NVMF_PROP_NSSR_LEN		0x4
#define SPDK_NVMF_PROP_AQA_LEN		0x4
#define SPDK_NVMF_PROP_ASQ_LEN		0x8
#define SPDK_NVMF_PROP_ACQ_LEN		0x8
#define SPDK_NVMF_PROP_CMBLOC_LEN	0x4
#define SPDK_NVMF_PROP_CMBSZ_LEN	0x4

union spdk_nvmf_property_size {
	uint32_t	raw;
	struct {
		uint32_t reserved	: 16;

		/** property address space size */
		uint32_t size		: 16;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_property_size) == 4, "Incorrect size");

union spdk_nvmf_capsule_attr_lo {
	uint32_t	raw;
	struct {
		/** maximum response capsule size */
		uint32_t rspsz		: 16;

		/** maximum command capsule size */
		uint32_t cmdsz		: 16;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_capsule_attr_lo) == 4, "Incorrect size");

union spdk_nvmf_capsule_attr_hi {
	uint32_t	raw;
	struct {
		/** support capsule alignment in response capsules */
		uint32_t reserved	: 26;

		/** support capsule alignment in response capsules */
		uint32_t cairsp		: 1;

		/** support capsule alignment in command capsules */
		uint32_t caicmd		: 1;

		/** support capsule metadata in response capsules */
		uint32_t cmirsp		: 1;

		/** support capsule metadata in command capsules */
		uint32_t cmicmd		: 1;

		/** support capsule data in response capsules */
		uint32_t cdirsp		: 1;

		/** support capsule data in command capsules */
		uint32_t cdicmd		: 1;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_capsule_attr_hi) == 4, "Incorrect size");

struct spdk_nvmf_ctrlr_properties {
	union spdk_nvme_cap_lo_register		cap_lo;
	union spdk_nvme_cap_hi_register		cap_hi;

	uint32_t				vs;
	uint32_t				intms;
	uint32_t				intmc;

	union spdk_nvme_cc_register		cc;

	uint32_t				reserved1;
	union spdk_nvme_csts_register		csts;
	uint32_t				nssr;

	union spdk_nvme_aqa_register		aqa;

	uint64_t				asq;
	uint64_t				acq;

	uint32_t				cmbloc;
	uint32_t				cmbsz;

	uint8_t					reserved2[0xEC0];
	uint8_t					reserved3[0x100];
	union spdk_nvmf_property_size		propsz;
	uint32_t				reserved4;
	union spdk_nvmf_capsule_attr_lo		capattr_lo;
	union spdk_nvmf_capsule_attr_hi		capattr_hi;
	uint8_t					reserved5[0x2F0];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr_properties) == 4864, "Incorrect size");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_CAP_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, cap_lo),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_VS_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, vs),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_INTMS_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, intms),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_INTMC_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, intmc),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_CC_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, cc),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_CSTS_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, csts),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_NSSR_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, nssr),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_AQA_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, aqa),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_ASQ_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, asq),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_ACQ_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, acq),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_CMBLOC_OFST == offsetof(struct spdk_nvmf_ctrlr_properties,
		   cmbloc),
		   "Incorrect property offset");
SPDK_STATIC_ASSERT(SPDK_NVMF_PROP_CMBSZ_OFST == offsetof(struct spdk_nvmf_ctrlr_properties, cmbsz),
		   "Incorrect property offset");

struct spdk_nvmf_fabric_prop_get_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype;
	uint8_t		reserved2[35];
	uint8_t		attrib;
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
	uint8_t		attrib;
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

struct spdk_nvmf_fabric_prop_set_rsp {
	uint8_t		reserved0[8];
	uint16_t	sqhd;
	uint16_t	reserved1;
	uint16_t	cid;
	struct spdk_nvme_status status;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_set_rsp) == 16, "Incorrect size");

struct spdk_nvmf_extended_identify_ctrlr_data {
	uint32_t	ioccsz;
	uint32_t	iorcsz;
	uint16_t	icdoff;
	uint8_t		ctrattr;
	uint8_t		msdbd;
	uint8_t		reserved[244];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_extended_identify_ctrlr_data) == 256, "Incorrect size");

#define SPDK_NVMF_DISCOVERY_NQN "nqn.2014-08.org.nvmexpress.discovery"

struct spdk_nvmf_discovery_identify_data {
	uint8_t		reserved0[64];
	uint64_t	fr;
	uint8_t		reserved1[5];
	uint8_t		mdts;
	uint16_t	cntlid;
	uint32_t	ver;
	uint8_t		reserved2[177];
	uint8_t		lpa;
	uint8_t		elpe;
	uint8_t		reserved3[505];
	uint8_t		subnqn[256];
	uint8_t		discovery[1024];
	uint8_t		reserved4[1024];
	uint8_t		vs[1024];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_discovery_identify_data) == 4096, "Incorrect size");

struct spdk_nvmf_rdma_transport_specific_address {
	uint8_t		rdma_qptype; /* see spdk_nvmf_rdma_qp_service_types */
	uint8_t		rdma_prtype; /* see spdk_nvmf_rdma_provider_types */
	uint8_t		rdma_cms; /* see spdk_nvmf_rdma_connection_mgmt_service */
	uint8_t		reserved0[5];
	uint16_t	rdma_pkey;
	uint8_t		reserved2[246];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_transport_specific_address) == 256,
		   "Incorrect size");

struct spdk_nvmf_discovery_log_page_entry {
	uint8_t		trtype; /* transport type */
	uint8_t		adrfam; /* address family */
	uint8_t		subtype;
	uint8_t		treq;
	uint16_t	portid;
	uint16_t	cntlid;
	uint8_t		reserved0[24];
	uint8_t		trsvcid[32];
	uint8_t		reserved1[192];
	uint8_t		subnqn[256];
	uint8_t		traddr[256];
	union	{
		struct spdk_nvmf_rdma_transport_specific_address rdma;
	} tsas;
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
	uint8_t		reserved[24];
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
};

#pragma pack(pop)

#endif /* __NVMF_SPEC_H__ */
