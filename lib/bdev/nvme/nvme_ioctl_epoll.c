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
#include "spdk/io_channel.h"

#include "spdk_internal/log.h"
#include "bdev_nvme.h"

#ifdef __linux__ /* Current ioctl utility is Linux-Specific */

#include <linux/fs.h>
#include <sys/epoll.h>

#define SPDK_IOCTL_DIR "/var/tmp/spdk/"
#define SPDK_IOCTL_DEV_DIR "/var/tmp/spdk/dev/"
#define SPDK_IOCTL_PCI_DIR "/var/tmp/spdk/pci/"

#define UNSOCK_LISTEN_NUM 8
#define MAX_EPOLL_EVENT 128

/** Global variables used for managing ioctl connections. */
static int g_ioctl_epoll_fd = 0;
static struct spdk_poller *g_ioctl_poller;

typedef void (* spdk_nvme_event_func)(uint32_t epoll_event, void *dev_ptr);

struct spdk_nvme_ioctl_event_data {
	spdk_nvme_event_func	func;
	void *dev_ptr;
};

static int
spdk_nvme_ioctl_epoll_check(void *ctx)
{
	struct epoll_event events[MAX_EPOLL_EVENT];
	int nfds, i;
	struct spdk_nvme_ioctl_event_data *ioctl_data;

	/* Perform a non-blocking epoll */
	nfds = epoll_wait(g_ioctl_epoll_fd, events, MAX_EPOLL_EVENT, 0);
	if (nfds == -1) {
		SPDK_ERRLOG("epoll_wait failed, errno %d: %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	for (i = 0; i < nfds; i++) {
		ioctl_data = (struct spdk_nvme_ioctl_event_data *)events[i].data.ptr;
		ioctl_data->func(events[i].events, ioctl_data->dev_ptr);
	}

	return nfds;
}

/* Create dir_path if it doesn't exist */
static int
nvme_ioctl_prepare_dir(char *dir_path)
{
	int rc;
	struct stat dir_stat;

	rc = stat(dir_path, &dir_stat);
	/* Check whether dir_path is a dir if it exists */
	if (rc == 0) {
		if (S_ISDIR(dir_stat.st_mode)) {
			return 0;
		}
	}

	/* Create dir_path if it doesn't exist */
	if (rc == -1 && errno == ENOENT) {
		rc = mkdir(dir_path, 0700);
		if (rc == 0) {
			return 0;
		}
	}

	SPDK_ERRLOG("Failed to create/check ioctl dir %s, errno %d: %s\n",
		    dir_path, errno, spdk_strerror(errno));
	return -1;
}

int
spdk_nvme_ioctl_init(void)
{
	if (nvme_ioctl_prepare_dir(SPDK_IOCTL_DIR) || nvme_ioctl_prepare_dir(SPDK_IOCTL_DEV_DIR)
	    || nvme_ioctl_prepare_dir(SPDK_IOCTL_PCI_DIR)) {
		return -1;
	}

	assert(g_ioctl_epoll_fd == 0);
	g_ioctl_epoll_fd = epoll_create1(0);
	if (g_ioctl_epoll_fd < 0) {
		SPDK_ERRLOG("epoll_create1() failed, errno %d: %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	g_ioctl_poller = spdk_poller_register(spdk_nvme_ioctl_epoll_check, NULL, 0);

	return 0;
}

void
spdk_nvme_ioctl_fini(void)
{
	spdk_poller_unregister(&g_ioctl_poller);
	if (g_ioctl_epoll_fd > 0) {
		close(g_ioctl_epoll_fd);
	}

	return;
}

/*
 * 1. delete conn_fd from epoll_fd
 * 2. close conn_fd
 * 3. remove this ioctl_conn from nvme_ctrlr/nvme_bdev
 * 4. clear ioctl_conn (check its state, then free resources at proper time)
 */
static int
nvme_ioctl_epoll_delete_conn(struct spdk_nvme_ioctl_conn *ioctl_conn)
{
	struct epoll_event event;
	int rc;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_bdev *bdev;

	/*
	 * The event parameter is ignored but needs to be non-NULL to work around a bug in old
	 * kernel versions.
	 */
	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_DEL, ioctl_conn->connfd, &event);
	if (rc) {
		SPDK_ERRLOG("Failed to close an ioctl connection, errno %d: %s\n", errno, spdk_strerror(errno));
	}
	/* release ioctl_data allocated in nvme_ioctl_epoll_add_XXX_conn */
	free(ioctl_conn->epoll_event_dataptr);

	close(ioctl_conn->connfd);
	if (ioctl_conn->type == IOCTL_CONN_TYPE_CHAR) {
		nvme_ctrlr = (struct nvme_ctrlr *)ioctl_conn->device;
		TAILQ_REMOVE(&nvme_ctrlr->conn_list, ioctl_conn, conn_tailq);
	} else {
		bdev = (struct nvme_bdev *)ioctl_conn->device;
		TAILQ_REMOVE(&bdev->conn_list, ioctl_conn, conn_tailq);
	}

	spdk_nvme_ioctl_conn_free(ioctl_conn);

	return 0;
}

static void
nvme_ioctl_epoll_conn_event(uint32_t epoll_event, void *dev_ptr)
{
	struct spdk_nvme_ioctl_conn *ioctl_conn;
	int ret = 0;

	ioctl_conn = (struct spdk_nvme_ioctl_conn *)dev_ptr;

	if ((epoll_event & EPOLLERR) || (epoll_event & EPOLLHUP)) {
		nvme_ioctl_epoll_delete_conn(ioctl_conn);
		goto exit;
	}

	if (epoll_event & EPOLLIN) {
		ret = spdk_nvme_ioctl_conn_recv(ioctl_conn);
		if (ret) {
			SPDK_NOTICELOG("Failed to receive ioctl sock data\n");
			nvme_ioctl_epoll_delete_conn(ioctl_conn);
			goto exit;
		}
	}
	if (epoll_event & EPOLLOUT) {
		ret = spdk_nvme_ioctl_conn_xmit(ioctl_conn);
		if (ret) {
			SPDK_NOTICELOG("Failed to xmit ioctl sock data\n");
			nvme_ioctl_epoll_delete_conn(ioctl_conn);
			goto exit;
		}
	}

exit:
	return;
}

static int
nvme_ioctl_epoll_add_blk_conn(struct nvme_bdev *bdev, int connfd)
{
	struct epoll_event event;
	int rc;
	struct spdk_nvme_ioctl_event_data *ioctl_data;
	struct spdk_nvme_ioctl_conn *ioctl_conn;

	ioctl_conn = calloc(1, sizeof(*ioctl_conn));
	if (!ioctl_conn) {
		SPDK_ERRLOG("Failed to allocate memory for ioctl_conn.\n");
		return -1;
	}

	ioctl_data = malloc(sizeof(*ioctl_data));
	if (!ioctl_data) {
		SPDK_ERRLOG("Failed to allocate memory for ioctl_data.\n");
		free(ioctl_conn);
		return -1;
	}

	ioctl_conn->connfd = connfd;
	ioctl_conn->device = (void *)bdev;
	ioctl_conn->type = IOCTL_CONN_TYPE_BLK;
	TAILQ_INSERT_TAIL(&bdev->conn_list, ioctl_conn, conn_tailq);

	ioctl_data->func = nvme_ioctl_epoll_conn_event;
	ioctl_data->dev_ptr = ioctl_conn;
	ioctl_conn->epoll_event_dataptr = ioctl_data;

	event.events = EPOLLIN | EPOLLOUT | EPOLLET;
	event.data.ptr = ioctl_data;

	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_ADD, connfd, &event);

	return rc;
}


static int
nvme_ioctl_epoll_add_char_conn(struct nvme_ctrlr *ctrlr, int connfd)
{
	struct epoll_event event;
	int rc;
	struct spdk_nvme_ioctl_event_data *ioctl_data;
	struct spdk_nvme_ioctl_conn *ioctl_conn;

	ioctl_conn = calloc(1, sizeof(*ioctl_conn));
	if (!ioctl_conn) {
		SPDK_ERRLOG("Failed to allocate memory for ioctl_conn.\n");
		return -1;
	}

	ioctl_data = malloc(sizeof(*ioctl_data));
	if (!ioctl_data) {
		SPDK_ERRLOG("Failed to allocate memory for ioctl_data.\n");
		free(ioctl_conn);
		return -1;
	}

	ioctl_conn->connfd = connfd;
	ioctl_conn->device = (void *)ctrlr;
	ioctl_conn->type = IOCTL_CONN_TYPE_CHAR;
	TAILQ_INSERT_TAIL(&ctrlr->conn_list, ioctl_conn, conn_tailq);

	ioctl_data->func = nvme_ioctl_epoll_conn_event;
	ioctl_data->dev_ptr = ioctl_conn;
	ioctl_conn->epoll_event_dataptr = ioctl_data;

	event.events = EPOLLIN | EPOLLOUT | EPOLLET;
	event.data.ptr = ioctl_data;

	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_ADD, connfd, &event);

	return rc;
}

