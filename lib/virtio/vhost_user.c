/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
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

#include "vhost_user.h"

#include "spdk/string.h"
#include "spdk_internal/vhost_user.h"

/* The version of the protocol we support */
#define VHOST_USER_VERSION    0x1

static int
vhost_user_write(int fd, void *buf, int len, int *fds, int fd_num)
{
	int r;
	struct msghdr msgh;
	struct iovec iov;
	size_t fd_size = fd_num * sizeof(int);
	char control[CMSG_SPACE(fd_size)];
	struct cmsghdr *cmsg;

	memset(&msgh, 0, sizeof(msgh));
	memset(control, 0, sizeof(control));

	iov.iov_base = (uint8_t *)buf;
	iov.iov_len = len;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;

	if (fds && fd_num > 0) {
		msgh.msg_control = control;
		msgh.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&msgh);
		cmsg->cmsg_len = CMSG_LEN(fd_size);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), fds, fd_size);
	} else {
		msgh.msg_control = NULL;
		msgh.msg_controllen = 0;
	}

	do {
		r = sendmsg(fd, &msgh, 0);
	} while (r < 0 && errno == EINTR);

	if (r == -1) {
		return -errno;
	}

	return 0;
}

static int
vhost_user_read(int fd, struct vhost_user_msg *msg)
{
	uint32_t valid_flags = VHOST_USER_REPLY_MASK | VHOST_USER_VERSION;
	ssize_t ret;
	size_t sz_hdr = VHOST_USER_HDR_SIZE, sz_payload;

	ret = recv(fd, (void *)msg, sz_hdr, 0);
	if ((size_t)ret != sz_hdr) {
		SPDK_WARNLOG("Failed to recv msg hdr: %zd instead of %zu.\n",
			     ret, sz_hdr);
		if (ret == -1) {
			return -errno;
		} else {
			return -EBUSY;
		}
	}

	/* validate msg flags */
	if (msg->flags != (valid_flags)) {
		SPDK_WARNLOG("Failed to recv msg: flags %"PRIx32" instead of %"PRIx32".\n",
			     msg->flags, valid_flags);
		return -EIO;
	}

	sz_payload = msg->size;

	if (sz_payload > VHOST_USER_PAYLOAD_SIZE) {
		SPDK_WARNLOG("Received oversized msg: payload size %zu > available space %zu\n",
			     sz_payload, VHOST_USER_PAYLOAD_SIZE);
		return -EIO;
	}

	if (sz_payload) {
		ret = recv(fd, (void *)((char *)msg + sz_hdr), sz_payload, 0);
		if ((size_t)ret != sz_payload) {
			SPDK_WARNLOG("Failed to recv msg payload: %zd instead of %"PRIu32".\n",
				     ret, msg->size);
			if (ret == -1) {
				return -errno;
			} else {
				return -EBUSY;
			}
		}
	}

	return 0;
}

struct hugepage_file_info {
	uint64_t addr;            /**< virtual addr */
	size_t   size;            /**< the file size */
	char     path[PATH_MAX];  /**< path to backing file */
};

/* Two possible options:
 * 1. Match HUGEPAGE_INFO_FMT to find the file storing struct hugepage_file
 * array. This is simple but cannot be used in secondary process because
 * secondary process will close and munmap that file.
 * 2. Match HUGEFILE_FMT to find hugepage files directly.
 *
 * We choose option 2.
 */
static int
get_hugepage_file_info(struct hugepage_file_info huges[], int max)
{
	int idx, rc;
	FILE *f;
	char buf[BUFSIZ], *tmp, *tail;
	char *str_underline, *str_start;
	int huge_index;
	uint64_t v_start, v_end;

	f = fopen("/proc/self/maps", "r");
	if (!f) {
		SPDK_ERRLOG("cannot open /proc/self/maps\n");
		rc = -errno;
		assert(rc < 0); /* scan-build hack */
		return rc;
	}

	idx = 0;
	while (fgets(buf, sizeof(buf), f) != NULL) {
		if (sscanf(buf, "%" PRIx64 "-%" PRIx64, &v_start, &v_end) < 2) {
			SPDK_ERRLOG("Failed to parse address\n");
			rc = -EIO;
			goto out;
		}

		tmp = strchr(buf, ' ') + 1; /** skip address */
		tmp = strchr(tmp, ' ') + 1; /** skip perm */
		tmp = strchr(tmp, ' ') + 1; /** skip offset */
		tmp = strchr(tmp, ' ') + 1; /** skip dev */
		tmp = strchr(tmp, ' ') + 1; /** skip inode */
		while (*tmp == ' ') {       /** skip spaces */
			tmp++;
		}
		tail = strrchr(tmp, '\n');  /** remove newline if exists */
		if (tail) {
			*tail = '\0';
		}

		/* Match HUGEFILE_FMT, aka "%s/%smap_%d",
		 * which is defined in eal_filesystem.h
		 */
		str_underline = strrchr(tmp, '_');
		if (!str_underline) {
			continue;
		}

		str_start = str_underline - strlen("map");
		if (str_start < tmp) {
			continue;
		}

		if (sscanf(str_start, "map_%d", &huge_index) != 1) {
			continue;
		}

		if (idx >= max) {
			SPDK_ERRLOG("Exceed maximum of %d\n", max);
			rc = -ENOSPC;
			goto out;
		}

		if (idx > 0 &&
		    strncmp(tmp, huges[idx - 1].path, PATH_MAX) == 0 &&
		    v_start == huges[idx - 1].addr + huges[idx - 1].size) {
			huges[idx - 1].size += (v_end - v_start);
			continue;
		}

		huges[idx].addr = v_start;
		huges[idx].size = v_end - v_start;
		snprintf(huges[idx].path, PATH_MAX, "%s", tmp);
		idx++;
	}

	rc = idx;
out:
	fclose(f);
	return rc;
}

