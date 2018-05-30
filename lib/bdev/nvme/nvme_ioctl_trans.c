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
#include "spdk/util.h"
#include "spdk_internal/log.h"
#include "bdev_nvme.h"

#if defined(__linux__) && defined(SPDK_CONFIG_IOCTL)

static void spdk_nvme_ioctl_io_free(struct spdk_nvme_ioctl_conn *ioctl_conn);

static int64_t
read_from_socket(int fd, void *buf, size_t length)
{
	ssize_t bytes_read;

	bytes_read = read(fd, buf, length);
	if (bytes_read == 0) {
		return -EIO;
	} else if (bytes_read == -1) {
		if (errno != EAGAIN) {
			return -errno;
		}
		return 0;
	} else {
		return bytes_read;
	}
}

static int64_t
write_to_socket(int fd, void *buf, size_t length)
{
	ssize_t bytes_written;

	bytes_written = write(fd, buf, length);
	if (bytes_written == 0) {
		return -EIO;
	} else if (bytes_written == -1) {
		if (errno != EAGAIN) {
			return -errno;
		}
		return 0;
	} else {
		return bytes_written;
	}
}

static int
spdk_nvme_ioctl_recv_internal(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ioctl_req *req = &ioctl_conn->req;
	int connfd;
	void *buf;
	size_t len;
	int ret = 0;

	connfd = ioctl_conn->connfd;

	if (ioctl_conn->state == IOCTL_CONN_STATE_RECV_HEAD) {
		buf = (char *)req + ioctl_conn->offset;
		len = IOCTL_HEAD_SIZE - ioctl_conn->offset;
		ret = read_from_socket(connfd, buf, len);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == IOCTL_HEAD_SIZE) {
			ioctl_conn->offset = 0;

			if (req->req_magic != IOCTL_REQ_MAGIC) {
				SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Bad request magic 0x%x (0x%x is required).\n",
					      req->req_magic, IOCTL_REQ_MAGIC);
				return -EINVAL;
			}

			ret = nvme_ioctl_cmd_recv_check(req, &ioctl_conn->state);
			if (ret < 0) {
				return ret;
			}
			if (ioctl_conn->state == IOCTL_CONN_STATE_PROC) {
				ret = spdk_nvme_ioctl_proc(ioctl_conn);
				return ret;
			}
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_RECV_CMD) {
		buf = (char *)req->cmd_buf + ioctl_conn->offset;
		len = req->cmd_len - ioctl_conn->offset;
		ret = read_from_socket(connfd, buf, len);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == req->cmd_len) {
			ioctl_conn->offset = 0;

			ret = nvme_ioctl_cmdbuf_recv_check(req, &ioctl_conn->state, ioctl_conn);
			if (ret < 0) {
				return ret;
			}
			if (ioctl_conn->state == IOCTL_CONN_STATE_PROC) {
				ret = spdk_nvme_ioctl_proc(ioctl_conn);
				return ret;
			}
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_RECV_DATA) {
		buf = (char *)req->data + ioctl_conn->offset;
		len = req->data_len - ioctl_conn->offset;
		ret = read_from_socket(connfd, buf, len);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == req->data_len) {
			ioctl_conn->offset = 0;

			if (req->md_len) {
				ioctl_conn->state = IOCTL_CONN_STATE_RECV_METADATA;
			} else {
				ioctl_conn->state = IOCTL_CONN_STATE_PROC;
				ret = spdk_nvme_ioctl_proc(ioctl_conn);
				return ret;
			}
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_RECV_METADATA) {
		buf = (char *)req->metadata + ioctl_conn->offset;
		len = req->md_len - ioctl_conn->offset;
		ret = read_from_socket(connfd, buf, len);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == req->md_len) {
			ioctl_conn->offset = 0;

			ioctl_conn->state = IOCTL_CONN_STATE_PROC;
			ret = spdk_nvme_ioctl_proc(ioctl_conn);
			return ret;
		}
	}

	return 0;
}

static int
spdk_nvme_ioctl_xmit_internal(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct spdk_nvme_ioctl_resp *resp = &ioctl_conn->resp;
	int connfd;
	int ret = 0;

	connfd = ioctl_conn->connfd;

	if (ioctl_conn->state == IOCTL_CONN_STATE_XMIT_HEAD) {
		ret = write_to_socket(connfd, (char *)resp + ioctl_conn->offset,
				      IOCTL_HEAD_SIZE - ioctl_conn->offset);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == IOCTL_HEAD_SIZE) {
			ioctl_conn->offset = 0;

			ioctl_conn->state = IOCTL_CONN_STATE_XMIT_RET;
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_XMIT_RET) {
		ret = write_to_socket(connfd, &resp->ioctl_ret + ioctl_conn->offset,
				      sizeof(resp->ioctl_ret) - ioctl_conn->offset);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == sizeof(resp->ioctl_ret)) {
			ioctl_conn->offset = 0;

			ioctl_conn->state = IOCTL_CONN_STATE_XMIT_CMD;
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_XMIT_CMD) {
		ret = write_to_socket(connfd, resp->cmd_buf + ioctl_conn->offset,
				      resp->cmd_len - ioctl_conn->offset);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == resp->cmd_len) {
			ioctl_conn->offset = 0;

			ioctl_conn->state = IOCTL_CONN_STATE_XMIT_DATA;
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_XMIT_DATA) {
		ret = write_to_socket(connfd, resp->data + ioctl_conn->offset,
				      resp->data_len - ioctl_conn->offset);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == resp->data_len) {
			ioctl_conn->offset = 0;

			ioctl_conn->state = IOCTL_CONN_STATE_XMIT_METADATA;
		}
	}

	if (ioctl_conn->state == IOCTL_CONN_STATE_XMIT_METADATA) {
		ret = write_to_socket(connfd, resp->metadata + ioctl_conn->offset,
				      resp->md_len - ioctl_conn->offset);
		if (ret < 0) {
			return ret;
		}

		ioctl_conn->offset += ret;
		if (ioctl_conn->offset == resp->md_len) {
			ioctl_conn->offset = 0;
			ioctl_conn->state = IOCTL_CONN_STATE_RECV_HEAD;
			spdk_nvme_ioctl_io_free(ioctl_conn);
			return 0;
		}
	}

	return 0;
}

int
spdk_nvme_ioctl_conn_recv(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	int ret;

	do {
		ret = spdk_nvme_ioctl_recv_internal(ioctl_conn);
	} while (ret > 0);

	return ret;
}

int
spdk_nvme_ioctl_conn_xmit(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	int ret;

	do {
		ret = spdk_nvme_ioctl_xmit_internal(ioctl_conn);
	} while (ret > 0);

	return ret;
}

/* free buffers and assign values inside resp and req */
static void
spdk_nvme_ioctl_io_free(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	// TODO: free and reset each pointer element in req/resp
}

void
spdk_nvme_ioctl_conn_free(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	/* delay ioctl_conn's free, since it may be still under processing asynchronously */
	if (ioctl_conn->state == IOCTL_CONN_STATE_PROC) {
		ioctl_conn->state = IOCTL_CONN_STATE_CLOSE;
		return;
	}

	spdk_nvme_ioctl_io_free(ioctl_conn);
	free(ioctl_conn);
}

#else

int
spdk_nvme_ioctl_conn_recv(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	return 0;
}

int
spdk_nvme_ioctl_conn_xmit(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	return 0;
}

void
spdk_nvme_ioctl_conn_free(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
}

#endif