static void
nvme_ioctl_epoll_blk_listen_event(uint32_t epoll_event, void *dev_ptr)
{
	int listenfd, connfd;
	struct nvme_bdev *bdev;
	int rc;

	bdev = (struct nvme_bdev *) dev_ptr;

	listenfd = bdev->sockfd;
	connfd = accept(listenfd, NULL, NULL);
	if (connfd > 0) {
		SPDK_INFOLOG(SPDK_LOG_BDEV_NVME, "Namespace %d of %s accepts an ioctl connection.\n",
			     spdk_nvme_ns_get_id(bdev->ns), bdev->nvme_ctrlr->name);
		fcntl(connfd, F_SETFL, O_NONBLOCK);
		rc = nvme_ioctl_epoll_add_blk_conn(bdev, connfd);
		if (rc) {
			SPDK_NOTICELOG("Failed to add conn fd into epoll\n");
			close(connfd);
		}
	} else {
		SPDK_ERRLOG("Namespace %d of %s failed to accept an ioctl connection.\n",
			    spdk_nvme_ns_get_id(bdev->ns), bdev->nvme_ctrlr->name);
	}

	return;
}

static void
nvme_ioctl_epoll_char_listen_event(uint32_t epoll_event, void *dev_ptr)
{
	int listenfd, connfd;
	struct nvme_ctrlr *ctrlr;
	int rc;

	ctrlr = (struct nvme_ctrlr *) dev_ptr;

	listenfd = ctrlr->sockfd;
	connfd = accept(listenfd, NULL, NULL);
	if (connfd > 0) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "%s accepts an ioctl connection.\n", ctrlr->name);
		fcntl(connfd, F_SETFL, O_NONBLOCK);
		rc = nvme_ioctl_epoll_add_char_conn(ctrlr, connfd);
		if (rc) {
			SPDK_NOTICELOG("Failed to add conn fd into epoll\n");
			close(connfd);
		}
	} else {
		SPDK_ERRLOG("%s failed to accept an ioctl connection, errno is %d.\n", ctrlr->name, errno);
	}

	return;
}

