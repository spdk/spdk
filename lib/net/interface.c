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

#include "net_internal.h"

#include "spdk/stdinc.h"
#include "spdk/string.h"

#include "spdk/log.h"
#include "spdk/net.h"

#ifdef __linux__ /* Interface management is Linux-specific */

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static TAILQ_HEAD(, spdk_interface) g_interface_head = TAILQ_HEAD_INITIALIZER(g_interface_head);

static pthread_mutex_t interface_lock = PTHREAD_MUTEX_INITIALIZER;

static int get_ifc_ipv4(void)
{
	int ret;
	int rtattrlen;
	int netlink_fd;
	uint32_t ipv4_addr;

	struct {
		struct nlmsghdr n;
		struct ifaddrmsg r;
		struct rtattr rta;
	} req;
	char buf[16384];
	struct nlmsghdr *nlmp;
	struct ifaddrmsg *rtmp;
	struct rtattr *rtatp;
	struct spdk_interface *ifc;

	netlink_fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (netlink_fd < 0) {
		SPDK_ERRLOG("socket failed!\n");
		return 1;
	}

	/*
	 * Prepare a message structure
	 */
	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	req.n.nlmsg_type = RTM_GETADDR;

	/* IPv4 only */
	req.r.ifa_family = AF_INET;

	/*
	 * Fill up all the attributes for the rtnetlink header.
	 */
	assert(&req.rta == (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len)));
	req.rta.rta_len = RTA_LENGTH(16);

	/* Send and recv the message from kernel */
	ret = send(netlink_fd, &req, req.n.nlmsg_len, 0);
	if (ret < 0) {
		SPDK_ERRLOG("netlink send failed: %s\n", spdk_strerror(errno));
		ret = 1;
		goto exit;
	}

	ret = recv(netlink_fd, buf, sizeof(buf), 0);
	if (ret <= 0) {
		SPDK_ERRLOG("netlink recv failed: %s\n", spdk_strerror(errno));
		ret = 1;
		goto exit;
	}

	for (nlmp = (struct nlmsghdr *)buf; ret > (int)sizeof(*nlmp);) {
		int len = nlmp->nlmsg_len;
		int req_len = len - sizeof(*nlmp);

		if (req_len < 0 || len > ret) {
			SPDK_ERRLOG("error\n");
			ret = 1;
			goto exit;
		}

		if (!NLMSG_OK(nlmp, (uint32_t)ret)) {
			SPDK_ERRLOG("NLMSG not OK\n");
			ret = 1;
			goto exit;
		}

		rtmp = (struct ifaddrmsg *)NLMSG_DATA(nlmp);
		rtatp = (struct rtattr *)IFA_RTA(rtmp);

		rtattrlen = IFA_PAYLOAD(nlmp);

		for (; RTA_OK(rtatp, rtattrlen); rtatp = RTA_NEXT(rtatp, rtattrlen)) {
			if (rtatp->rta_type == IFA_LOCAL) {
				memcpy(&ipv4_addr, (struct in_addr *)RTA_DATA(rtatp),
				       sizeof(struct in_addr));
				TAILQ_FOREACH(ifc, &g_interface_head, tailq) {
					if (ifc->index == rtmp->ifa_index) {
						/* add a new IP address to interface */
						if (ifc->num_ip_addresses >= SPDK_MAX_IP_PER_IFC) {
							SPDK_ERRLOG("SPDK: number of IP addresses supported for %s excceded. limit=%d\n",
								    ifc->name,
								    SPDK_MAX_IP_PER_IFC);
							break;
						}
						ifc->ip_address[ifc->num_ip_addresses] = ipv4_addr;
						ifc->num_ip_addresses++;
						break;
					}
				}
			}
		}
		ret -= NLMSG_ALIGN(len);
		nlmp = (struct nlmsghdr *)((char *)nlmp + NLMSG_ALIGN(len));
	}
	ret = 0;

exit:
	close(netlink_fd);
	return ret;
}


static int process_new_interface_msg(struct nlmsghdr *h)
{
	int len;
	struct spdk_interface *ifc;
	struct ifinfomsg *iface;
	struct rtattr *attribute;

	iface = (struct ifinfomsg *)NLMSG_DATA(h);

	ifc = (struct spdk_interface *) malloc(sizeof(*ifc));
	if (ifc == NULL) {
		SPDK_ERRLOG("Malloc failed\n");
		return 1;
	}

	memset(ifc, 0, sizeof(*ifc));

	/* Set interface index */
	ifc->index = iface->ifi_index;

	len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*iface));

	/* Loop over all attributes for the NEWLINK message */
	for (attribute = IFLA_RTA(iface); RTA_OK(attribute, len); attribute = RTA_NEXT(attribute, len)) {
		switch (attribute->rta_type) {
		case IFLA_IFNAME:
			if (if_indextoname(iface->ifi_index, ifc->name) == NULL) {
				SPDK_ERRLOG("Indextoname failed!\n");
				free(ifc);
				return 2;
			}
			break;
		default:
			break;
		}
	}
	TAILQ_INSERT_TAIL(&g_interface_head, ifc, tailq);
	return 0;
}

