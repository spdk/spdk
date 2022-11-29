/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include <sys/eventfd.h>

#include "spdk/string.h"
#include "spdk/config.h"
#include "spdk/util.h"

#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

/* The version of the protocol we support */
#define VHOST_USER_VERSION    0x1

#define VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES \
	((1ULL << VHOST_USER_PROTOCOL_F_MQ) | \
	(1ULL << VHOST_USER_PROTOCOL_F_CONFIG))

struct virtio_user_dev {
	int		vhostfd;

	int		callfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
	int		kickfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
	uint32_t	queue_size;

	uint8_t		status;
	bool		is_stopping;
	char		path[PATH_MAX];
	uint64_t	protocol_features;
	struct vring	vrings[SPDK_VIRTIO_MAX_VIRTQUEUES];
	struct spdk_mem_map *mem_map;
};

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
		if (!cmsg) {
			SPDK_WARNLOG("First HDR is NULL\n");
			return -EIO;
		}
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
get_hugepage_file_info(struct hugepage_file_info hugepages[], int max)
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
		    strncmp(tmp, hugepages[idx - 1].path, PATH_MAX) == 0 &&
		    v_start == hugepages[idx - 1].addr + hugepages[idx - 1].size) {
			hugepages[idx - 1].size += (v_end - v_start);
			continue;
		}

		hugepages[idx].addr = v_start;
		hugepages[idx].size = v_end - v_start;
		snprintf(hugepages[idx].path, PATH_MAX, "%s", tmp);
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
	struct hugepage_file_info hugepages[VHOST_USER_MEMORY_MAX_NREGIONS];

	num = get_hugepage_file_info(hugepages, VHOST_USER_MEMORY_MAX_NREGIONS);
	if (num < 0) {
		SPDK_ERRLOG("Failed to prepare memory for vhost-user\n");
		return num;
	}

	for (i = 0; i < num; ++i) {
		/* the memory regions are unaligned */
		msg->payload.memory.regions[i].guest_phys_addr = hugepages[i].addr; /* use vaddr! */
		msg->payload.memory.regions[i].userspace_addr = hugepages[i].addr;
		msg->payload.memory.regions[i].memory_size = hugepages[i].size;
		msg->payload.memory.regions[i].flags_padding = 0;
		fds[i] = open(hugepages[i].path, O_RDWR);
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

static int
virtio_user_create_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Of all per virtqueue MSGs, make sure VHOST_SET_VRING_CALL come
	 * firstly because vhost depends on this msg to allocate virtqueue
	 * pair.
	 */
	struct vhost_vring_file file;

	file.index = queue_sel;
	file.fd = dev->callfds[queue_sel];
	return vhost_user_sock(dev, VHOST_USER_SET_VRING_CALL, &file);
}

static int
virtio_user_set_vring_addr(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vring *vring = &dev->vrings[queue_sel];
	struct vhost_vring_addr addr = {
		.index = queue_sel,
		.desc_user_addr = (uint64_t)(uintptr_t)vring->desc,
		.avail_user_addr = (uint64_t)(uintptr_t)vring->avail,
		.used_user_addr = (uint64_t)(uintptr_t)vring->used,
		.log_guest_addr = 0,
		.flags = 0, /* disable log */
	};

	return vhost_user_sock(dev, VHOST_USER_SET_VRING_ADDR, &addr);
}

static int
virtio_user_kick_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_file file;
	struct vhost_vring_state state;
	struct vring *vring = &dev->vrings[queue_sel];
	int rc;

	state.index = queue_sel;
	state.num = vring->num;
	rc = vhost_user_sock(dev, VHOST_USER_SET_VRING_NUM, &state);
	if (rc < 0) {
		return rc;
	}

	state.index = queue_sel;
	state.num = 0; /* no reservation */
	rc = vhost_user_sock(dev, VHOST_USER_SET_VRING_BASE, &state);
	if (rc < 0) {
		return rc;
	}

	virtio_user_set_vring_addr(vdev, queue_sel);

	/* Of all per virtqueue MSGs, make sure VHOST_USER_SET_VRING_KICK comes
	 * lastly because vhost depends on this msg to judge if
	 * virtio is ready.
	 */
	file.index = queue_sel;
	file.fd = dev->kickfds[queue_sel];
	return vhost_user_sock(dev, VHOST_USER_SET_VRING_KICK, &file);
}

