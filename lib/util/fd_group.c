/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk_internal/usdt.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/queue.h"

#include "spdk/fd_group.h"

#ifdef __linux__
#include <sys/epoll.h>
#endif

#define SPDK_MAX_EVENT_NAME_LEN 256

enum event_handler_state {
	/* The event_handler is added into an fd_group waiting for event,
	 * but not currently in the execution of a wait loop.
	 */
	EVENT_HANDLER_STATE_WAITING,

	/* The event_handler is currently in the execution of a wait loop. */
	EVENT_HANDLER_STATE_RUNNING,

	/* The event_handler was removed during the execution of a wait loop. */
	EVENT_HANDLER_STATE_REMOVED,
};

/* file descriptor of the interrupt event */

/* Taking "ehdlr" as short name for file descriptor handler of the interrupt event. */
struct event_handler {
	TAILQ_ENTRY(event_handler)	next;
	enum event_handler_state	state;

	spdk_fd_fn			fn;
	void				*fn_arg;
	/* file descriptor of the interrupt event */
	int				fd;
	uint32_t			events;
	char				name[SPDK_MAX_EVENT_NAME_LEN + 1];
};

struct spdk_fd_group {
	int epfd;
	int num_fds; /* Number of fds registered in this group. */

	struct spdk_fd_group *parent;

	/* interrupt sources list */
	TAILQ_HEAD(, event_handler) event_handlers;
};

int
spdk_fd_group_get_fd(struct spdk_fd_group *fgrp)
{
	return fgrp->epfd;
}

#ifdef __linux__

static __thread struct epoll_event *g_event = NULL;

int
spdk_fd_group_get_epoll_event(struct epoll_event *event)
{
	if (g_event == NULL) {
		return -EINVAL;
	}
	*event = *g_event;
	return 0;
}

static int
_fd_group_del_all(int epfd, struct spdk_fd_group *grp)
{
	struct event_handler *ehdlr = NULL;
	struct epoll_event epevent = {0};
	int rc;
	int ret = 0;

	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		rc = epoll_ctl(epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
		if (rc < 0) {
			if (errno == ENOENT) {
				/* This is treated as success. It happens if there are multiple
				 * attempts to remove fds from the group.
				 */
				continue;
			}

			ret = -errno;
			SPDK_ERRLOG("Failed to remove fd %d from group: %s\n", ehdlr->fd, strerror(errno));
			goto recover;
		}
	}

	return 0;

recover:
	/* We failed to remove everything. Let's try to get everything put back into
	 * the original group. */
	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		epevent.events = ehdlr->events;
		epevent.data.ptr = ehdlr;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ehdlr->fd, &epevent);
		if (rc < 0) {
			if (errno == EEXIST) {
				/* This is fine. Keep going. */
				continue;
			}

			/* Continue on even though we've failed. But indicate
			 * this is a fatal error. */
			SPDK_ERRLOG("Failed to recover fd_group_del_all: %s\n", strerror(errno));
			ret = -ENOTRECOVERABLE;
		}
	}

	return ret;
}

static int
_fd_group_add_all(int epfd, struct spdk_fd_group *grp)
{
	struct event_handler *ehdlr = NULL;
	struct epoll_event epevent = {0};
	int rc;
	int ret = 0;

	/* Hoist the fds from the child up into the parent */
	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		epevent.events = ehdlr->events;
		epevent.data.ptr = ehdlr;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ehdlr->fd, &epevent);
		if (rc < 0) {
			if (errno == EEXIST) {
				/* This is treated as success */
				continue;
			}

			ret = -errno;
			SPDK_ERRLOG("Failed to add fd to fd group: %s\n", strerror(errno));
			goto recover;
		}
	}

	return 0;

recover:
	/* We failed to add everything, so try to remove what we did add. */
	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		rc = epoll_ctl(epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
		if (rc < 0) {
			if (errno == ENOENT) {
				/* This is treated as success. */
				continue;
			}


			/* Continue on even though we've failed. But indicate
			 * this is a fatal error. */
			SPDK_ERRLOG("Failed to recover fd_group_del_all: %s\n", strerror(errno));
			ret = -ENOTRECOVERABLE;
		}
	}

	return ret;
}