static int prepare_ifc_list(void)
{
	int ret = 0;
	struct nl_req_s {
		struct nlmsghdr hdr;
		struct rtgenmsg gen;
		struct ifinfomsg ifi;
	};
	int netlink_fd;
	struct sockaddr_nl local;	/* Our local (user space) side of the communication */
	struct sockaddr_nl kernel;	/* The remote (kernel space) side of the communication */

	struct msghdr rtnl_msg;		/* Generic msghdr struct for use with sendmsg */
	struct iovec io;		/* IO vector for sendmsg */

	struct nl_req_s req;		/* Structure that describes the rtnetlink packet itself */
	char reply[16384];		/* a large buffer to receive lots of link information */

	pid_t pid = getpid();		/* Our process ID to build the correct netlink address */
	int end = 0;			/* some flag to end loop parsing */

	/*
	 * Prepare netlink socket for kernel/user space communication
	 */
	netlink_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (netlink_fd < 0) {
		SPDK_ERRLOG("socket failed!\n");
		return 1;
	}

	memset(&local, 0, sizeof(local)); /* Fill-in local address information */
	local.nl_family = AF_NETLINK;
	local.nl_pid = pid;
	local.nl_groups = 0;

	/* RTNL socket is ready to use, prepare and send L2 request. */
	memset(&rtnl_msg, 0, sizeof(rtnl_msg));
	memset(&kernel, 0, sizeof(kernel));
	memset(&req, 0, sizeof(req));

	kernel.nl_family = AF_NETLINK; /* Fill-in kernel address (destination of our message) */

	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
	req.hdr.nlmsg_type = RTM_GETLINK;
	req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.hdr.nlmsg_seq = 1;
	req.hdr.nlmsg_pid = pid;

	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_type = 1;

	io.iov_base = &req;
	io.iov_len = req.hdr.nlmsg_len;
	rtnl_msg.msg_iov = &io;
	rtnl_msg.msg_iovlen = 1;
	rtnl_msg.msg_name = &kernel;
	rtnl_msg.msg_namelen = sizeof(kernel);

	if (sendmsg(netlink_fd, &rtnl_msg, 0) == -1) {
		SPDK_ERRLOG("Sendmsg failed!\n");
		ret = 1;
		goto exit;
	}

	/* Parse reply */
	while (!end) {
		int len;
		struct nlmsghdr *msg_ptr;	/* Pointer to current message part */

		struct msghdr rtnl_reply;	/* Generic msghdr structure for use with recvmsg */
		struct iovec io_reply;

		memset(&io_reply, 0, sizeof(io_reply));
		memset(&rtnl_reply, 0, sizeof(rtnl_reply));

		io.iov_base = reply;
		io.iov_len = 8192;
		rtnl_reply.msg_iov = &io;
		rtnl_reply.msg_iovlen = 1;
		rtnl_reply.msg_name = &kernel;
		rtnl_reply.msg_namelen = sizeof(kernel);

		/* Read as much data as fits in the receive buffer */
		len = recvmsg(netlink_fd, &rtnl_reply, 0);
		if (len) {
			for (msg_ptr = (struct nlmsghdr *) reply; NLMSG_OK(msg_ptr, (uint32_t)len);
			     msg_ptr = NLMSG_NEXT(msg_ptr, len)) {
				switch (msg_ptr->nlmsg_type) {
				case NLMSG_DONE:		/* This is the special meaning NLMSG_DONE message we asked for by using NLM_F_DUMP flag */
					end++;
					break;
				case RTM_NEWLINK:	/* This is a RTM_NEWLINK message, which contains lots of information about a link */
					ret = process_new_interface_msg(msg_ptr);
					if (ret != 0) {
						goto exit;
					}
					break;
				default:
					break;
				}
			}
		}
	}
exit:
	close(netlink_fd);
	return ret;
}

static struct spdk_interface *
interface_find_by_index(uint32_t ifc_index)
{
	struct spdk_interface *ifc_entry;

	/* Mutex must has benn held by the caller */
	TAILQ_FOREACH(ifc_entry, &g_interface_head, tailq) {
		if (ifc_entry->index == ifc_index) {
			return ifc_entry;
		}
	}

	return NULL;
}

