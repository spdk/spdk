/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

/* Current ioctl utility is Linux-Specific */
#if defined(__linux__) && defined(SPDK_CONFIG_NVME_IOCTL)

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include "spdk/user_ioctl.h"

/*
 * These are sent over Unix domain socket in the request/response magic fields.
 */
#define IOCTL_REQ_MAGIC		0x58444F4E
#define IOCTL_RESP_MAGIC	0x58464549

#define BLK_IOCTL_MAGIC 0x12
#define NVME_IOCTL_MAGIC 'N'
#define IOCTL_HEAD_SIZE ( sizeof(uint32_t) * 4 )

struct usr_nvme_ioctl_req {
	uint32_t	req_magic;
	uint32_t	ioctl_cmd;
	uint32_t	handle;
	uint32_t	total_len;

	char		*cmd_buf;
	char		*data;
	char		*metadata;
	uint32_t	cmd_len;
	uint32_t	data_len;
	uint32_t	md_len;
};

struct usr_nvme_ioctl_resp {
	uint32_t	resp_magic;
	uint32_t	ioctl_cmd;
	uint32_t	handle;
	uint32_t	total_len;

	/*
	 * If ioctl_ret is 0, that means cmd is executed succesfully;
	 * If (int)ioctl_ret is >0, ioctl_ret represents CQE status;
	 * If (int)ioctl_ret is <0, ioctl_ret means cmd is not executed due to some error.
	 */
	uint32_t	ioctl_ret;

	char		*cmd_buf;
	char		*data;
	char		*metadata;
	uint32_t	cmd_len;
	uint32_t	data_len;
	uint32_t	md_len;
};

/*
 * Construct request and response structures
 * Return <0 if failed, return -errno
 * Return 0 on success.
 */
static int
usr_ioctl_rr_construct(struct usr_nvme_ioctl_req *req, struct usr_nvme_ioctl_resp *resp,
		       uint32_t ioctl_cmd, char *cmd_buf, int sockfd)
{
	// TODO: Construct request and response structures
	return 0;
}

static void
usr_ioctl_rr_destruct(uint32_t ioctl_cmd, struct usr_nvme_ioctl_req *req,
		      struct usr_nvme_ioctl_resp *resp)
{
}

/*
 * Send n bytes insistently
 * Return -1 if failed;
 * Return n if send successfully
 */
static ssize_t
write_n(int fd, const void *buf, size_t n)
{
	ssize_t bytes_written;
	ssize_t offset = 0;

	while (offset < (ssize_t)n) {
		bytes_written = write(fd, buf + offset, n - offset);
		if (bytes_written > 0) {
			offset += bytes_written;
		} else if (bytes_written < 0 && errno == EINTR) {
		} else {
			return -1;
		}
	}

	return offset;
}

/*
 * Receive n bytes insistently
 * Return -1 if failed, errno is set properly;
 * Return n if receive successfully
 */
static ssize_t
read_n(int fd, void *buf, size_t n)
{
	ssize_t bytes_read;
	ssize_t offset = 0;

	while (offset < (ssize_t)n) {
		bytes_read = read(fd, buf + offset, n - offset);
		if (bytes_read > 0) {
			offset += bytes_read;
		} else if (bytes_read < 0 && errno == EINTR) {
		} else if (bytes_read == 0) {
			errno = EIO;
			return -1;
		} else {
			return -1;
		}
	}

	return offset;
}

/*
 * Send ioctl request out
 * Return -1 if failed
 * Return 0 if send successfully
 */
static int
usr_ioctl_xmit(int sock, struct usr_nvme_ioctl_req *req)
{
	int ret;

	ret = write_n(sock, req, IOCTL_HEAD_SIZE);
	if (ret > 0 && req->cmd_len) {
		ret = write_n(sock, req->cmd_buf, req->cmd_len);
	}
	if (ret > 0 && req->data_len) {
		ret = write_n(sock, req->data, req->data_len);
	}
	if (ret > 0 && req->md_len) {
		ret = write_n(sock, req->metadata, req->md_len);
	}

	return (ret > 0 ? 0 : -1);
}

/*
 * Receive ioctl request out
 * Return -1 if failed, errno is set properly;
 * Return 0 if receive successfully
 */