int
spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	int rc;

	if (parent == NULL || child == NULL) {
		return -EINVAL;
	}

	if (child->parent != parent) {
		return -EINVAL;
	}

	rc = _fd_group_del_all(parent->epfd, child);
	if (rc < 0) {
		return rc;
	}

	child->parent = NULL;

	return _fd_group_add_all(child->epfd, child);
}

int
spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	int rc;

	if (parent == NULL || child == NULL) {
		return -EINVAL;
	}

	if (child->parent) {
		return -EINVAL;
	}

	if (parent->parent) {
		/* More than one layer of nesting is not currently supported */
		assert(false);
		return -ENOTSUP;
	}

	rc = _fd_group_del_all(child->epfd, child);
	if (rc < 0) {
		return rc;
	}

	child->parent = parent;

	return _fd_group_add_all(parent->epfd, child);
}

int
spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn,
		  void *arg, const char *name)
{
	struct event_handler *ehdlr = NULL;
	struct epoll_event epevent = {0};
	int rc;
	int epfd;

	/* parameter checking */
	if (fgrp == NULL || efd < 0 || fn == NULL) {
		return -EINVAL;
	}

	/* check if there is already one function registered for this fd */
	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			return -EEXIST;
		}
	}

	/* create a new event src */
	ehdlr = calloc(1, sizeof(*ehdlr));
	if (ehdlr == NULL) {
		return -errno;
	}

	ehdlr->fd = efd;
	ehdlr->fn = fn;
	ehdlr->fn_arg = arg;
	ehdlr->state = EVENT_HANDLER_STATE_WAITING;
	ehdlr->events = EPOLLIN;
	snprintf(ehdlr->name, sizeof(ehdlr->name), "%s", name);

	if (fgrp->parent) {
		epfd = fgrp->parent->epfd;
	} else {
		epfd = fgrp->epfd;
	}

	epevent.events = ehdlr->events;
	epevent.data.ptr = ehdlr;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &epevent);
	if (rc < 0) {
		free(ehdlr);
		return -errno;
	}

	TAILQ_INSERT_TAIL(&fgrp->event_handlers, ehdlr, next);
	fgrp->num_fds++;

	return 0;
}

void
spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd)
{
	struct event_handler *ehdlr;
	int rc;
	int epfd;

	if (fgrp == NULL || efd < 0) {
		SPDK_ERRLOG("Invalid to remvoe efd(%d) from fd_group(%p).\n", efd, fgrp);
		assert(0);
		return;
	}


	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			break;
		}
	}

	if (ehdlr == NULL) {
		SPDK_ERRLOG("efd(%d) is not existed in fgrp(%p)\n", efd, fgrp);
		return;
	}

	assert(ehdlr->state != EVENT_HANDLER_STATE_REMOVED);

	if (fgrp->parent) {
		epfd = fgrp->parent->epfd;
	} else {
		epfd = fgrp->epfd;
	}

	rc = epoll_ctl(epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to delete the fd(%d) from the epoll group(%p)\n", efd, fgrp);
		return;
	}

	assert(fgrp->num_fds > 0);
	fgrp->num_fds--;
	TAILQ_REMOVE(&fgrp->event_handlers, ehdlr, next);

	/* Delay ehdlr's free in case it is waiting for execution in fgrp wait loop */
	if (ehdlr->state == EVENT_HANDLER_STATE_RUNNING) {
		ehdlr->state = EVENT_HANDLER_STATE_REMOVED;
	} else {
		free(ehdlr);
	}
}

int
spdk_fd_group_event_modify(struct spdk_fd_group *fgrp,
			   int efd, int event_types)
{
	struct epoll_event epevent;
	struct event_handler *ehdlr;
	int epfd;

	if (fgrp == NULL || efd < 0) {
		return -EINVAL;
	}

	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			break;
		}
	}

	if (ehdlr == NULL) {
		return -EINVAL;
	}

	assert(ehdlr->state != EVENT_HANDLER_STATE_REMOVED);

	ehdlr->events = event_types;

	if (fgrp->parent) {
		epfd = fgrp->parent->epfd;
	} else {
		epfd = fgrp->epfd;
	}

	epevent.events = ehdlr->events;
	epevent.data.ptr = ehdlr;

	return epoll_ctl(epfd, EPOLL_CTL_MOD, ehdlr->fd, &epevent);
}

