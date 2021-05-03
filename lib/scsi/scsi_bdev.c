/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "scsi_internal.h"

/*
 * TODO: move bdev SCSI error code translation tests to bdev unit test
 * and remove this include.
 */
#include "spdk/bdev_module.h"

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"

#define SPDK_WORK_BLOCK_SIZE		(4ULL * 1024ULL * 1024ULL)
#define SPDK_WORK_ATS_BLOCK_SIZE	(1ULL * 1024ULL * 1024ULL)
#define MAX_SERIAL_STRING		32

#define DEFAULT_DISK_VENDOR		"INTEL"
#define DEFAULT_DISK_REVISION		"0001"
#define DEFAULT_DISK_ROTATION_RATE	1	/* Non-rotating medium */
#define DEFAULT_DISK_FORM_FACTOR	0x02	/* 3.5 inch */
#define DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT	256

#define INQUIRY_OFFSET(field)		offsetof(struct spdk_scsi_cdb_inquiry_data, field) + \
					sizeof(((struct spdk_scsi_cdb_inquiry_data *)0x0)->field)

static void bdev_scsi_process_block_resubmit(void *arg);

static int
hex2bin(char ch)
{
	if ((ch >= '0') && (ch <= '9')) {
		return ch - '0';
	}
	ch = tolower(ch);
	if ((ch >= 'a') && (ch <= 'f')) {
		return ch - 'a' + 10;
	}
	return (int)ch;
}

static void
bdev_scsi_set_naa_ieee_extended(const char *name, uint8_t *buf)
{
	int i, value, count = 0;
	uint64_t local_value;

	for (i = 0; (i < 16) && (name[i] != '\0'); i++) {
		value = hex2bin(name[i]);
		if (i % 2) {
			buf[count++] |= value << 4;
		} else {
			buf[count] = value;
		}
	}

	local_value = *(uint64_t *)buf;
	/*
	 * see spc3r23 7.6.3.6.2,
	 *  NAA IEEE Extended identifer format
	 */
	local_value &= 0x0fff000000ffffffull;
	/* NAA 02, and 00 03 47 for IEEE Intel */
	local_value |= 0x2000000347000000ull;

	to_be64((void *)buf, local_value);
}

static int
bdev_scsi_report_luns(struct spdk_scsi_lun *lun,
		      int sel, uint8_t *data, int alloc_len)
{
	struct spdk_scsi_dev *dev;
	uint64_t fmt_lun;
	int hlen, len = 0;
	int i;

	if (alloc_len < 8) {
		return -1;
	}

	if (sel == 0x00) {
		/* logical unit with addressing method */
	} else if (sel == 0x01) {
		/* well known logical unit */
	} else if (sel == 0x02) {
		/* logical unit */
	} else {
		return -1;
	}

	/* LUN LIST LENGTH */
	memset(data, 0, 4);

	/* Reserved */
	memset(&data[4], 0, 4);
	hlen = 8;

	dev = lun->dev;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		if (dev->lun[i] == NULL) {
			continue;
		}

		if (alloc_len - (hlen + len) < 8) {
			return -1;
		}

		fmt_lun = spdk_scsi_lun_id_int_to_fmt(i);

		/* LUN */
		to_be64(&data[hlen + len], fmt_lun);
		len += 8;
	}

	/* LUN LIST LENGTH */
	to_be32(data, len);

	return hlen + len;
}

static int
bdev_scsi_pad_scsi_name(char *dst, const char *name)
{
	size_t len;

	len = strlen(name);
	memcpy(dst, name, len);
	do {
		dst[len++] = '\0';
	} while (len & 3);

	return len;
}