static int
virtio_user_stop_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;

	state.index = queue_sel;
	state.num = 0;

	return vhost_user_sock(dev, VHOST_USER_GET_VRING_BASE, &state);
}

static int
virtio_user_queue_setup(struct virtio_dev *vdev,
			int (*fn)(struct virtio_dev *, uint32_t))
{
	uint32_t i;
	int rc;

	for (i = 0; i < vdev->max_queues; ++i) {
		rc = fn(vdev, i);
		if (rc < 0) {
			SPDK_ERRLOG("setup tx vq fails: %"PRIu32".\n", i);
			return rc;
		}
	}

	return 0;
}

static int
virtio_user_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		       enum spdk_mem_map_notify_action action,
		       void *vaddr, size_t size)
{
	struct virtio_dev *vdev = cb_ctx;
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int ret;

	/* We do not support dynamic memory allocation with virtio-user.  If this is the
	 * initial notification when the device is started, dev->mem_map will be NULL.  If
	 * this is the final notification when the device is stopped, dev->is_stopping will
	 * be true.  All other cases are unsupported.
	 */
	if (dev->mem_map != NULL && !dev->is_stopping) {
		assert(false);
		SPDK_ERRLOG("Memory map change with active virtio_user_devs not allowed.\n");
		SPDK_ERRLOG("Pre-allocate memory for application using -s (mem_size) option.\n");
		return -1;
	}

	/* We have to resend all mappings anyway, so don't bother with any
	 * page tracking.
	 */
	ret = vhost_user_sock(dev, VHOST_USER_SET_MEM_TABLE, NULL);
	if (ret < 0) {
		return ret;
	}

	/* Since we might want to use that mapping straight away, we have to
	 * make sure the guest has already processed our SET_MEM_TABLE message.
	 * F_REPLY_ACK is just a feature and the host is not obliged to
	 * support it, so we send a simple message that always has a response
	 * and we wait for that response. Messages are always processed in order.
	 */
	return vhost_user_sock(dev, VHOST_USER_GET_FEATURES, &features);
}

static int
virtio_user_register_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	const struct spdk_mem_map_ops virtio_user_map_ops = {
		.notify_cb = virtio_user_map_notify,
		.are_contiguous = NULL
	};

	dev->mem_map = spdk_mem_map_alloc(0, &virtio_user_map_ops, vdev);
	if (dev->mem_map == NULL) {
		SPDK_ERRLOG("spdk_mem_map_alloc() failed\n");
		return -1;
	}

	return 0;
}

static void
virtio_user_unregister_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	dev->is_stopping = true;
	spdk_mem_map_free(&dev->mem_map);
}

static int
virtio_user_start_device(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t host_max_queues;
	int ret;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ)) == 0 &&
	    vdev->max_queues > 1 + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues, but the "
			     "host doesn't support VHOST_USER_PROTOCOL_F_MQ. "
			     "Only one request queue will be used.\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num);
		vdev->max_queues = 1 + vdev->fixed_queues_num;
	}

	/* negotiate the number of I/O queues. */
	ret = vhost_user_sock(dev, VHOST_USER_GET_QUEUE_NUM, &host_max_queues);
	if (ret < 0) {
		return ret;
	}

	if (vdev->max_queues > host_max_queues + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues"
			     "but only %"PRIu64" available\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num,
			     host_max_queues);
		vdev->max_queues = host_max_queues;
	}

	/* tell vhost to create queues */
	ret = virtio_user_queue_setup(vdev, virtio_user_create_queue);
	if (ret < 0) {
		return ret;
	}

	ret = virtio_user_register_mem(vdev);
	if (ret < 0) {
		return ret;
	}

	return virtio_user_queue_setup(vdev, virtio_user_kick_queue);
}

static int
virtio_user_stop_device(struct virtio_dev *vdev)
{
	int ret;

	ret = virtio_user_queue_setup(vdev, virtio_user_stop_queue);
	/* a queue might fail to stop for various reasons, e.g. socket
	 * connection going down, but this mustn't prevent us from freeing
	 * the mem map.
	 */
	virtio_user_unregister_mem(vdev);
	return ret;
}

