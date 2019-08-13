/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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


#ifndef SPDK_NVMF_H
#define SPDK_NVMF_H

#include "spdk/stdinc.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/uuid.h"


#ifdef __cplusplus
extern "C" {
#endif

struct spdk_ftp_poll_group;
struct spdk_ftp_server ;
struct spdk_ftp_server_poll_group;
struct spdk_ftp_server;
struct spdk_ftp_conn;

struct spdk_ftp_server_opts {
	bool		spdk_ftpd_anonymous_enable;
	bool		spdk_ftpd_local_enable;
	bool		spdk_ftpd_log_enable;
	/* char *		spdk_ftpd_log_file; */
	/* char *		spdk_ftpd_banner; */
	bool		spdk_ftpd_deny_email_enable;
	/* char *		spdk_ftpd_banned_email_file; */
	uint16_t	spdk_ftpd_listen_port;
	/* ftp passive mode */
	bool		spdk_ftpd_pasv_enable;
	uint16_t	spdk_ftpd_pasv_min_port;
	uint16_t	spdk_ftpd_pasv_max_port;
	uint16_t	spdk_ftpd_idle_session_timeout;
	uint16_t	spdk_ftpd_connect_timeout;
	uint16_t	spdk_ftpd_max_clients;
	uint16_t	spdk_ftpd_max_per_ip;

	// rui add
	char *ipaddr;
	uint32_t	max_io_size;
	uint32_t	io_unit_size;
	uint32_t	num_shared_buffers;


};

/**
 * Function to be called for each newly discovered connection.
 *
 * \param conn The newly discovered connection.
 */
typedef void (*new_conn_fn)(struct spdk_ftp_conn *conn);

struct spdk_ftp_server {
	struct spdk_ftp_tgt   *tgt;
	struct spdk_ftp_server_opts opts;
	const struct spdk_ftp_server_ops  *ops;

	/* A mempool for server related data transfers */
	struct spdk_mempool			*data_buf_pool;


	TAILQ_ENTRY(spdk_ftp_server) link;
};
enum spdk_ftp_server_type {
	SPDK_FTP_TCP = 0x1,
	SPDK_FTP_RDMA = 0x2,
};
struct spdk_ftp_server_ops {
	/**
	 * Transport type
	 */
	enum spdk_ftp_server_type type;

	/**
	 * Initialize server options to default value
	 */
	void (*opts_init)(struct spdk_ftp_server_opts *opts);

	/**
	 * Create a server for the given server opts
	 */
	struct spdk_ftp_server *(*create)(struct spdk_ftp_server_opts *opts);

	/**
	 * Destroy the transport
	 */
	int (*destroy)(struct spdk_ftp_server *transport);

	/**
	 * Create a new poll group
	 */
	struct spdk_ftp_server_poll_group *(*poll_group_create)(struct spdk_ftp_server *server);

	/**
	 * Poll the group to process I/O
	 */
	int (*poll_group_poll)(struct spdk_ftp_server_poll_group *group);

	/**
	  * Instruct the server to accept new connections at the address
	  * provided. This may be called multiple times.
	  */
	int (*listen)(struct spdk_ftp_server *server);

	/**
	 * Check for new connections on the server.
	 */
	void (*accept)(struct spdk_ftp_server *server, new_conn_fn cb_fn);

	/**
	 * Add a conn to a poll group
	 */
	int (*poll_group_add)(struct spdk_ftp_server_poll_group *group,
			      struct spdk_ftp_conn *conn);

};



typedef void (spdk_ftp_tgt_destroy_done_fn)(void *ctx, int status);

struct spdk_ftp_tgt {
	TAILQ_HEAD(, spdk_ftp_server) ftpds;

	spdk_ftp_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;
};


void spdk_ftp_tgt_destroy_server(struct spdk_ftp_tgt *tgt);

struct spdk_ftp_tgt *spdk_ftp_tgt_create(void);

/**
 * create a protocol ftp server
 * \param type the server type to create, TCP or RDMA
 * \param opts the server options
 * \return new server or NULL if create fails
 */
struct spdk_ftp_server *spdk_ftp_server_create(enum spdk_ftp_server_type type,
		struct spdk_ftp_server_opts *opts);

void spdk_ftp_poll_group_destroy(struct spdk_ftp_poll_group *group);



/**
 * Poll the target for incoming connections.
 *
 * The new_conn_fn cb_fn will be called for each newly discovered
 * connection. The user is expected to add that connection to a poll group to
 * establish the connection.
 *
 * \param tgt The target associated with the listen address.
 * \param cb_fn Called for each newly discovered connection
 */
void spdk_ftp_tgt_accept(struct spdk_ftp_tgt *tgt, new_conn_fn cb_fn);

/**
 * Function to be called once server add is complete
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_ftp_tgt_add_server_done_fn)(void *cb_arg, int status);

/**
 * Add a server to a target
 *
 * \param tgt The FTP target
 * \param server The server to add
 * \param cb_fn A callback that will be called once the server is created
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return void. The callback status argument will be 0 on success
 *	   or a negated errno on failure.
 */
void spdk_ftp_tgt_add_server(struct spdk_ftp_tgt *tgt,
			     struct spdk_ftp_server *server,
			     spdk_ftp_tgt_add_server_done_fn cb_fn,
			     void *cb_arg);

/**
 * get ftp transport type
 * */
int spdk_ftp_server_parse_type(enum spdk_ftp_server_type *stype, const char *str);

/**
 * get ftp trasport name
 */
const char *
spdk_ftp_server_trtype_str(enum spdk_ftp_server_type stype);


/**
 * Function to be called once the target is listening.
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_ftp_tgt_listen_done_fn)(void *ctx, int status);


/**
 * Begin accepting new connections at the address provided.
 * \param tgt The target associated with this listen address.
 * \param server The address to listen at.
 * \param cb_fn A callback that will be called once the target is listening
 * \param cb_arg A context argument passed to cb_fn.
 * \return void. The callback status argument will be 0 on success
 *	   or a negated errno on failure.
 */
void spdk_ftp_tgt_listen(struct spdk_ftp_tgt *tgt,
			 enum spdk_ftp_server_type type,
			 spdk_ftp_tgt_listen_done_fn cb_fn,
			 void *cb_arg);


/**
 *
 * Add listener to server and begin accepting new connections.
 *
 * \param server The server to add listener to
 *
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */

int spdk_ftp_server_listen(struct spdk_ftp_server *server);

typedef void (*ftp_conn_disconnect_cb)(void *ctx);
/**
 * Disconnect an FTP connection
 *
 * \param conn The FTP connection to disconnect.
 * \param cb_fn The function to call upon completion of the disconnect.
 * \param ctx The context to pass to the callback function.
 *
 * \return 0 upon success.
 * \return -ENOMEM if the function specific context could not be allocated.
 */
int spdk_ftp_conn_disconnect(struct spdk_ftp_conn *conn, ftp_conn_disconnect_cb cb_fn,
			     void *ctx);


/**
 * Add the given conn to the poll group.
 *
 * \param group The group to add conn to.
 * \param conn The conn to add.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_ftp_poll_group_add(struct spdk_ftp_poll_group *group,
			    struct spdk_ftp_conn *conn);


// transport.h
/**
 * init ftp server opts according to different type
 * */
bool spdk_ftp_server_opts_init(enum spdk_ftp_server_type type,
			       struct spdk_ftp_server_opts *opts);
int
spdk_ftp_server_poll_group_poll(struct spdk_ftp_server_poll_group *group);

int spdk_ftp_server_poll_group_add(struct spdk_ftp_server_poll_group *group,
				   struct spdk_ftp_conn *conn);
extern const struct spdk_ftp_server_ops spdk_ftp_server_tcp;
#ifdef __cplusplus
}
#endif


#endif
