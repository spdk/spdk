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

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/string.h"

#define SPDK_WORK_BLOCK_SIZE		(1ULL * 1024ULL * 1024ULL)
#define SPDK_WORK_ATS_BLOCK_SIZE	(1ULL * 1024ULL * 1024ULL)
#define MAX_SERIAL_STRING		32

#define DEFAULT_DISK_VENDOR		"INTEL"
#define DEFAULT_DISK_REVISION		"0001"
#define DEFAULT_DISK_ROTATION_RATE	1	/* Non-rotating medium */
#define DEFAULT_DISK_FORM_FACTOR	0x02	/* 3.5 inch */

#define INQUIRY_OFFSET(field)		offsetof(struct spdk_scsi_cdb_inquiry_data, field) + \
					sizeof(((struct spdk_scsi_cdb_inquiry_data *)0x0)->field)

static int
spdk_hex2bin(char ch)
{
	if ((ch >= '0') && (ch <= '9'))
		return ch - '0';
	ch = tolower(ch);
	if ((ch >= 'a') && (ch <= 'f'))
		return ch - 'a' + 10;
	return (int)ch;
}

static void
spdk_bdev_scsi_set_local_naa(char *name, uint8_t *buf)
{
	int i, value, count = 0;
	uint64_t naa, local_value;

	for (i = 0; (i < 16) && (name[i] != '\0'); i++) {
		value = spdk_hex2bin(name[i]);
		if (i % 2) {
			buf[count++] |= value << 4;
		} else {
			buf[count] = value;
		}
	}
	/* NAA locally assigned filed */
	naa = 0x3;
	local_value = *(uint64_t *)buf;
	local_value = (naa << 60) | (local_value >> 4);
	to_be64((void *)buf, local_value);
}

static int
spdk_bdev_scsi_report_luns(struct spdk_scsi_lun *lun,
			   int sel, uint8_t *data, int alloc_len)
{
	struct spdk_scsi_dev *dev;
	uint64_t fmt_lun, lun_id, method;
	int hlen, len = 0;
	int i;

	if (alloc_len < 8)
		return -1;

	if (sel == 0x00) {
		/* logical unit with addressing method */
	} else if (sel == 0x01) {
		/* well known logical unit */
	} else if (sel == 0x02) {
		/* logical unit */
	} else
		return -1;

	/* LUN LIST LENGTH */
	memset(data, 0, 4);

	/* Reserved */
	memset(&data[4], 0, 4);
	hlen = 8;

	dev = lun->dev;
	for (i = 0; i < dev->maxlun; i++) {
		if (dev->lun[i] == NULL)
			continue;

		if (alloc_len - (hlen + len) < 8)
			return -1;

		lun_id = (uint64_t)i;

		if (dev->maxlun <= 0x0100) {
			/* below 256 */
			method = 0x00U;
			fmt_lun = (method & 0x03U) << 62;
			fmt_lun |= (lun_id & 0x00ffU) << 48;
		} else if (dev->maxlun <= 0x4000) {
			/* below 16384 */
			method = 0x01U;
			fmt_lun = (method & 0x03U) << 62;
			fmt_lun |= (lun_id & 0x3fffU) << 48;
		} else {
			/* XXX */
			fmt_lun = 0;
		}

		/* LUN */
		to_be64(&data[hlen + len], fmt_lun);
		len += 8;
	}

	/* LUN LIST LENGTH */
	to_be32(data, len);

	return hlen + len;
}

