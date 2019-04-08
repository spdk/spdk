/*
 * memcached_cmd.c
 *
 *  Created on: Jul 5, 2019
 *      Author: root
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"
#include "spdk/env.h"

#include "memcached/memcached.h"
#include "memcached/cmd_handler.h"
#include "memcached/memcached_def.h"
#include "memcached/recv_buf.h"
#include "memcached/conn.h"
#include "memcached/murmur3_hash.h"
#include "memcached/diskitem.h"

#include "spdk/hashtable.h"

#define RECV_STEPIN	64

#if 1 /* spdk_memcached_cmd_read start */
/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1) {
                    e++;
                    s = e; /* so we don't add an extra token */
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e) {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* Return 0 if successfully extract 1 cmd */
static int
memcached_extract_cmd(struct spdk_memcached_cmd *cmd, char *command, int cmd_size)
{
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;
    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    int rc;
    enum memcached_connection_state next;

    assert(command[cmd_size - 1] == '\n');
    if (command[cmd_size - 2] == '\r') {
    	cmd_size--;
    }
    command[cmd_size - 1] = '\0';

	ntokens = tokenize_command(command, tokens, MAX_TOKENS);

	if (ntokens >= 3 &&
			(strcmp(tokens[COMMAND_TOKEN].value, "get") == 0 ||
			strcmp(tokens[COMMAND_TOKEN].value, "bget") == 0 )) {

		hd->opcode = MEMCACHED_CMD_GET;
		next = MEMCACHED_CMD_STATE_EXE;
	} else if ((ntokens == 6 || ntokens == 7) &&
			(strcmp(tokens[COMMAND_TOKEN].value, "add") == 0)) {

		hd->opcode = MEMCACHED_CMD_ADD;
		next = MEMCACHED_CMD_STATE_RECV_DATA;
	} else if (ntokens >= 3 && ntokens <= 5 &&
			(strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {

		hd->opcode = MEMCACHED_CMD_DELETE;
		next = MEMCACHED_CMD_STATE_EXE;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Not supported cmd %s\n", tokens[COMMAND_TOKEN].value);
		hd->opcode = MEMCACHED_CMD_INVALID_CMD;
		next = MEMCACHED_CMD_STATE_EXE;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "extract cmd name is %s\n", cmd_extracters[hd->opcode].cmd_name);

	rc = cmd_extracters[hd->opcode].extract_fn(cmd, tokens, ntokens);
	if (rc != 0) {
		next = MEMCACHED_CMD_STATE_ERROR;
	}

	cmd->state = next;
	return rc;
}

/* Return -X if error occurs */
static int
try_read_command(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd)
{
	int rc;
	bool brc = false;
	struct spdk_memcached_conn_recv_buf *recv_buf = &cmd->conn->recv_buf;
	char *buf;
	int cmd_size;

	rc = 1;
	while (rc > 0) {
		brc = memcached_conn_recv_buf_contain_end(recv_buf);
		if (brc == true) {
			break;
		}

		buf = memcached_conn_recv_buf_get_recv_addr(recv_buf);
		rc = spdk_memcached_conn_read_data(conn, RECV_STEPIN, buf);
		if (rc <= 0) {
			//TODO: error
			return rc;
		}

		memcached_conn_recv_buf_incr_recv_addr(recv_buf, rc);
	}

	cmd_size = memcached_conn_recv_buf_get_cmd_size(recv_buf);
	buf = memcached_conn_recv_buf_get_start_addr(recv_buf);

	rc = memcached_extract_cmd(cmd, buf, cmd_size);
	if (rc == 0) {
		if (cmd->cmd_hd.key) {
			cmd->key_hash = MurmurHash3_x64_64(cmd->cmd_hd.key, cmd->cmd_hd.key_len);
		}
		return 1;
	}
	return rc;
}

/* Return 1 if one cmd is captured; return 0 if wait for a whole cmd */
int
spdk_memcached_cmd_read(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd **_cmd)
{
	struct spdk_memcached_cmd *cmd;
	int store_len;
	int rc;
	int offset;
	struct spdk_memcached_diskitem *ditem;

	if (conn->cmd_in_recv == NULL) {
		conn->cmd_in_recv = spdk_memcached_get_cmd();
		if (conn->cmd_in_recv == NULL) {
			return SPDK_MEMCACHED_CONNECTION_FATAL;
		}
		memcached_conn_recv_buf_revise(&conn->recv_buf);
		conn->cmd_in_recv->state = MEMCACHED_CMD_STATE_RECV_HEAD;
		conn->cmd_in_recv->protocol = ASCII_PROT;
		conn->cmd_in_recv->conn = conn;
	}

	cmd = conn->cmd_in_recv;

	if (cmd->state == MEMCACHED_CMD_STATE_RECV_HEAD) { // cmd part haven't been read out
		rc = try_read_command(conn, cmd); // return -X: socket err or parse error
		if (rc < 0) {
			return rc;
		}

		if (cmd->state == MEMCACHED_CMD_STATE_RECV_DATA) {
			assert(cmd->mobj_write == NULL);

			store_len = memcached_diskitem_required_size(&cmd->cmd_hd);
			assert(store_len <= MEMCACHED_MAX_STORE_LENGTH);

			cmd->mobj_write = spdk_mempool_get(g_spdk_memcached.diskdata_pool);
			assert(cmd->mobj_write);
			ditem = (struct spdk_memcached_diskitem *)cmd->mobj_write->buf;

			/* move key and other info into store buf before recvbuf's change */
			memcached_diskitem_set_head_key(ditem, &cmd->cmd_hd);

			/* update positions of key and data */
			cmd->cmd_hd.key = memcached_diskitem_get_key(ditem);
			cmd->cmd_hd.data = memcached_diskitem_get_data(ditem);

			cmd->recv_buf = cmd->cmd_hd.data;
			cmd->recv_len = cmd->cmd_hd.data_len;

			/* for text cmd, data is indicated ending by END-CHAR (CR,LF), so reserve 2B; TODO: Check it */
			cmd->recv_len += 2;


			offset = memcached_conn_recv_buf_extract_data(&conn->recv_buf, cmd->recv_buf, cmd->recv_len);
			cmd->recv_off = offset;
		} else if (cmd->state == MEMCACHED_CMD_STATE_EXE ||
				cmd->state == MEMCACHED_CMD_STATE_ERROR) {
			conn->cmd_in_recv = NULL;

			*_cmd = cmd;
			return 1;
		}
	}

	if (cmd->state == MEMCACHED_CMD_STATE_RECV_DATA) {
		uint32_t data_remain = cmd->recv_len - cmd->recv_off;

		offset = spdk_memcached_conn_read_data(conn, data_remain, cmd->recv_buf + cmd->recv_off);
		if (offset < 0) {
			//TODO: error
			return offset;
		}

		cmd->recv_off += offset;
		if(cmd->recv_off == cmd->recv_len) {
			cmd->state = MEMCACHED_CMD_STATE_EXE;
			conn->cmd_in_recv = NULL;

			*_cmd = cmd;
			return 1;
		}
	}

	assert(cmd->state == MEMCACHED_CMD_STATE_RECV_HEAD ||
			cmd->state == MEMCACHED_CMD_STATE_RECV_DATA);
	return 0;
}
#endif

static void
memcached_cmd_print(struct spdk_memcached_cmd_header *hd)
{
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Memcached CMD Print:\n");
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Name\t%s\n", cmd_extracters[hd->opcode].cmd_name);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Key\t%s\n", hd->key);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Key-len\t%d\n", hd->key_len);
	if (hd->data_len > 0) {
		SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Data\t%s\n", hd->data);
		SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Data-len\t%d\n", hd->data_len);
	}
}

#if 1 /* spdk_memcached_cmd_execute start */
static void
_memcached_execute_cmd(void *cb_arg)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;
	int opcode = hd->opcode;
	int rc;

	rc = cmd_processors[opcode].process_fn(cmd);
	if (rc < 0) {
		assert(false);
		return;
	}
}

int
spdk_memcached_cmd_execute(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd)
{
	struct spdk_thread *handle_td = spdk_hashtable_locate_thread(cmd->key_hash);
	struct spdk_thread *submit_td = spdk_get_thread();

	memcached_cmd_print(&cmd->cmd_hd);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Key handler thread is %p(Name %s)\n", handle_td, spdk_thread_get_name(handle_td));
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Key receive thread is %p(Name %s)\n", submit_td, spdk_thread_get_name(submit_td));

	conn->thd = submit_td;
	if (submit_td != handle_td) {
		spdk_thread_send_msg(handle_td, _memcached_execute_cmd, cmd);
		return 0;
	}

	_memcached_execute_cmd(cmd);
	return 0;
}
#endif

#if 1 /* spdk_memcached_cmd_done start */
static int
_memcached_cmd_send(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd)
{
	TAILQ_INSERT_TAIL(&conn->write_cmd_list, cmd, tailq);
	spdk_memcached_conn_flush_cmds(conn);

	return 0;
}

static void
_memcached_execute_cmd_done(void *cb_arg)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;

	if (hd->opcode == MEMCACHED_CMD_GET) {
		cmd->state = MEMCACHED_CMD_STATE_SEND_HEAD;
	} else {
		cmd->state = MEMCACHED_CMD_STATE_SEND_RESP;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "sending str is %s\n", cmd->response);
	_memcached_cmd_send(cmd->conn, cmd);
}