static int netlink_addr_msg(uint32_t ifc_idx, uint32_t ip_address, uint32_t create)
{
	int fd;
	struct sockaddr_nl la;
	struct sockaddr_nl pa;
	struct msghdr msg;
	struct iovec iov;
	int ifal;
	struct {
		struct nlmsghdr n;
		struct ifaddrmsg r;
		char buf[16384];
	} req;
	struct rtattr *rta;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) {
		SPDK_ERRLOG("socket failed!\n");
		return errno;
	}

	/* setup local address & bind using this address. */
	bzero(&la, sizeof(la));
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	bind(fd, (struct sockaddr *) &la, sizeof(la));

	/* initialize RTNETLINK request buffer. */
	bzero(&req, sizeof(req));

	/* compute the initial length of the service request. */
	ifal = sizeof(struct ifaddrmsg);

	/* add first attrib: set IP addr and RTNETLINK buffer size. */
	rta = (struct rtattr *) req.buf;
	rta->rta_type = IFA_ADDRESS;
	rta->rta_len = sizeof(struct rtattr) + 4;
	memcpy(((char *)rta) + sizeof(struct rtattr), &ip_address, sizeof(ip_address));
	ifal += rta->rta_len;

	/* add second attrib. */
	rta = (struct rtattr *)(((char *)rta) + rta->rta_len);
	rta->rta_type = IFA_LOCAL;
	rta->rta_len = sizeof(struct rtattr) + 4;
	memcpy(((char *)rta) + sizeof(struct rtattr), &ip_address, sizeof(ip_address));
	ifal += rta->rta_len;

	/* setup the NETLINK header. */
	req.n.nlmsg_len = NLMSG_LENGTH(ifal);
	if (create) {
		req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_APPEND;
		req.n.nlmsg_type = RTM_NEWADDR;
	} else {
		req.n.nlmsg_flags = NLM_F_REQUEST;
		req.n.nlmsg_type = RTM_DELADDR;
	}

	/* setup the service header (struct rtmsg). */
	req.r.ifa_family = AF_INET;
	req.r.ifa_prefixlen = 32; /* hardcoded */
	req.r.ifa_flags = IFA_F_PERMANENT | IFA_F_SECONDARY;
	req.r.ifa_index = ifc_idx;
	req.r.ifa_scope = 0;

	/* create the remote address to communicate. */
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;

	/* initialize & create the struct msghdr supplied to the sendmsg() function. */
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void *) &pa;
	msg.msg_namelen = sizeof(pa);

	/* place the pointer & size of the RTNETLINK message in the struct msghdr. */
	iov.iov_base = (void *) &req.n;
	iov.iov_len = req.n.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	/* send the RTNETLINK message to kernel. */
	sendmsg(fd, &msg, 0);
	close(fd);
	return 0;
}

static void interface_ip_update(void)
{
	struct spdk_interface *ifc_entry;

	pthread_mutex_lock(&interface_lock);
	TAILQ_FOREACH(ifc_entry, &g_interface_head, tailq) {
		ifc_entry->num_ip_addresses = 0;
		memset(ifc_entry->ip_address, 0, sizeof(ifc_entry->ip_address));
	}
	get_ifc_ipv4();
	pthread_mutex_unlock(&interface_lock);
}

static int
interface_is_ip_address_in_use(int ifc_index, uint32_t addr, bool add)
{
	struct spdk_interface *ifc_entry;
	bool in_use = false;
	uint32_t idx = 0;

	interface_ip_update();

	pthread_mutex_lock(&interface_lock);
	ifc_entry = interface_find_by_index(ifc_index);
	if (ifc_entry == NULL) {
		pthread_mutex_unlock(&interface_lock);
		return -ENODEV;
	}

	for (idx = 0; idx < ifc_entry->num_ip_addresses; idx++) {
		if (ifc_entry->ip_address[idx] == addr) {
			in_use = true;
			break;
		}
	}
	pthread_mutex_unlock(&interface_lock);

	/* The IP address to add is alerady in use */
	if (add == true && in_use == true) {
		return -EADDRINUSE;
	}

	/* The IP address to delete is not in use */
	if (add == false && in_use == false) {
		return -ENXIO;
	}

	return 0;
}

int
spdk_interface_init(void)
{
	int rc = 0;

	rc = prepare_ifc_list();
	if (!rc) {
		rc = get_ifc_ipv4();
	}

	return rc;
}

void
spdk_interface_destroy(void)
{
	struct spdk_interface *ifc_entry;

	while (!TAILQ_EMPTY(&g_interface_head)) {
		ifc_entry = TAILQ_FIRST(&g_interface_head);
		TAILQ_REMOVE(&g_interface_head, ifc_entry, tailq);
		free(ifc_entry);
	}
}

int
interface_net_interface_add_ip_address(int ifc_index, char *ip_addr)
{
	uint32_t addr;
	int ret;

	addr = inet_addr(ip_addr);

	ret = interface_is_ip_address_in_use(ifc_index, addr, true);
	if (ret < 0) {
		return ret;
	}

	return netlink_addr_msg(ifc_index, addr, 1);
}

int
interface_net_interface_delete_ip_address(int ifc_index, char *ip_addr)
{
	uint32_t addr;
	int ret;

	addr = inet_addr(ip_addr);

	ret = interface_is_ip_address_in_use(ifc_index, addr, false);
	if (ret < 0) {
		return ret;
	}

	return netlink_addr_msg(ifc_index, addr, 0);
}

void *interface_get_list(void)
{
	interface_ip_update();
	return &g_interface_head;
}

#else /* Not Linux */

int
spdk_interface_init(void)
{
	return 0;
}

void
spdk_interface_destroy(void)
{
}

int
interface_net_interface_add_ip_address(int ifc_index, char *ip_addr)
{
	return -1;
}

int
interface_net_interface_delete_ip_address(int ifc_index, char *ip_addr)
{
	return -1;
}

void *
interface_get_list(void)
{
	return NULL;
}

#endif