static int
bdev_scsi_inquiry(struct spdk_bdev *bdev, struct spdk_scsi_task *task,
		  uint8_t *cdb, uint8_t *data, uint16_t alloc_len)
{
	struct spdk_scsi_lun	*lun;
	struct spdk_scsi_dev	*dev;
	struct spdk_scsi_port	*port;
	uint32_t blocks, optimal_blocks;
	int hlen = 0, plen, plen2;
	uint16_t len = 0;
	int pc;
	int pd;
	int evpd;
	int i;
	struct spdk_scsi_cdb_inquiry *inq = (struct spdk_scsi_cdb_inquiry *)cdb;

	/* standard inquiry command at lease with 36 Bytes */
	if (alloc_len < 0x24) {
		goto inq_error;
	}

	lun = task->lun;
	dev = lun->dev;
	port = task->target_port;

	pd = SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DISK;
	pc = inq->page_code;
	evpd = inq->evpd & 0x1;

	if (!evpd && pc) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -1;
	}

	if (evpd) {
		struct spdk_scsi_vpd_page *vpage = (struct spdk_scsi_vpd_page *)data;

		/* PERIPHERAL QUALIFIER(7-5) PERIPHERAL DEVICE TYPE(4-0) */
		vpage->peripheral_device_type = pd;
		vpage->peripheral_qualifier = SPDK_SPC_PERIPHERAL_QUALIFIER_CONNECTED;
		/* PAGE CODE */
		vpage->page_code = pc;

		/* Vital product data */
		switch (pc) {
		case SPDK_SPC_VPD_SUPPORTED_VPD_PAGES:
			hlen = 4;

			vpage->params[0] = SPDK_SPC_VPD_SUPPORTED_VPD_PAGES;
			vpage->params[1] = SPDK_SPC_VPD_UNIT_SERIAL_NUMBER;
			vpage->params[2] = SPDK_SPC_VPD_DEVICE_IDENTIFICATION;
			vpage->params[3] = SPDK_SPC_VPD_MANAGEMENT_NETWORK_ADDRESSES;
			vpage->params[4] = SPDK_SPC_VPD_EXTENDED_INQUIRY_DATA;
			vpage->params[5] = SPDK_SPC_VPD_MODE_PAGE_POLICY;
			vpage->params[6] = SPDK_SPC_VPD_SCSI_PORTS;
			vpage->params[7] = SPDK_SPC_VPD_BLOCK_LIMITS;
			vpage->params[8] = SPDK_SPC_VPD_BLOCK_DEV_CHARS;
			len = 9;
			if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
				vpage->params[9] = SPDK_SPC_VPD_BLOCK_THIN_PROVISION;
				len++;
			}

			/* PAGE LENGTH */
			to_be16(vpage->alloc_len, len);
			break;

		case SPDK_SPC_VPD_UNIT_SERIAL_NUMBER: {
			const char *name = spdk_bdev_get_name(bdev);

			hlen = 4;

			/* PRODUCT SERIAL NUMBER */
			len = strlen(name) + 1;
			if (len > MAX_SERIAL_STRING) {
				len = MAX_SERIAL_STRING;
			}

			memcpy(vpage->params, name, len - 1);
			vpage->params[len - 1] = 0;

			/* PAGE LENGTH */
			to_be16(vpage->alloc_len, len);
			break;
		}

		case SPDK_SPC_VPD_DEVICE_IDENTIFICATION: {
			const char *name = spdk_bdev_get_name(bdev);
			const char *product_name = spdk_bdev_get_product_name(bdev);
			uint8_t protocol_id = dev->protocol_id;
			uint8_t *buf = vpage->params;
			struct spdk_scsi_desig_desc *desig;

			hlen = 4;

			/* Check total length by calculated how much space all entries take */
			len = sizeof(struct spdk_scsi_desig_desc) + 8;
			len += sizeof(struct spdk_scsi_desig_desc) + 8 + 16 + MAX_SERIAL_STRING;
			len += sizeof(struct spdk_scsi_desig_desc) + SPDK_SCSI_DEV_MAX_NAME + 1;
			len += sizeof(struct spdk_scsi_desig_desc) + SPDK_SCSI_PORT_MAX_NAME_LENGTH;
			len += sizeof(struct spdk_scsi_desig_desc) + 4;
			len += sizeof(struct spdk_scsi_desig_desc) + 4;
			len += sizeof(struct spdk_scsi_desig_desc) + 4;
			if (sizeof(struct spdk_scsi_vpd_page) + len > alloc_len) {
				spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
							  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
							  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
							  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
				return -1;
			}

			/* Now fill out the designator array */

			/* NAA designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_BINARY;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_NAA;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_LOGICAL_UNIT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 8;
			bdev_scsi_set_naa_ieee_extended(name, desig->desig);
			len = sizeof(struct spdk_scsi_desig_desc) + 8;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* T10 Vendor ID designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_ASCII;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_T10_VENDOR_ID;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_LOGICAL_UNIT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 8 + 16 + MAX_SERIAL_STRING;
			spdk_strcpy_pad(desig->desig, DEFAULT_DISK_VENDOR, 8, ' ');
			spdk_strcpy_pad(&desig->desig[8], product_name, 16, ' ');
			spdk_strcpy_pad(&desig->desig[24], name, MAX_SERIAL_STRING, ' ');
			len += sizeof(struct spdk_scsi_desig_desc) + 8 + 16 + MAX_SERIAL_STRING;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* SCSI Device Name designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_UTF8;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_SCSI_NAME;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_TARGET_DEVICE;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = bdev_scsi_pad_scsi_name(desig->desig, dev->name);
			len += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* SCSI Port Name designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_UTF8;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_SCSI_NAME;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_TARGET_PORT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = snprintf(desig->desig, SPDK_SCSI_PORT_MAX_NAME_LENGTH, "%s", port->name);
			len += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* Relative Target Port designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_BINARY;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_RELATIVE_TARGET_PORT;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_TARGET_PORT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 4;
			memset(desig->desig, 0, 2); /* Reserved */
			to_be16(&desig->desig[2], port->index);
			len += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* Target port group designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_BINARY;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_TARGET_PORT_GROUP;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_TARGET_PORT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 4;
			memset(desig->desig, 0, 4);
			len += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* Logical unit group designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_BINARY;
			desig->protocol_id = protocol_id;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_LOGICAL_UNIT_GROUP;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_LOGICAL_UNIT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 4;
			memset(desig->desig, 0, 2); /* Reserved */
			to_be16(&desig->desig[2], dev->id);
			len += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			to_be16(vpage->alloc_len, len);

			break;
		}

		case SPDK_SPC_VPD_EXTENDED_INQUIRY_DATA: {
			struct spdk_scsi_vpd_ext_inquiry *vext = (struct spdk_scsi_vpd_ext_inquiry *)vpage;

			hlen = 4;
			memset((uint8_t *)vext + hlen, 0, sizeof(*vext) - hlen);

			/* RTO(3) GRD_CHK(2) APP_CHK(1) REF_CHK(0) */

			/* GROUP_SUP(4) PRIOR_SUP(3) HEADSUP(2) ORDSUP(1) SIMPSUP(0) */
			vext->sup = SPDK_SCSI_VEXT_HEADSUP | SPDK_SCSI_VEXT_SIMPSUP;

			/* NV_SUP(1) V_SUP(0) */

			/* Reserved[7-63] */

			len = 64 - hlen;

			/* PAGE LENGTH */
			to_be16(vpage->alloc_len, len);
			break;
		}

		case SPDK_SPC_VPD_MANAGEMENT_NETWORK_ADDRESSES:
			/* PAGE LENGTH */
			hlen = 4;

			to_be16(vpage->alloc_len, len);
			break;

		case SPDK_SPC_VPD_MODE_PAGE_POLICY: {
			struct spdk_scsi_mpage_policy_desc *pdesc =
				(struct spdk_scsi_mpage_policy_desc *)vpage->params;

			hlen = 4;

			/* Mode page policy descriptor 1 */

			/* POLICY PAGE CODE(5-0) */
			/* all page code */
			pdesc->page_code = 0x3f;

			/* POLICY SUBPAGE CODE */
			/* all sub page */
			pdesc->sub_page_code = 0xff;

			/* MLUS(7) MODE PAGE POLICY(1-0) */
			/* MLUS own copy */
			/* Shared MODE PAGE policy */
			pdesc->policy = 0;
			/* Reserved */
			pdesc->reserved = 0;

			len += 4;

			to_be16(vpage->alloc_len, len);
			break;
		}

		case SPDK_SPC_VPD_SCSI_PORTS: {
			/* PAGE LENGTH */
			hlen = 4;

			/* Identification descriptor list */
			for (i = 0; i < SPDK_SCSI_DEV_MAX_PORTS; i++) {
				struct spdk_scsi_port_desc *sdesc;
				struct spdk_scsi_tgt_port_desc *pdesc;

				if (!dev->port[i].is_used) {
					continue;
				}

				/* Identification descriptor N */
				sdesc = (struct spdk_scsi_port_desc *)&vpage->params[len];

				/* Reserved */
				sdesc->reserved = 0;

				/* RELATIVE PORT IDENTIFIER */
				to_be16(&sdesc->rel_port_id, dev->port[i].index);

				/* Reserved */
				sdesc->reserved2 = 0;

				/* INITIATOR PORT TRANSPORTID LENGTH */
				sdesc->init_port_len = 0;

				/* Reserved */
				sdesc->init_port_id = 0;

				/* TARGET PORT DESCRIPTORS LENGTH */
				sdesc->tgt_desc_len = 0;

				len += 12;

				plen2 = 0;
				/* Target port descriptor 1 */
				pdesc = (struct spdk_scsi_tgt_port_desc *)sdesc->tgt_desc;

				/* PROTOCOL IDENTIFIER(7-4) CODE SET(3-0) */
				pdesc->code_set =
					SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI << 4 |
					SPDK_SPC_VPD_CODE_SET_UTF8;

				/* PIV(7) ASSOCIATION(5-4) IDENTIFIER TYPE(3-0) */
				pdesc->desig_type = SPDK_SPC_VPD_DESIG_PIV |
						    SPDK_SPC_VPD_ASSOCIATION_TARGET_PORT << 4 |
						    SPDK_SPC_VPD_IDENTIFIER_TYPE_SCSI_NAME;

				/* Reserved */
				pdesc->reserved = 0;

				/* IDENTIFIER */
				plen = snprintf((char *)pdesc->designator,
						SPDK_SCSI_PORT_MAX_NAME_LENGTH, "%s",
						dev->port[i].name);
				pdesc->len = plen;

				plen2 += 4 + plen;

				/* TARGET PORT DESCRIPTORS LENGTH */
				to_be16(&sdesc->tgt_desc_len, plen2);

				len += plen2;
			}

			to_be16(vpage->alloc_len, len);
			break;
		}

		case SPDK_SPC_VPD_BLOCK_LIMITS: {
			uint32_t block_size = spdk_bdev_get_data_block_size(bdev);

			/* PAGE LENGTH */
			memset(&data[4], 0, 60);

			hlen = 4;

			/* WSNZ(0) */
			/* support zero length in WRITE SAME */

			/* MAXIMUM COMPARE AND WRITE LENGTH */
			blocks = SPDK_WORK_ATS_BLOCK_SIZE / block_size;

			if (blocks > 0xff) {
				blocks = 0xff;
			}

			data[5] = (uint8_t)blocks;

			/* force align to 4KB */
			if (block_size < 4096) {
				optimal_blocks = 4096 / block_size;
			} else {
				optimal_blocks = 1;
			}

			/* OPTIMAL TRANSFER LENGTH GRANULARITY */
			to_be16(&data[6], optimal_blocks);

			blocks = SPDK_WORK_BLOCK_SIZE / block_size;

			/* MAXIMUM TRANSFER LENGTH */
			to_be32(&data[8], blocks);
			/* OPTIMAL TRANSFER LENGTH */
			to_be32(&data[12], blocks);

			/* MAXIMUM PREFETCH XDREAD XDWRITE TRANSFER LENGTH */

			len = 20 - hlen;

			if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
				/*
				 * MAXIMUM UNMAP LBA COUNT: indicates the
				 * maximum  number of LBAs that may be
				 * unmapped by an UNMAP command.
				 */
				/* For now, choose 4MB as the maximum. */
				to_be32(&data[20], 4194304);

				/*
				 * MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT:
				 * indicates the maximum number of UNMAP
				 * block descriptors that shall be contained
				 * in the parameter data transferred to the
				 * device server for an UNMAP command.
				 * The bdev layer automatically splits unmap
				 * requests, so pick an arbitrary high number here.
				 */
				to_be32(&data[24], DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT);

				/*
				 * The UGAVALID bit is left as 0 which means neither the
				 * OPTIMAL UNMAP GRANULARITY nor the UNMAP GRANULARITY
				 * ALIGNMENT fields are valid.
				 */

				/*
				 * MAXIMUM WRITE SAME LENGTH: indicates the
				 * maximum number of contiguous logical blocks
				 * that the device server allows to be unmapped
				 * or written in a single WRITE SAME command.
				 */
				to_be64(&data[36], 512);

				/* Reserved */
				/* not specified */
				len = 64 - hlen;
			}

			to_be16(vpage->alloc_len, len);
			break;
		}

		case SPDK_SPC_VPD_BLOCK_DEV_CHARS: {
			/* PAGE LENGTH */
			hlen = 4;
			len = 64 - hlen;

			to_be16(&data[4], DEFAULT_DISK_ROTATION_RATE);

			/* Reserved */
			data[6] = 0;
			/* NOMINAL FORM FACTOR(3-0) */
			data[7] = DEFAULT_DISK_FORM_FACTOR << 4;
			/* Reserved */
			memset(&data[8], 0, 64 - 8);

			to_be16(vpage->alloc_len, len);
			break;
		}

		case SPDK_SPC_VPD_BLOCK_THIN_PROVISION: {
			if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
				goto inq_error;
			}

			hlen = 4;
			len = 7;

			/*
			 *  PAGE LENGTH : if the DP bit is set to one, then the
			 *  page length shall be set  0004h.
			 */
			to_be16(&data[2], 0x0004);

			/*
			 * THRESHOLD EXPONENT : it indicates the threshold set
			 * size in LBAs as a power of 2( i.e., the threshold
			 * set size  = 2 ^ (threshold exponent).
			 */
			data[4] = 0;

			/*
			 * Set the LBPU bit to indicate  the support for UNMAP
			 * command.
			 */
			data[5] |= SPDK_SCSI_UNMAP_LBPU;

			/*
			 * Set the provisioning type to thin provision.
			 */
			data[6] = SPDK_SCSI_UNMAP_THIN_PROVISIONING;

			to_be16(vpage->alloc_len, len);
			break;
		}

		default:
			if (pc >= 0xc0 && pc <= 0xff) {
				SPDK_DEBUGLOG(scsi, "Vendor specific INQUIRY VPD page 0x%x\n", pc);
			} else {
				SPDK_ERRLOG("unsupported INQUIRY VPD page 0x%x\n", pc);
			}
			goto inq_error;
		}
	} else {
		struct spdk_scsi_cdb_inquiry_data *inqdata =
			(struct spdk_scsi_cdb_inquiry_data *)data;

		/* Standard INQUIRY data */
		/* PERIPHERAL QUALIFIER(7-5) PERIPHERAL DEVICE TYPE(4-0) */
		inqdata->peripheral_device_type = pd;
		inqdata->peripheral_qualifier = SPDK_SPC_PERIPHERAL_QUALIFIER_CONNECTED;

		/* RMB(7) */
		inqdata->rmb = 0;

		/* VERSION */
		/* See SPC3/SBC2/MMC4/SAM2 for more details */
		inqdata->version = SPDK_SPC_VERSION_SPC3;

		/* NORMACA(5) HISUP(4) RESPONSE DATA FORMAT(3-0) */
		/* format 2 */ /* hierarchical support */
		inqdata->response = 2 | 1 << 4;

		hlen = 5;

		/* SCCS(7) ACC(6) TPGS(5-4) 3PC(3) PROTECT(0) */
		/* Not support TPGS */
		inqdata->flags = 0;

		/* MULTIP */
		inqdata->flags2 = 0x10;

		/* WBUS16(5) SYNC(4) LINKED(3) CMDQUE(1) VS(0) */
		/* CMDQUE */
		inqdata->flags3 = 0x2;

		/* T10 VENDOR IDENTIFICATION */
		spdk_strcpy_pad(inqdata->t10_vendor_id, DEFAULT_DISK_VENDOR, 8, ' ');

		/* PRODUCT IDENTIFICATION */
		spdk_strcpy_pad(inqdata->product_id, spdk_bdev_get_product_name(bdev), 16, ' ');

		/* PRODUCT REVISION LEVEL */
		spdk_strcpy_pad(inqdata->product_rev, DEFAULT_DISK_REVISION, 4, ' ');

		/*
		 * Standard inquiry data ends here.  Only populate remaining fields if alloc_len
		 *  indicates enough space to hold it.
		 */
		len = INQUIRY_OFFSET(product_rev) - 5;

		if (alloc_len >= INQUIRY_OFFSET(vendor)) {
			/* Vendor specific */
			memset(inqdata->vendor, 0x20, 20);
			len += sizeof(inqdata->vendor);
		}

		if (alloc_len >= INQUIRY_OFFSET(ius)) {
			/* CLOCKING(3-2) QAS(1) IUS(0) */
			inqdata->ius = 0;
			len += sizeof(inqdata->ius);
		}

		if (alloc_len >= INQUIRY_OFFSET(reserved)) {
			/* Reserved */
			inqdata->reserved = 0;
			len += sizeof(inqdata->reserved);
		}

		/* VERSION DESCRIPTOR 1-8 */
		if (alloc_len >= INQUIRY_OFFSET(reserved) + 2) {
			to_be16(&inqdata->desc[0], 0x0960);
			len += 2;
		}

		if (alloc_len >= INQUIRY_OFFSET(reserved) + 4) {
			to_be16(&inqdata->desc[2], 0x0300); /* SPC-3 (no version claimed) */
			len += 2;
		}

		if (alloc_len >= INQUIRY_OFFSET(reserved) + 6) {
			to_be16(&inqdata->desc[4], 0x320); /* SBC-2 (no version claimed) */
			len += 2;
		}

		if (alloc_len >= INQUIRY_OFFSET(reserved) + 8) {
			to_be16(&inqdata->desc[6], 0x0040); /* SAM-2 (no version claimed) */
			len += 2;
		}

		/*
		 * We only fill out 4 descriptors, but if the allocation length goes past
		 *  that, zero the remaining bytes.  This fixes some SCSI compliance tests
		 *  which expect a full 96 bytes to be returned, including the unpopulated
		 *  version descriptors 5-8 (4 * 2 = 8 bytes) plus the 22 bytes of reserved
		 *  space (bytes 74-95) - for a total of 30 bytes.
		 */
		if (alloc_len > INQUIRY_OFFSET(reserved) + 8) {
			i = alloc_len - (INQUIRY_OFFSET(reserved) + 8);
			if (i > 30) {
				i = 30;
			}
			memset(&inqdata->desc[8], 0, i);
			len += i;
		}

		/* ADDITIONAL LENGTH */
		inqdata->add_len = len;
	}

	return hlen + len;

