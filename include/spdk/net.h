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

/** \file
 * Net framework abstraction layer
 */

#ifndef SPDK_NET_H
#define SPDK_NET_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_sock;

int spdk_interface_init(void);
void spdk_interface_destroy(void);

const char *spdk_net_framework_get_name(void);
int spdk_net_framework_start(void);
void spdk_net_framework_clear_socket_association(struct spdk_sock *sock);
void spdk_net_framework_fini(void);

#define SPDK_IFNAMSIZE		32
#define SPDK_MAX_IP_PER_IFC	32

struct spdk_interface {
	char name[SPDK_IFNAMSIZE];
	uint32_t index;
	uint32_t num_ip_addresses; /* number of IP addresses defined */
	uint32_t ip_address[SPDK_MAX_IP_PER_IFC];
	TAILQ_ENTRY(spdk_interface)	tailq;
};

int spdk_interface_add_ip_address(int ifc_index, char *ip_addr);
int spdk_interface_delete_ip_address(int ifc_index, char *ip_addr);
void *spdk_interface_get_list(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NET_H */