/*
 * Return 0, if created successfully.
 */
static int
nvme_ioctl_epoll_add_blk_listen(struct nvme_bdev *bdev)
{
	struct epoll_event event;
	int rc;
	struct spdk_nvme_ioctl_event_data *ioctl_data;

	ioctl_data = malloc(sizeof(*ioctl_data));
	if (!ioctl_data) {
		SPDK_ERRLOG("Failed to allocate memory for ioctl_data.\n");
		return -1;
	}

	ioctl_data->func = nvme_ioctl_epoll_blk_listen_event;
	ioctl_data->dev_ptr = bdev;
	bdev->epoll_event_dataptr = ioctl_data;

	event.events = EPOLLIN;
	event.data.ptr = ioctl_data;

	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_ADD, bdev->sockfd, &event);
	/*  When an error occurs, epoll_ctl() returns -1 and errno is set */
	return rc;
}

/*
 * Return 0, if created successfully.
 */
static int
nvme_ioctl_epoll_add_char_listen(struct nvme_ctrlr *nvme_ctrlr)
{
	struct epoll_event event;
	int rc;
	struct spdk_nvme_ioctl_event_data *ioctl_data;

	ioctl_data = malloc(sizeof(*ioctl_data));
	if (!ioctl_data) {
		SPDK_ERRLOG("Failed to allocate memory for ioctl_data.\n");
		return -1;
	}

	ioctl_data->func = nvme_ioctl_epoll_char_listen_event;
	ioctl_data->dev_ptr = nvme_ctrlr;
	nvme_ctrlr->epoll_event_dataptr = ioctl_data;

	event.events = EPOLLIN;
	event.data.ptr = ioctl_data;

	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_ADD, nvme_ctrlr->sockfd, &event);
	/*  When an error occurs, epoll_ctl() returns -1 and errno is set */
	return rc;
}

