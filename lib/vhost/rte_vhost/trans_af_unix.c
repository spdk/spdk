/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Copyright (C) 2017 Red Hat, Inc.
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

#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>

#include <rte_log.h>

#include "fd_man.h"
#include "vhost.h"
#include "vhost_user.h"

#define MAX_VIRTIO_BACKLOG 128

static struct fdset af_unix_fdset = {
	.fd = { [0 ... MAX_FDS - 1] = {-1, NULL, NULL, NULL, 0} },
	.fd_mutex = PTHREAD_MUTEX_INITIALIZER,
	.num = 0
};

TAILQ_HEAD(vhost_user_connection_list, vhost_user_connection);

struct vhost_user_connection {
	struct virtio_net device; /* must be the first field! */
	struct vhost_user_socket *vsocket;
	int connfd;

	TAILQ_ENTRY(vhost_user_connection) next;
};

struct af_unix_socket {
	struct vhost_user_socket socket; /* must be the first field! */
	struct vhost_user_connection_list conn_list;
	pthread_mutex_t conn_mutex;
	int socket_fd;
	struct sockaddr_un un;
};

static int create_unix_socket(struct vhost_user_socket *vsocket);
static int vhost_user_start_server(struct vhost_user_socket *vsocket);
static int vhost_user_start_client(struct vhost_user_socket *vsocket);
static void vhost_user_read_cb(int connfd, void *dat, int *remove);

/* return bytes# of read on success or negative val on failure. */
static int
read_fd_message(int sockfd, char *buf, int buflen, int *fds, int fd_num)
{
	struct iovec iov;
	struct msghdr msgh;
	size_t fdsize = fd_num * sizeof(int);
	char control[CMSG_SPACE(fdsize)];
	struct cmsghdr *cmsg;
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
		RTE_LOG(ERR, VHOST_CONFIG, "recvmsg failed\n");
		return ret;
	}

	if (msgh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		RTE_LOG(ERR, VHOST_CONFIG, "truncted msg\n");
		return -1;
	}

	for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg != NULL;
		cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
		if ((cmsg->cmsg_level == SOL_SOCKET) &&
			(cmsg->cmsg_type == SCM_RIGHTS)) {
			memcpy(fds, CMSG_DATA(cmsg), fdsize);
			break;
		}
	}

	return ret;
}

static int
send_fd_message(int sockfd, void *buf, int buflen, int *fds, int fd_num)
{
	struct iovec iov;
	struct msghdr msgh;
	size_t fdsize = fd_num * sizeof(int);
	char control[CMSG_SPACE(fdsize)];
	struct cmsghdr *cmsg;
	int ret;

	memset(&msgh, 0, sizeof(msgh));
	iov.iov_base = buf;
	iov.iov_len = buflen;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;

	if (fds && fd_num > 0) {
		msgh.msg_control = control;
		msgh.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&msgh);
		if (cmsg == NULL) {
			RTE_LOG(ERR, VHOST_CONFIG,  "cmsg == NULL\n");
			errno = EINVAL;
			return -1;
		}
		cmsg->cmsg_len = CMSG_LEN(fdsize);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), fds, fdsize);
	} else {
		msgh.msg_control = NULL;
		msgh.msg_controllen = 0;
	}

	do {
		ret = sendmsg(sockfd, &msgh, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,  "sendmsg error\n");
		return ret;
	}

	return ret;
}

static int
af_unix_send_reply(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);

	return send_fd_message(conn->connfd, msg,
			       VHOST_USER_HDR_SIZE + msg->size, NULL, 0);
}

static void
vhost_user_add_connection(int fd, struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	size_t size;
	struct virtio_net *dev;
	struct vhost_user_connection *conn;
	int ret;

	dev = vhost_new_device(vsocket->trans_ops, vsocket->features);
	if (!dev) {
		return;
	}

	conn = container_of(dev, struct vhost_user_connection, device);
	conn->connfd = fd;
	conn->vsocket = vsocket;

	size = strnlen(vsocket->path, PATH_MAX);
	vhost_set_ifname(dev->vid, vsocket->path, size);

	if (vsocket->dequeue_zero_copy)
		vhost_enable_dequeue_zero_copy(dev->vid);

	RTE_LOG(INFO, VHOST_CONFIG, "new device, handle is %d\n", dev->vid);

	if (vsocket->notify_ops->new_connection) {
		ret = vsocket->notify_ops->new_connection(dev->vid);
		if (ret < 0) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"failed to add vhost user connection with fd %d\n",
				fd);
			goto err;
		}
	}

	ret = fdset_add(&af_unix_fdset, fd, vhost_user_read_cb,
			NULL, conn);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to add fd %d into vhost server fdset\n",
			fd);

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(dev->vid);

		goto err;
	}

	pthread_mutex_lock(&s->conn_mutex);
	TAILQ_INSERT_TAIL(&s->conn_list, conn, next);
	pthread_mutex_unlock(&s->conn_mutex);
	return;

