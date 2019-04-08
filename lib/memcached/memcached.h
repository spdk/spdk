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
 * Memcached Service Layer
 */

#ifndef SPDK_MEMCACHED_H_
#define SPDK_MEMCACHED_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/thread.h"

#include "spdk/assert.h"
#include "spdk/dif.h"
#include "spdk/util.h"

//#include "iscsi/param.h"
//#include "iscsi/tgt_node.h"
#include "memcached/memcached_def.h"
#include "memcached/recv_buf.h"
#include "memcached/cmd_handler.h"

#define DEFAULT_MAXR2T 4
#define MAX_INITIATOR_PORT_NAME 256
#define MAX_INITIATOR_NAME 223
#define MAX_TARGET_NAME 223

#define MAX_PORTAL 1024
#define MAX_INITIATOR 256
#define MAX_NETMASK 256


#define DEFAULT_PORT 32600
#define DEFAULT_TIMEOUT 60
#define DEFAULT_CONNECTIONS_PER_LCORE 4

#define DEFAULT_MAX_QUEUE_DEPTH	64

#define MAX_DISKDATA_PER_CONNECTION 256

#define MEMCACHED_DATA_BUFFER_ALIGNMENT	(0x1000)
#define MEMCACHED_DATA_BUFFER_MASK		(MEMCACHED_DATA_BUFFER_ALIGNMENT - 1)

#define SPDK_MEMCACHED_MAX_DISKDATA_LENGTH	(1 * 0x1000)

enum memcached_connection_state {
	MEMCACHED_CONN_STATE_INVALID = 0,
	MEMCACHED_CONN_STATE_RUNNING = 1,
	MEMCACHED_CONN_STATE_EXITING = 2,
	MEMCACHED_CONN_STATE_EXITED = 3,
};

enum spdk_error_codes {
	SPDK_SUCCESS		= 0,
	SPDK_MEMCACHED_CONNECTION_FATAL	= -1,
	SPDK_CMD_FATAL		= -2,
};

struct spdk_mobj {
	struct spdk_mempool *mp;	/* indicate which pool this buf is come from when release it */
	void *buf;
	//TODO: consider large data
//	struct spdk_mobj *next;
};

struct spdk_memcached_poll_group {
	uint32_t						core;
	struct spdk_poller				*poller;
	struct spdk_poller				*nop_poller;
	STAILQ_HEAD(connections, spdk_memcached_conn)	connections;
	struct spdk_sock_group				*sock_group;
};

struct spdk_memcached_opts {
	int32_t timeout;
	uint32_t MaxConnections;
	uint32_t MaxQueueDepth;
	uint32_t min_connections_per_core;
};

struct spdk_memcached_globals {
	pthread_mutex_t mutex;
	TAILQ_HEAD(, spdk_memcached_portal)	portal_head;
	TAILQ_HEAD(, spdk_memcached_portal_grp)	pg_head;
	TAILQ_HEAD(, spdk_memcached_init_grp)	ig_head;
	TAILQ_HEAD(, spdk_memcached_tgt_node)	target_head;

	int32_t timeout;
	uint32_t MaxConnections;
	uint32_t MaxQueueDepth;

	struct spdk_mempool *cmd_pool;
	struct spdk_mempool *diskdata_pool;

	struct spdk_memcached_poll_group *poll_group;
};

#if 1  // functions in memcached subsystem.c
extern struct spdk_memcached_globals g_spdk_memcached;
extern struct spdk_memcached_opts *g_spdk_memcached_opts;

typedef void (*spdk_memcached_init_cb)(void *cb_arg, int rc);
void spdk_memcached_init(spdk_memcached_init_cb cb_fn,
			 void *cb_arg); // call spdk_initialize_memcached_conns
typedef void (*spdk_memcached_fini_cb)(void *arg);

void spdk_memcached_fini(spdk_memcached_fini_cb cb_fn, void *cb_arg);

void spdk_shutdown_memcached_conns_done(void); // called by conn

//void spdk_iscsi_config_text(FILE *fp);
//void spdk_iscsi_config_json(struct spdk_json_write_ctx *w);

//struct spdk_memcached_opts *spdk_memcached_opts_alloc(void); // opt functions are used externally by rpc
//void spdk_memcached_opts_free(struct spdk_memcached_opts *opts);
//struct spdk_memcached_opts *spdk_memcached_opts_copy(struct spdk_memcached_opts *src);
//void spdk_iscsi_opts_info_json(struct spdk_json_write_ctx *w);
#endif

/**
 * Write memcached subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_memcached_write_config_json(struct spdk_json_write_ctx *w);

int spdk_memcached_construct(char *ip_str, char *port_str, char *cpumask,
			     char *bdev_name, char *readonly);

#ifdef __cplusplus
}
#endif

#endif
