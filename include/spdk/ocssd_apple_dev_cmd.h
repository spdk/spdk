/*
 * INTEL CONFIDENTIAL
 *
 * Copyright 2018 Intel Corporation
 *
 * This software and the related documents are Intel copyrighted materials, and
 * your use of them is governed by the express license under which they were
 * provided to you (License). Unless the License provides otherwise, you may not
 * use, modify, copy, publish, distribute, disclose or transmit this software or
 * the related documents without Intel's prior written permission.
 * This software and the related documents are provided as is, with no express or
 * implied warranties, other than those that are expressly stated in the License.
 */

#ifndef SPDK_OCSSD_APPLE_DEV_CMD_H
#define SPDK_OCSSD_APPLE_DEV_CMD_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apple is the nickname for Intel AB04 device
 * cmds in this file are described in Intel® Direct Access for American Bar Revision 0.37
 */

//TODO: r/w sys area opc are confused in table(0xF9) and description(0xC9).
#define SPDK_OCSSD_APPLE_OPC_WRITE_SYS		0xF9
#define SPDK_OCSSD_APPLE_OPC_READ_SYS		0xFA
#define SPDK_OCSSD_APPLE_OPC_ERR_INJECT		0xCC
#define SPDK_OCSSD_APPLE_OPC_ERR_INJECT_PEND	0xCD

#define SPDK_OCSSD_APPLE_LOG_CHUNK_INFO		0xCA

#define SPDK_OCSSD_APPLE_OPC_PARITY_INIT	0xA1

/* Admin command set */
//TODO: add set/get feature, like LED blink

/**
 * Chunk Information
 * Note: caller should check whether page_align and buffer_in_boundry is required.
 *
 * @param cmd nvme_cmd pointer
 * @param chunk_info_offset Offset of requested first chunk based on order of chunk descriptors.
 * @param nchunks Number of chunks to get their chunk information
 */
static inline void
spdk_ocssd_apple_chunkinfo_cmd(struct spdk_nvme_cmd *cmd, uint64_t chunk_info_offset, uint32_t nchunks)
{
	uint32_t payload_size, numd;
	uint16_t numdu, numdl;

	cmd->opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd->nsid = 1;

	payload_size = sizeof(struct spdk_ocssd_chunk_information_entry) * nchunks;
	numd = (payload_size >> 2) - 1;
	numdu = numd >> 16;
	numdl = numd & 0xffff;

	cmd->cdw10 = 0xC4 | (numdl << 16);
	cmd->cdw11 = numdu;

	*(uint64_t *)&cmd->cdw12 = chunk_info_offset;
}

/**
 * Writes 32kB of critical system data to a device managed region
 * Note: LBA and PPA fields will be ignored.
         LBA count must be set to 8 to indicate 32kB transfer
 *
 * @param cmd nvme_cmd pointer
 */
static inline void
spdk_ocssd_apple_write_sys_cmd(struct spdk_nvme_cmd *cmd)
{
	cmd->opc = SPDK_OCSSD_APPLE_OPC_WRITE_SYS;
	cmd->nsid = 1;
}

/**
 * Reads 32kB of critical system data from a device managed region.
 * Note: LBA field will be ignored.
         LBA count must be set to 8 to indicate 32kB transfer
 *
 * @param cmd nvme_cmd pointer
 */
static inline void
spdk_ocssd_apple_read_sys_cmd(struct spdk_nvme_cmd *cmd)
{
	cmd->opc = SPDK_OCSSD_APPLE_OPC_READ_SYS;
	cmd->nsid = 1;
}


/* IO/NVM command set */
/**
 * Writes data and metadata, if applicable, to the NVM controller for the blocks indicated.
 *
 * @param cmd nvme_cmd pointer
 * @param ppa Indicate the 64-bit address of the first physical location
 * to be written
 * @param lba Indicates the logical block address of the first logical
 * block to be written as part of the operation.
 */
static inline void
spdk_ocssd_apple_write_cmd(struct spdk_nvme_cmd *cmd, uint64_t ppa, uint32_t lba)
{
	cmd->opc = SPDK_NVME_OPC_WRITE;
	cmd->nsid = 1;

	*(uint64_t *)&cmd->cdw10 = (uint64_t)lba;
	*(uint64_t *)&cmd->cdw14 = ppa;
}

