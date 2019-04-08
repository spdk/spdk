/*
 * memcached_cmd.h
 *
 *  Created on: Jul 17, 2019
 *      Author: root
 */

#ifndef LIB_MEMCACHED_MEMCACHED_CMD_H_
#define LIB_MEMCACHED_MEMCACHED_CMD_H_

#include "memcached_def.h"

enum memcached_cmd_conn_state {
	MEMCACHED_CMD_STATE_IDLE = 0,
	MEMCACHED_CMD_STATE_RECV_HEAD,
	MEMCACHED_CMD_STATE_RECV_DATA,
	MEMCACHED_CMD_STATE_EXE,
	MEMCACHED_CMD_STATE_SEND_HEAD,	/* GET cmd sends "VALUE key ... nbytes" */
	MEMCACHED_CMD_STATE_SEND_DATA,	/* GET cmd sends data */
	MEMCACHED_CMD_STATE_SEND_RESP,
	MEMCACHED_CMD_STATE_ERROR,
};

struct spdk_memcached_cmd_header {
	int opcode;
	uint32_t flags;
	int32_t exptime_int;
	char *key;
	uint32_t key_len;
	uint8_t *data;
	uint32_t data_len;
	bool noreply;
	char maybe_key[KEY_MAX_LENGTH]; // for get/delete cmd, their keys can be put here
};

struct spdk_memcached_cmd {
	struct spdk_memcached_conn *conn;
	enum memcached_cmd_conn_state state; // cmd state in conn

	int ref;
	TAILQ_ENTRY(spdk_memcached_cmd)	tailq;

	enum memcached_protocol protocol;
	struct spdk_memcached_cmd_header cmd_hd;

	uint64_t key_hash;
	char keybuf[KEY_MAX_LENGTH];	/* For "GET/DELETE", key is placed here. For "SET", key will be directly placed in mobj_write */
	struct spdk_mobj
		*mobj_write;	/* for "SET", buffer used to write disk data in,  and it is also used to receive data */

	char outbuf[1024];	/* For "GET", response of "VALUE key ... bytes" is orgnized here. outbuf[0] = '\0' indicates there is no outstring */
	struct spdk_mobj
		*mobj_read;	/* buffer used to read disk data out, for "GET", it is also used to send data */
	char response[100];	/* General response of each cmd */
	enum store_item_type status;	/* execution status which used to indicate response string */
	uint32_t send_len;		/* total len of send datat: <outbuf + data + > response */
	uint32_t send_off;		/* how much data already been sent */

	uint8_t *recv_buf; /* socket data to */
	uint32_t recv_len;
	uint32_t recv_off;

	struct spdk_memcached_cmd_cb_args args;
};

/* Receive data from conn, and judge whether a whole cmd is received.
 *
 * Return 0 A whole cmd hasn't been received.
 * 	  1 A whole cmd is received, and returned by **cmd
 */
int spdk_memcached_cmd_read(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd **cmd);

/*
 * Start to execute a cmd, and execution result is handled in inside its callbacks.
 *
 * Return 0
 */
int spdk_memcached_cmd_execute(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd);

/*
 * Put completed cmd into send list. (It should be called by processor functions)
 */
void spdk_memcached_cmd_done(struct spdk_memcached_cmd *cmd);

/*
 * Build iov for unsent data in a cmd
 *
 * @Param _mapped_length Return how much data are built inside iov
 *
 * Return N How many iov are used
 */
int spdk_memcached_cmd_build_iovs(struct iovec *iovs, int num_iovs,
				  struct spdk_memcached_cmd *cmd, uint32_t *_mapped_length);

/* Get cmd parameters for send */
uint32_t spdk_memcached_cmd_get_sendoff(struct spdk_memcached_cmd *cmd);
void spdk_memcached_cmd_incr_sendoff(struct spdk_memcached_cmd *cmd, uint32_t offset_incr);
uint32_t spdk_memcached_cmd_get_sendlen(struct spdk_memcached_cmd *cmd);

/* Memory management */
struct spdk_memcached_cmd *spdk_memcached_get_cmd(void);
void spdk_memcached_put_cmd(struct spdk_memcached_cmd *cmd);

#endif /* LIB_MEMCACHED_MEMCACHED_CMD_H_ */
