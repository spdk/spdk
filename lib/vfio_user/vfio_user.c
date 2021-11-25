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

/*
 * vfio-user client socket messages.
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/vfio_user_spec.h"

#include "vfio_user_internal.h"

struct vfio_user_request {
	struct vfio_user_header hdr;
#define VFIO_USER_MAX_PAYLOAD_SIZE	(4096)
	uint8_t payload[VFIO_USER_MAX_PAYLOAD_SIZE];
	int fds[VFIO_MAXIMUM_SPARSE_MMAP_REGIONS];
	int fd_num;
};

#ifdef DEBUG
static const char *vfio_user_message_str[VFIO_USER_MAX] = {
	[VFIO_USER_VERSION]			= "VFIO_USER_VERSION",
	[VFIO_USER_DMA_MAP]			= "VFIO_USER_DMA_MAP",
	[VFIO_USER_DMA_UNMAP]			= "VFIO_USER_DMA_UNMAP",
	[VFIO_USER_DEVICE_GET_INFO]		= "VFIO_USER_DEVICE_GET_INFO",
	[VFIO_USER_DEVICE_GET_REGION_INFO]	= "VFIO_USER_DEVICE_GET_REGION_INFO",
	[VFIO_USER_DEVICE_GET_IRQ_INFO]		= "VFIO_USER_DEVICE_GET_IRQ_INFO",
	[VFIO_USER_DEVICE_SET_IRQS]		= "VFIO_USER_DEVICE_SET_IRQS",
	[VFIO_USER_REGION_READ]			= "VFIO_USER_REGION_READ",
	[VFIO_USER_REGION_WRITE]		= "VFIO_USER_REGION_WRITE",
	[VFIO_USER_DMA_READ]			= "VFIO_USER_DMA_READ",
	[VFIO_USER_DMA_WRITE]			= "VFIO_USER_DMA_WRITE",
	[VFIO_USER_DEVICE_RESET]		= "VFIO_USER_DEVICE_RESET",
};
#endif

static int
vfio_user_write(int fd, void *buf, int len, int *fds, int num_fds)
{
	int r;
	struct msghdr msgh;
	struct iovec iov;
	size_t fd_size = num_fds * sizeof(int);
	char control[CMSG_SPACE(VFIO_MAXIMUM_SPARSE_MMAP_REGIONS * sizeof(int))];
	struct cmsghdr *cmsg;

	memset(&msgh, 0, sizeof(msgh));
	memset(control, 0, sizeof(control));

	iov.iov_base = (uint8_t *)buf;
	iov.iov_len = len;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;

	assert(num_fds <= VFIO_MAXIMUM_SPARSE_MMAP_REGIONS);

	if (fds && num_fds) {
		msgh.msg_control = control;
		msgh.msg_controllen = CMSG_SPACE(fd_size);
		cmsg = CMSG_FIRSTHDR(&msgh);
		assert(cmsg != NULL);
		cmsg->cmsg_len = CMSG_LEN(fd_size);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), fds, fd_size);
	} else {
		msgh.msg_control = NULL;
		msgh.msg_controllen = 0;
	}

	do {
		r = sendmsg(fd, &msgh, MSG_NOSIGNAL);
	} while (r < 0 && errno == EINTR);

	if (r == -1) {
		return -errno;
	}

	return 0;
}

static int
read_fd_message(int sockfd, char *buf, int buflen, int *fds, int *fd_num)
{
	struct iovec iov;
	struct msghdr msgh;
	char control[CMSG_SPACE(VFIO_MAXIMUM_SPARSE_MMAP_REGIONS * sizeof(int))];
	struct cmsghdr *cmsg;
	int got_fds = 0;
	int ret;

	memset(&msgh, 0, sizeof(msgh));
	iov.iov_base = buf;
	iov.iov_len  = buflen;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = control;
	msgh.msg_controllen = sizeof(control);

	ret = recvmsg(sockfd, &msgh, 0);
	if (ret <= 0) {
		return ret;
	}

	if (msgh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		return -ENOTSUP;
	}

	for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg != NULL;
	     cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
		if ((cmsg->cmsg_level == SOL_SOCKET) &&
		    (cmsg->cmsg_type == SCM_RIGHTS)) {
			got_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			*fd_num = got_fds;
			assert(got_fds <= VFIO_MAXIMUM_SPARSE_MMAP_REGIONS);
			memcpy(fds, CMSG_DATA(cmsg), got_fds * sizeof(int));
			break;
		}
	}

	return ret;
}

static int
vfio_user_read(int fd, struct vfio_user_request *req)
{
	int ret;
	size_t sz_payload;

	ret = read_fd_message(fd, (char *)req, sizeof(struct vfio_user_header), req->fds, &req->fd_num);
	if (ret <= 0) {
		return ret;
	}

	if (req->hdr.flags.error) {
		SPDK_ERRLOG("Command %u return failure\n", req->hdr.cmd);
		errno = req->hdr.error_no;
		return -EFAULT;
	}

	if (req->hdr.msg_size > sizeof(struct vfio_user_header)) {
		sz_payload = req->hdr.msg_size - sizeof(struct vfio_user_header);
		ret = read(fd, req->payload, sz_payload);
		if (ret <= 0) {
			return ret;
		}
	}

	return 0;
}

static int
vfio_user_dev_send_request(struct vfio_device *dev, enum vfio_user_command command,
			   void *arg, size_t arg_len, size_t buf_len, int *fds, int max_fds)
{
	struct vfio_user_request req = {};
	size_t sz_payload;
	int ret;
	bool fds_write = false;

	if (arg_len > VFIO_USER_MAX_PAYLOAD_SIZE) {
		SPDK_ERRLOG("Oversized argument length, command %u\n", command);
		return -EINVAL;
	}

	req.hdr.cmd = command;
	req.hdr.msg_size = sizeof(struct vfio_user_header) + arg_len;
	memcpy(req.payload, arg, arg_len);

	if (command == VFIO_USER_DMA_MAP || command == VFIO_USER_DMA_UNMAP) {
		fds_write = true;
	}

	SPDK_DEBUGLOG(vfio_user, "[I] Command %s, msg size %u, fds %p, max_fds %d\n",
		      vfio_user_message_str[command], req.hdr.msg_size, fds, max_fds);

	if (fds_write && fds) {
		ret = vfio_user_write(dev->fd, (void *)&req, req.hdr.msg_size, fds, max_fds);
	} else {
		ret = vfio_user_write(dev->fd, (void *)&req, req.hdr.msg_size, NULL, 0);
	}

	if (ret) {
		return ret;
	}

	/* a reply is mandatory */
	memset(&req, 0, sizeof(req));
	ret = vfio_user_read(dev->fd, &req);
	if (ret) {
		return ret;
	}

	SPDK_DEBUGLOG(vfio_user, "[I] Command %s response, msg size %u\n",
		      vfio_user_message_str[req.hdr.cmd], req.hdr.msg_size);

	assert(req.hdr.flags.type == VFIO_USER_MESSAGE_REPLY);
	sz_payload = req.hdr.msg_size - sizeof(struct vfio_user_header);
	if (!sz_payload) {
		return 0;
	}

	if (!fds_write) {
		if (sz_payload > buf_len) {
			SPDK_ERRLOG("Payload size error sz %zd, buf_len %zd\n", sz_payload, buf_len);
			return -EIO;
		}
		memcpy(arg, req.payload, sz_payload);
		/* VFIO_USER_DEVICE_GET_REGION_INFO may contains BAR fd */
		if (fds && req.fd_num) {
			assert(req.fd_num < max_fds);
			memcpy(fds, req.fds, sizeof(int) * req.fd_num);
		}
	}

	return 0;
}