inq_error:
	task->data_transferred = 0;
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
				  SPDK_SCSI_SENSE_NO_SENSE,
				  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -1;
}

static void
mode_sense_page_init(uint8_t *buf, int len, int page, int subpage)
{
	if (!buf) {
		return;
	}

	memset(buf, 0, len);
	if (subpage != 0) {
		buf[0] = page | 0x40; /* PAGE + SPF=1 */
		buf[1] = subpage;
		to_be16(&buf[2], len - 4);
	} else {
		buf[0] = page;
		buf[1] = len - 2;
	}
}

static int
bdev_scsi_mode_sense_page(struct spdk_bdev *bdev,
			  uint8_t *cdb, int pc, int page, int subpage,
			  uint8_t *data, struct spdk_scsi_task *task)
{
	uint8_t *cp = data;
	int len = 0;
	int plen;
	int i;

	if (pc == 0x00) {
		/* Current values */
	} else if (pc == 0x01) {
		/* Changeable values */
		/* As we currently do not support changeable values,
		   all parameters are reported as zero. */
	} else if (pc == 0x02) {
		/* Default values */
	} else {
		/* Saved values not supported */
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_SAVING_PARAMETERS_NOT_SUPPORTED,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -1;
	}

	switch (page) {
	case 0x00:
		/* Vendor specific */
		break;
	case 0x01:
		/* Read-Write Error Recovery */
		SPDK_DEBUGLOG(scsi,
			      "MODE_SENSE Read-Write Error Recovery\n");
		if (subpage != 0x00) {
			break;
		}
		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x02:
		/* Disconnect-Reconnect */
		SPDK_DEBUGLOG(scsi,
			      "MODE_SENSE Disconnect-Reconnect\n");
		if (subpage != 0x00) {
			break;
		}
		plen = 0x0e + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x03:
		/* Obsolete (Format Device) */
		break;
	case 0x04:
		/* Obsolete (Rigid Disk Geometry) */
		break;
	case 0x05:
		/* Obsolete (Rigid Disk Geometry) */
		break;
	case 0x06:
		/* Reserved */
		break;
	case 0x07:
		/* Verify Error Recovery */
		SPDK_DEBUGLOG(scsi,
			      "MODE_SENSE Verify Error Recovery\n");

		if (subpage != 0x00) {
			break;
		}

		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x08: {
		/* Caching */
		SPDK_DEBUGLOG(scsi, "MODE_SENSE Caching\n");
		if (subpage != 0x00) {
			break;
		}

		plen = 0x12 + 2;
		mode_sense_page_init(cp, plen, page, subpage);

		if (cp && spdk_bdev_has_write_cache(bdev) && pc != 0x01) {
			cp[2] |= 0x4;        /* WCE */
		}

		/* Read Cache Disable (RCD) = 1 */
		if (cp && pc != 0x01) {
			cp[2] |= 0x1;
		}

		len += plen;
		break;
	}
	case 0x09:
		/* Obsolete */
		break;
	case 0x0a:
		switch (subpage) {
		case 0x00:
			/* Control */
			SPDK_DEBUGLOG(scsi,
				      "MODE_SENSE Control\n");
			plen = 0x0a + 2;
			mode_sense_page_init(cp, plen, page, subpage);
			len += plen;
			break;
		case 0x01:
			/* Control Extension */
			SPDK_DEBUGLOG(scsi,
				      "MODE_SENSE Control Extension\n");
			plen = 0x1c + 4;
			mode_sense_page_init(cp, plen, page, subpage);
			len += plen;
			break;
		case 0xff:
			/* All subpages */
			len += bdev_scsi_mode_sense_page(bdev,
							 cdb, pc, page,
							 0x00,
							 cp ? &cp[len] : NULL, task);
			len += bdev_scsi_mode_sense_page(bdev,
							 cdb, pc, page,
							 0x01,
							 cp ? &cp[len] : NULL, task);
			break;
		default:
			/* 0x02-0x3e: Reserved */
			break;
		}
		break;
	case 0x0b:
		/* Obsolete (Medium Types Supported) */
		break;
	case 0x0c:
		/* Obsolete (Notch And Partitio) */
		break;
	case 0x0d:
		/* Obsolete */
		break;
	case 0x0e:
	case 0x0f:
		/* Reserved */
		break;
	case 0x10:
		/* XOR Control */
		SPDK_DEBUGLOG(scsi, "MODE_SENSE XOR Control\n");
		if (subpage != 0x00) {
			break;
		}
		plen = 0x16 + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x11:
	case 0x12:
	case 0x13:
		/* Reserved */
		break;
	case 0x14:
		/* Enclosure Services Management */
		break;
	case 0x15:
	case 0x16:
	case 0x17:
		/* Reserved */
		break;
	case 0x18:
		/* Protocol-Specific LUN */
		break;
	case 0x19:
		/* Protocol-Specific Port */
		break;
	case 0x1a:
		/* Power Condition */
		SPDK_DEBUGLOG(scsi,
			      "MODE_SENSE Power Condition\n");
		if (subpage != 0x00) {
			break;
		}
		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x1b:
		/* Reserved */
		break;
	case 0x1c:
		/* Informational Exceptions Control */
		SPDK_DEBUGLOG(scsi,
			      "MODE_SENSE Informational Exceptions Control\n");
		if (subpage != 0x00) {
			break;
		}

		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x1d:
	case 0x1e:
	case 0x1f:
		/* Reserved */
		break;
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
	case 0x28:
	case 0x29:
	case 0x2a:
	case 0x2b:
	case 0x2c:
	case 0x2d:
	case 0x2e:
	case 0x2f:
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x36:
	case 0x37:
	case 0x38:
	case 0x39:
	case 0x3a:
	case 0x3b:
	case 0x3c:
	case 0x3d:
	case 0x3e:
		/* Vendor-specific */
		break;
	case 0x3f:
		switch (subpage) {
		case 0x00:
			/* All mode pages */
			for (i = 0x00; i < 0x3e; i ++) {
				len += bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0x00,
					       cp ? &cp[len] : NULL, task);
			}
			break;
		case 0xff:
			/* All mode pages and subpages */
			for (i = 0x00; i < 0x3e; i ++) {
				len += bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0x00,
					       cp ? &cp[len] : NULL, task);
			}
			for (i = 0x00; i < 0x3e; i ++) {
				len += bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0xff,
					       cp ? &cp[len] : NULL, task);
			}
			break;
		default:
			/* 0x01-0x3e: Reserved */
			break;
		}
	}

	return len;
}