/**
 * Reads data and metadata, if applicable, from the NVM controller for the blocks indicated.
 * Note:
 * NULL LBA Value of 0x1FFFFFFFF will indicate the device should return the LBA
 * saved to media to the host as part of the Completion Queue Entry defined in spec.
 *
 * @param cmd nvme_cmd pointer
 * @param ppa Indicate the 64-bit address of the first physical location
 * to be read.
 * @param lba Indicates the logical block address of the first logical
 * block to be read as part of the operation.
 */
static inline void
spdk_ocssd_apple_read_cmd(struct spdk_nvme_cmd *cmd, uint64_t ppa, uint32_t lba)
{
	cmd->opc = SPDK_NVME_OPC_READ;
	cmd->nsid = 1;

	*(uint64_t *)&cmd->cdw10 = (uint64_t)lba;
	*(uint64_t *)&cmd->cdw14 = ppa;
}

/**
 * Best effort to commit data and metadata to nonvolatile media.
 *
 * @param cmd nvme_cmd pointer
 */
static inline void
spdk_ocssd_apple_flush_cmd(struct spdk_nvme_cmd *cmd)
{
	cmd->opc = SPDK_NVME_OPC_FLUSH;
	cmd->nsid = 1;
}

/**
 * Erases data and metadata, if applicable, on the NVM controller for the block indicated
 *
 * @param cmd nvme_cmd pointer
 * @param ppa Indicate the 64-bit address of the chunk to be reset as part of the operation
 * @param type Reset type: 0 is physical erase, put chunk in free state;
 * 			   1 is logical reset, put chunk in vacant state;
 */
static inline void
spdk_ocssd_apple_chunk_reset_cmd(struct spdk_nvme_cmd *cmd, uint64_t ppa, uint32_t type)
{
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_RESET;
	cmd->nsid = 1;

	*(uint64_t *)&cmd->cdw10 = (uint64_t)type;
	*(uint64_t *)&cmd->cdw14 = ppa;
}

/**
 * Copies data and metadata, if applicable, from a source location on media to a
 * destination location on media for the logical blocks indicated.
 *
 * @param cmd nvme_cmd pointer
 * @param src_ppas Single logical block address or a pointer to a list of logical block addresses.
 * @param dest_ppas Single logical block address or a pointer to a list of logical block addresses.
 * @param nblks Number of logical blocks in source and destination lists.
 * @param dest_ppa_seq If set to 1, indicates that the destination PPA list is sequential
based on a single starting PPA passed in DWords 14 and 15.
 */
static inline void
spdk_ocssd_apple_chunk_copy_cmd(struct spdk_nvme_cmd *cmd, uint64_t src_ppa_list,
		uint64_t dest_ppa_list, uint16_t nblks, uint16_t dest_ppa_seq)
{
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_COPY;
	cmd->nsid = 1;

	/* dword 7:6 */
	cmd->dptr.prp.prp1 = src_ppa_list;
	cmd->cdw12 = (dest_ppa_seq << 16) | nblks;
	*(uint64_t *)&cmd->cdw14 = dest_ppa_list;
}

/**
 * Notifies the device to initialize internal RAID engine for a new page stripe in
 * parity group. PPA information for parity group is passed in.
 * Note: nchks is a zeroes based value (value of 0 implies single chunk in the list).
 *
 * @param cmd nvme_cmd pointer
 * @param chk_ppas Starting PPA of each chunk to be included in this parity accumulation
 * @param nchks (value of 0 implies 1 chunk in list) Starting PPA of each chunk to be included in this parity accumulation.
 * @param parity_ppa Starting PPA of the chunk that parity will be automatically written to by device
 */
static inline void
spdk_ocssd_apple_parity_init_cmd(struct spdk_nvme_cmd *cmd, uint64_t chk_ppa_list,
		uint16_t nchks, uint64_t parity_ppa)
{
	cmd->opc = SPDK_OCSSD_APPLE_OPC_PARITY_INIT;
	cmd->nsid = 1;

	/* dword 7:6 */
	cmd->dptr.prp.prp1 = chk_ppa_list;
	cmd->cdw12 = (uint32_t)nchks;
	*(uint64_t *)&cmd->cdw14 = parity_ppa;
}

#ifdef __cplusplus
}
#endif


#endif // SPDK_OCSSD_APPLE_DEV_CMD_H
