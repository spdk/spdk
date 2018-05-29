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

int
spdk_nvme_ioctl_init(void)
{
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

#endif