err:
	close(conn->connfd);
	vhost_destroy_device(dev->vid);
}

/* call back when there is new vhost-user connection from client  */
static void
vhost_user_server_new_connection(int fd, void *dat, int *remove __rte_unused)
{
	struct vhost_user_socket *vsocket = dat;

	fd = accept(fd, NULL, NULL);
	if (fd < 0)
		return;

	RTE_LOG(INFO, VHOST_CONFIG, "new vhost user connection is %d\n", fd);
	vhost_user_add_connection(fd, vsocket);
}

/* return bytes# of read on success or negative val on failure. */
static int
read_vhost_message(int sockfd, struct VhostUserMsg *msg)
{
	int ret;

	ret = read_fd_message(sockfd, (char *)msg, VHOST_USER_HDR_SIZE,
		msg->fds, VHOST_MEMORY_MAX_NREGIONS);
	if (ret <= 0)
		return ret;

	if (msg && msg->size) {
		if (msg->size > sizeof(msg->payload)) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"invalid msg size: %d\n", msg->size);
			return -1;
		}
		ret = read(sockfd, &msg->payload, msg->size);
		if (ret <= 0)
			return ret;
		if (ret != (int)msg->size) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"read control message failed\n");
			return -1;
		}
	}

	return ret;
}

static void
vhost_user_read_cb(int connfd, void *dat, int *remove)
{
	struct vhost_user_connection *conn = dat;
	struct vhost_user_socket *vsocket = conn->vsocket;
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	struct VhostUserMsg msg;
	int ret;

	ret = read_vhost_message(connfd, &msg);
	if (ret <= 0) {
		if (ret < 0)
			RTE_LOG(ERR, VHOST_CONFIG,
				"vhost read message failed\n");
		else if (ret == 0)
			RTE_LOG(INFO, VHOST_CONFIG,
				"vhost peer closed\n");
		goto err;
	}

	ret = vhost_user_msg_handler(conn->device.vid, &msg);
	if (ret < 0) {
err:
		*remove = 1;

		pthread_mutex_lock(&s->conn_mutex);
		TAILQ_REMOVE(&s->conn_list, conn, next);
		if (conn->connfd != -1) {
			close(conn->connfd);
			conn->connfd = -1;
		}
		pthread_mutex_unlock(&s->conn_mutex);

		int vid = conn->device.vid;
		vhost_destroy_device(conn->device.vid);

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(vid);

		if (vsocket->reconnect) {
			create_unix_socket(vsocket);
			vhost_user_start_client(vsocket);
		}
	}
}

static int
create_unix_socket(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	int fd;
	struct sockaddr_un *un = &s->un;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	RTE_LOG(INFO, VHOST_CONFIG, "vhost-user %s: socket created, fd: %d\n",
		vsocket->is_server ? "server" : "client", fd);

	if (!vsocket->is_server && fcntl(fd, F_SETFL, O_NONBLOCK)) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"vhost-user: can't set nonblocking mode for socket, fd: "
			"%d (%s)\n", fd, strerror(errno));
		close(fd);
		return -1;
	}

	memset(un, 0, sizeof(*un));
	un->sun_family = AF_UNIX;
	strncpy(un->sun_path, vsocket->path, sizeof(un->sun_path));
	un->sun_path[sizeof(un->sun_path) - 1] = '\0';

	s->socket_fd = fd;
	return 0;
}

static int
vhost_user_start_server(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	int ret;
	int fd = s->socket_fd;
	const char *path = vsocket->path;

	ret = bind(fd, (struct sockaddr *)&s->un, sizeof(s->un));
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to bind to %s: %s; remove it and try again\n",
			path, strerror(errno));
		goto err;
	}
	RTE_LOG(INFO, VHOST_CONFIG, "bind to %s\n", path);

	ret = listen(fd, MAX_VIRTIO_BACKLOG);
	if (ret < 0)
		goto err;

	ret = fdset_add(&af_unix_fdset, fd, vhost_user_server_new_connection,
		  NULL, vsocket);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to add listen fd %d to vhost server fdset\n",
			fd);
		goto err;
	}

	return 0;

err:
	close(fd);
	return -1;
}

struct vhost_user_reconnect {
	struct sockaddr_un un;
	int fd;
	struct vhost_user_socket *vsocket;

	TAILQ_ENTRY(vhost_user_reconnect) next;
};

TAILQ_HEAD(vhost_user_reconnect_tailq_list, vhost_user_reconnect);
struct vhost_user_reconnect_list {
	struct vhost_user_reconnect_tailq_list head;
	pthread_mutex_t mutex;
};

static struct vhost_user_reconnect_list reconn_list;
static pthread_t reconn_tid;