static int
vfio_user_check_version(struct vfio_device *dev)
{
	int ret;
	struct vfio_user_request req = {};
	struct vfio_user_version *version = (struct vfio_user_version *)req.payload;

	version->major = VFIO_USER_MAJOR_VER;
	version->minor = VFIO_USER_MINOR_VER;

	ret = vfio_user_dev_send_request(dev, VFIO_USER_VERSION, req.payload,
					 sizeof(struct vfio_user_version), sizeof(req.payload), NULL, 0);
	if (ret < 0) {
		return ret;
	}

	SPDK_DEBUGLOG(vfio_user, "%s Negotiate version %u.%u\n", vfio_user_message_str[VFIO_USER_VERSION],
		      version->major, version->minor);

	return 0;
}

int
vfio_user_get_dev_region_info(struct vfio_device *dev, struct vfio_region_info *region_info,
			      size_t buf_len, int *fds, int num_fds)
{
	assert(buf_len > sizeof(struct vfio_region_info));
	region_info->argsz = buf_len - sizeof(struct vfio_region_info);
	return vfio_user_dev_send_request(dev, VFIO_USER_DEVICE_GET_REGION_INFO,
					  region_info, region_info->argsz, buf_len, fds, num_fds);
}

int
vfio_user_get_dev_info(struct vfio_device *dev, struct vfio_user_device_info *dev_info,
		       size_t buf_len)
{
	dev_info->argsz = sizeof(struct vfio_user_device_info);
	return vfio_user_dev_send_request(dev, VFIO_USER_DEVICE_GET_INFO,
					  dev_info, dev_info->argsz, buf_len, NULL, 0);
}

