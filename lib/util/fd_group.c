/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk_internal/usdt.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#include "spdk/fd_group.h"

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

/* Taking "ehdlr" as short name for file descriptor handler of the interrupt event. */
struct event_handler {
	TAILQ_ENTRY(event_handler)	next;
	enum event_handler_state	state;

	spdk_fd_fn			fn;
	void				*fn_arg;
	/* file descriptor of the interrupt event */
	int				fd;
	uint32_t			events;
	uint32_t			fd_type;
	struct spdk_fd_group		*owner;
	char				name[SPDK_MAX_EVENT_NAME_LEN + 1];
};

struct spdk_fd_group {
	int epfd;

	/* Number of fds registered in this group. The epoll file descriptor of this fd group
	 * i.e. epfd waits for interrupt event on all the fds from its interrupt sources list, as
	 * well as from all its children fd group interrupt sources list.
	 */
	uint32_t num_fds;

	struct spdk_fd_group *parent;
	spdk_fd_group_wrapper_fn wrapper_fn;
	void *wrapper_arg;

	/* interrupt sources list */
	TAILQ_HEAD(, event_handler) event_handlers;
	TAILQ_HEAD(, spdk_fd_group) children;
	TAILQ_ENTRY(spdk_fd_group) link;
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
			SPDK_ERRLOG("Failed to remove fd: %d from group: %s\n",
				    ehdlr->fd, strerror(errno));
			goto recover;
		}
		ret++;
	}

	return ret;

recover:
	/* We failed to remove everything. Let's try to put everything back into
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
			SPDK_ERRLOG("Failed to add fd: %d to fd group: %s\n",
				    ehdlr->fd, strerror(errno));
			goto recover;
		}
		ret++;
	}

	return ret;

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

static struct spdk_fd_group *
fd_group_get_root(struct spdk_fd_group *fgrp)
{
	while (fgrp->parent != NULL) {
		fgrp = fgrp->parent;
	}

	return fgrp;
}

static int
fd_group_change_parent(struct spdk_fd_group *fgrp, struct spdk_fd_group *old,
		       struct spdk_fd_group *new)
{
	struct spdk_fd_group *child, *tmp;
	int rc, ret;

	TAILQ_FOREACH(child, &fgrp->children, link) {
		ret = fd_group_change_parent(child, old, new);
		if (ret != 0) {
			goto recover_children;
		}
	}

	ret = _fd_group_del_all(old->epfd, fgrp);
	if (ret < 0) {
		goto recover_children;
	}

	assert(old->num_fds >= (uint32_t)ret);
	old->num_fds -= ret;

	ret = _fd_group_add_all(new->epfd, fgrp);
	if (ret < 0) {
		goto recover_epfd;
	}

	new->num_fds += ret;
	return 0;

recover_epfd:
	if (ret == -ENOTRECOVERABLE) {
		goto recover_children;
	}
	rc = _fd_group_add_all(old->epfd, fgrp);
	if (rc >= 0) {
		old->num_fds += rc;
	} else {
		SPDK_ERRLOG("Failed to recover epfd\n");
		ret = -ENOTRECOVERABLE;
	}
recover_children:
	TAILQ_FOREACH(tmp, &fgrp->children, link) {
		if (tmp == child) {
			break;
		}
		rc = fd_group_change_parent(tmp, new, old);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to recover fd_group_change_parent\n");
			ret = -ENOTRECOVERABLE;
		}
	}
	return ret;
}

int
spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	struct spdk_fd_group *root;
	int rc;

	if (parent == NULL || child == NULL) {
		return -EINVAL;
	}

	if (child->parent != parent) {
		return -EINVAL;
	}

	root = fd_group_get_root(parent);
	assert(root == parent || parent->num_fds == 0);

	rc = fd_group_change_parent(child, root, child);
	if (rc != 0) {
		return rc;
	}

	child->parent = NULL;
	TAILQ_REMOVE(&parent->children, child, link);

	return 0;
}

int
spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	struct spdk_fd_group *root;
	int rc;

	if (parent == NULL || child == NULL) {
		return -EINVAL;
	}

	if (child->parent) {
		return -EINVAL;
	}

	if (parent->wrapper_fn != NULL) {
		return -EINVAL;
	}

	/* The epoll instance at the root holds all fds, so either the parent is the root or it
	 * doesn't hold any fds.
	 */
	root = fd_group_get_root(parent);
	assert(root == parent || parent->num_fds == 0);

	rc = fd_group_change_parent(child, child, root);
	if (rc != 0) {
		return rc;
	}

	child->parent = parent;
	TAILQ_INSERT_TAIL(&parent->children, child, link);

	return 0;
}