static int
vhost_user_connect_nonblock(int fd, struct sockaddr *un, size_t sz)
{
	int ret, flags;

	ret = connect(fd, un, sz);
	if (ret < 0 && errno != EISCONN)
		return -1;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"can't get flags for connfd %d\n", fd);
		return -2;
	}
	if ((flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) {
		RTE_LOG(ERR, VHOST_CONFIG,
				"can't disable nonblocking on fd %d\n", fd);
		return -2;
	}
	return 0;
}

static void *
vhost_user_client_reconnect(void *arg __rte_unused)
{
	int ret;
	struct vhost_user_reconnect *reconn, *next;

	while (1) {
		pthread_mutex_lock(&reconn_list.mutex);

		/*
		 * An equal implementation of TAILQ_FOREACH_SAFE,
		 * which does not exist on all platforms.
		 */
		for (reconn = TAILQ_FIRST(&reconn_list.head);
		     reconn != NULL; reconn = next) {
			next = TAILQ_NEXT(reconn, next);

			ret = vhost_user_connect_nonblock(reconn->fd,
						(struct sockaddr *)&reconn->un,
						sizeof(reconn->un));
			if (ret == -2) {
				close(reconn->fd);
				RTE_LOG(ERR, VHOST_CONFIG,
					"reconnection for fd %d failed\n",
					reconn->fd);
				goto remove_fd;
			}
			if (ret == -1)
				continue;

			RTE_LOG(INFO, VHOST_CONFIG,
				"%s: connected\n", reconn->vsocket->path);
			vhost_user_add_connection(reconn->fd, reconn->vsocket);
remove_fd:
			TAILQ_REMOVE(&reconn_list.head, reconn, next);
			free(reconn);
		}

		pthread_mutex_unlock(&reconn_list.mutex);
		sleep(1);
	}

	return NULL;
}

static int
vhost_user_reconnect_init(void)
{
	int ret;

	pthread_mutex_init(&reconn_list.mutex, NULL);
	TAILQ_INIT(&reconn_list.head);

	ret = pthread_create(&reconn_tid, NULL,
			     vhost_user_client_reconnect, NULL);
	if (ret < 0)
		RTE_LOG(ERR, VHOST_CONFIG, "failed to create reconnect thread");

	return ret;
}

static int
vhost_user_start_client(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	int ret;
	int fd = s->socket_fd;
	const char *path = vsocket->path;
	struct vhost_user_reconnect *reconn;

	ret = vhost_user_connect_nonblock(fd, (struct sockaddr *)&s->un,
					  sizeof(s->un));
	if (ret == 0) {
		vhost_user_add_connection(fd, vsocket);
		return 0;
	}

	RTE_LOG(WARNING, VHOST_CONFIG,
		"failed to connect to %s: %s\n",
		path, strerror(errno));

	if (ret == -2 || !vsocket->reconnect) {
		close(fd);
		return -1;
	}

	RTE_LOG(INFO, VHOST_CONFIG, "%s: reconnecting...\n", path);
	reconn = malloc(sizeof(*reconn));
	if (reconn == NULL) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to allocate memory for reconnect\n");
		close(fd);
		return -1;
	}
	reconn->un = s->un;
	reconn->fd = fd;
	reconn->vsocket = vsocket;
	pthread_mutex_lock(&reconn_list.mutex);
	TAILQ_INSERT_TAIL(&reconn_list.head, reconn, next);
	pthread_mutex_unlock(&reconn_list.mutex);

	return 0;
}

static bool
vhost_user_remove_reconnect(struct vhost_user_socket *vsocket)
{
	int found = false;
	struct vhost_user_reconnect *reconn, *next;

	pthread_mutex_lock(&reconn_list.mutex);

	for (reconn = TAILQ_FIRST(&reconn_list.head);
	     reconn != NULL; reconn = next) {
		next = TAILQ_NEXT(reconn, next);

		if (reconn->vsocket == vsocket) {
			TAILQ_REMOVE(&reconn_list.head, reconn, next);
			close(reconn->fd);
			free(reconn);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&reconn_list.mutex);
	return found;
}

static int
af_unix_socket_init(struct vhost_user_socket *vsocket,
		    uint64_t flags __rte_unused)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	int ret;

	if (vsocket->reconnect && reconn_tid == 0) {
		if (vhost_user_reconnect_init() < 0)
			return -1;
	}

	TAILQ_INIT(&s->conn_list);

	ret = create_unix_socket(vsocket);
	if (ret == 0) {
		int rc;
		rc = pthread_mutex_init(&s->conn_mutex, NULL);
		if (rc) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"error: failed to init connection mutex\n");
			return -1;
		}
	}
	return ret;
}

