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
#include "spdk/endian.h"

#define SPDK_WORK_BLOCK_SIZE		(1ULL * 1024ULL * 1024ULL)
#define SPDK_WORK_ATS_BLOCK_SIZE	(1ULL * 1024ULL * 1024ULL)
#define MAX_SERIAL_STRING		32

#define DEFAULT_DISK_VENDOR		"Intel"
#define DEFAULT_DISK_REVISION		"0001"
#define DEFAULT_DISK_ROTATION_RATE	7200	/* 7200 rpm */
#define DEFAULT_DISK_FORM_FACTOR	0x02	/* 3.5 inch */

static void
spdk_strcpy_pad(uint8_t *dst, size_t size, const char *src, int pad)
{
	size_t len;

	len = strlen(src);
	if (len < size) {
		memcpy(dst, src, len);
		memset(dst + len, pad, (size - len));
	} else {
		memcpy(dst, src, size);
	}
}

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
	for (i = 0; (name[i] != '\0') && (i < 16); i++) {
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
	uint32_t blocks;
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
		spdk_scsi_task_set_check_condition(task,
						   SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
						   0x24, 0x0);
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
			len = strlen(bdev->name);
			if (len > MAX_SERIAL_STRING) {
				len = MAX_SERIAL_STRING;
			}

			spdk_strcpy_pad(vpage->params, len, bdev->name, ' ');

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
			if (sizeof(struct spdk_scsi_vpd_page) + len > task->alloc_len) {
				spdk_scsi_task_set_check_condition(task,
								   SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
								   0x24, 0x0);
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
			spdk_strcpy_pad(desig->desig, 8, DEFAULT_DISK_VENDOR, ' ');
			spdk_strcpy_pad(&desig->desig[8], 16, bdev->product_name, ' ');
			spdk_strcpy_pad(&desig->desig[24], MAX_SERIAL_STRING,
					bdev->name, ' ');
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

			/* should not exceed the data_in buffer length */
			if (sizeof(struct spdk_scsi_vpd_page) + len > alloc_len) {
				spdk_scsi_task_set_check_condition(task,
								   SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
								   0x24, 0x0);
				return -1;
			}
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
			/* Shared MODE PAGE policy*/
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
				sdesc->rel_port_id = htobe16(dev->port[i].index);

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
				sdesc->tgt_desc_len = htobe16(plen2);

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
				blocks = 4096 / bdev->blocklen;
				/* OPTIMAL TRANSFER LENGTH GRANULARITY */
				to_be16(&data[6], blocks);

				/* MAXIMUM TRANSFER LENGTH */

				/* OPTIMAL TRANSFER LENGTH */
				blocks = SPDK_WORK_BLOCK_SIZE / bdev->blocklen;

				to_be32(&data[12], blocks);

				/* MAXIMUM PREFETCH XDREAD XDWRITE TRANSFER LENGTH */
			} else {
				blocks = 1;

				/* OPTIMAL TRANSFER LENGTH GRANULARITY */
				to_be16(&data[6], blocks);

				/* MAXIMUM TRANSFER LENGTH */

				/* OPTIMAL TRANSFER LENGTH */
				blocks = SPDK_WORK_BLOCK_SIZE / bdev->blocklen;
				to_be32(&data[12], blocks);

				/* MAXIMUM PREFETCH XDREAD XDWRITE TRANSFER LENGTH */
			}

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
		spdk_strcpy_pad(inqdata->t10_vendor_id, 8, DEFAULT_DISK_VENDOR, ' ');

		/* PRODUCT IDENTIFICATION */
		spdk_strcpy_pad(inqdata->product_id, 16,
				bdev->product_name, ' ');

		/* PRODUCT REVISION LEVEL */
		spdk_strcpy_pad(inqdata->product_rev, 4, DEFAULT_DISK_REVISION, ' ');

		/* Vendor specific */
		memset(inqdata->vendor, 0x20, 20);

		/* CLOCKING(3-2) QAS(1) IUS(0) */
		inqdata->ius = 0;

		/* Reserved */
		inqdata->reserved = 0;

		/* VERSION DESCRIPTOR 1-8 */
		to_be16(inqdata->desc, 0x0960);
		to_be16(&inqdata->desc[2], 0x0300); /* SPC-3 (no version claimed) */
		to_be16(&inqdata->desc[4], 0x320); /* SBC-2 (no version claimed) */
		to_be16(&inqdata->desc[6], 0x0040); /* SAM-2 (no version claimed) */
		/* 96 - 74 + 8 */
		/* Reserved[74-95] */
		memset(&inqdata->desc[8], 0, 30);

		len = alloc_len - hlen;

		/* ADDITIONAL LENGTH */
		inqdata->add_len = len;
	}

	return hlen + len;

