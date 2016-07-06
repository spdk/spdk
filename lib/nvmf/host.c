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

#include <arpa/inet.h>
#include <string.h>

#include "host.h"
#include "nvmf_internal.h"
#include "subsystem.h"
#include "spdk/log.h"
#include "spdk/trace.h"

#define MAX_MASKBUF 128
#define MAX_INITIATOR 8
#define MAX_INITIATOR_GROUP 32

#define MAX_ADDRBUF 64
#define MAX_INITIATOR_ADDR (MAX_ADDRBUF)
#define MAX_INITIATOR_NAME 256
#define MAX_NETMASK 256

static TAILQ_HEAD(, spdk_nvmf_host) g_host_head = TAILQ_HEAD_INITIALIZER(g_host_head);

struct spdk_nvmf_host *
spdk_nvmf_host_create(int tag,
		      int num_netmasks,
		      char **netmasks)
{
	int i;
	struct spdk_nvmf_host *host = NULL;

	/* Make sure there are no duplicate initiator group tags */
	if (spdk_nvmf_host_find_by_tag(tag)) {
		SPDK_ERRLOG("Initiator group creation failed due to duplicate initiator group tag (%d)\n",
			    tag);
		return NULL;
	}

	if (num_netmasks > MAX_NETMASK) {
		SPDK_ERRLOG("%d > MAX_NETMASK\n", num_netmasks);
		return NULL;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG,
		      "add initiator group (from initiator list) tag=%d, #masks=%d\n",
		      tag, num_netmasks);

	host = calloc(1, sizeof(*host));
	if (!host) {
		SPDK_ERRLOG("Unable to allocate host (%d)\n", tag);
		return NULL;
	}

	host->tag = tag;

	host->nnetmasks = num_netmasks;
	host->netmasks = netmasks;
	for (i = 0; i < num_netmasks; i++) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Netmask %s\n", host->netmasks[i]);
	}

	host->state = GROUP_INIT;

	pthread_mutex_lock(&g_nvmf_tgt.mutex);
	host->state = GROUP_READY;
	TAILQ_INSERT_TAIL(&g_host_head, host, tailq);
	pthread_mutex_unlock(&g_nvmf_tgt.mutex);

	return host;
}

static void
nvmf_initiator_group_destroy(struct spdk_nvmf_host *host)
{
#if 0 // TODO: fix bogus scan-build warning about use-after-free
	int i;

	if (!host) {
		return;
	}

	for (i = 0; i < host->nnetmasks; i++) {
		free(host->netmasks[i]);
	}

	free(host->netmasks);
	free(host);
#endif
}


static int
spdk_nvmf_allow_ipv6(const char *netmask, const char *addr)
{
	struct in6_addr in6_mask;
	struct in6_addr in6_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	size_t n;
	int bits, bmask;
	int i;

	if (netmask[0] != '[')
		return 0;
	p = strchr(netmask, ']');
	if (p == NULL)
		return 0;
	n = p - (netmask + 1);
	if (n + 1 > sizeof mask)
		return 0;

	memcpy(mask, netmask + 1, n);
	mask[n] = '\0';
	p++;

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits < 0 || bits > 128)
			return 0;
	} else {
		bits = 128;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "input %s\n", addr);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "mask  %s / %d\n", mask, bits);

	/* presentation to network order binary */
	if (inet_pton(AF_INET6, mask, &in6_mask) <= 0
	    || inet_pton(AF_INET6, addr, &in6_addr) <= 0) {
		return 0;
	}

	/* check 128bits */
	for (i = 0; i < (bits / 8); i++) {
		if (in6_mask.s6_addr[i] != in6_addr.s6_addr[i])
			return 0;
	}
	if (bits % 8 && i < (MAX_MASKBUF / 8)) {
		bmask = (0xffU << (8 - (bits % 8))) & 0xffU;
		if ((in6_mask.s6_addr[i] & bmask) != (in6_addr.s6_addr[i] & bmask))
			return 0;
	}

	/* match */
	return 1;
}

static int
spdk_nvmf_allow_ipv4(const char *netmask, const char *addr)
{
	struct in_addr in4_mask;
	struct in_addr in4_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	uint32_t bmask;
	size_t n;
	int bits;

	p = strchr(netmask, '/');
	if (p == NULL) {
		p = netmask + strlen(netmask);
	}
	n = p - netmask;
	if (n + 1 > sizeof mask)
		return 0;

	memcpy(mask, netmask, n);
	mask[n] = '\0';

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits < 0 || bits > 32)
			return 0;
	} else {
		bits = 32;
	}

	/* presentation to network order binary */
	if (inet_pton(AF_INET, mask, &in4_mask) <= 0
	    || inet_pton(AF_INET, addr, &in4_addr) <= 0) {
		return 0;
	}

	/* check 32bits */
	bmask = (0xffffffffULL << (32 - bits)) & 0xffffffffU;
	if ((ntohl(in4_mask.s_addr) & bmask) != (ntohl(in4_addr.s_addr) & bmask))
		return 0;

	/* match */
	return 1;
}

static int
spdk_nvmf_allow_netmask(const char *netmask, const char *addr)
{
	if (netmask == NULL || addr == NULL)
		return 0;
	if (strcasecmp(netmask, "ALL") == 0)
		return 1;
	if (netmask[0] == '[') {
		/* IPv6 */
		if (spdk_nvmf_allow_ipv6(netmask, addr))
			return 1;
	} else {
		/* IPv4 */
		if (spdk_nvmf_allow_ipv4(netmask, addr))
			return 1;
	}
	return 0;
}

struct spdk_nvmf_host *
spdk_nvmf_host_find_by_addr(char *addr)
{
	struct spdk_nvmf_host	*host;
	int i;
	int rc;

	if (addr == NULL)
		return NULL;

	TAILQ_FOREACH(host, &g_host_head, tailq) {
		/* check netmask of each group looking for permission */
		for (i = 0; i < host->nnetmasks; i++) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, "netmask=%s, addr=%s\n",
				      host->netmasks[i], addr);
			rc = spdk_nvmf_allow_netmask(host->netmasks[i], addr);
			if (rc > 0) {
				/* OK netmask */
				return host;
			}
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "No initiator group addr match for %s\n",
		      addr);
	return NULL;
}

struct spdk_nvmf_host *
spdk_nvmf_host_find_by_tag(int tag)
{
	struct spdk_nvmf_host *host;

	TAILQ_FOREACH(host, &g_host_head, tailq) {
		if (host->tag == tag) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, " found initiator group with tag: host %p\n", host);
			return host;
		}
	}

	return NULL;
}

void
spdk_nvmf_host_destroy_all(void)
{
	struct spdk_nvmf_host *host;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");
	pthread_mutex_lock(&g_nvmf_tgt.mutex);
	while (!TAILQ_EMPTY(&g_host_head)) {
		host = TAILQ_FIRST(&g_host_head);
		host->state = GROUP_DESTROY;
		TAILQ_REMOVE(&g_host_head, host, tailq);
		nvmf_initiator_group_destroy(host);
	}
	pthread_mutex_unlock(&g_nvmf_tgt.mutex);
}