int
vfio_user_dev_dma_map_unmap(struct vfio_device *dev, struct vfio_memory_region *mr, bool map)
{
	struct vfio_user_dma_map dma_map = { 0 };
	struct vfio_user_dma_unmap dma_unmap = { 0 };

	if (map) {
		dma_map.argsz = sizeof(struct vfio_user_dma_map);
		dma_map.addr = mr->iova;
		dma_map.size = mr->size;
		dma_map.offset = mr->offset;
		dma_map.flags = VFIO_USER_F_DMA_REGION_READ | VFIO_USER_F_DMA_REGION_WRITE;

		return vfio_user_dev_send_request(dev, VFIO_USER_DMA_MAP,
						  &dma_map, sizeof(dma_map), sizeof(dma_map), &mr->fd, 1);
	} else {
		dma_unmap.argsz = sizeof(struct vfio_user_dma_unmap);
		dma_unmap.addr = mr->iova;
		dma_unmap.size = mr->size;
		return vfio_user_dev_send_request(dev, VFIO_USER_DMA_UNMAP,
						  &dma_unmap, sizeof(dma_unmap), sizeof(dma_unmap), &mr->fd, 1);
	}
}

int
vfio_user_dev_mmio_access(struct vfio_device *dev, uint32_t index, uint64_t offset,
			  size_t len, void *buf, bool is_write)
{
	struct vfio_user_region_access *access;
	size_t arg_len;
	int ret;

	arg_len = sizeof(*access) + len;
	access = calloc(1, arg_len);
	if (!access) {
		return -ENOMEM;
	}

	access->offset = offset;
	access->region = index;
	access->count = len;
	if (is_write) {
		memcpy(access->data, buf, len);
		ret = vfio_user_dev_send_request(dev, VFIO_USER_REGION_WRITE,
						 access, arg_len, arg_len, NULL, 0);
	} else {
		ret = vfio_user_dev_send_request(dev, VFIO_USER_REGION_READ,
						 access, sizeof(*access), arg_len, NULL, 0);
	}

	if (ret) {
		free(access);
		return ret;
	}

	if (!is_write) {
		memcpy(buf, (void *)access->data, len);
	}

	free(access);
	return 0;
}

int
vfio_user_dev_setup(struct vfio_device *dev)
{
	int fd;
	int flag;
	struct sockaddr_un un;
	ssize_t rc;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		SPDK_ERRLOG("socket() error\n");
		return -errno;
	}

	flag = fcntl(fd, F_GETFD);
	if (fcntl(fd, F_SETFD, flag | FD_CLOEXEC) < 0) {
		SPDK_ERRLOG("fcntl failed\n");
	}

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	rc = snprintf(un.sun_path, sizeof(un.sun_path), "%s", dev->path);
	if (rc < 0 || (size_t)rc >= sizeof(un.sun_path)) {
		SPDK_ERRLOG("socket path too long\n");
		close(fd);
		if (rc < 0) {
			return -errno;
		} else {
			return -EINVAL;
		}
	}
	if (connect(fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
		SPDK_ERRLOG("connect error\n");
		close(fd);
		return -errno;
	}

	dev->fd = fd;

	if (vfio_user_check_version(dev)) {
		SPDK_ERRLOG("Check VFIO_USER_VERSION message failed\n");
		close(fd);
		return -EFAULT;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(vfio_user)