inq_error:
	spdk_scsi_task_set_check_condition(task,
					   SPDK_SCSI_SENSE_NO_SENSE,
					   0x0, 0x0);
	return -1;
}

static void
mode_sense_page_init(uint8_t *buf, int len, int page, int subpage)
{
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
			       uint8_t *data, int alloc_len)
{
	uint8_t *cp;
	int len = 0;
	int plen;
	int i;

	if (pc == 0x00) {
		/* Current values */
	} else if (pc == 0x01) {
		/* Changeable values not supported */
		return -1;
	} else if (pc == 0x02) {
		/* Default values */
	} else {
		/* Saved values not supported */
		return -1;
	}

	cp = &data[len];
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

		if (bdev->write_cache)
			cp[2] |= 0x4; /* WCE */
		// TODO:
		//fd = bdev->fd;
		//rc = fcntl(fd , F_GETFL, 0);
		//if (rc != -1 && !(rc & O_FSYNC))
		//	cp[2] |= 0x4; /* WCE=1 */
		//else
		//	cp[2] &= 0xfb; /* WCE = 0 */

		/* Read Cache Disable (RCD) = 1 */
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
							      &data[len],
							      alloc_len);
			len += spdk_bdev_scsi_mode_sense_page(bdev,
							      cdb, pc, page,
							      0x01,
							      &data[len],
							      alloc_len);
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
					       &cp[len], alloc_len);
			}
			break;
		case 0xff:
			/* All mode pages and subpages */
			for (i = 0x00; i < 0x3e; i ++) {
				len += spdk_bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0x00,
					       &cp[len], alloc_len);
			}
			for (i = 0x00; i < 0x3e; i ++) {
				len += spdk_bdev_scsi_mode_sense_page(
					       bdev, cdb, pc, i, 0xff,
					       &cp[len], alloc_len);
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
spdk_bdev_scsi_mode_sense6(struct spdk_bdev *bdev,
			   uint8_t *cdb, int dbd, int pc, int page,
			   int subpage, uint8_t *data, int alloc_len)
{
	uint8_t *cp;
	int hlen, len = 0, plen;
	int total;
	int llbaa = 0;

	data[0] = 0;                    /* Mode Data Length */
	data[1] = 0;                    /* Medium Type */
	data[2] = 0;                    /* Device-Specific Parameter */
	data[3] = 0;                    /* Block Descripter Length */
	hlen = 4;

	cp = &data[4];
	if (dbd) {                      /* Disable Block Descripters */
		len = 0;
	} else {
		if (llbaa) {
			/* Number of Blocks */
			to_be64(cp, bdev->blockcnt);
			/* Reserved */
			memset(&cp[8], 0, 4);
			/* Block Length */
			to_be32(&cp[12], bdev->blocklen);
			len = 16;
		} else {
			/* Number of Blocks */
			if (bdev->blockcnt > 0xffffffffULL)
				memset(cp, 0xff, 4);
			else
				to_be32(cp, bdev->blockcnt);

			/* Block Length */
			to_be32(&cp[4], bdev->blocklen);
			len = 8;
		}

		cp += len;
	}

	data[3] = len;                  /* Block Descripter Length */

	plen = spdk_bdev_scsi_mode_sense_page(bdev, cdb, pc, page,
					      subpage, &cp[0], alloc_len);
	if (plen < 0) {
		return -1;
	}

	total = hlen + len + plen;
	data[0] = total - 1;            /* Mode Data Length */

	return total;
}

static int
spdk_bdev_scsi_mode_sense10(struct spdk_bdev *bdev,
			    uint8_t *cdb, int dbd, int llbaa, int pc,
			    int page, int subpage, uint8_t *data,
			    int alloc_len)
{
	uint8_t *cp;
	int hlen, len = 0, plen;
	int total;

	/* Mode Data Length */
	/* Medium Type */
	/* Device-Specific Parameter */
	memset(data, 0, 4);

	if (llbaa) {
		data[4] = 0x1; /* Long LBA */
	} else {
		data[4] = 0; /* Short LBA */
	}

	/* Reserved */
	/* Block Descripter Length */
	memset(&data[5], 0, 3);
	hlen = 8;

	cp = &data[8];
	if (dbd) { /* Disable Block Descripters */
		len = 0;
	} else {
		if (llbaa) {
			/* Number of Blocks */
			to_be64(cp, bdev->blockcnt);
			/* Reserved */
			memset(&cp[8], 0, 4);
			/* Block Length */
			to_be32(&cp[12], bdev->blocklen);
			len = 16;
		} else {
			/* Number of Blocks */
			if (bdev->blockcnt > 0xffffffffULL)
				memset(cp, 0xff, 4);
			else
				to_be32(cp, bdev->blockcnt);

			/* Block Length */
			to_be32(&cp[4], bdev->blocklen);
			len = 8;
		}
		cp += len;
	}

	to_be16(&data[6], len);	/* Block Descripter Length */

	plen = spdk_bdev_scsi_mode_sense_page(bdev, cdb, pc, page,
					      subpage, &cp[0], alloc_len);
	if (plen < 0)
		return -1;

	total = hlen + len + plen;
	to_be16(data, total - 2);	/* Mode Data Length */

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
spdk_bdev_scsi_task_complete(spdk_event_t event)
{
	struct spdk_bdev_io		*bdev_io = spdk_event_get_arg2(event);
	struct spdk_scsi_task		*task = spdk_event_get_arg1(event);
	enum spdk_bdev_io_status	status = bdev_io->status;

	if (task->type == SPDK_SCSI_TASK_TYPE_CMD) {
		if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ABORTED_COMMAND, 0, 0);
		}

		/* If status was not set to CHECK_CONDITION yet, then we can set
		 * status to GOOD.
		 */
		if (task->status != SPDK_SCSI_STATUS_CHECK_CONDITION) {
			task->status = SPDK_SCSI_STATUS_GOOD;
		}

		/* command completed. remove from outstanding task list */
		TAILQ_REMOVE(&task->lun->tasks, task, scsi_link);
	} else if (task->type == SPDK_SCSI_TASK_TYPE_MANAGE) {
		if (status == SPDK_BDEV_IO_STATUS_SUCCESS)
			task->response = SPDK_SCSI_TASK_MGMT_RESP_SUCCESS;
		if (task->function == SPDK_SCSI_TASK_FUNC_LUN_RESET) {
			spdk_scsi_lun_clear_all(task->lun);
		}
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		task->rbuf = bdev_io->u.read.buf;
	}

	spdk_scsi_lun_complete_task(task->lun, task);
}