static int
virtio_user_dev_setup(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint16_t i;

	dev->vhostfd = -1;

	for (i = 0; i < SPDK_VIRTIO_MAX_VIRTQUEUES; ++i) {
		dev->callfds[i] = -1;
		dev->kickfds[i] = -1;
	}

	return vhost_user_setup(dev);
}

static int
virtio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
			    void *dst, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};
	int rc;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
		return -ENOTSUP;
	}

	cfg.offset = 0;
	cfg.size = VHOST_USER_MAX_CONFIG_SIZE;

	rc = vhost_user_sock(dev, VHOST_USER_GET_CONFIG, &cfg);
	if (rc < 0) {
		SPDK_ERRLOG("get_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	memcpy(dst, cfg.region + offset, length);
	return 0;
}

static int
virtio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
			     const void *src, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};
	int rc;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
		return -ENOTSUP;
	}

	cfg.offset = offset;
	cfg.size = length;
	memcpy(cfg.region, src, length);

	rc = vhost_user_sock(dev, VHOST_USER_SET_CONFIG, &cfg);
	if (rc < 0) {
		SPDK_ERRLOG("set_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static void
virtio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int rc = 0;

	if ((dev->status & VIRTIO_CONFIG_S_NEEDS_RESET) &&
	    status != VIRTIO_CONFIG_S_RESET) {
		rc = -1;
	} else if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		rc = virtio_user_start_device(vdev);
	} else if (status == VIRTIO_CONFIG_S_RESET &&
		   (dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		rc = virtio_user_stop_device(vdev);
	}

	if (rc != 0) {
		dev->status |= VIRTIO_CONFIG_S_NEEDS_RESET;
	} else {
		dev->status = status;
	}
}

static uint8_t
virtio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	return dev->status;
}

static uint64_t
virtio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int rc;

	rc = vhost_user_sock(dev, VHOST_USER_GET_FEATURES, &features);
	if (rc < 0) {
		SPDK_ERRLOG("get_features failed: %s\n", spdk_strerror(-rc));
		return 0;
	}

	return features;
}

static int
virtio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t protocol_features;
	int ret;

	ret = vhost_user_sock(dev, VHOST_USER_SET_FEATURES, &features);
	if (ret < 0) {
		return ret;
	}

	vdev->negotiated_features = features;
	vdev->modern = virtio_dev_has_feature(vdev, VIRTIO_F_VERSION_1);

	if (!virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
		/* nothing else to do */
		return 0;
	}

	ret = vhost_user_sock(dev, VHOST_USER_GET_PROTOCOL_FEATURES, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	protocol_features &= VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES;
	ret = vhost_user_sock(dev, VHOST_USER_SET_PROTOCOL_FEATURES, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	dev->protocol_features = protocol_features;
	return 0;
}

static uint16_t
virtio_user_get_queue_size(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Currently each queue has same queue size */
	return dev->queue_size;
}

static int
virtio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;
	uint16_t queue_idx = vq->vq_queue_index;
	void *queue_mem;
	uint64_t desc_addr, avail_addr, used_addr;
	int callfd, kickfd, rc;

	if (dev->callfds[queue_idx] != -1 || dev->kickfds[queue_idx] != -1) {
		SPDK_ERRLOG("queue %"PRIu16" already exists\n", queue_idx);
		return -EEXIST;
	}

	/* May use invalid flag, but some backend uses kickfd and
	 * callfd as criteria to judge if dev is alive. so finally we
	 * use real event_fd.
	 */
	callfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (callfd < 0) {
		SPDK_ERRLOG("callfd error, %s\n", spdk_strerror(errno));
		return -errno;
	}

	kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (kickfd < 0) {
		SPDK_ERRLOG("kickfd error, %s\n", spdk_strerror(errno));
		close(callfd);
		return -errno;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VIRTIO_PCI_VRING_ALIGN, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (queue_mem == NULL) {
		close(kickfd);
		close(callfd);
		return -ENOMEM;
	}

	vq->vq_ring_mem = SPDK_VTOPHYS_ERROR;
	vq->vq_ring_virt_mem = queue_mem;

	state.index = vq->vq_queue_index;
	state.num = vq->vq_nentries;

	if (virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
		rc = vhost_user_sock(dev, VHOST_USER_SET_VRING_ENABLE, &state);
		if (rc < 0) {
			SPDK_ERRLOG("failed to send VHOST_USER_SET_VRING_ENABLE: %s\n",
				    spdk_strerror(-rc));
			close(kickfd);
			close(callfd);
			spdk_free(queue_mem);
			return -rc;
		}
	}

	dev->callfds[queue_idx] = callfd;
	dev->kickfds[queue_idx] = kickfd;

	desc_addr = (uintptr_t)vq->vq_ring_virt_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = SPDK_ALIGN_CEIL(avail_addr + offsetof(struct vring_avail,
				    ring[vq->vq_nentries]),
				    VIRTIO_PCI_VRING_ALIGN);

	dev->vrings[queue_idx].num = vq->vq_nentries;
	dev->vrings[queue_idx].desc = (void *)(uintptr_t)desc_addr;
	dev->vrings[queue_idx].avail = (void *)(uintptr_t)avail_addr;
	dev->vrings[queue_idx].used = (void *)(uintptr_t)used_addr;

	return 0;
}

static void
virtio_user_del_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	/* For legacy devices, write 0 to VIRTIO_PCI_QUEUE_PFN port, QEMU
	 * correspondingly stops the ioeventfds, and reset the status of
	 * the device.
	 * For modern devices, set queue desc, avail, used in PCI bar to 0,
	 * not see any more behavior in QEMU.
	 *
	 * Here we just care about what information to deliver to vhost-user.
	 * So we just close ioeventfd for now.
	 */
	struct virtio_user_dev *dev = vdev->ctx;

	close(dev->callfds[vq->vq_queue_index]);
	close(dev->kickfds[vq->vq_queue_index]);
	dev->callfds[vq->vq_queue_index] = -1;
	dev->kickfds[vq->vq_queue_index] = -1;

	spdk_free(vq->vq_ring_virt_mem);
}

