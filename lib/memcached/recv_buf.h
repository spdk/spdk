/*
 * recv_buf.h
 *
 *  Created on: Apr 23, 2019
 *      Author: root
 */

#ifndef LIB_MEMCACHED_RECV_BUF_H_
#define LIB_MEMCACHED_RECV_BUF_H_

#define RECV_BUF_LEN	1024
#define END_CHAR		'\n'

/* conn_recv_buf is used to simplify the operation to get the cmd header */

struct spdk_memcached_conn_recv_buf {
	char buf[RECV_BUF_LEN];
	int recv_len; // data received in buffer
	int valid_len; // data valid to current command
};

/* API definitions for receive buffer */

/* Revise is used to copy the remaining data to its head offset
 * It should be called before we start to recv next command.
 */
static inline void memcached_conn_recv_buf_revise(struct spdk_memcached_conn_recv_buf *recv_buf);

/* Contain-end is used to check whether newly received data has '\n' which indicates one command is received.
 * It should be called after received some data.
 */
static inline bool memcached_conn_recv_buf_contain_end(struct spdk_memcached_conn_recv_buf
		*recv_buf);


/* Get-valid-size is used to get the cmd length if one command is fully returned.
 */
static inline int memcached_conn_recv_buf_get_cmd_size(struct spdk_memcached_conn_recv_buf
		*recv_buf);

/* Get-start-addr is used to return the start address of internal buffer.
 */
static inline char *memcached_conn_recv_buf_get_start_addr(struct spdk_memcached_conn_recv_buf
		*recv_buf);


/* Get-recv-addr is used to return the buf+offset, where new received data should be written.
 */
static inline char *memcached_conn_recv_buf_get_recv_addr(struct spdk_memcached_conn_recv_buf
		*recv_buf);

/* Incr-recv-addr is used to increase received length recorded inside recv_buf
 */
static inline void memcached_conn_recv_buf_incr_recv_addr(struct spdk_memcached_conn_recv_buf
		*recv_buf, int recv_size);


/* Extract-data is used to move the data part of "set/add/..."from recv-buf out, and return how much data moved.
 *
 * It should be called before step into data stage.
 * Some front part of cmd data may be received in recv-buf, so they should be copied out to data buffer.
 * Actual moved data is probably less than required.
 */
static inline int memcached_conn_recv_buf_extract_data(struct spdk_memcached_conn_recv_buf
		*recv_buf,
		char *data_buf, int max_data_len);


/* API implementation for receive buffer */
static inline void
memcached_conn_recv_buf_revise(struct spdk_memcached_conn_recv_buf *recv_buf)
{
	char *buf = recv_buf->buf;
	int recv_len = recv_buf->recv_len;
	int new_start = recv_buf->valid_len;
	int remaining_len = recv_len - new_start;

	assert(remaining_len >= 0);
	if (remaining_len > 0) {
		/* Remove END_CHAR left by last text cmd if it has data part */
		while (buf[new_start] == END_CHAR) {
			new_start++;
			if (new_start == recv_len) {
				goto moved;
			}
		}

		remaining_len = recv_len - new_start;
		memmove(buf, &buf[new_start], remaining_len);
	}

moved:
	recv_buf->recv_len = remaining_len;
	recv_buf->valid_len = 0;
}

static inline bool
memcached_conn_recv_buf_contain_end(struct spdk_memcached_conn_recv_buf *recv_buf)
{
	char *buf = recv_buf->buf;
	int recv_len = recv_buf->recv_len;
	int valid_len = recv_buf->valid_len;
	int i;

	for (i = valid_len; i < recv_len; i++) {
		recv_buf->valid_len++;
		if (buf[i] != END_CHAR) {
			continue;
		}

		/* find the ending char */
		return true;
	}

	return false;
}

static inline int
memcached_conn_recv_buf_get_cmd_size(struct spdk_memcached_conn_recv_buf *recv_buf)
{
	return recv_buf->valid_len;
}

static inline char *
memcached_conn_recv_buf_get_start_addr(struct spdk_memcached_conn_recv_buf *recv_buf)
{
	return recv_buf->buf;
}

static inline char *
memcached_conn_recv_buf_get_recv_addr(struct spdk_memcached_conn_recv_buf *recv_buf)
{
	return recv_buf->buf + recv_buf->recv_len;
}

static inline void
memcached_conn_recv_buf_incr_recv_addr(struct spdk_memcached_conn_recv_buf *recv_buf, int recv_size)
{
	recv_buf->recv_len += recv_size;
	assert(recv_buf->recv_len < RECV_BUF_LEN);
}

static inline int
memcached_conn_recv_buf_extract_data(struct spdk_memcached_conn_recv_buf *recv_buf,
				     char *data_buf, int max_data_len)
{
	char *buf = recv_buf->buf;
	int recv_len = recv_buf->recv_len;
	int valid_len = recv_buf->valid_len;
	int remaining_len = recv_len - valid_len;

	assert(remaining_len >= 0);

	remaining_len = (max_data_len < remaining_len) ? max_data_len : remaining_len;
	memmove(data_buf, buf + valid_len, remaining_len);

	recv_buf->valid_len += remaining_len;
	return remaining_len;
}

#endif /* LIB_MEMCACHED_RECV_BUF_H_ */