static int
spdk_bdev_scsi_read(struct spdk_bdev *bdev,
		    struct spdk_scsi_task *task, uint64_t lba,
		    uint32_t len)
{
	uint64_t maxlba;
	uint64_t llen;
	uint64_t blen;
	off_t offset;
	uint64_t nbytes;

	maxlba = bdev->blockcnt;
	blen = bdev->blocklen;
	lba += (task->offset / blen);
	offset = lba * blen;
	nbytes = task->length;
	llen = task->length / blen;

	SPDK_TRACELOG(SPDK_TRACE_SCSI,
		      "Read: max=%"PRIu64", lba=%"PRIu64", len=%"PRIu64"\n",
		      maxlba, lba, llen);

	if (lba >= maxlba || llen > maxlba || lba > (maxlba - llen)) {
		SPDK_ERRLOG("end of media\n");
		return -1;
	}

	task->blockdev_io = spdk_bdev_read(bdev, task->rbuf, nbytes,
					   offset, spdk_bdev_scsi_task_complete,
					   task);
	if (!task->blockdev_io) {
		SPDK_ERRLOG("spdk_bdev_read() failed\n");
		return -1;
	}

	task->data_transferred = nbytes;

	return 0;
}

static int
spdk_bdev_scsi_write(struct spdk_bdev *bdev,
		     struct spdk_scsi_task *task, uint64_t lba, uint32_t len)
{
	uint64_t maxlba;
	uint64_t llen;
	uint64_t blen;
	off_t offset;
	uint64_t nbytes;
	struct spdk_scsi_task *primary = task->parent;