static int
bdev_scsi_mode_sense(struct spdk_bdev *bdev, int md,
		     uint8_t *cdb, int dbd, int llbaa, int pc,
		     int page, int subpage, uint8_t *data, struct spdk_scsi_task *task)
{
	uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_data_block_size(bdev);
	uint8_t *hdr, *bdesc, *pages;
	int hlen;
	int blen;
	int plen, total;

	assert(md == 6 || md == 10);

	if (md == 6) {
		hlen = 4;
		blen = 8; /* For MODE SENSE 6 only short LBA */
	} else {
		hlen = 8;
		blen = llbaa ? 16 : 8;
	}

	if (dbd) {
		blen = 0;
	}

	pages = data ? &data[hlen + blen] : NULL;
	plen = bdev_scsi_mode_sense_page(bdev, cdb, pc, page,
					 subpage,
					 pages, task);
	if (plen < 0) {
		return -1;
	}

	total = hlen + blen + plen;
	if (data == NULL) {
		return total;
	}

	hdr = &data[0];
	if (hlen == 4) {
		hdr[0] = total - 1;            /* Mode Data Length */
		hdr[1] = 0;                    /* Medium Type */
		hdr[2] = 0;                    /* Device-Specific Parameter */
		hdr[3] = blen;                 /* Block Descripter Length */
	} else {
		to_be16(&hdr[0], total - 2);   /* Mode Data Length */
		hdr[2] = 0;                    /* Medium Type */
		hdr[3] = 0;                    /* Device-Specific Parameter */
		hdr[4] = llbaa ? 0x1 : 0;      /* Long/short LBA */
		hdr[5] = 0;                    /* Reserved */
		to_be16(&hdr[6], blen);        /* Block Descripter Length */
	}

	bdesc = &data[hlen];
	if (blen == 16) {
		/* Number of Blocks */
		to_be64(&bdesc[0], num_blocks);
		/* Reserved */
		memset(&bdesc[8], 0, 4);
		/* Block Length */
		to_be32(&bdesc[12], block_size);
	} else if (blen == 8) {
		/* Number of Blocks */
		if (num_blocks > 0xffffffffULL) {
			memset(&bdesc[0], 0xff, 4);
		} else {
			to_be32(&bdesc[0], num_blocks);
		}

		/* Block Length */
		to_be32(&bdesc[4], block_size);
	}

	return total;
}