void
spdk_memcached_cmd_done(struct spdk_memcached_cmd *cmd)
{
	struct spdk_thread *handle_td = spdk_get_thread();

	if (cmd->conn->thd != handle_td) {
		spdk_thread_send_msg(cmd->conn->thd, _memcached_execute_cmd_done, cmd);
		return;
	}

	_memcached_execute_cmd_done(cmd);
}
#endif

#if 1 /* spdk_memcached_cmd_build_iovs */
struct _iov_ctx {
	struct iovec	*iov;		/* Current iov, stepped in */
	uint32_t	iov_offset;	/* How much data already be sent in current and following iovs */
	int		num_iovs;	/* Maximum size of original iov */
	int		iovcnt;		/* Number of valid original iov */
	uint32_t	mapped_len;	/* Howm much data are waiting to be sent */
};

static inline void
_iov_ctx_init(struct _iov_ctx *ctx, struct iovec *iovs, int num_iovs,
	      uint32_t iov_offset)
{
	ctx->iov = iovs;
	ctx->num_iovs = num_iovs;
	ctx->iov_offset = iov_offset;
	ctx->iovcnt = 0;
	ctx->mapped_len = 0;
}

static inline bool
_iov_ctx_set_iov(struct _iov_ctx *ctx, uint8_t *data, uint32_t data_len)
{
	if (ctx->iov_offset >= data_len) {
		ctx->iov_offset -= data_len;
	} else {
		ctx->iov->iov_base = data + ctx->iov_offset;
		ctx->iov->iov_len = data_len - ctx->iov_offset;
		ctx->mapped_len += data_len - ctx->iov_offset;
		ctx->iov_offset = 0;
		ctx->iov++;
		ctx->iovcnt++;
		if (ctx->iovcnt == ctx->num_iovs) {
			return false;
		}
	}

	return true;
}