	if (len == 0) {
		task->data_transferred = 0;
		return -1;
	}

	maxlba = bdev->blockcnt;
	llen = (uint64_t) len;
	blen = bdev->blocklen;
	offset = lba * blen;
	nbytes = llen * blen;

	SPDK_TRACELOG(SPDK_TRACE_SCSI,
		      "Write: max=%"PRIu64", lba=%"PRIu64", len=%u\n",
		      maxlba, lba, len);

	if (lba >= maxlba || llen > maxlba || lba > (maxlba - llen)) {
		SPDK_ERRLOG("end of media\n");
		return -1;
	}

	if (nbytes > task->transfer_len) {
		SPDK_ERRLOG("nbytes(%zu) > transfer_len(%u)\n",
			    (size_t)nbytes, task->transfer_len);
		return -1;
	}

	offset += task->offset;
	task->blockdev_io = spdk_bdev_writev(bdev, &task->iov,
					     1, task->length, offset,
					     spdk_bdev_scsi_task_complete,
					     task);

	if (!task->blockdev_io) {
		SPDK_ERRLOG("spdk_bdev_writev failed\n");
		return -1;
	} else {
		if (!primary) {
			task->data_transferred += task->length;
		} else {
			primary->data_transferred += task->length;
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_SCSI, "Wrote %"PRIu64"/%"PRIu64" bytes\n",
		      (uint64_t)task->length, nbytes);
	return 0;
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
		spdk_scsi_task_set_check_condition(task, SPDK_SCSI_SENSE_NO_SENSE, 0x0, 0x0);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	task->blockdev_io = spdk_bdev_flush(bdev, offset, nbytes,
					    spdk_bdev_scsi_task_complete, task);

	if (!task->blockdev_io) {
		SPDK_ERRLOG("spdk_bdev_flush() failed\n");
		spdk_scsi_task_set_check_condition(task, SPDK_SCSI_SENSE_NO_SENSE, 0x0, 0x0);
		return SPDK_SCSI_TASK_COMPLETE;
	}
	task->data_transferred = 0;
	task->status = SPDK_SCSI_STATUS_GOOD;
	return SPDK_SCSI_TASK_PENDING;
}

static int
spdk_bdev_scsi_readwrite(struct spdk_bdev *bdev,
			 struct spdk_scsi_task *task,
			 uint64_t lba, uint32_t xfer_len)
{
	int rc;

	if (task->dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		rc = spdk_bdev_scsi_read(bdev, task, lba, xfer_len);
	} else if (task->dxfer_dir == SPDK_SCSI_DIR_TO_DEV) {
		rc = spdk_bdev_scsi_write(bdev, task, lba, xfer_len);
	} else {
		SPDK_ERRLOG("Incorrect data direction\n");
		spdk_scsi_task_set_check_condition(task, SPDK_SCSI_SENSE_NO_SENSE, 0x0, 0x0);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	if (rc < 0) {
		SPDK_ERRLOG("disk op (rw) failed\n");
		spdk_scsi_task_set_check_condition(task, SPDK_SCSI_SENSE_NO_SENSE, 0x0, 0x0);

		return SPDK_SCSI_TASK_COMPLETE;
	} else {
		task->status = SPDK_SCSI_STATUS_GOOD;
	}

	return SPDK_SCSI_TASK_PENDING;
}

static int
spdk_bdev_scsi_unmap(struct spdk_bdev *bdev,
		     struct spdk_scsi_task *task)
{

	uint8_t *data;
	uint16_t bdesc_data_len, bdesc_count;

	data = (uint8_t *)task->iov.iov_base;

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

	if (bdesc_count > bdev->max_unmap_bdesc_count) {
		SPDK_ERRLOG("Error - supported unmap block descriptor count limit"
			    " is %u\n", bdev->max_unmap_bdesc_count);
		spdk_scsi_task_set_check_condition(task,
						   SPDK_SCSI_SENSE_NO_SENSE,
						   0x0, 0x0);
		return SPDK_SCSI_TASK_COMPLETE;
	}

	task->blockdev_io = spdk_bdev_unmap(bdev, (struct spdk_scsi_unmap_bdesc *)&data[8],
					    bdesc_count, spdk_bdev_scsi_task_complete,
					    task);

	if (!task->blockdev_io) {
		SPDK_ERRLOG("SCSI Unmapping failed\n");
		spdk_scsi_task_set_check_condition(task,
						   SPDK_SCSI_SENSE_NO_SENSE,
						   0x0, 0x0);
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
	uint8_t *data;

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
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len);

	case SPDK_SBC_READ_10:
	case SPDK_SBC_WRITE_10:
		lba = from_be32(&cdb[2]);
		xfer_len = from_be16(&cdb[7]);
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len);

	case SPDK_SBC_READ_12:
	case SPDK_SBC_WRITE_12:
		lba = from_be32(&cdb[2]);
		xfer_len = from_be32(&cdb[6]);
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len);