void
spdk_fd_group_get_default_event_handler_opts(struct spdk_event_handler_opts *opts,
		size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);
	opts->opts_size = opts_size;

#define FIELD_OK(field) \
        offsetof(struct spdk_event_handler_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \

	SET_FIELD(events, EPOLLIN);
	SET_FIELD(fd_type, SPDK_FD_TYPE_DEFAULT);

#undef FIELD_OK
#undef SET_FIELD
}

static void
event_handler_opts_copy(const struct spdk_event_handler_opts *src,
			struct spdk_event_handler_opts *dst)
{
	if (!src->opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		assert(false);
	}

#define FIELD_OK(field) \
        offsetof(struct spdk_event_handler_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(events);
	SET_FIELD(fd_type);

	dst->opts_size = src->opts_size;

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_event_handler_opts) == 16, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

int
spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn,
		  void *arg, const char *name)
{
	return spdk_fd_group_add_for_events(fgrp, efd, EPOLLIN, fn, arg, name);
}

int
spdk_fd_group_add_for_events(struct spdk_fd_group *fgrp, int efd, uint32_t events,
			     spdk_fd_fn fn, void *arg, const char *name)
{
	struct spdk_event_handler_opts opts = {};

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	opts.events = events;
	opts.fd_type = SPDK_FD_TYPE_DEFAULT;

	return spdk_fd_group_add_ext(fgrp, efd, fn, arg, name, &opts);
}

int
spdk_fd_group_add_ext(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn, void *arg,
		      const char *name, struct spdk_event_handler_opts *opts)
{
	struct event_handler *ehdlr = NULL;
	struct epoll_event epevent = {0};
	struct spdk_event_handler_opts eh_opts = {};
	struct spdk_fd_group *root;
	int rc;

	/* parameter checking */
	if (fgrp == NULL || efd < 0 || fn == NULL) {
		return -EINVAL;
	}

	spdk_fd_group_get_default_event_handler_opts(&eh_opts, sizeof(eh_opts));
	if (opts) {
		event_handler_opts_copy(opts, &eh_opts);
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
	ehdlr->events = eh_opts.events;
	ehdlr->fd_type = eh_opts.fd_type;
	ehdlr->owner = fgrp;
	snprintf(ehdlr->name, sizeof(ehdlr->name), "%s", name);

	root = fd_group_get_root(fgrp);
	epevent.events = ehdlr->events;
	epevent.data.ptr = ehdlr;
	rc = epoll_ctl(root->epfd, EPOLL_CTL_ADD, efd, &epevent);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add fd: %d to fd group(%p): %s\n",
			    efd, fgrp, strerror(errno));
		free(ehdlr);
		return -errno;
	}

	TAILQ_INSERT_TAIL(&fgrp->event_handlers, ehdlr, next);
	root->num_fds++;

	return 0;
}

void
spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd)
{
	struct event_handler *ehdlr;
	struct spdk_fd_group *root;
	int rc;

	if (fgrp == NULL || efd < 0) {
		SPDK_ERRLOG("Cannot remove fd: %d from fd group(%p)\n", efd, fgrp);
		assert(0);
		return;
	}


	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			break;
		}
	}

	if (ehdlr == NULL) {
		SPDK_ERRLOG("fd: %d doesn't exist in fd group(%p)\n", efd, fgrp);
		return;
	}

	assert(ehdlr->state != EVENT_HANDLER_STATE_REMOVED);
	root = fd_group_get_root(fgrp);

	rc = epoll_ctl(root->epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove fd: %d from fd group(%p): %s\n",
			    ehdlr->fd, fgrp, strerror(errno));
		assert(0);
		return;
	}

	assert(root->num_fds > 0);
	root->num_fds--;
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

	epevent.events = ehdlr->events;
	epevent.data.ptr = ehdlr;

	return epoll_ctl(fd_group_get_root(fgrp)->epfd, EPOLL_CTL_MOD, ehdlr->fd, &epevent);
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
	TAILQ_INIT(&fgrp->children);

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
		if (!fgrp) {
			SPDK_ERRLOG("fd_group doesn't exist.\n");
		} else {
			SPDK_ERRLOG("Cannot delete fd group(%p) as (%u) fds are still registered to it.\n",
				    fgrp, fgrp->num_fds);
		}
		assert(0);
		return;
	}

	/* Check if someone tried to delete the fd group before unnesting it */
	if (!TAILQ_EMPTY(&fgrp->event_handlers)) {
		SPDK_ERRLOG("Interrupt sources list not empty.\n");
		assert(0);
		return;
	}

	assert(fgrp->parent == NULL);
	assert(TAILQ_EMPTY(&fgrp->children));
	close(fgrp->epfd);
	free(fgrp);

	return;
}

