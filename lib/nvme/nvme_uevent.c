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

#include "spdk/stdinc.h"
#include "spdk/string.h"

#include "spdk/log.h"

#include "nvme_uevent.h"

#ifdef __linux__

#include <linux/netlink.h>

#define SPDK_UEVENT_MSG_LEN 4096
#define SPDK_UEVENT_RECVBUF_SIZE 1024 * 1024

int
nvme_uevent_connect(void)
{
	struct sockaddr_nl addr;
	int netlink_fd;
	int size = SPDK_UEVENT_RECVBUF_SIZE;
	int flag;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = getpid();
	addr.nl_groups = 0xffffffff;

	netlink_fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (netlink_fd < 0) {
		return -1;
	}

	setsockopt(netlink_fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size));

	flag = fcntl(netlink_fd, F_GETFL);
	if (fcntl(netlink_fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n", netlink_fd,
			    spdk_strerror(errno));
		close(netlink_fd);
		return -1;
	}

	if (bind(netlink_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(netlink_fd);
		return -1;
	}
	return netlink_fd;
}

/* Note: We only parse the event from uio subsystem and will ignore
 *       all the event from other subsystem. the event from uio subsystem
 *       as below:
 *       action: "add" or "remove"
 *       subsystem: "uio"
 *       dev_path: "/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0"
 */
static int
parse_event(const char *buf, struct spdk_uevent *event)
{
	char action[SPDK_UEVENT_MSG_LEN];
	char subsystem[SPDK_UEVENT_MSG_LEN];
	char dev_path[SPDK_UEVENT_MSG_LEN];
	char driver[SPDK_UEVENT_MSG_LEN];
	char vfio_pci_addr[SPDK_UEVENT_MSG_LEN];

	memset(action, 0, SPDK_UEVENT_MSG_LEN);
	memset(subsystem, 0, SPDK_UEVENT_MSG_LEN);
	memset(dev_path, 0, SPDK_UEVENT_MSG_LEN);
	memset(driver, 0, SPDK_UEVENT_MSG_LEN);
	memset(vfio_pci_addr, 0, SPDK_UEVENT_MSG_LEN);

	while (*buf) {
		if (!strncmp(buf, "ACTION=", 7)) {
			buf += 7;
			snprintf(action, sizeof(action), "%s", buf);
		} else if (!strncmp(buf, "DEVPATH=", 8)) {
			buf += 8;
			snprintf(dev_path, sizeof(dev_path), "%s", buf);
		} else if (!strncmp(buf, "SUBSYSTEM=", 10)) {
			buf += 10;
			snprintf(subsystem, sizeof(subsystem), "%s", buf);
		} else if (!strncmp(buf, "DRIVER=", 7)) {
			buf += 7;
			snprintf(driver, sizeof(driver), "%s", buf);
		} else if (!strncmp(buf, "PCI_SLOT_NAME=", 14)) {
			buf += 14;
			snprintf(vfio_pci_addr, sizeof(vfio_pci_addr), "%s", buf);
		}
		while (*buf++)
			;
	}

	if (!strncmp(subsystem, "uio", 3)) {
		char *pci_address, *tmp;
		struct spdk_pci_addr pci_addr;

		event->subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UIO;
		if (!strncmp(action, "add", 3)) {
			event->action = SPDK_NVME_UEVENT_ADD;
		}
		if (!strncmp(action, "remove", 6)) {
			event->action = SPDK_NVME_UEVENT_REMOVE;
		}
		tmp = strstr(dev_path, "/uio/");
		if (!tmp) {
			SPDK_ERRLOG("Invalid format of uevent: %s\n", dev_path);
			return -1;
		}
		memset(tmp, 0, SPDK_UEVENT_MSG_LEN - (tmp - dev_path));

		pci_address = strrchr(dev_path, '/');
		if (!pci_address) {
			SPDK_ERRLOG("Not found NVMe BDF in uevent: %s\n", dev_path);
			return -1;
		}
		pci_address++;
		if (spdk_pci_addr_parse(&pci_addr, pci_address) != 0) {
			SPDK_ERRLOG("Invalid format for NVMe BDF: %s\n", pci_address);
			return -1;
		}
		spdk_pci_addr_fmt(event->traddr, sizeof(event->traddr), &pci_addr);
	} else if (!strncmp(driver, "vfio-pci", 8)) {
		struct spdk_pci_addr pci_addr;

		event->subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_VFIO;
		if (!strncmp(action, "bind", 4)) {
			event->action = SPDK_NVME_UEVENT_ADD;
		}
		if (!strncmp(action, "remove", 6)) {
			event->action = SPDK_NVME_UEVENT_REMOVE;
		}
		if (spdk_pci_addr_parse(&pci_addr, vfio_pci_addr) != 0) {
			SPDK_ERRLOG("Invalid format for NVMe BDF: %s\n", vfio_pci_addr);
			return -1;
		}
		spdk_pci_addr_fmt(event->traddr, sizeof(event->traddr), &pci_addr);
	} else {
		event->subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED;
	}

	return 1;
}

int
nvme_get_uevent(int fd, struct spdk_uevent *uevent)
{
	int ret;
	char buf[SPDK_UEVENT_MSG_LEN];

	memset(uevent, 0, sizeof(struct spdk_uevent));
	memset(buf, 0, SPDK_UEVENT_MSG_LEN);

	ret = recv(fd, buf, SPDK_UEVENT_MSG_LEN - 1, MSG_DONTWAIT);
	if (ret > 0) {
		return parse_event(buf, uevent);
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			SPDK_ERRLOG("Socket read error(%d): %s\n", errno, spdk_strerror(errno));
			return -1;
		}
	}

	/* connection closed */
	if (ret == 0) {
		return -1;
	}
	return 0;
}

#else /* Not Linux */

int
nvme_uevent_connect(void)
{
	return -1;
}

int
nvme_get_uevent(int fd, struct spdk_uevent *uevent)
{
	return -1;
}
#endif
