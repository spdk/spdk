/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "scsi_internal.h"

#include "spdk/endian.h"

struct spdk_scsi_port *
spdk_scsi_port_create(uint64_t id, uint16_t index, const char *name)
{
	struct spdk_scsi_port *port;

	port = calloc(1, sizeof(struct spdk_scsi_port));

	if (!port) {
		return NULL;
	}

	if (scsi_port_construct(port, id, index, name) != 0) {
		spdk_scsi_port_free(&port);
		return NULL;
	}

	return port;
}

void
spdk_scsi_port_free(struct spdk_scsi_port **pport)
{
	struct spdk_scsi_port *port;

	if (!pport) {
		return;
	}

	port = *pport;
	*pport = NULL;
	free(port);
}

int
scsi_port_construct(struct spdk_scsi_port *port, uint64_t id, uint16_t index,
		    const char *name)
{
	if (strlen(name) >= sizeof(port->name)) {
		SPDK_ERRLOG("port name too long\n");
		return -1;
	}

	port->is_used = 1;
	port->id = id;
	port->index = index;
	snprintf(port->name, sizeof(port->name), "%s", name);
	return 0;
}

void
scsi_port_destruct(struct spdk_scsi_port *port)
{
	memset(port, 0, sizeof(struct spdk_scsi_port));
}

const char *
spdk_scsi_port_get_name(const struct spdk_scsi_port *port)
{
	return port->name;
}

/*
 * spc3r23 7.5.4.6 iSCSI initiator port TransportID,
 * using code format 0x01.
 */
void
spdk_scsi_port_set_iscsi_transport_id(struct spdk_scsi_port *port, char *iscsi_name,
				      uint64_t isid)
{
	struct spdk_scsi_iscsi_transport_id *data;
	uint32_t len;
	char *name;

	memset(port->transport_id, 0, sizeof(port->transport_id));
	port->transport_id_len = 0;

	data = (struct spdk_scsi_iscsi_transport_id *)port->transport_id;

	data->protocol_id = (uint8_t)SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
	data->format = 0x1;

	name = data->name;
	len = snprintf(name, SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH - sizeof(*data),
		       "%s,i,0x%12.12" PRIx64, iscsi_name, isid);
	do {
		name[len++] = '\0';
	} while (len & 3);

	if (len < 20) {
		SPDK_ERRLOG("The length of Transport ID should >= 20 bytes\n");
		return;
	}

	to_be16(&data->additional_len, len);
	port->transport_id_len = len + sizeof(*data);
}