static void
virtio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	uint64_t buf = 1;
	struct virtio_user_dev *dev = vdev->ctx;

	if (write(dev->kickfds[vq->vq_queue_index], &buf, sizeof(buf)) < 0) {
		SPDK_ERRLOG("failed to kick backend: %s.\n", spdk_strerror(errno));
	}
}

static void
virtio_user_destroy(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	if (dev) {
		close(dev->vhostfd);
		free(dev);
	}
}

static void
virtio_user_dump_json_info(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "type", "user");
	spdk_json_write_named_string(w, "socket", dev->path);
}

static void
virtio_user_write_json_config(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "trtype", "user");
	spdk_json_write_named_string(w, "traddr", dev->path);
	spdk_json_write_named_uint32(w, "vq_count", vdev->max_queues - vdev->fixed_queues_num);
	spdk_json_write_named_uint32(w, "vq_size", virtio_dev_backend_ops(vdev)->get_queue_size(vdev, 0));
}

static const struct virtio_dev_ops virtio_user_ops = {
	.read_dev_cfg	= virtio_user_read_dev_config,
	.write_dev_cfg	= virtio_user_write_dev_config,
	.get_status	= virtio_user_get_status,
	.set_status	= virtio_user_set_status,
	.get_features	= virtio_user_get_features,
	.set_features	= virtio_user_set_features,
	.destruct_dev	= virtio_user_destroy,
	.get_queue_size	= virtio_user_get_queue_size,
	.setup_queue	= virtio_user_setup_queue,
	.del_queue	= virtio_user_del_queue,
	.notify_queue	= virtio_user_notify_queue,
	.dump_json_info = virtio_user_dump_json_info,
	.write_json_config = virtio_user_write_json_config,
};

int
virtio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path,
		     uint32_t queue_size)
{
	struct virtio_user_dev *dev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name gived for controller: %s\n", path);
		return -EINVAL;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -ENOMEM;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_user_ops, dev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return rc;
	}

	vdev->is_hw = 0;

	snprintf(dev->path, PATH_MAX, "%s", path);
	dev->queue_size = queue_size;

	rc = virtio_user_dev_setup(vdev);
	if (rc < 0) {
		SPDK_ERRLOG("backend set up fails\n");
		goto err;
	}

	rc = vhost_user_sock(dev, VHOST_USER_SET_OWNER, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("set_owner fails: %s\n", spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	virtio_dev_destruct(vdev);
	return rc;
}
SPDK_LOG_REGISTER_COMPONENT(virtio_user)