static int
spdk_bdev_scsi_inquiry(struct spdk_bdev *bdev, struct spdk_scsi_task *task,
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
	if (alloc_len < 0x24)
		goto inq_error;

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
		vpage->peripheral = pd;
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
			/* SBC Block Limits */
			vpage->params[7] = 0xb0;
			/* SBC Block Device Characteristics */
			vpage->params[8] = 0xb1;
			len = 9;
			if (bdev->thin_provisioning) {
				/* SBC Thin Provisioning */
				vpage->params[9] = 0xb2;
				len++;
			}

			/* PAGE LENGTH */
			to_be16(vpage->alloc_len, len);
			break;

		case SPDK_SPC_VPD_UNIT_SERIAL_NUMBER:
			hlen = 4;

			/* PRODUCT SERIAL NUMBER */
			len = strlen(bdev->name) + 1;
			if (len > MAX_SERIAL_STRING) {
				len = MAX_SERIAL_STRING;
			}

			memcpy(vpage->params, bdev->name, len - 1);
			vpage->params[len - 1] = 0;

			/* PAGE LENGTH */
			to_be16(vpage->alloc_len, len);
			break;

		case SPDK_SPC_VPD_DEVICE_IDENTIFICATION: {
			uint8_t *buf = vpage->params;
			struct spdk_scsi_desig_desc *desig;

			hlen = 4;

			/* Check total length by calculated how much space all entries take */
			len = sizeof(struct spdk_scsi_desig_desc) + 8;
			len += sizeof(struct spdk_scsi_desig_desc) + 8 + 16 + MAX_SERIAL_STRING;
			len += sizeof(struct spdk_scsi_desig_desc) + SPDK_SCSI_DEV_MAX_NAME;
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
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_NAA;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_LOGICAL_UNIT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 8;
			spdk_bdev_scsi_set_local_naa(bdev->name, desig->desig);
			len = sizeof(struct spdk_scsi_desig_desc) + 8;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* T10 Vendor ID designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_ASCII;
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_T10_VENDOR_ID;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_LOGICAL_UNIT;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = 8 + 16 + MAX_SERIAL_STRING;
			spdk_strcpy_pad(desig->desig, DEFAULT_DISK_VENDOR, 8, ' ');
			spdk_strcpy_pad(&desig->desig[8], bdev->product_name, 16, ' ');
			spdk_strcpy_pad(&desig->desig[24], bdev->name, MAX_SERIAL_STRING, ' ');
			len += sizeof(struct spdk_scsi_desig_desc) + 8 + 16 + MAX_SERIAL_STRING;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* SCSI Device Name designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_UTF8;
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
			desig->type = SPDK_SPC_VPD_IDENTIFIER_TYPE_SCSI_NAME;
			desig->association = SPDK_SPC_VPD_ASSOCIATION_TARGET_DEVICE;
			desig->reserved0 = 0;
			desig->piv = 1;
			desig->reserved1 = 0;
			desig->len = snprintf(desig->desig, SPDK_SCSI_DEV_MAX_NAME, "%s", dev->name);
			len += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			buf += sizeof(struct spdk_scsi_desig_desc) + desig->len;

			/* SCSI Port Name designator */
			desig = (struct spdk_scsi_desig_desc *)buf;
			desig->code_set = SPDK_SPC_VPD_CODE_SET_UTF8;
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
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
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
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
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
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
			desig->protocol_id = SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
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

			memset(vext, 0, sizeof(*vext));
			hlen = 4;

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
			for (i = 0; i < dev->num_ports; i++) {
				struct spdk_scsi_port_desc *sdesc;
				struct spdk_scsi_tgt_port_desc *pdesc;

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
			/* PAGE LENGTH */

			memset(&data[4], 0, 60);

			hlen = 4;

			/* WSNZ(0) */
			/* support zero length in WRITE SAME */

			/* MAXIMUM COMPARE AND WRITE LENGTH */
			blocks = SPDK_WORK_ATS_BLOCK_SIZE / bdev->blocklen;

			if (blocks > 0xff)
				blocks = 0xff;

			data[5] = (uint8_t)blocks;

			/* force align to 4KB */
			if (bdev->blocklen < 4096) {
				optimal_blocks = 4096 / bdev->blocklen;
			} else {
				optimal_blocks = 1;
			}

			/* OPTIMAL TRANSFER LENGTH GRANULARITY */
			to_be16(&data[6], optimal_blocks);

			blocks = SPDK_WORK_BLOCK_SIZE / bdev->blocklen;

			/* MAXIMUM TRANSFER LENGTH */
			to_be32(&data[8], blocks);
			/* OPTIMAL TRANSFER LENGTH */
			to_be32(&data[12], blocks);

			/* MAXIMUM PREFETCH XDREAD XDWRITE TRANSFER LENGTH */

			len = 20 - hlen;

			if (bdev->thin_provisioning) {
				/*
				 * MAXIMUM UNMAP LBA COUNT: indicates the
				 * maximum  number of LBAs that may be
				 * unmapped by an UNMAP command.
				 */
				to_be32(&data[20], g_spdk_scsi.scsi_params.max_unmap_lba_count);

				/*
				 * MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT:
				 * indicates the maximum number of UNMAP
				 * block descriptors that shall be contained
				 * in the parameter data transferred to the
				 * device server for an UNMAP command.
				 */
				if (bdev->max_unmap_bdesc_count <
				    g_spdk_scsi.scsi_params.max_unmap_block_descriptor_count)
					to_be32(&data[24], bdev->max_unmap_bdesc_count);
				else
					to_be32(&data[24], g_spdk_scsi.scsi_params.max_unmap_block_descriptor_count);

				/*
				 * OPTIMAL UNMAP GRANULARITY: indicates the
				 * optimal granularity in logical blocks
				 * for unmap request.
				 */
				to_be32(&data[28], g_spdk_scsi.scsi_params.optimal_unmap_granularity);

				/*
				 * UNMAP GRANULARITY ALIGNMENT: indicates the
				 * LBA of the first logical block to which the
				 * OPTIMAL UNMAP GRANULARITY field applies.
				 */
				/* not specified */
				to_be32(&data[32], g_spdk_scsi.scsi_params.unmap_granularity_alignment);

				/*
				 * UGAVALID(7): bit set to one indicates that
				 * the UNMAP GRANULARITY ALIGNMENT field is
				 * valid.
				 */
				/* not specified */
				if (g_spdk_scsi.scsi_params.ugavalid)
					data[32] |= 1 << 7;

				/*
				 * MAXIMUM WRITE SAME LENGTH: indicates the
				 * maximum number of contiguous logical blocks
				 * that the device server allows to be unmapped
				 * or written in a single WRITE SAME command.
				 */
				to_be64(&data[36], g_spdk_scsi.scsi_params.max_write_same_length);

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
			if (!bdev->thin_provisioning) {
				SPDK_ERRLOG("unsupported INQUIRY VPD page 0x%x\n", pc);
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
				SPDK_TRACELOG(SPDK_TRACE_SCSI, "Vendor specific INQUIRY VPD page 0x%x\n", pc);
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
		inqdata->peripheral = pd;

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
		spdk_strcpy_pad(inqdata->product_id, bdev->product_name, 16, ' ');

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
	if (!buf)
		return;

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
spdk_bdev_scsi_mode_sense_page(struct spdk_bdev *bdev,
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
		SPDK_TRACELOG(SPDK_TRACE_SCSI,
			      "MODE_SENSE Read-Write Error Recovery\n");
		if (subpage != 0x00)
			break;
		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x02:
		/* Disconnect-Reconnect */
		SPDK_TRACELOG(SPDK_TRACE_SCSI,
			      "MODE_SENSE Disconnect-Reconnect\n");
		if (subpage != 0x00)
			break;
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
		SPDK_TRACELOG(SPDK_TRACE_SCSI,
			      "MODE_SENSE Verify Error Recovery\n");

		if (subpage != 0x00)
			break;

		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x08: {
		/* Caching */
		SPDK_TRACELOG(SPDK_TRACE_SCSI, "MODE_SENSE Caching\n");
		if (subpage != 0x00)
			break;

		plen = 0x12 + 2;
		mode_sense_page_init(cp, plen, page, subpage);

		if (cp && bdev->write_cache && pc != 0x01)
			cp[2] |= 0x4; /* WCE */

		/* Read Cache Disable (RCD) = 1 */
		if (cp && pc != 0x01)
			cp[2] |= 0x1;

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
			SPDK_TRACELOG(SPDK_TRACE_SCSI,
				      "MODE_SENSE Control\n");
			plen = 0x0a + 2;
			mode_sense_page_init(cp, plen, page, subpage);
			len += plen;
			break;
		case 0x01:
			/* Control Extension */
			SPDK_TRACELOG(SPDK_TRACE_SCSI,
				      "MODE_SENSE Control Extension\n");
			plen = 0x1c + 4;
			mode_sense_page_init(cp, plen, page, subpage);
			len += plen;
			break;
		case 0xff:
			/* All subpages */
			len += spdk_bdev_scsi_mode_sense_page(bdev,
							      cdb, pc, page,
							      0x00,
							      cp ? &cp[len] : NULL, task);
			len += spdk_bdev_scsi_mode_sense_page(bdev,
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
		SPDK_TRACELOG(SPDK_TRACE_SCSI, "MODE_SENSE XOR Control\n");
		if (subpage != 0x00)
			break;
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
		SPDK_TRACELOG(SPDK_TRACE_SCSI,
			      "MODE_SENSE Power Condition\n");
		if (subpage != 0x00)
			break;
		plen = 0x0a + 2;
		mode_sense_page_init(cp, plen, page, subpage);
		len += plen;
		break;
	case 0x1b:
		/* Reserved */
		break;
	case 0x1c:
		/* Informational Exceptions Control */
		SPDK_TRACELOG(SPDK_TRACE_SCSI,
			      "MODE_SENSE Informational Exceptions Control\n");
		if (subpage != 0x00)
			break;

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
				len += spdk_bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0x00,
					       cp ? &cp[len] : NULL, task);
			}
			break;
		case 0xff:
			/* All mode pages and subpages */
			for (i = 0x00; i < 0x3e; i ++) {
				len += spdk_bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0x00,
					       cp ? &cp[len] : NULL, task);
			}
			for (i = 0x00; i < 0x3e; i ++) {
				len += spdk_bdev_scsi_mode_sense_page(
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
spdk_bdev_scsi_mode_sense(struct spdk_bdev *bdev, int md,
			  uint8_t *cdb, int dbd, int llbaa, int pc,
			  int page, int subpage, uint8_t *data, struct spdk_scsi_task *task)
{
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
	plen = spdk_bdev_scsi_mode_sense_page(bdev, cdb, pc, page,
					      subpage,
					      pages, task);
	if (plen < 0) {
		return -1;
	}

	total = hlen + blen + plen;
	if (data == NULL)
		return total;

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
		to_be64(&bdesc[0], bdev->blockcnt);
		/* Reserved */
		memset(&bdesc[8], 0, 4);
		/* Block Length */
		to_be32(&bdesc[12], bdev->blocklen);
	} else if (blen == 8) {
		/* Number of Blocks */
		if (bdev->blockcnt > 0xffffffffULL)
			memset(&bdesc[0], 0xff, 4);
		else
			to_be32(&bdesc[0], bdev->blockcnt);

		/* Block Length */
		to_be32(&bdesc[4], bdev->blocklen);
	}

	return total;
}

static int
spdk_bdev_scsi_mode_select_page(struct spdk_bdev *bdev,
				uint8_t *cdb, int pf, int sp,
				uint8_t *data, size_t len)
{
	size_t hlen, plen;
	int spf, page, subpage;
	int rc;

	/* vendor specific */
	if (pf == 0) {
		return 0;
	}

	if (len < 1) {
		return 0;
	}

	spf = !!(data[0] & 0x40);
	page = data[0] & 0x3f;
	if (spf) {
		/* Sub_page mode page format */
		hlen = 4;
		if (len < hlen)
			return 0;
		subpage = data[1];

		plen = from_be16(&data[2]);
	} else {
		/* Page_0 mode page format */
		hlen = 2;
		if (len < hlen)
			return 0;
		subpage = 0;
		plen = data[1];
	}

	plen += hlen;
	if (len < plen) {
		return 0;
	}

	switch (page) {
	case 0x08: { /* Caching */
		//int wce;

		SPDK_TRACELOG(SPDK_TRACE_SCSI, "MODE_SELECT Caching\n");
		if (subpage != 0x00)
			break;

		if (plen != 0x12 + hlen) {
			/* unknown format */
			break;
		}

		// TODO:
		//wce = data[2] & 0x4; /* WCE */

		//fd = bdev->fd;
		//
		//rc = fcntl(fd, F_GETFL, 0);
		//if (rc != -1) {
		//	if (wce) {
		//		SPDK_TRACELOG(SPDK_TRACE_SCSI, "MODE_SELECT Writeback cache enable\n");
		//		rc = fcntl(fd, F_SETFL, (rc & ~O_FSYNC));
		//		bdev->write_cache = 1;
		//	} else {
		//		rc = fcntl(fd, F_SETFL, (rc | O_FSYNC));
		//		bdev->write_cache = 0;
		//	}
		//}

		break;
	}
	default:
		/* not supported */
		break;
	}

	len -= plen;
	if (len != 0) {
		rc = spdk_bdev_scsi_mode_select_page(bdev, cdb, pf, sp, &data[plen], len);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static void
spdk_bdev_scsi_task_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status,
			     void *cb_arg)
{
	struct spdk_scsi_task		*task = cb_arg;

	if (task->type == SPDK_SCSI_TASK_TYPE_CMD) {
		if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			task->status = SPDK_SCSI_STATUS_GOOD;
		} else {
			int sc, sk, asc, ascq;

			switch (status) {
			case SPDK_BDEV_IO_STATUS_NVME_ERROR:
				spdk_scsi_nvme_translate(bdev_io, &sc, &sk, &asc, &ascq);
				break;
			case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
				sc   = bdev_io->error.scsi.sc;
				sk   = bdev_io->error.scsi.sk;
				asc  = bdev_io->error.scsi.asc;
				ascq = bdev_io->error.scsi.ascq;
				break;
			default:
				sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
				sk   = SPDK_SCSI_SENSE_ABORTED_COMMAND;
				asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
				ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
				break;
			}
			spdk_scsi_task_set_status(task, sc, sk, asc, ascq);
		}
	} else if (task->type == SPDK_SCSI_TASK_TYPE_MANAGE) {
		if (status == SPDK_BDEV_IO_STATUS_SUCCESS)
			task->response = SPDK_SCSI_TASK_MGMT_RESP_SUCCESS;
	}
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ && task->iovs != bdev_io->u.read.iovs) {
		assert(task->iovcnt == bdev_io->u.read.iovcnt);
		memcpy(task->iovs, bdev_io->u.read.iovs, sizeof(task->iovs[0]) * task->iovcnt);
	}

	spdk_scsi_lun_complete_task(task->lun, task);
}

static int
spdk_bdev_scsi_read_write_lba_check(struct spdk_scsi_task *primary,
				    struct spdk_scsi_task *task,
				    uint64_t lba, uint64_t cmd_lba_count,
				    uint64_t maxlba)
{
	if (!primary) {
		/*
		 * Indicates this task is a primary task, we check whether the LBA and
		 * range is valid. If such info of primary is valid, all subtasks' are valid.
		 */
		if (lba >= maxlba || cmd_lba_count > maxlba || lba > (maxlba - cmd_lba_count)) {
			SPDK_TRACELOG(SPDK_TRACE_SCSI, "end of media\n");
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
						  SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return -1;
		}
	} else {
		/*
		 * Indicates this task is a subtask, we do not need to check the LBA range.
		 * Need to check condition of primary task.
		 */
		if (primary->status == SPDK_SCSI_STATUS_CHECK_CONDITION) {
			memcpy(task->sense_data, primary->sense_data,
			       primary->sense_data_len);
			task->status = SPDK_SCSI_STATUS_CHECK_CONDITION;
			task->sense_data_len = primary->sense_data_len;
			return -1;
		}
	}

	return 0;
}

static int
spdk_bdev_scsi_read(struct spdk_bdev *bdev,
		    struct spdk_scsi_task *task, uint64_t lba,
		    uint32_t len)
{
	uint64_t maxlba;
	uint64_t blen;
	uint64_t offset;
	uint64_t nbytes;
	int rc;

	maxlba = bdev->blockcnt;
	blen = bdev->blocklen;

	rc = spdk_bdev_scsi_read_write_lba_check(task->parent, task, lba,
			task->transfer_len / blen, maxlba);
	if (rc < 0) {
		return SPDK_SCSI_TASK_COMPLETE;
	}

	lba += (task->offset / blen);
	offset = lba * blen;
	nbytes = task->length;

	SPDK_TRACELOG(SPDK_TRACE_SCSI,
		      "Read: max=%"PRIu64", lba=%"PRIu64", len=%"PRIu64"\n",
		      maxlba, lba, (uint64_t)task->length / blen);

	task->blockdev_io = spdk_bdev_readv(bdev, task->ch, task->iovs,
					    task->iovcnt, offset, nbytes,
					    spdk_bdev_scsi_task_complete, task);
	if (!task->blockdev_io) {
		SPDK_ERRLOG("spdk_bdev_readv() failed\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	task->data_transferred = nbytes;
	task->status = SPDK_SCSI_STATUS_GOOD;

	return SPDK_SCSI_TASK_PENDING;
}

static int
spdk_bdev_scsi_write(struct spdk_bdev *bdev,
		     struct spdk_scsi_task *task, uint64_t lba, uint32_t len)
{
	uint64_t maxlba;
	uint64_t blen;
	uint64_t offset;
	uint64_t nbytes;
	int rc;
	struct spdk_scsi_task *primary = task->parent;

	if (len == 0) {
		task->data_transferred = 0;
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	maxlba = bdev->blockcnt;
	rc = spdk_bdev_scsi_read_write_lba_check(primary, task, lba, len, maxlba);
	if (rc < 0) {
		return SPDK_SCSI_TASK_COMPLETE;
	}

	blen = bdev->blocklen;
	offset = lba * blen;
	nbytes = ((uint64_t)len) * blen;

	SPDK_TRACELOG(SPDK_TRACE_SCSI,
		      "Write: max=%"PRIu64", lba=%"PRIu64", len=%u\n",
		      maxlba, lba, len);

	if (nbytes > task->transfer_len) {
		SPDK_ERRLOG("nbytes(%zu) > transfer_len(%u)\n",
			    (size_t)nbytes, task->transfer_len);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	offset += task->offset;
	task->blockdev_io = spdk_bdev_writev(bdev, task->ch, task->iovs,
					     task->iovcnt, offset, task->length,
					     spdk_bdev_scsi_task_complete,
					     task);

	if (!task->blockdev_io) {
		SPDK_ERRLOG("spdk_bdev_writev failed\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	} else {
		if (!primary) {
			task->data_transferred += task->length;
		} else {
			primary->data_transferred += task->length;
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_SCSI, "Wrote %"PRIu64"/%"PRIu64" bytes\n",
		      (uint64_t)task->length, nbytes);

	task->status = SPDK_SCSI_STATUS_GOOD;
	return SPDK_SCSI_TASK_PENDING;
}

static int
spdk_bdev_scsi_sync(struct spdk_bdev *bdev, struct spdk_scsi_task *task,
		    uint64_t lba, uint32_t len)
{
	uint64_t maxlba;
	uint64_t llen;
	uint64_t blen;
	uint64_t offset;
	uint64_t nbytes;

	if (len == 0) {
		return SPDK_SCSI_TASK_COMPLETE;
	}

	maxlba = bdev->blockcnt;
	llen = len;
	blen = bdev->blocklen;
	offset = lba * blen;
	nbytes = llen * blen;

	if (lba >= maxlba || llen > maxlba || lba > (maxlba - llen)) {
		SPDK_ERRLOG("end of media\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	task->blockdev_io = spdk_bdev_flush(bdev, task->ch, offset, nbytes,
					    spdk_bdev_scsi_task_complete, task);

	if (!task->blockdev_io) {
		SPDK_ERRLOG("spdk_bdev_flush() failed\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}
	task->data_transferred = 0;
	task->status = SPDK_SCSI_STATUS_GOOD;
	return SPDK_SCSI_TASK_PENDING;
}

static int
spdk_bdev_scsi_readwrite(struct spdk_bdev *bdev,
			 struct spdk_scsi_task *task,
			 uint64_t lba, uint32_t xfer_len, bool is_read)
{
	if (is_read) {
		if ((task->dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) ||
		    (task->dxfer_dir == SPDK_SCSI_DIR_NONE)) {
			return spdk_bdev_scsi_read(bdev, task, lba, xfer_len);
		} else {
			SPDK_ERRLOG("Incorrect data direction\n");
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return SPDK_SCSI_TASK_COMPLETE;
		}
	} else {
		if ((task->dxfer_dir == SPDK_SCSI_DIR_TO_DEV) ||
		    (task->dxfer_dir == SPDK_SCSI_DIR_NONE)) {
			return spdk_bdev_scsi_write(bdev, task, lba, xfer_len);
		} else {
			SPDK_ERRLOG("Incorrect data direction\n");
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return SPDK_SCSI_TASK_COMPLETE;
		}
	}
}

static int
spdk_bdev_scsi_unmap(struct spdk_bdev *bdev,
		     struct spdk_scsi_task *task)
{

	uint8_t *data;
	struct spdk_scsi_unmap_bdesc *desc;
	uint32_t bdesc_count;
	int bdesc_data_len;
	int data_len;

	if (task->iovcnt == 1) {
		data = (uint8_t *)task->iovs[0].iov_base;
		data_len = task->iovs[0].iov_len;
	} else {
		data = spdk_scsi_task_gather_data(task, &data_len);
	}

	/*
	 * The UNMAP BLOCK DESCRIPTOR DATA LENGTH field specifies the length in
	 * bytes of the UNMAP block descriptors that are available to be
	 * transferred from the Data-Out Buffer. The unmap block descriptor data
	 * length should be a multiple of 16. If the unmap block descriptor data
	 * length is not a multiple of 16, then the last unmap block descriptor
	 * is incomplete and shall be ignored.
	 */
	bdesc_data_len = from_be16(&data[2]);
	bdesc_count = bdesc_data_len / 16;
	assert(bdesc_data_len <= data_len);

	if (task->iovcnt == 1) {
		desc = (struct spdk_scsi_unmap_bdesc *)&data[8];
	} else {
		desc = spdk_scsi_task_alloc_data(task, bdesc_data_len - 8);
		memcpy(desc, &data[8], bdesc_data_len - 8);
		spdk_aligned_free(data);
	}

	if (bdesc_count > bdev->max_unmap_bdesc_count) {
		SPDK_ERRLOG("Error - supported unmap block descriptor count limit"
			    " is %u\n", bdev->max_unmap_bdesc_count);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	} else if (bdesc_data_len > data_len) {
		SPDK_ERRLOG("Error - bdesc_data_len (%d) > data_len (%d)",
			    bdesc_data_len, data_len);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	task->blockdev_io = spdk_bdev_unmap(bdev, task->ch, desc,
					    bdesc_count, spdk_bdev_scsi_task_complete,
					    task);

	if (!task->blockdev_io) {
		SPDK_ERRLOG("SCSI Unmapping failed\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	return SPDK_SCSI_TASK_PENDING;
}

static int
spdk_bdev_scsi_process_block(struct spdk_bdev *bdev,
			     struct spdk_scsi_task *task)
{
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
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len,
						cdb[0] == SPDK_SBC_READ_6);

	case SPDK_SBC_READ_10:
	case SPDK_SBC_WRITE_10:
		lba = from_be32(&cdb[2]);
		xfer_len = from_be16(&cdb[7]);
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len,
						cdb[0] == SPDK_SBC_READ_10);

	case SPDK_SBC_READ_12:
	case SPDK_SBC_WRITE_12:
		lba = from_be32(&cdb[2]);
		xfer_len = from_be32(&cdb[6]);
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len,
						cdb[0] == SPDK_SBC_READ_12);
	case SPDK_SBC_READ_16:
	case SPDK_SBC_WRITE_16:
		lba = from_be64(&cdb[2]);
		xfer_len = from_be32(&cdb[10]);
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len,
						cdb[0] == SPDK_SBC_READ_16);

	case SPDK_SBC_READ_CAPACITY_10: {
		uint8_t buffer[8];

		if (bdev->blockcnt - 1 > 0xffffffffULL) {
			memset(buffer, 0xff, 4);
		} else {
			to_be32(buffer, bdev->blockcnt - 1);
		}
		to_be32(&buffer[4], bdev->blocklen);

		len = SPDK_MIN(task->length, sizeof(buffer));
		if (spdk_scsi_task_scatter_data(task, buffer, len) < 0)
			break;

		task->data_transferred = len;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;
	}

	case SPDK_SPC_SERVICE_ACTION_IN_16:
		switch (cdb[1] & 0x1f) { /* SERVICE ACTION */
		case SPDK_SBC_SAI_READ_CAPACITY_16: {
			uint8_t buffer[32] = {0};

			to_be64(&buffer[0], bdev->blockcnt - 1);
			to_be32(&buffer[8], bdev->blocklen);
			/*
			 * Set the TPE bit to 1 to indicate thin provisioning.
			 * The position of TPE bit is the 7th bit in 14th byte
			 * in READ CAPACITY (16) parameter data.
			 */
			if (bdev->thin_provisioning) {
				buffer[14] |= 1 << 7;
			}

			len = SPDK_MIN(from_be32(&cdb[10]), sizeof(buffer));
			if (spdk_scsi_task_scatter_data(task, buffer, len) < 0)
				break;

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
			len = bdev->blockcnt - lba;
		}

		return spdk_bdev_scsi_sync(bdev, task, lba, len);
		break;

	case SPDK_SBC_UNMAP:
		return spdk_bdev_scsi_unmap(bdev, task);

	default:
		return SPDK_SCSI_TASK_UNKNOWN;
	}

	return SPDK_SCSI_TASK_COMPLETE;
}

static int
spdk_bdev_scsi_check_len(struct spdk_scsi_task *task, int len, int min_len)
{
	if (len >= min_len)
		return 0;

	/* INVALID FIELD IN CDB */
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
				  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
				  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -1;
}

static int
spdk_bdev_scsi_process_primary(struct spdk_bdev *bdev,
			       struct spdk_scsi_task *task)
{
	int alloc_len = -1;
	int data_len = -1;
	uint8_t *cdb = task->cdb;
	uint8_t *data = NULL;
	int rc = 0;
	int pllen, md = 0;
	int pf, sp;
	int bdlen, llba;
	int dbd, pc, page, subpage;
	int cmd_parsed = 0;


	switch (cdb[0]) {
	case SPDK_SPC_INQUIRY:
		alloc_len = from_be16(&cdb[3]);
		data_len = SPDK_MAX(4096, alloc_len);
		data = spdk_aligned_zmalloc(data_len, 0, NULL);
		assert(data != NULL);
		rc = spdk_bdev_scsi_inquiry(bdev, task, cdb, data, data_len);
		data_len = SPDK_MIN(rc, data_len);
		if (rc < 0) {
			break;
		}

		SPDK_TRACEDUMP(SPDK_TRACE_DEBUG, "INQUIRY", data, data_len);
		break;

	case SPDK_SPC_REPORT_LUNS: {
		int sel;

		sel = cdb[2];
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sel=%x\n", sel);

		alloc_len = from_be32(&cdb[6]);
		rc = spdk_bdev_scsi_check_len(task, alloc_len, 16);
		if (rc < 0) {
			break;
		}

		data_len = SPDK_MAX(4096, alloc_len);
		data = spdk_aligned_zmalloc(data_len, 0, NULL);
		assert(data != NULL);
		rc = spdk_bdev_scsi_report_luns(task->lun, sel, data, data_len);
		data_len = rc;
		if (rc < 0) {
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			break;
		}

		SPDK_TRACEDUMP(SPDK_TRACE_DEBUG, "REPORT LUNS", data, data_len);
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

		rc = spdk_bdev_scsi_check_len(task, pllen, md);
		if (rc < 0) {
			break;
		}

		data = spdk_scsi_task_gather_data(task, &rc);
		if (rc < 0) {
			break;
		}

		data_len = rc;
		if (cdb[0] == SPDK_SPC_MODE_SELECT_6) {
			rc = spdk_bdev_scsi_check_len(task, data_len, 4);
			if (rc >= 0) {
				bdlen = data[3];
			}

		} else {
			rc = spdk_bdev_scsi_check_len(task, data_len, 8);
			if (rc >= 0) {
				bdlen = from_be16(&data[6]);
			}
		}

		if (rc < 0) {
			break;
		}
		pf = !!(cdb[1] & 0x10);
		sp = !!(cdb[1] & 0x1);

		/* page data */
		rc = spdk_bdev_scsi_mode_select_page(
			     bdev, cdb,
			     pf, sp,
			     &data[md + bdlen],
			     pllen - (md + bdlen));
		if (rc < 0) {
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			break;
		}

		rc = pllen;
		data_len = 0;
		break;

	case SPDK_SPC_MODE_SENSE_6:
		alloc_len = cdb[4];
		md = 6;

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
		rc = spdk_bdev_scsi_mode_sense(bdev, md,
					       cdb, dbd, llba, pc,
					       page, subpage,
					       NULL, task);
		if (rc < 0) {
			break;
		}

		data_len = rc;
		data = spdk_aligned_zmalloc(data_len, 0, NULL);
		assert(data != NULL);

		/* First call with no buffer to discover needed buffer size */
		rc = spdk_bdev_scsi_mode_sense(bdev, md,
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
		data = spdk_aligned_zmalloc(data_len, 0, NULL);
		assert(data != NULL);
		memcpy(data, task->sense_data, data_len);
		break;
	}

	case SPDK_SPC_LOG_SELECT:
		SPDK_TRACELOG(SPDK_TRACE_SCSI, "LOG_SELECT\n");
		cmd_parsed = 1;
	case SPDK_SPC_LOG_SENSE:
		if (!cmd_parsed) {
			SPDK_TRACELOG(SPDK_TRACE_SCSI, "LOG_SENSE\n");
		}

		/* INVALID COMMAND OPERATION CODE */
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_COMMAND_OPERATION_CODE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		rc = -1;
		break;

	case SPDK_SPC_TEST_UNIT_READY:
		SPDK_TRACELOG(SPDK_TRACE_SCSI, "TEST_UNIT_READY\n");
		cmd_parsed = 1;
	case SPDK_SBC_START_STOP_UNIT:
		if (!cmd_parsed) {
			SPDK_TRACELOG(SPDK_TRACE_SCSI, "START_STOP_UNIT\n");
		}

		rc = 0;
		break;

	default:
		return SPDK_SCSI_TASK_UNKNOWN;
	}

	if (rc >= 0 && data_len > 0) {
		assert(alloc_len >= 0);
		spdk_scsi_task_scatter_data(task, data, SPDK_MIN(alloc_len, data_len));
		rc = SPDK_MIN(data_len, alloc_len);
	}

	if (rc >= 0) {
		task->data_transferred = rc;
		task->status = SPDK_SCSI_STATUS_GOOD;
	}

	if (data)
		spdk_aligned_free(data);

	return SPDK_SCSI_TASK_COMPLETE;
}

int
spdk_bdev_scsi_execute(struct spdk_bdev *bdev, struct spdk_scsi_task *task)
{
	int rc;

	if ((rc = spdk_bdev_scsi_process_block(bdev, task)) == SPDK_SCSI_TASK_UNKNOWN) {
		if ((rc = spdk_bdev_scsi_process_primary(bdev, task)) == SPDK_SCSI_TASK_UNKNOWN) {
			SPDK_TRACELOG(SPDK_TRACE_SCSI, "unsupported SCSI OP=0x%x\n", task->cdb[0]);
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

int
spdk_bdev_scsi_reset(struct spdk_bdev *bdev, struct spdk_scsi_task *task)
{
	return spdk_bdev_reset(bdev, SPDK_BDEV_RESET_SOFT,
			       spdk_bdev_scsi_task_complete, task);
}