static int
usr_ioctl_recv(int sock, struct usr_nvme_ioctl_resp *resp, struct usr_nvme_ioctl_req *req)
{
	int ret;
	int len_diff;

	ret = read_n(sock, resp, IOCTL_HEAD_SIZE + sizeof(resp->ioctl_ret));
	if (ret < 0) {
		return -1;
	}
	/* ioctl magic check */
	if (resp->resp_magic != IOCTL_RESP_MAGIC) {
		syslog(LOG_WARNING, "resp_mgic check failed. received magic is 0x%x, expected magic is 0x%x\n",
		       resp->resp_magic, IOCTL_RESP_MAGIC);
		errno = EIO;
		return -1;
	}
	/* ioctl_cmd check */
	if (resp->ioctl_cmd != req->ioctl_cmd) {
		syslog(LOG_WARNING, "ioctl_cmd check failed. req is 0x%x, resp is 0x%x\n", req->ioctl_cmd,
		       resp->ioctl_cmd);
		errno = EIO;
		return -1;
	}
	/* ioctl ret check */
	if ((int32_t)resp->ioctl_ret < 0) {
		errno = -resp->ioctl_ret;
		return -1;
	}
	/* total_len check */
	len_diff = resp->total_len - IOCTL_HEAD_SIZE - sizeof(resp->ioctl_ret)
		   - resp->cmd_len - resp->data_len - resp->md_len;
	if (len_diff) {
		syslog(LOG_WARNING, "total_len check failed. difference is %d\n", len_diff);
		errno = EIO;
		return -1;
	}

	if (ret > 0 && resp->cmd_len) {
		ret = read_n(sock, resp->cmd_buf, resp->cmd_len);
	}
	if (ret > 0 && resp->data_len) {
		ret = read_n(sock, resp->data, resp->data_len);
	}
	if (ret > 0 && resp->md_len) {
		ret = read_n(sock, resp->metadata, resp->md_len);
	}

	return (ret > 0 ? 0 : -1);
}

/*
 * User ioctl API
 * Return -1 if failed, errno is set properly;
 * Return 0 on success.
 * Return >0:
 *   for nvme io/amd cmd, if user_ioctl is successfully executed, but ctrlr replied an error.
 *   for NVME_IOCTL_ID, ID will returned.
 */
static int
_user_ioctl(int sockfd, uint32_t ioctl_cmd, char *cmd_buf)
{
	struct usr_nvme_ioctl_resp _resp, *resp = &_resp;
	struct usr_nvme_ioctl_req _req, *req = &_req;
	int ret;
	int ioctl_ret = 0;

	int ioctlfd;
	struct sockaddr_un peer_addr;
	socklen_t addr_len;

	addr_len = sizeof(peer_addr);
	getpeername(sockfd, (struct sockaddr *)&peer_addr, &addr_len);

	if ((ioctlfd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		syslog(LOG_WARNING, "Failed to create Unix Domain Socket, errno is %d\n", errno);
		return -1;
	}

	ret = connect(ioctlfd, (struct sockaddr *)&peer_addr, addr_len);
	if (ret) {
		syslog(LOG_WARNING, "connect error, errno is %d\n", errno);
		/* Treat NOENT as NODEV */
		if (errno == ENOENT) {
			errno = ENODEV;
		}
		return ret;
	}

	memset(req, 0, sizeof(*req));
	memset(resp, 0, sizeof(*resp));

	ret = usr_ioctl_rr_construct(req, resp, ioctl_cmd, cmd_buf, ioctlfd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	} else if (ret > 0) {
		return ret;
	}

	ret = usr_ioctl_xmit(ioctlfd, req);
	if (ret == 0) {
		ret = usr_ioctl_recv(ioctlfd, resp, req);
	}
	close(ioctlfd);

	ioctl_ret = resp->ioctl_ret;
	usr_ioctl_rr_destruct(ioctl_cmd, req, resp);

	/* if no socket or param error, return ioctl_ret */
	return ret ? ret : ioctl_ret;
}

int
user_ioctl(int fd, unsigned long request, ...)
{
	// TODO: add extra argument
	return _user_ioctl(fd, request, NULL);
}

int
user_open(const char *path, int oflag)
{
	int ret;
	int sockfd;
	struct sockaddr_un servaddr;

	if ((sockfd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		syslog(LOG_WARNING, "Failed to create Unix Domain Socket, errno is %d\n", errno);
		return -1;
	}

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sun_family = AF_UNIX;
	memcpy(servaddr.sun_path, path, spdk_min(sizeof(servaddr.sun_path) - 1, strlen(path) + 1));

	ret = connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
	if (ret) {
		syslog(LOG_WARNING, "Failed to connect %s, errno is %d\n", path, errno);
		return -1;
	}

	return sockfd;
}
__attribute__((constructor)) static void user_ioctl_log_open(void)
{
	openlog("user_ioctl", LOG_PID, LOG_USER);
}

__attribute__((destructor)) static void user_ioctl_log_close(void)
{
	closelog();
}

#endif
