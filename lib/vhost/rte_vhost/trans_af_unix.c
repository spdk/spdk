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
#include <sys/un.h>
#include <fcntl.h>

#include <rte_log.h>

#include "vhost.h"
#include "vhost_user.h"

#define MAX_VIRTIO_BACKLOG 128

struct af_unix_socket {
	struct vhost_user_socket socket; /* must be the first field! */
	int socket_fd;
	struct sockaddr_un un;
};

static int create_unix_socket(struct vhost_user_socket *vsocket);
static int vhost_user_start_server(struct vhost_user_socket *vsocket);
static int vhost_user_start_client(struct vhost_user_socket *vsocket);
static void vhost_user_read_cb(int connfd, void *dat, int *remove);

/* return bytes# of read on success or negative val on failure. */
int
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

int
send_fd_message(int sockfd, char *buf, int buflen, int *fds, int fd_num)
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

static void
vhost_user_add_connection(int fd, struct vhost_user_socket *vsocket)
{
	int vid;
	size_t size;
	struct vhost_user_connection *conn;
	int ret;

	conn = malloc(sizeof(*conn));
	if (conn == NULL) {
		close(fd);
		return;
	}

	vid = vhost_new_device(vsocket->features);
	if (vid == -1) {
		goto err;
	}

	size = strnlen(vsocket->path, PATH_MAX);
	vhost_set_ifname(vid, vsocket->path, size);

	if (vsocket->dequeue_zero_copy)
		vhost_enable_dequeue_zero_copy(vid);

	RTE_LOG(INFO, VHOST_CONFIG, "new device, handle is %d\n", vid);

	if (vsocket->notify_ops->new_connection) {
		ret = vsocket->notify_ops->new_connection(vid);
		if (ret < 0) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"failed to add vhost user connection with fd %d\n",
				fd);
			goto err;
		}
	}

	conn->connfd = fd;
	conn->vsocket = vsocket;
	conn->vid = vid;
	ret = fdset_add(&vhost_user.fdset, fd, vhost_user_read_cb,
			NULL, conn);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to add fd %d into vhost server fdset\n",
			fd);

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(conn->vid);

		goto err;
	}

	pthread_mutex_lock(&vsocket->conn_mutex);
	TAILQ_INSERT_TAIL(&vsocket->conn_list, conn, next);
	pthread_mutex_unlock(&vsocket->conn_mutex);
	return;

err:
	free(conn);
	close(fd);
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

static void
vhost_user_read_cb(int connfd, void *dat, int *remove)
{
	struct vhost_user_connection *conn = dat;
	struct vhost_user_socket *vsocket = conn->vsocket;
	int ret;

	ret = vhost_user_msg_handler(conn->vid, connfd);
	if (ret < 0) {
		*remove = 1;
		vhost_destroy_device(conn->vid);

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(conn->vid);

		pthread_mutex_lock(&vsocket->conn_mutex);
		TAILQ_REMOVE(&vsocket->conn_list, conn, next);
		if (conn->connfd != -1) {
			close(conn->connfd);
			conn->connfd = -1;
		}
		pthread_mutex_unlock(&vsocket->conn_mutex);

		free(conn);

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

	ret = fdset_add(&vhost_user.fdset, fd, vhost_user_server_new_connection,
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
pthread_t reconn_tid;

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

int
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
	return create_unix_socket(vsocket);
}

static void
af_unix_socket_cleanup(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *s =
		container_of(vsocket, struct af_unix_socket, socket);

	if (vsocket->is_server) {
		fdset_del(&vhost_user.fdset, s->socket_fd);
		close(s->socket_fd);
		unlink(vsocket->path);
	} else if (vsocket->reconnect) {
		vhost_user_remove_reconnect(vsocket);
	}
}

static int
af_unix_socket_start(struct vhost_user_socket *vsocket)
{
	if (vsocket->is_server)
		return vhost_user_start_server(vsocket);
	else
		return vhost_user_start_client(vsocket);
}

const struct vhost_transport_ops af_unix_trans_ops = {
	.socket_size = sizeof(struct af_unix_socket),
	.socket_init = af_unix_socket_init,
	.socket_cleanup = af_unix_socket_cleanup,
	.socket_start = af_unix_socket_start,
};