static void
bdev_scsi_task_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
			    void *cb_arg)
{
	struct spdk_scsi_task *task = cb_arg;
	int sc, sk, asc, ascq;

	spdk_bdev_io_get_scsi_status(bdev_io, &sc, &sk, &asc, &ascq);

	spdk_bdev_free_io(bdev_io);

	spdk_scsi_task_set_status(task, sc, sk, asc, ascq);
	scsi_lun_complete_task(task->lun, task);
}

static void
bdev_scsi_read_task_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
				 void *cb_arg)
{
	struct spdk_scsi_task *task = cb_arg;
	int sc, sk, asc, ascq;

	task->bdev_io = bdev_io;

	spdk_bdev_io_get_scsi_status(bdev_io, &sc, &sk, &asc, &ascq);

	spdk_scsi_task_set_status(task, sc, sk, asc, ascq);
	scsi_lun_complete_task(task->lun, task);
}

static void
bdev_scsi_task_complete_reset(struct spdk_bdev_io *bdev_io, bool success,
			      void *cb_arg)
{
	struct spdk_scsi_task *task = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		task->response = SPDK_SCSI_TASK_MGMT_RESP_SUCCESS;
	}

	scsi_lun_complete_reset_task(task->lun, task);
}

static void
bdev_scsi_queue_io(struct spdk_scsi_task *task, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_bdev *bdev = lun->bdev;
	struct spdk_io_channel *ch = lun->io_channel;
	int rc;

	task->bdev_io_wait.bdev = bdev;
	task->bdev_io_wait.cb_fn = cb_fn;
	task->bdev_io_wait.cb_arg = cb_arg;

	rc = spdk_bdev_queue_io_wait(bdev, ch, &task->bdev_io_wait);
	if (rc != 0) {
		assert(false);
	}
}

