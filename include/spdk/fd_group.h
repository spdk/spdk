/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * File descriptor group utility functions
 */

#ifndef SPDK_FD_GROUP_H
#define SPDK_FD_GROUP_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback function registered for the event source file descriptor.
 *
 * \param ctx Context passed as arg to spdk_fd_group_add().
 *
 * \return 0 to indicate that event notification took place but no events were found;
 * positive to indicate that event notification took place and some events were processed;
 * negative if no event information is provided.
 */
typedef int (*spdk_fd_fn)(void *ctx);

/**
 * A file descriptor group of event sources which gather the events to an epoll instance.
 *
 * Taking "fgrp" as short name for file descriptor group of event sources.
 */
struct spdk_fd_group;

/**
 * Initialize one fd_group.
 *
 * \param fgrp A pointer to return the initialized fgrp.
 *
 * \return 0 if success or -errno if failed
 */
int spdk_fd_group_create(struct spdk_fd_group **fgrp);

/**
 * Release all resources associated with this fgrp.
 *
 * Users need to remove all event sources from the fgrp before destroying it.
 *
 * \param fgrp The fgrp to destroy.
 */
void spdk_fd_group_destroy(struct spdk_fd_group *fgrp);

/**
 * Wait for new events generated inside fgrp, and process them with their
 * registered spdk_fd_fn.
 *
 * \param fgrp The fgrp to wait and process.
 * \param timeout Specifies the number of milliseconds that will block.
 * -1 causes indefinitely blocking; 0 causes immediately return.
 *
 * \return the number of processed events
 * or -errno if failed
 */
int spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout);

/**
 * Return the internal epoll_fd of specific fd_group
 *
 * \param fgrp The pointer of specified fgrp.
 *
 * \return The epoll_fd of specific fgrp.
 */
int spdk_fd_group_get_fd(struct spdk_fd_group *fgrp);

/**
 * Register one event source to specified fgrp.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 *
 * \return 0 if success or -errno if failed
 */
int spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd,
		      spdk_fd_fn fn, void *arg, const char *name);

/*
 * \brief Register an event source with the name set to the string of the
 * callback function.
 */
#define SPDK_FD_GROUP_ADD(fgrp, efd, fn, arg) \
	spdk_fd_group_add(fgrp, efd, fn, arg, #fn)

/**
 * Unregister one event source from one fgrp.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 */
void spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd);

/**
 * Change the event notification types associated with the event source.
 *
 * Modules like nbd, need this api to add EPOLLOUT when having data to send, and remove EPOLLOUT if no data to send.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param event_types The event notification types.
 *
 * \return 0 if success or -errno if failed
 */
int spdk_fd_group_event_modify(struct spdk_fd_group *fgrp,
			       int efd, int event_types);

/*
 * Forward declaration of epoll_event to avoid having to conditionally compile
 * spdk_fd_group_get_epoll_event on non-Linux systems.
 */
struct epoll_event;

/**
 * Copies the epoll(7) event that caused a callback function to execute.
 * This function can only be called by the callback function, doing otherwise
 * results in undefined behavior.
 *
 * \param event pointer to an epoll(7) event to copy to
 * \return 0 on success, -errno on error
 */
int spdk_fd_group_get_epoll_event(struct epoll_event *event);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FD_GROUP_H */