static void
spdk_nvme_ioctl_bdev_remove(void *remove_ctx)
{
	struct nvme_bdev *bdev = remove_ctx;

	spdk_nvme_bdev_delete_ioctl_sockfd(bdev);

	if (bdev->bdev_ch) {
		spdk_put_io_channel(bdev->bdev_ch);
	}
	if (bdev->bdev_desc) {
		spdk_bdev_close(bdev->bdev_desc);
	}
}

/*
 * Return 0, if created successfully.
 */
int
spdk_nvme_bdev_create_ioctl_sockfd(struct nvme_bdev *bdev, int ns_id)
{
	char *socketpath;
	struct sockaddr_un un;
	int rc;

	rc = spdk_bdev_open(&bdev->disk, true, spdk_nvme_ioctl_bdev_remove, bdev, &bdev->bdev_desc);
	if (rc) {
		SPDK_ERRLOG("Failed to open bdev %s, rc = %d\n", bdev->disk.name, rc);
		return rc;
	}
	bdev->bdev_ch = spdk_bdev_get_io_channel(bdev->bdev_desc);
	if (!bdev->bdev_ch) {
		SPDK_ERRLOG("Failed to get io_channel from %s.\n", bdev->disk.name);
		return -1;
	}

	TAILQ_INIT(&bdev->conn_list);

	/*
	 * Create socket fd for NVME block device
	 * ex: /var/tmp/spdk/dev/nvme0n1 corresponding to /dev/nvme0n1
	 */
	socketpath = spdk_sprintf_alloc("%s/%sn%u", SPDK_IOCTL_DEV_DIR, bdev->nvme_ctrlr->name,
					ns_id);
	if (!socketpath) {
		SPDK_ERRLOG("Failed to allocate memory for socketpath.\n");
		return -1;
	}
	unlink(socketpath);

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	memcpy(un.sun_path, socketpath, spdk_min(sizeof(un.sun_path) - 1, strlen(socketpath) + 1));
	free(socketpath);

	bdev->sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (bdev->sockfd <= 0) {
		SPDK_ERRLOG("Failed to create unix socket, errno is %d\n", errno);
		return -1;
	}
	rc = bind(bdev->sockfd, (struct sockaddr *)&un, sizeof(un));
	if (rc) {
		SPDK_ERRLOG("Failed to bind sock_fd into epoll\n");
		close(bdev->sockfd);
		bdev->sockfd = -1;
		return -1;
	}
	listen(bdev->sockfd, UNSOCK_LISTEN_NUM);

	rc = nvme_ioctl_epoll_add_blk_listen(bdev);
	if (rc) {
		SPDK_ERRLOG("Failed to add listen fd into epoll\n");
		close(bdev->sockfd);
		bdev->sockfd = -1;
		return -1;
	}

	return 0;
}