static int
bdev_scsi_sync(struct spdk_bdev *bdev, struct spdk_bdev_desc *bdev_desc,
	       struct spdk_io_channel *bdev_ch, struct spdk_scsi_task *task,
	       uint64_t lba, uint32_t num_blocks)
{
	uint64_t bdev_num_blocks;
	int rc;

	if (num_blocks == 0) {
		return SPDK_SCSI_TASK_COMPLETE;
	}

	bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);

	if (lba >= bdev_num_blocks || num_blocks > bdev_num_blocks ||
	    lba > (bdev_num_blocks - num_blocks)) {
		SPDK_ERRLOG("end of media\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	rc = spdk_bdev_flush_blocks(bdev_desc, bdev_ch, lba, num_blocks,
				    bdev_scsi_task_complete_cmd, task);

	if (rc) {
		if (rc == -ENOMEM) {
			bdev_scsi_queue_io(task, bdev_scsi_process_block_resubmit, task);
			return SPDK_SCSI_TASK_PENDING;
		}
		SPDK_ERRLOG("spdk_bdev_flush_blocks() failed\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}
	task->data_transferred = 0;
	return SPDK_SCSI_TASK_PENDING;
}

static uint64_t
_bytes_to_blocks(uint32_t block_size, uint64_t offset_bytes, uint64_t *offset_blocks,
		 uint64_t num_bytes, uint64_t *num_blocks)
{
	uint8_t shift_cnt;

	/* Avoid expensive div operations if possible. These spdk_u32 functions are very cheap. */
	if (spdk_likely(spdk_u32_is_pow2(block_size))) {
		shift_cnt = spdk_u32log2(block_size);
		*offset_blocks = offset_bytes >> shift_cnt;
		*num_blocks = num_bytes >> shift_cnt;
		return (offset_bytes - (*offset_blocks << shift_cnt)) |
		       (num_bytes - (*num_blocks << shift_cnt));
	} else {
		*offset_blocks = offset_bytes / block_size;
		*num_blocks = num_bytes / block_size;
		return (offset_bytes % block_size) | (num_bytes % block_size);
	}
}

static int
bdev_scsi_readwrite(struct spdk_bdev *bdev, struct spdk_bdev_desc *bdev_desc,
		    struct spdk_io_channel *bdev_ch, struct spdk_scsi_task *task,
		    uint64_t lba, uint32_t xfer_len, bool is_read)
{
	uint64_t bdev_num_blocks, offset_blocks, num_blocks;
	uint32_t max_xfer_len, block_size;
	int sk = SPDK_SCSI_SENSE_NO_SENSE, asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
	int rc;

	task->data_transferred = 0;

	if (spdk_unlikely(task->dxfer_dir != SPDK_SCSI_DIR_NONE &&
			  task->dxfer_dir != (is_read ? SPDK_SCSI_DIR_FROM_DEV : SPDK_SCSI_DIR_TO_DEV))) {
		SPDK_ERRLOG("Incorrect data direction\n");
		goto check_condition;
	}

	bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	if (spdk_unlikely(bdev_num_blocks <= lba || bdev_num_blocks - lba < xfer_len)) {
		SPDK_DEBUGLOG(scsi, "end of media\n");
		sk = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
		asc = SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		goto check_condition;
	}

	if (spdk_unlikely(xfer_len == 0)) {
		task->status = SPDK_SCSI_STATUS_GOOD;
		return SPDK_SCSI_TASK_COMPLETE;
	}

	block_size = spdk_bdev_get_data_block_size(bdev);

	/* Transfer Length is limited to the Block Limits VPD page Maximum Transfer Length */
	max_xfer_len = SPDK_WORK_BLOCK_SIZE / block_size;
	if (spdk_unlikely(xfer_len > max_xfer_len)) {
		SPDK_ERRLOG("xfer_len %" PRIu32 " > maximum transfer length %" PRIu32 "\n",
			    xfer_len, max_xfer_len);
		sk = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
		asc = SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB;
		goto check_condition;
	}

	if (!is_read) {
		/* Additional check for Transfer Length */
		if (xfer_len * block_size > task->transfer_len) {
			SPDK_ERRLOG("xfer_len %" PRIu32 " * block_size %" PRIu32 " > transfer_len %u\n",
				    xfer_len, block_size, task->transfer_len);
			goto check_condition;
		}
	}

	if (_bytes_to_blocks(block_size, task->offset, &offset_blocks, task->length, &num_blocks) != 0) {
		SPDK_ERRLOG("task's offset %" PRIu64 " or length %" PRIu32 " is not block multiple\n",
			    task->offset, task->length);
		goto check_condition;
	}

	offset_blocks += lba;

	SPDK_DEBUGLOG(scsi,
		      "%s: lba=%"PRIu64", len=%"PRIu64"\n",
		      is_read ? "Read" : "Write", offset_blocks, num_blocks);

	if (is_read) {
		rc = spdk_bdev_readv_blocks(bdev_desc, bdev_ch, task->iovs, task->iovcnt,
					    offset_blocks, num_blocks,
					    bdev_scsi_read_task_complete_cmd, task);
	} else {
		rc = spdk_bdev_writev_blocks(bdev_desc, bdev_ch, task->iovs, task->iovcnt,
					     offset_blocks, num_blocks,
					     bdev_scsi_task_complete_cmd, task);
	}

	if (rc) {
		if (rc == -ENOMEM) {
			bdev_scsi_queue_io(task, bdev_scsi_process_block_resubmit, task);
			return SPDK_SCSI_TASK_PENDING;
		}
		SPDK_ERRLOG("spdk_bdev_%s_blocks() failed\n", is_read ? "readv" : "writev");
		goto check_condition;
	}

	task->data_transferred = task->length;
	return SPDK_SCSI_TASK_PENDING;

check_condition:
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION, sk, asc,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return SPDK_SCSI_TASK_COMPLETE;
}

struct spdk_bdev_scsi_unmap_ctx {
	struct spdk_scsi_task		*task;
	struct spdk_scsi_unmap_bdesc	desc[DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT];
	uint32_t			count;
};

static int bdev_scsi_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *bdev_desc,
			   struct spdk_io_channel *bdev_ch, struct spdk_scsi_task *task,
			   struct spdk_bdev_scsi_unmap_ctx *ctx);

static void
bdev_scsi_task_complete_unmap_cmd(struct spdk_bdev_io *bdev_io, bool success,
				  void *cb_arg)
{
	struct spdk_bdev_scsi_unmap_ctx *ctx = cb_arg;
	struct spdk_scsi_task *task = ctx->task;
	int sc, sk, asc, ascq;

	ctx->count--;

	task->bdev_io = bdev_io;

	if (task->status == SPDK_SCSI_STATUS_GOOD) {
		spdk_bdev_io_get_scsi_status(bdev_io, &sc, &sk, &asc, &ascq);
		spdk_scsi_task_set_status(task, sc, sk, asc, ascq);
	}

	if (ctx->count == 0) {
		scsi_lun_complete_task(task->lun, task);
		free(ctx);
	}
}

static int
__copy_desc(struct spdk_bdev_scsi_unmap_ctx *ctx, uint8_t *data, size_t data_len)
{
	uint16_t	desc_data_len;
	uint16_t	desc_count;

	if (!data) {
		return -EINVAL;
	}

	if (data_len < 8) {
		/* We can't even get the reported length, so fail. */
		return -EINVAL;
	}

	desc_data_len = from_be16(&data[2]);
	desc_count = desc_data_len / 16;

	if (desc_data_len > (data_len - 8)) {
		SPDK_ERRLOG("Error - desc_data_len (%u) > data_len (%zu) - 8\n",
			    desc_data_len, data_len);
		return -EINVAL;
	}

	if (desc_count > DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT) {
		SPDK_ERRLOG("desc_count (%u) greater than max allowed (%u)\n",
			    desc_count, DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT);
		return -EINVAL;
	}

	memcpy(ctx->desc, &data[8], desc_data_len);
	return desc_count;
}

static void
bdev_scsi_unmap_resubmit(void *arg)
{
	struct spdk_bdev_scsi_unmap_ctx	*ctx = arg;
	struct spdk_scsi_task *task = ctx->task;
	struct spdk_scsi_lun *lun = task->lun;

	bdev_scsi_unmap(lun->bdev, lun->bdev_desc, lun->io_channel, task, ctx);
}

static int
bdev_scsi_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *bdev_desc,
		struct spdk_io_channel *bdev_ch, struct spdk_scsi_task *task,
		struct spdk_bdev_scsi_unmap_ctx *ctx)
{
	uint8_t				*data;
	int				i, desc_count = -1;
	int				data_len;
	int				rc;

	assert(task->status == SPDK_SCSI_STATUS_GOOD);

	if (ctx == NULL) {
		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return SPDK_SCSI_TASK_COMPLETE;
		}

		ctx->task = task;
		ctx->count = 0;
	}


	if (task->iovcnt == 1) {
		data = (uint8_t *)task->iovs[0].iov_base;
		data_len = task->iovs[0].iov_len;
		desc_count = __copy_desc(ctx, data, data_len);
	} else {
		data = spdk_scsi_task_gather_data(task, &data_len);
		if (data) {
			desc_count = __copy_desc(ctx, data, data_len);
			free(data);
		}
	}

	if (desc_count < 0) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		free(ctx);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	for (i = ctx->count; i < desc_count; i++) {
		struct spdk_scsi_unmap_bdesc	*desc;
		uint64_t offset_blocks;
		uint64_t num_blocks;

		desc = &ctx->desc[i];

		offset_blocks = from_be64(&desc->lba);
		num_blocks = from_be32(&desc->block_count);

		if (num_blocks == 0) {
			continue;
		}

		ctx->count++;
		rc = spdk_bdev_unmap_blocks(bdev_desc, bdev_ch, offset_blocks, num_blocks,
					    bdev_scsi_task_complete_unmap_cmd, ctx);

		if (rc) {
			if (rc == -ENOMEM) {
				bdev_scsi_queue_io(task, bdev_scsi_unmap_resubmit, ctx);
				/* Unmap was not yet submitted to bdev */
				ctx->count--;
				return SPDK_SCSI_TASK_PENDING;
			}
			SPDK_ERRLOG("SCSI Unmapping failed\n");
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			ctx->count--;
			/* We can't complete here - we may have to wait for previously
			 * submitted unmaps to complete */
			break;
		}
	}

	if (ctx->count == 0) {
		free(ctx);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	return SPDK_SCSI_TASK_PENDING;
}