static void
af_unix_socket_cleanup(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);
	struct vhost_user_connection *conn;

	if (vsocket->is_server) {
		fdset_del(&af_unix_fdset, s->socket_fd);
		close(s->socket_fd);
		unlink(vsocket->path);
	} else if (vsocket->reconnect) {
		vhost_user_remove_reconnect(vsocket);
	}

	pthread_mutex_lock(&s->conn_mutex);
	TAILQ_FOREACH(conn, &s->conn_list, next) {
		close(conn->connfd);
		conn->connfd = -1;
	}
	pthread_mutex_unlock(&s->conn_mutex);

	do {
		pthread_mutex_lock(&s->conn_mutex);
		conn = TAILQ_FIRST(&s->conn_list);
		pthread_mutex_unlock(&s->conn_mutex);
	} while (conn != NULL);
}

static int
af_unix_socket_start(struct vhost_user_socket *vsocket)
{
	static pthread_t fdset_tid;

	if (fdset_tid == 0) {
		int ret = pthread_create(&fdset_tid, NULL, fdset_event_dispatch,
					&af_unix_fdset);
		if (ret < 0)
			RTE_LOG(ERR, VHOST_CONFIG,
				"failed to create fdset handling thread");
	}

	if (vsocket->is_server)
		return vhost_user_start_server(vsocket);
	else
		return vhost_user_start_client(vsocket);
}

static uint64_t
get_blk_size(int fd)
{
	struct stat stat;
	int ret;

	ret = fstat(fd, &stat);
	return ret == -1 ? (uint64_t)-1 : (uint64_t)stat.st_blksize;
}

static int
af_unix_map_mem_regions(struct virtio_net *dev)
{
	uint32_t i;

	for (i = 0; i < dev->mem->nregions; i++) {
		struct rte_vhost_mem_region *reg = &dev->mem->regions[i];
		uint64_t mmap_size = reg->mmap_size;
		uint64_t mmap_offset = mmap_size - reg->size;
		uint64_t alignment;
		void *mmap_addr;

		/* mmap() without flag of MAP_ANONYMOUS, should be called
		 * with length argument aligned with hugepagesz at older
		 * longterm version Linux, like 2.6.32 and 3.2.72, or
		 * mmap() will fail with EINVAL.
		 *
		 * to avoid failure, make sure in caller to keep length
		 * aligned.
		 */
		alignment = get_blk_size(reg->fd);
		if (alignment == (uint64_t)-1) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"couldn't get hugepage size through fstat\n");
			return -1;
		}
		mmap_size = RTE_ALIGN_CEIL(mmap_size, alignment);

		mmap_addr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED | MAP_POPULATE, reg->fd, 0);

		if (mmap_addr == MAP_FAILED) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"mmap region %u failed.\n", i);
			return -1;
		}

		if (madvise(mmap_addr, mmap_size, MADV_DONTDUMP) != 0) {
			RTE_LOG(INFO, VHOST_CONFIG,
				"MADV_DONTDUMP advice setting failed.\n");
		}

		reg->mmap_addr = mmap_addr;
		reg->mmap_size = mmap_size;
		reg->host_user_addr = (uint64_t)(uintptr_t)reg->mmap_addr +
				      mmap_offset;

		if (dev->dequeue_zero_copy)
			vhost_add_guest_pages(dev, reg, alignment);
	}

	return 0;
}

static void
af_unix_unmap_mem_regions(struct virtio_net *dev)
{
	uint32_t i;
	struct rte_vhost_mem_region *reg;

	for (i = 0; i < dev->mem->nregions; i++) {
		reg = &dev->mem->regions[i];
		if (reg->host_user_addr) {
			munmap(reg->mmap_addr, reg->mmap_size);
			close(reg->fd);
		}
	}
}

static int
af_unix_vring_call(struct virtio_net *dev __rte_unused,
		   struct vhost_virtqueue *vq)
{
	if (vq->callfd >= 0) {
		int ret;
		ret = eventfd_write(vq->callfd, (eventfd_t)1);

		RTE_LOG(DEBUG, VHOST_CONFIG, "%s vq_idx %u callfd %d"
				" (eventfd_write returned with %d)\n",
				__func__,
				vq->vring_idx,
				vq->callfd,
				ret);
		return ret;
	}
	return 0;
}

const struct vhost_transport_ops af_unix_trans_ops = {
	.socket_size = sizeof(struct af_unix_socket),
	.device_size = sizeof(struct vhost_user_connection),
	.socket_init = af_unix_socket_init,
	.socket_cleanup = af_unix_socket_cleanup,
	.socket_start = af_unix_socket_start,
	.send_reply = af_unix_send_reply,
	.map_mem_regions = af_unix_map_mem_regions,
	.unmap_mem_regions = af_unix_unmap_mem_regions,
	.vring_call = af_unix_vring_call,
	.cleanup_device = NULL,
};