/*
 * Return 0, if created successfully.
 */
int
spdk_nvme_ctrlr_create_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr)
{
	char *socketpath;
	struct sockaddr_un un;
	int rc;

	TAILQ_INIT(&nvme_ctrlr->conn_list);
	/*
	 * Create socket fd for NVME character device
	 * ex: /var/tmp/spdk/dev/nvme0 corresponding to /dev/nvme0
	 */
	socketpath = spdk_sprintf_alloc("%s/%s", SPDK_IOCTL_DEV_DIR, nvme_ctrlr->name);
	if (!socketpath) {
		SPDK_ERRLOG("Failed to allocate memory for socketpath.\n");
		return -1;
	}
	unlink(socketpath);

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	memcpy(un.sun_path, socketpath, spdk_min(sizeof(un.sun_path) - 1, strlen(socketpath) + 1));
	free(socketpath);

	nvme_ctrlr->sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (nvme_ctrlr->sockfd <= 0) {
		SPDK_ERRLOG("Failed to create unix socket, errno is %d\n", errno);
		return -1;
	}

	rc = bind(nvme_ctrlr->sockfd, (struct sockaddr *)&un, sizeof(un));
	if (rc) {
		SPDK_ERRLOG("Failed to bind sock_fd into epoll\n");
		close(nvme_ctrlr->sockfd);
		nvme_ctrlr->sockfd = -1;
		return -1;
	}

	listen(nvme_ctrlr->sockfd, UNSOCK_LISTEN_NUM);

	rc = nvme_ioctl_epoll_add_char_listen(nvme_ctrlr);
	if (rc) {
		SPDK_ERRLOG("Failed to add listen fd into epoll\n");
		close(nvme_ctrlr->sockfd);
		nvme_ctrlr->sockfd = -1;
		return -1;
	}

	return 0;
}

/*
 * 1. delete sockfd from epoll_fd
 * 2. close sock_fd
 * 3. delete ioctl_conn if have
 * 4. unlink socket path
 */
void
spdk_nvme_bdev_delete_ioctl_sockfd(struct nvme_bdev *bdev)
{
	struct spdk_nvme_ioctl_conn *conn, *conn_tmp;
	struct epoll_event event;
	char *socketpath;
	int ns_id;
	int rc;

	if (bdev->sockfd <= 0) {
		return;
	}

	/*
	 * The event parameter is ignored but needs to be non-NULL to work around a bug in old
	 * kernel versions.
	 */
	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_DEL, bdev->sockfd, &event);
	if (rc) {
		SPDK_ERRLOG("epoll_ctl(EPOLL_CTL_DEL) failed\n");
	}
	/* release ioctl_data allocated in add_ioctl_XXX_conn */
	free(bdev->epoll_event_dataptr);

	if (!TAILQ_EMPTY(&bdev->conn_list)) {
		TAILQ_FOREACH_SAFE(conn, &bdev->conn_list, conn_tailq, conn_tmp) {
			nvme_ioctl_epoll_delete_conn(conn);
		}
	}
	close(bdev->sockfd);

	ns_id = spdk_nvme_ns_get_id(bdev->ns);
	socketpath = spdk_sprintf_alloc("%s/%sn%u", SPDK_IOCTL_DEV_DIR, bdev->nvme_ctrlr->name,
					ns_id);
	if (!socketpath) {
		SPDK_ERRLOG("Failed to allocate memory for socketpath.\n");
		return;
	}

	unlink(socketpath);
	free(socketpath);
}

/*
 * 1. delete sockfd from epoll_fd
 * 2. close sock_fd
 * 3. delete ioctl_conn if have
 * 4. unlink socket path
 */