static int
bdev_scsi_process_block(struct spdk_scsi_task *task)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_bdev *bdev = lun->bdev;
	uint64_t lba;
	uint32_t xfer_len;
	uint32_t len = 0;
	uint8_t *cdb = task->cdb;

	/* XXX: We need to support FUA bit for writes! */
	switch (cdb[0]) {
	case SPDK_SBC_READ_6:
	case SPDK_SBC_WRITE_6:
		lba = (uint64_t)cdb[1] << 16;
		lba |= (uint64_t)cdb[2] << 8;
		lba |= (uint64_t)cdb[3];
		xfer_len = cdb[4];
		if (xfer_len == 0) {
			xfer_len = 256;
		}
		return bdev_scsi_readwrite(bdev, lun->bdev_desc, lun->io_channel,
					   task, lba, xfer_len,
					   cdb[0] == SPDK_SBC_READ_6);

	case SPDK_SBC_READ_10:
	case SPDK_SBC_WRITE_10:
		lba = from_be32(&cdb[2]);
		xfer_len = from_be16(&cdb[7]);
		return bdev_scsi_readwrite(bdev, lun->bdev_desc, lun->io_channel,
					   task, lba, xfer_len,
					   cdb[0] == SPDK_SBC_READ_10);

	case SPDK_SBC_READ_12:
	case SPDK_SBC_WRITE_12:
		lba = from_be32(&cdb[2]);
		xfer_len = from_be32(&cdb[6]);
		return bdev_scsi_readwrite(bdev, lun->bdev_desc, lun->io_channel,
					   task, lba, xfer_len,
					   cdb[0] == SPDK_SBC_READ_12);
	case SPDK_SBC_READ_16:
	case SPDK_SBC_WRITE_16:
		lba = from_be64(&cdb[2]);
		xfer_len = from_be32(&cdb[10]);
		return bdev_scsi_readwrite(bdev, lun->bdev_desc, lun->io_channel,
					   task, lba, xfer_len,
					   cdb[0] == SPDK_SBC_READ_16);

	case SPDK_SBC_READ_CAPACITY_10: {
		uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);
		uint8_t buffer[8];

		if (num_blocks - 1 > 0xffffffffULL) {
			memset(buffer, 0xff, 4);
		} else {
			to_be32(buffer, num_blocks - 1);
		}
		to_be32(&buffer[4], spdk_bdev_get_data_block_size(bdev));

		len = spdk_min(task->length, sizeof(buffer));
		if (spdk_scsi_task_scatter_data(task, buffer, len) < 0) {
			break;
		}

		task->data_transferred = len;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;
	}

	case SPDK_SPC_SERVICE_ACTION_IN_16:
		switch (cdb[1] & 0x1f) { /* SERVICE ACTION */
		case SPDK_SBC_SAI_READ_CAPACITY_16: {
			uint8_t buffer[32] = {0};

			to_be64(&buffer[0], spdk_bdev_get_num_blocks(bdev) - 1);
			to_be32(&buffer[8], spdk_bdev_get_data_block_size(bdev));
			/*
			 * Set the TPE bit to 1 to indicate thin provisioning.
			 * The position of TPE bit is the 7th bit in 14th byte
			 * in READ CAPACITY (16) parameter data.
			 */
			if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
				buffer[14] |= 1 << 7;
			}

			len = spdk_min(from_be32(&cdb[10]), sizeof(buffer));
			if (spdk_scsi_task_scatter_data(task, buffer, len) < 0) {
				break;
			}

			task->data_transferred = len;
			task->status = SPDK_SCSI_STATUS_GOOD;
			break;
		}

		default:
			return SPDK_SCSI_TASK_UNKNOWN;
		}
		break;

	case SPDK_SBC_SYNCHRONIZE_CACHE_10:
	case SPDK_SBC_SYNCHRONIZE_CACHE_16:
		if (cdb[0] == SPDK_SBC_SYNCHRONIZE_CACHE_10) {
			lba = from_be32(&cdb[2]);
			len = from_be16(&cdb[7]);
		} else {
			lba = from_be64(&cdb[2]);
			len = from_be32(&cdb[10]);
		}

		if (len == 0) {
			len = spdk_bdev_get_num_blocks(bdev) - lba;
		}

		return bdev_scsi_sync(bdev, lun->bdev_desc, lun->io_channel, task, lba, len);
		break;

	case SPDK_SBC_UNMAP:
		return bdev_scsi_unmap(bdev, lun->bdev_desc, lun->io_channel, task, NULL);

	default:
		return SPDK_SCSI_TASK_UNKNOWN;
	}

	return SPDK_SCSI_TASK_COMPLETE;
}

static void
bdev_scsi_process_block_resubmit(void *arg)
{
	struct spdk_scsi_task *task = arg;

	bdev_scsi_process_block(task);
}

static int
bdev_scsi_check_len(struct spdk_scsi_task *task, int len, int min_len)
{
	if (len >= min_len) {
		return 0;
	}

	/* INVALID FIELD IN CDB */
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
				  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
				  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -1;
}