int
spdk_memcached_cmd_build_iovs(struct iovec *iovs, int num_iovs,
		      struct spdk_memcached_cmd *cmd, uint32_t *_mapped_length)
{
	struct spdk_memcached_diskitem *ditem;
	struct _iov_ctx ctx;
	uint32_t data_len;
	uint8_t *data_buf;

	if (num_iovs == 0) {
		return 0;
	}

	_iov_ctx_init(&ctx, iovs, num_iovs, cmd->send_off);

	/* build iov for "GET"'s "VALUE key ... nbyte" and data */
	if (cmd->outbuf[0] != '\0') {
		if (!_iov_ctx_set_iov(&ctx, (uint8_t *)cmd->outbuf, strlen(cmd->outbuf))) {
			goto end;
		}

		ditem = (struct spdk_memcached_diskitem *)cmd->mobj_read->buf;
		data_buf = memcached_diskitem_get_data(ditem);
		data_len = memcached_diskitem_get_data_len(ditem);
		if (!_iov_ctx_set_iov(&ctx, data_buf, data_len)) {
			goto end;
		}
	}


	/* build iov for cmd general response */
	if (cmd->response[0] != '\0') {
		if (!_iov_ctx_set_iov(&ctx, (uint8_t *)cmd->response, strlen(cmd->response))) {
			goto end;
		}
	}

end:
	if (_mapped_length != NULL) {
		*_mapped_length = ctx.mapped_len;
	}

	return ctx.iovcnt;
}
#endif


uint32_t
spdk_memcached_cmd_get_sendoff(struct spdk_memcached_cmd *cmd)
{
	return cmd->send_off;
}

void
spdk_memcached_cmd_incr_sendoff(struct spdk_memcached_cmd *cmd, uint32_t offset_incr)
{
	cmd->send_off += offset_incr;
}

uint32_t
spdk_memcached_cmd_get_sendlen(struct spdk_memcached_cmd *cmd)
{
	return cmd->send_len;
}

void
spdk_memcached_put_cmd(struct spdk_memcached_cmd *cmd)
{
	if (!cmd) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Put memcached cmd %p\n", cmd);
	cmd->ref--;

	if (cmd->ref < 0) {
		SPDK_ERRLOG("Negative CMD refcount: %p\n", cmd);
		cmd->ref = 0;
	}

	if (cmd->ref == 0) {
		if (cmd->mobj_write) {
			spdk_mempool_put(cmd->mobj_write->mp, (void *)cmd->mobj_write);
			cmd->mobj_write = NULL;
		}

		if (cmd->mobj_read) {
			spdk_mempool_put(cmd->mobj_read->mp, (void *)cmd->mobj_read);
			cmd->mobj_read = NULL;
		}

		/* GET command will set outbut if find the key, and build iov will check outbuf[0] */
		cmd->outbuf[0] = '\0';

		spdk_mempool_put(g_spdk_memcached.cmd_pool, (void *)cmd);
	}
}

struct spdk_memcached_cmd *
spdk_memcached_get_cmd(void)
{
	struct spdk_memcached_cmd *cmd;

	cmd = spdk_mempool_get(g_spdk_memcached.cmd_pool);
	if (!cmd) {
		SPDK_ERRLOG("Unable to get CMD\n");
		abort();
	}

	cmd->ref = 1;
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Get memcached cmd %p\n", cmd);

	return cmd;
}