	case SPDK_SBC_READ_16:
	case SPDK_SBC_WRITE_16:
		lba = from_be64(&cdb[2]);
		xfer_len = from_be32(&cdb[10]);
		return spdk_bdev_scsi_readwrite(bdev, task, lba, xfer_len);

	case SPDK_SBC_READ_CAPACITY_10:
		spdk_scsi_task_alloc_data(task, 8, &data);
		if (bdev->blockcnt - 1 > 0xffffffffULL) {
			memset(data, 0xff, 4);
		} else {
			to_be32(data, bdev->blockcnt - 1);
		}
		to_be32(&data[4], bdev->blocklen);
		task->data_transferred = 8;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;

	case SPDK_SPC_SERVICE_ACTION_IN_16:
		switch (cdb[1] & 0x1f) { /* SERVICE ACTION */
		case SPDK_SBC_SAI_READ_CAPACITY_16:
			spdk_scsi_task_alloc_data(task, 32, &data);
			to_be64(&data[0], bdev->blockcnt - 1);
			to_be32(&data[8], bdev->blocklen);
			/*
			 * Set the TPE bit to 1 to indicate thin provisioning.
			 * The position of TPE bit is the 7th bit in 14th byte
			 * in READ CAPACITY (16) parameter data.
			 */
			if (bdev->thin_provisioning) {
				data[14] |= 1 << 7;
			}
			task->data_transferred = 32;
			task->status = SPDK_SCSI_STATUS_GOOD;
			break;

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
spdk_bdev_scsi_process_primary(struct spdk_bdev *bdev,
			       struct spdk_scsi_task *task)
{
	uint32_t alloc_len;
	int data_len;
	uint8_t *cdb = task->cdb;
	uint8_t *data;
	int pllen, md = 0;
	int pf, sp;
	int bdlen, llba;
	int dbd, pc, page, subpage;
	int cmd_parsed = 0;

	switch (cdb[0]) {
	case SPDK_SPC_INQUIRY:
		alloc_len = from_be16(&cdb[3]);
		spdk_scsi_task_alloc_data(task, alloc_len, &data);
		data_len = spdk_bdev_scsi_inquiry(bdev, task, cdb,
						  data, alloc_len);
		if (data_len < 0) {
			break;
		}

		SPDK_TRACEDUMP(SPDK_TRACE_DEBUG, "INQUIRY", data, data_len);
		task->data_transferred = (uint64_t)data_len;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;

	case SPDK_SPC_REPORT_LUNS: {
		int sel;

		sel = cdb[2];
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sel=%x\n", sel);

		alloc_len = from_be32(&cdb[6]);
		if (alloc_len < 16) {
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
			break;
		}

		spdk_scsi_task_alloc_data(task, alloc_len, &data);
		data_len = spdk_bdev_scsi_report_luns(task->lun, sel, data, task->alloc_len);
		if (data_len < 0) {
			spdk_scsi_task_set_check_condition(task, SPDK_SCSI_SENSE_NO_SENSE, 0x0, 0x0);
			break;
		}

		SPDK_TRACEDUMP(SPDK_TRACE_DEBUG, "REPORT LUNS", data, data_len);
		task->data_transferred = (uint64_t)data_len;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;
	}

	case SPDK_SPC_MODE_SELECT_6:
	case SPDK_SPC_MODE_SELECT_10:
		data = task->iobuf;

		if (cdb[0] == SPDK_SPC_MODE_SELECT_6) {
			md = 4;
			pllen = cdb[4];
		} else {
			md = 8;
			pllen = from_be16(&cdb[7]);
		}

		if (pllen == 0) {
			task->data_transferred = 0;
			task->status = SPDK_SCSI_STATUS_GOOD;
			break;
		} else if (cdb[0] == SPDK_SPC_MODE_SELECT_6 && pllen < 4) {
			/* MODE_SELECT(6) must have at least a 4 byte header. */
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
			break;
		} else if (cdb[0] == SPDK_SPC_MODE_SELECT_10 && pllen < 8) {
			/* MODE_SELECT(10) must have at least an 8 byte header. */
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
			break;
		}

		if (cdb[0] == SPDK_SPC_MODE_SELECT_6) {
			bdlen = data[3];
		} else {
			bdlen = from_be16(&data[6]);
		}

		pf = !!(cdb[1] & 0x10);
		sp = !!(cdb[1] & 0x1);

		/* page data */
		data_len = spdk_bdev_scsi_mode_select_page(
				   bdev, cdb,
				   pf, sp,
				   &data[md + bdlen],
				   pllen - (md + bdlen));
		if (data_len != 0) {
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_NO_SENSE,
							   0x0, 0x0);
			break;
		}

		task->data_transferred = pllen;
		task->status = SPDK_SCSI_STATUS_GOOD;
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
		pc = (cdb[2] & 0xc) >> 6;
		page = cdb[2] & 0x3f;
		subpage = cdb[3];

		spdk_scsi_task_alloc_data(task, alloc_len, &data);

		if (md == 6) {
			data_len = spdk_bdev_scsi_mode_sense6(bdev,
							      cdb, dbd, pc,
							      page, subpage,
							      data,
							      alloc_len);
		} else {
			data_len = spdk_bdev_scsi_mode_sense10(bdev,
							       cdb, dbd,
							       llba, pc,
							       page,
							       subpage,
							       data,
							       alloc_len);
		}

		if (data_len < 0) {
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
			break;
		}

		task->data_transferred = (uint64_t)data_len;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;

	case SPDK_SPC_REQUEST_SENSE: {
		int desc;
		int sk, asc, ascq;

		desc = cdb[1] & 0x1;
		if (desc != 0) {
			/* INVALID FIELD IN CDB */
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
			break;
		}

		alloc_len = cdb[4];
		spdk_scsi_task_alloc_data(task, alloc_len, &data);

		/* NO ADDITIONAL SENSE INFORMATION */
		sk = SPDK_SCSI_SENSE_NO_SENSE;
		asc = 0x00;
		ascq = 0x00;

		data_len = spdk_scsi_task_build_sense_data(task, sk, asc, ascq);

		/* omit SenseLength */
		data_len -= 2;
		memcpy(data, &task->sense_data[2], data_len);
		task->data_transferred = (uint64_t)data_len;
		task->status = SPDK_SCSI_STATUS_GOOD;
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
		spdk_scsi_task_set_check_condition(task,
						   SPDK_SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
		break;

	case SPDK_SPC_TEST_UNIT_READY:
		SPDK_TRACELOG(SPDK_TRACE_SCSI, "TEST_UNIT_READY\n");
		cmd_parsed = 1;
	case SPDK_SBC_START_STOP_UNIT:
		if (!cmd_parsed) {
			SPDK_TRACELOG(SPDK_TRACE_SCSI, "START_STOP_UNIT\n");
		}

		task->data_transferred = 0;
		task->status = SPDK_SCSI_STATUS_GOOD;
		break;

	default:
		return SPDK_SCSI_TASK_UNKNOWN;
	}

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
			spdk_scsi_task_set_check_condition(task,
							   SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
							   0x20, 0x00);
			return SPDK_SCSI_TASK_COMPLETE;
		}
	}

	return rc;
}

int
spdk_bdev_scsi_reset(struct spdk_bdev *bdev, struct spdk_scsi_task *task)
{
	return spdk_bdev_reset(bdev, SPDK_BDEV_RESET_HARD,
			       spdk_bdev_scsi_task_complete, task);
}