static int
bdev_scsi_process_primary(struct spdk_scsi_task *task)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_bdev *bdev = lun->bdev;
	int alloc_len = -1;
	int data_len = -1;
	uint8_t *cdb = task->cdb;
	uint8_t *data = NULL;
	int rc = 0;
	int pllen, md = 0;
	int llba;
	int dbd, pc, page, subpage;
	int cmd_parsed = 0;

	switch (cdb[0]) {
	case SPDK_SPC_INQUIRY:
		alloc_len = from_be16(&cdb[3]);
		data_len = spdk_max(4096, alloc_len);
		data = calloc(1, data_len);
		assert(data != NULL);
		rc = bdev_scsi_inquiry(bdev, task, cdb, data, data_len);
		data_len = spdk_min(rc, data_len);
		if (rc < 0) {
			break;
		}

		SPDK_LOGDUMP(scsi, "INQUIRY", data, data_len);
		break;

	case SPDK_SPC_REPORT_LUNS: {
		int sel;

		sel = cdb[2];
		SPDK_DEBUGLOG(scsi, "sel=%x\n", sel);

		alloc_len = from_be32(&cdb[6]);
		rc = bdev_scsi_check_len(task, alloc_len, 16);
		if (rc < 0) {
			break;
		}

		data_len = spdk_max(4096, alloc_len);
		data = calloc(1, data_len);
		assert(data != NULL);
		rc = bdev_scsi_report_luns(task->lun, sel, data, data_len);
		data_len = rc;
		if (rc < 0) {
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			break;
		}

		SPDK_LOGDUMP(scsi, "REPORT LUNS", data, data_len);
		break;
	}

	case SPDK_SPC_MODE_SELECT_6:
	case SPDK_SPC_MODE_SELECT_10:
		if (cdb[0] == SPDK_SPC_MODE_SELECT_6) {
			/* MODE_SELECT(6) must have at least a 4 byte header. */
			md = 4;
			pllen = cdb[4];
		} else {
			/* MODE_SELECT(10) must have at least an 8 byte header. */
			md = 8;
			pllen = from_be16(&cdb[7]);
		}

		if (pllen == 0) {
			break;
		}

		rc = bdev_scsi_check_len(task, pllen, md);
		if (rc < 0) {
			break;
		}

		data = spdk_scsi_task_gather_data(task, &rc);
		if (rc < 0) {
			break;
		}
		data_len = rc;

		rc = bdev_scsi_check_len(task, data_len, spdk_max(pllen, md));
		if (rc < 0) {
			break;
		}

		rc = pllen;
		data_len = 0;
		break;

	case SPDK_SPC_MODE_SENSE_6:
		alloc_len = cdb[4];
		md = 6;
	/* FALLTHROUGH */
	case SPDK_SPC_MODE_SENSE_10:
		llba = 0;

		if (md == 0) {
			alloc_len = from_be16(&cdb[7]);
			llba = !!(cdb[1] & 0x10);
			md = 10;
		}

		dbd = !!(cdb[1] & 0x8);
		pc = (cdb[2] & 0xc0) >> 6;
		page = cdb[2] & 0x3f;
		subpage = cdb[3];

		/* First call with no buffer to discover needed buffer size */
		rc = bdev_scsi_mode_sense(bdev, md,
					  cdb, dbd, llba, pc,
					  page, subpage,
					  NULL, task);
		if (rc < 0) {
			break;
		}

		data_len = rc;
		data = calloc(1, data_len);
		assert(data != NULL);

		/* First call with no buffer to discover needed buffer size */
		rc = bdev_scsi_mode_sense(bdev, md,
					  cdb, dbd, llba, pc,
					  page, subpage,
					  data, task);
		if (rc < 0) {
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
						  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			break;
		}
		break;

	case SPDK_SPC_REQUEST_SENSE: {
		int desc;
		int sk, asc, ascq;

		desc = cdb[1] & 0x1;
		if (desc != 0) {
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
						  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			break;
		}

		alloc_len = cdb[4];

		/* NO ADDITIONAL SENSE INFORMATION */
		sk = SPDK_SCSI_SENSE_NO_SENSE;
		asc = 0x00;
		ascq = 0x00;

		spdk_scsi_task_build_sense_data(task, sk, asc, ascq);

		data_len = task->sense_data_len;
		data = calloc(1, data_len);
		assert(data != NULL);
		memcpy(data, task->sense_data, data_len);
		break;
	}

	case SPDK_SPC_LOG_SELECT:
		SPDK_DEBUGLOG(scsi, "LOG_SELECT\n");
		cmd_parsed = 1;
	/* FALLTHROUGH */
	case SPDK_SPC_LOG_SENSE:
		if (!cmd_parsed) {
			SPDK_DEBUGLOG(scsi, "LOG_SENSE\n");
		}

		/* INVALID COMMAND OPERATION CODE */
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_COMMAND_OPERATION_CODE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		rc = -1;
		break;

	case SPDK_SPC_TEST_UNIT_READY:
		SPDK_DEBUGLOG(scsi, "TEST_UNIT_READY\n");
		cmd_parsed = 1;
	/* FALLTHROUGH */
	case SPDK_SBC_START_STOP_UNIT:
		if (!cmd_parsed) {
			SPDK_DEBUGLOG(scsi, "START_STOP_UNIT\n");
		}

		rc = 0;
		break;

	case SPDK_SPC_PERSISTENT_RESERVE_OUT:
		pllen = from_be32(&cdb[5]);
		rc = bdev_scsi_check_len(task, pllen, 24);
		if (rc < 0) {
			break;
		}

		data = spdk_scsi_task_gather_data(task, &rc);
		if (rc < 0) {
			break;
		}
		data_len = rc;
		if (data_len < 24) {
			rc = -1;
			break;
		}

		rc = scsi_pr_out(task, cdb, data, data_len);
		if (rc < 0) {
			break;
		}
		rc = pllen;
		data_len = 0;
		break;

	case SPDK_SPC_PERSISTENT_RESERVE_IN:
		alloc_len = from_be16(&cdb[7]);
		data_len = alloc_len;
		data = calloc(1, data_len);
		assert(data != NULL);
		rc = scsi_pr_in(task, cdb, data, data_len);
		break;

	case SPDK_SPC2_RESERVE_6:
	case SPDK_SPC2_RESERVE_10:
		rc = scsi2_reserve(task, cdb);
		if (rc == 0) {
			if (cdb[0] == SPDK_SPC2_RESERVE_10) {
				rc = from_be16(&cdb[7]);
			}
			data_len = 0;
		}
		break;

	case SPDK_SPC2_RELEASE_6:
	case SPDK_SPC2_RELEASE_10:
		rc = scsi2_release(task);
		break;

	default:
		return SPDK_SCSI_TASK_UNKNOWN;
	}

	if (rc >= 0 && data_len > 0) {
		assert(alloc_len >= 0);
		spdk_scsi_task_scatter_data(task, data, spdk_min(alloc_len, data_len));
		rc = spdk_min(data_len, alloc_len);
	}

	if (rc >= 0) {
		task->data_transferred = rc;
		task->status = SPDK_SCSI_STATUS_GOOD;
	}

	if (data) {
		free(data);
	}

	return SPDK_SCSI_TASK_COMPLETE;
}

int
bdev_scsi_execute(struct spdk_scsi_task *task)
{
	int rc;

	if ((rc = bdev_scsi_process_block(task)) == SPDK_SCSI_TASK_UNKNOWN) {
		if ((rc = bdev_scsi_process_primary(task)) == SPDK_SCSI_TASK_UNKNOWN) {
			SPDK_DEBUGLOG(scsi, "unsupported SCSI OP=0x%x\n", task->cdb[0]);
			/* INVALID COMMAND OPERATION CODE */
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
						  SPDK_SCSI_ASC_INVALID_COMMAND_OPERATION_CODE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return SPDK_SCSI_TASK_COMPLETE;
		}
	}

	return rc;
}

static void
bdev_scsi_reset_resubmit(void *arg)
{
	struct spdk_scsi_task *task = arg;

	bdev_scsi_reset(task);
}

void
bdev_scsi_reset(struct spdk_scsi_task *task)
{
	struct spdk_scsi_lun *lun = task->lun;
	int rc;

	rc = spdk_bdev_reset(lun->bdev_desc, lun->io_channel, bdev_scsi_task_complete_reset,
			     task);
	if (rc == -ENOMEM) {
		bdev_scsi_queue_io(task, bdev_scsi_reset_resubmit, task);
	}
}

bool
bdev_scsi_get_dif_ctx(struct spdk_bdev *bdev, struct spdk_scsi_task *task,
		      struct spdk_dif_ctx *dif_ctx)
{
	uint32_t ref_tag = 0, dif_check_flags = 0, data_offset;
	uint8_t *cdb;
	int rc;

	if (spdk_likely(spdk_bdev_get_md_size(bdev) == 0)) {
		return false;
	}

	cdb = task->cdb;
	data_offset = task->offset;

	/* We use lower 32 bits of LBA as Reference. Tag */
	switch (cdb[0]) {
	case SPDK_SBC_READ_6:
	case SPDK_SBC_WRITE_6:
		ref_tag = (uint32_t)cdb[1] << 16;
		ref_tag |= (uint32_t)cdb[2] << 8;
		ref_tag |= (uint32_t)cdb[3];
		break;
	case SPDK_SBC_READ_10:
	case SPDK_SBC_WRITE_10:
	case SPDK_SBC_READ_12:
	case SPDK_SBC_WRITE_12:
		ref_tag = from_be32(&cdb[2]);
		break;
	case SPDK_SBC_READ_16:
	case SPDK_SBC_WRITE_16:
		ref_tag = (uint32_t)from_be64(&cdb[2]);
		break;
	default:
		return false;
	}

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD)) {
		dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	rc = spdk_dif_ctx_init(dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       dif_check_flags,
			       ref_tag, 0, 0, data_offset, 0);

	return (rc == 0) ? true : false;
}