static int
prepare_vhost_memory_user(struct vhost_user_msg *msg, int fds[])
{
	int i, num;
	struct hugepage_file_info huges[VHOST_USER_MEMORY_MAX_NREGIONS];

	num = get_hugepage_file_info(huges, VHOST_USER_MEMORY_MAX_NREGIONS);
	if (num < 0) {
		SPDK_ERRLOG("Failed to prepare memory for vhost-user\n");
		return num;
	}

	for (i = 0; i < num; ++i) {
		/* the memory regions are unaligned */
		msg->payload.memory.regions[i].guest_phys_addr = huges[i].addr; /* use vaddr! */
		msg->payload.memory.regions[i].userspace_addr = huges[i].addr;
		msg->payload.memory.regions[i].memory_size = huges[i].size;
		msg->payload.memory.regions[i].flags_padding = 0;
		fds[i] = open(huges[i].path, O_RDWR);
	}

	msg->payload.memory.nregions = num;
	msg->payload.memory.padding = 0;

	return 0;
}

static const char *const vhost_msg_strings[VHOST_USER_MAX] = {
	[VHOST_USER_SET_OWNER] = "VHOST_SET_OWNER",
	[VHOST_USER_RESET_OWNER] = "VHOST_RESET_OWNER",
	[VHOST_USER_SET_FEATURES] = "VHOST_SET_FEATURES",
	[VHOST_USER_GET_FEATURES] = "VHOST_GET_FEATURES",
	[VHOST_USER_SET_VRING_CALL] = "VHOST_SET_VRING_CALL",
	[VHOST_USER_GET_PROTOCOL_FEATURES] = "VHOST_USER_GET_PROTOCOL_FEATURES",
	[VHOST_USER_SET_PROTOCOL_FEATURES] = "VHOST_USER_SET_PROTOCOL_FEATURES",
	[VHOST_USER_SET_VRING_NUM] = "VHOST_SET_VRING_NUM",
	[VHOST_USER_SET_VRING_BASE] = "VHOST_SET_VRING_BASE",
	[VHOST_USER_GET_VRING_BASE] = "VHOST_GET_VRING_BASE",
	[VHOST_USER_SET_VRING_ADDR] = "VHOST_SET_VRING_ADDR",
	[VHOST_USER_SET_VRING_KICK] = "VHOST_SET_VRING_KICK",
	[VHOST_USER_SET_MEM_TABLE] = "VHOST_SET_MEM_TABLE",
	[VHOST_USER_SET_VRING_ENABLE] = "VHOST_SET_VRING_ENABLE",
	[VHOST_USER_GET_QUEUE_NUM] = "VHOST_USER_GET_QUEUE_NUM",
	[VHOST_USER_GET_CONFIG] = "VHOST_USER_GET_CONFIG",
	[VHOST_USER_SET_CONFIG] = "VHOST_USER_SET_CONFIG",
};