int
spdk_fd_group_create(struct spdk_fd_group **_egrp)
{
	struct spdk_fd_group *fgrp;

	if (_egrp == NULL) {
		return -EINVAL;
	}

	fgrp = calloc(1, sizeof(*fgrp));
	if (fgrp == NULL) {
		return -ENOMEM;
	}

	/* init the event source head */
	TAILQ_INIT(&fgrp->event_handlers);

	fgrp->num_fds = 0;
	fgrp->epfd = epoll_create1(EPOLL_CLOEXEC);
	if (fgrp->epfd < 0) {
		free(fgrp);
		return -errno;
	}

	*_egrp = fgrp;

	return 0;
}

void
spdk_fd_group_destroy(struct spdk_fd_group *fgrp)
{
	if (fgrp == NULL || fgrp->num_fds > 0) {
		SPDK_ERRLOG("Invalid fd_group(%p) to destroy.\n", fgrp);
		assert(0);
		return;
	}

	close(fgrp->epfd);
	free(fgrp);

	return;
}

int
spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout)
{
	int totalfds = fgrp->num_fds;
	struct epoll_event events[totalfds];
	struct event_handler *ehdlr;
	int n;
	int nfds;

	if (fgrp->parent != NULL) {
		if (timeout < 0) {
			SPDK_ERRLOG("Calling spdk_fd_group_wait on a group nested in another group without a timeout will block indefinitely.\n");
			assert(false);
			return -EINVAL;
		} else {
			SPDK_WARNLOG("Calling spdk_fd_group_wait on a group nested in another group will never find any events\n");
			return 0;
		}
	}

	nfds = epoll_wait(fgrp->epfd, events, totalfds, timeout);
	if (nfds < 0) {
		if (errno != EINTR) {
			SPDK_ERRLOG("fgrp epoll_wait returns with fail. errno is %d\n", errno);
		}

		return -errno;
	} else if (nfds == 0) {
		return 0;
	}

	for (n = 0; n < nfds; n++) {
		/* find the event_handler */
		ehdlr = events[n].data.ptr;

		if (ehdlr == NULL) {
			continue;
		}

		/* Tag ehdlr as running state in case that it is removed
		 * during this wait loop but before or when it get executed.
		 */
		assert(ehdlr->state == EVENT_HANDLER_STATE_WAITING);
		ehdlr->state = EVENT_HANDLER_STATE_RUNNING;
	}

	for (n = 0; n < nfds; n++) {
		/* find the event_handler */
		ehdlr = events[n].data.ptr;

		if (ehdlr == NULL || ehdlr->fn == NULL) {
			continue;
		}

		/* It is possible that the ehdlr was removed
		 * during this wait loop but before it get executed.
		 */
		if (ehdlr->state == EVENT_HANDLER_STATE_REMOVED) {
			free(ehdlr);
			continue;
		}

		g_event = &events[n];
		/* call the interrupt response function */
		ehdlr->fn(ehdlr->fn_arg);
		g_event = NULL;

		/* It is possible that the ehdlr was removed
		 * during this wait loop when it get executed.
		 */
		if (ehdlr->state == EVENT_HANDLER_STATE_REMOVED) {
			free(ehdlr);
		} else {
			ehdlr->state = EVENT_HANDLER_STATE_WAITING;
		}
	}

	return nfds;
}

#else

int
spdk_fd_group_get_epoll_event(struct epoll_event *event)
{
	return -ENOTSUP;
}

int
spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn,
		  void *arg, const char *name)
{
	return -ENOTSUP;
}

void
spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd)
{
}

int
spdk_fd_group_event_modify(struct spdk_fd_group *fgrp,
			   int efd, int event_types)
{
	return -ENOTSUP;
}

int
spdk_fd_group_create(struct spdk_fd_group **fgrp)
{
	return -ENOTSUP;
}

void
spdk_fd_group_destroy(struct spdk_fd_group *fgrp)
{
}

int
spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout)
{
	return -ENOTSUP;
}

int
spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	return -ENOTSUP;
}

int
spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	return -ENOTSUP;
}

#endif