int
spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout)
{
	struct spdk_fd_group *owner;
	uint32_t totalfds = fgrp->num_fds;
	struct epoll_event events[totalfds];
	struct event_handler *ehdlr;
	uint64_t count;
	int n;
	int nfds;
	int bytes_read;
	int read_errno;

	if (fgrp->parent != NULL) {
		if (timeout < 0) {
			SPDK_ERRLOG("Calling spdk_fd_group_wait on a group nested in another group without a timeout will block indefinitely.\n");
			assert(false);
			return -EINVAL;
		} else {
			SPDK_WARNLOG("Calling spdk_fd_group_wait on a group nested in another group will never find any events.\n");
			return 0;
		}
	}

	nfds = epoll_wait(fgrp->epfd, events, totalfds, timeout);
	if (nfds < 0) {
		if (errno != EINTR) {
			SPDK_ERRLOG("fd group(%p) epoll_wait failed: %s\n",
				    fgrp, strerror(errno));
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

		/* read fd to reset the internal eventfd object counter value to 0 */
		if (ehdlr->fd_type == SPDK_FD_TYPE_EVENTFD) {
			bytes_read = read(ehdlr->fd, &count, sizeof(count));
			if (bytes_read < 0) {
				g_event = NULL;
				if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN) {
					continue;
				}
				read_errno = errno;
				/* TODO: Device is buggy. Handle this properly */
				SPDK_ERRLOG("Failed to read fd (%d) %s\n",
					    ehdlr->fd, strerror(errno));
				return -read_errno;
			} else if (bytes_read == 0) {
				SPDK_ERRLOG("Read nothing from fd (%d)\n", ehdlr->fd);
				g_event = NULL;
				return -EINVAL;
			}
		}

		/* call the interrupt response function */
		owner = ehdlr->owner;
		if (owner->wrapper_fn != NULL) {
			owner->wrapper_fn(owner->wrapper_arg, ehdlr->fn, ehdlr->fn_arg);
		} else {
			ehdlr->fn(ehdlr->fn_arg);
		}
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

int
spdk_fd_group_set_wrapper(struct spdk_fd_group *fgrp, spdk_fd_group_wrapper_fn fn, void *ctx)
{
	if (fgrp->wrapper_fn != NULL && fn != NULL) {
		return -EEXIST;
	}

	if (!TAILQ_EMPTY(&fgrp->children)) {
		return -EINVAL;
	}

	fgrp->wrapper_fn = fn;
	fgrp->wrapper_arg = ctx;

	return 0;
}

#else /* !__linux__ */

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

int
spdk_fd_group_add_for_events(struct spdk_fd_group *fgrp, int efd, uint32_t events, spdk_fd_fn fn,
			     void *arg, const char *name)
{
	return -ENOTSUP;
}

int
spdk_fd_group_add_ext(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn, void *arg,
		      const char *name, struct spdk_event_handler_opts *opts)
{
	return -ENOTSUP;
}

void
spdk_fd_group_get_default_event_handler_opts(struct spdk_event_handler_opts *opts,
		size_t opts_size)
{
	assert(false);
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

int
spdk_fd_group_set_wrapper(struct spdk_fd_group *fgrp, spdk_fd_group_wrapper_fn fn, void *ctx)
{
	return -ENOTSUP;
}

#endif /* __linux__ */