static int
vhost_user_sock(struct virtio_user_dev *dev,
		enum vhost_user_request req,
		void *arg)
{
	struct vhost_user_msg msg;
	struct vhost_vring_file *file = 0;
	int need_reply = 0;
	int fds[VHOST_USER_MEMORY_MAX_NREGIONS];
	int fd_num = 0;
	int i, len, rc;
	int vhostfd = dev->vhostfd;

	SPDK_DEBUGLOG(virtio_user, "sent message %d = %s\n", req, vhost_msg_strings[req]);

	msg.request = req;
	msg.flags = VHOST_USER_VERSION;
	msg.size = 0;

	switch (req) {
	case VHOST_USER_GET_FEATURES:
	case VHOST_USER_GET_PROTOCOL_FEATURES:
	case VHOST_USER_GET_QUEUE_NUM:
		need_reply = 1;
		break;

	case VHOST_USER_SET_FEATURES:
	case VHOST_USER_SET_LOG_BASE:
	case VHOST_USER_SET_PROTOCOL_FEATURES:
		msg.payload.u64 = *((__u64 *)arg);
		msg.size = sizeof(msg.payload.u64);
		break;

	case VHOST_USER_SET_OWNER:
	case VHOST_USER_RESET_OWNER:
		break;

	case VHOST_USER_SET_MEM_TABLE:
		rc = prepare_vhost_memory_user(&msg, fds);
		if (rc < 0) {
			return rc;
		}
		fd_num = msg.payload.memory.nregions;
		msg.size = sizeof(msg.payload.memory.nregions);
		msg.size += sizeof(msg.payload.memory.padding);
		msg.size += fd_num * sizeof(struct vhost_memory_region);
		break;

	case VHOST_USER_SET_LOG_FD:
		fds[fd_num++] = *((int *)arg);
		break;

	case VHOST_USER_SET_VRING_NUM:
	case VHOST_USER_SET_VRING_BASE:
	case VHOST_USER_SET_VRING_ENABLE:
		memcpy(&msg.payload.state, arg, sizeof(msg.payload.state));
		msg.size = sizeof(msg.payload.state);
		break;

	case VHOST_USER_GET_VRING_BASE:
		memcpy(&msg.payload.state, arg, sizeof(msg.payload.state));
		msg.size = sizeof(msg.payload.state);
		need_reply = 1;
		break;

	case VHOST_USER_SET_VRING_ADDR:
		memcpy(&msg.payload.addr, arg, sizeof(msg.payload.addr));
		msg.size = sizeof(msg.payload.addr);
		break;

	case VHOST_USER_SET_VRING_KICK:
	case VHOST_USER_SET_VRING_CALL:
	case VHOST_USER_SET_VRING_ERR:
		file = arg;
		msg.payload.u64 = file->index & VHOST_USER_VRING_IDX_MASK;
		msg.size = sizeof(msg.payload.u64);
		if (file->fd > 0) {
			fds[fd_num++] = file->fd;
		} else {
			msg.payload.u64 |= VHOST_USER_VRING_NOFD_MASK;
		}
		break;

	case VHOST_USER_GET_CONFIG:
		memcpy(&msg.payload.cfg, arg, sizeof(msg.payload.cfg));
		msg.size = sizeof(msg.payload.cfg);
		need_reply = 1;
		break;

	case VHOST_USER_SET_CONFIG:
		memcpy(&msg.payload.cfg, arg, sizeof(msg.payload.cfg));
		msg.size = sizeof(msg.payload.cfg);
		break;

	default:
		SPDK_ERRLOG("trying to send unknown msg\n");
		return -EINVAL;
	}

	len = VHOST_USER_HDR_SIZE + msg.size;
	rc = vhost_user_write(vhostfd, &msg, len, fds, fd_num);
	if (rc < 0) {
		SPDK_ERRLOG("%s failed: %s\n",
			    vhost_msg_strings[req], spdk_strerror(-rc));
		return rc;
	}

	if (req == VHOST_USER_SET_MEM_TABLE)
		for (i = 0; i < fd_num; ++i) {
			close(fds[i]);
		}

	if (need_reply) {
		rc = vhost_user_read(vhostfd, &msg);
		if (rc < 0) {
			SPDK_WARNLOG("Received msg failed: %s\n", spdk_strerror(-rc));
			return rc;
		}

		if (req != msg.request) {
			SPDK_WARNLOG("Received unexpected msg type\n");
			return -EIO;
		}

		switch (req) {
		case VHOST_USER_GET_FEATURES:
		case VHOST_USER_GET_PROTOCOL_FEATURES:
		case VHOST_USER_GET_QUEUE_NUM:
			if (msg.size != sizeof(msg.payload.u64)) {
				SPDK_WARNLOG("Received bad msg size\n");
				return -EIO;
			}
			*((__u64 *)arg) = msg.payload.u64;
			break;
		case VHOST_USER_GET_VRING_BASE:
			if (msg.size != sizeof(msg.payload.state)) {
				SPDK_WARNLOG("Received bad msg size\n");
				return -EIO;
			}
			memcpy(arg, &msg.payload.state,
			       sizeof(struct vhost_vring_state));
			break;
		case VHOST_USER_GET_CONFIG:
			if (msg.size != sizeof(msg.payload.cfg)) {
				SPDK_WARNLOG("Received bad msg size\n");
				return -EIO;
			}
			memcpy(arg, &msg.payload.cfg, sizeof(msg.payload.cfg));
			break;
		default:
			SPDK_WARNLOG("Received unexpected msg type\n");
			return -EBADMSG;
		}
	}

	return 0;
}

/**
 * Set up environment to talk with a vhost user backend.
 *
 * @return
 *   - (-1) if fail;
 *   - (0) if succeed.
 */
static int
vhost_user_setup(struct virtio_user_dev *dev)
{
	int fd;
	int flag;
	struct sockaddr_un un;
	ssize_t rc;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		SPDK_ERRLOG("socket() error, %s\n", spdk_strerror(errno));
		return -errno;
	}

	flag = fcntl(fd, F_GETFD);
	if (fcntl(fd, F_SETFD, flag | FD_CLOEXEC) < 0) {
		SPDK_ERRLOG("fcntl failed, %s\n", spdk_strerror(errno));
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
		SPDK_ERRLOG("connect error, %s\n", spdk_strerror(errno));
		close(fd);
		return -errno;
	}

	dev->vhostfd = fd;
	return 0;
}

struct virtio_user_backend_ops ops_user = {
	.setup = vhost_user_setup,
	.send_request = vhost_user_sock,
};

SPDK_LOG_REGISTER_COMPONENT(virtio_user)