void
spdk_nvme_ctrlr_delete_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_ioctl_conn *conn, *conn_tmp;
	struct epoll_event event;
	char *socketpath;
	int rc;

	if (nvme_ctrlr->sockfd <= 0) {
		return;
	}

	/*
	 * The event parameter is ignored but needs to be non-NULL to work around a bug in old
	 * kernel versions.
	 */
	rc = epoll_ctl(g_ioctl_epoll_fd, EPOLL_CTL_DEL, nvme_ctrlr->sockfd, &event);
	if (rc) {
		SPDK_ERRLOG("epoll_ctl(EPOLL_CTL_DEL) failed\n");
	}
	/* release ioctl_data allocated in add_ioctl_XXX_conn */
	free(nvme_ctrlr->epoll_event_dataptr);

	if (!TAILQ_EMPTY(&nvme_ctrlr->conn_list)) {
		TAILQ_FOREACH_SAFE(conn, &nvme_ctrlr->conn_list, conn_tailq, conn_tmp) {
			nvme_ioctl_epoll_delete_conn(conn);
		}
	}
	close(nvme_ctrlr->sockfd);

	socketpath = spdk_sprintf_alloc("%s/%s", SPDK_IOCTL_DEV_DIR, nvme_ctrlr->name);
	if (!socketpath) {
		SPDK_ERRLOG("Failed to allocate memory for socketpath.\n");
		return;
	}
	unlink(socketpath);
	free(socketpath);
}

/*
 * Ctrlr id for this NVMe device is returned by _ctrlr_id
 * Return 0, if created successfully.
 */
int
spdk_nvme_ctrlr_create_pci_symlink(struct nvme_ctrlr *nvme_ctrlr)
{
	char *target, *linkpath;
	int rc;

	/*
	 * Create PCI accesses symbol link
	 * ex: in spdk /var/tmp/spdk/pci/nvme0      ---> /sys/bus/pci/devices/0000:05:00.0
	 *     in kern /sys/class/nvme/nvme0/device ---> /sys/bus/pci/devices/0000:05:00.0
	 */
	target = spdk_sprintf_alloc("/sys/bus/pci/devices/%s", nvme_ctrlr->trid.traddr);
	if (!target) {
		SPDK_ERRLOG("Failed to allocate memory for target.\n");
		return -1;
	}

	linkpath = spdk_sprintf_alloc("%s/%s", SPDK_IOCTL_PCI_DIR, nvme_ctrlr->name);
	if (!linkpath) {
		SPDK_ERRLOG("Failed to allocate memory for linkpath.\n");
		free(target);
		return -1;
	}

	unlink(linkpath);
	rc = symlink(target, linkpath);
	if (rc) {
		SPDK_ERRLOG("Failed to create PCI symlink %s to %s, errno is %d\n", linkpath, target, errno);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Successfully create PCI symlink %s to %s.\n", linkpath, target);
	}

	free(target);
	free(linkpath);
	return rc;
}

void
spdk_nvme_ctrlr_delete_pci_symlink(struct nvme_ctrlr *nvme_ctrlr)
{
	char  *linkpath;

	linkpath = spdk_sprintf_alloc("%s/%s", SPDK_IOCTL_PCI_DIR, nvme_ctrlr->name);
	if (!linkpath) {
		SPDK_ERRLOG("Failed to allocate memory for linkpath.\n");
		return;
	}

	unlink(linkpath);
	free(linkpath);
}

#else /* Not Linux */

int
spdk_nvme_ioctl_init(void)
{
	return 0;
}

void
spdk_nvme_ioctl_fini(void)
{
}

int
spdk_nvme_bdev_create_ioctl_sockfd(struct nvme_bdev *bdev, int ns_id)
{
	return 0;
}

int
spdk_nvme_ctrlr_create_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr)
{
	return 0;
}

void
spdk_nvme_bdev_delete_ioctl_sockfd(struct nvme_bdev *bdev)
{
}

void
spdk_nvme_ctrlr_delete_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr)
{
}

int
spdk_nvme_ctrlr_create_pci_symlink(struct nvme_ctrlr *nvme_ctrlr)
{
	return 0;
}

void
spdk_nvme_ctrlr_delete_pci_symlink(struct nvme_ctrlr *nvme_ctrlr)
{
}

#endif
