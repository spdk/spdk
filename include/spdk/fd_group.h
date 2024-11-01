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

#include "spdk/assert.h"

/**
 * File descriptor type. The event handler may have extra checks and can do extra
 * processing based on this.
 */
enum spdk_fd_type {
	SPDK_FD_TYPE_DEFAULT		= 0x0,
	/**
	 * Event file descriptors. Once an event is generated on these file descriptors, event
	 * handler will perform a read operation on it to reset its internal eventfd object
	 * counter value to 0.
	 */
	SPDK_FD_TYPE_EVENTFD		= 0x1,
};

struct spdk_event_handler_opts {
	/**
	 * The size of spdk_event_handler_opts according to the caller of this library is used for
	 * ABI compatibility. The library uses this field to know how many fields in this structure
	 * are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	/** Event notification types */
	uint32_t events;

	/** fd type \ref spdk_fd_type */
	uint32_t fd_type;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_event_handler_opts) == 16, "Incorrect size");

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
 * Initialize a spdk_event_handler_opts structure to the default values.
 *
 * \param[out] opts Will be filled with default option.
 * \param opts_size Must be the size of spdk_event_handler_opts structure.
 */
void spdk_fd_group_get_default_event_handler_opts(struct spdk_event_handler_opts *opts,
		size_t opts_size);

/**
 * Initialize one fd_group.
 *
 * \param fgrp A pointer to return the initialized fgrp.
 *
 * \return 0 on success, negated errno on failure.
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
 * \return the number of events processed on success, negated errno on failure.
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
 * Nest the child fd_group in the parent fd_group. After this operation
 * completes, calling spdk_fd_group_wait() on the parent will include events
 * from the child.
 *
 * \param parent The parent fd_group.
 * \param child The child fd_group.
 *
 * \return 0 on success. Negated errno on failure. However, on all errno values other
 * than -ENOTRECOVERABLE, the operation has not changed the state of the fd_group.
 */
int spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child);

/**
 * Remove the nested child from the parent.
 *
 * \param parent The parent fd_group.
 * \param child The child fd_group.
 *
 * \return 0 on success. Negated errno on failure. However, on all errno values other
 * than -ENOTRECOVERABLE, the operation has not changed the state of the fd_group.
 */
int spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child);

/**
 * Register SPDK_INTERRUPT_EVENT_IN event source to specified fgrp.
 *
 * Use spdk_fd_group_add_for_events() for other event types.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd,
		      spdk_fd_fn fn, void *arg, const char *name);

/**
 * Register one event source to specified fgrp with specific event types.
 *
 * Event types argument is a bit mask composed by ORing together
 * enum spdk_interrupt_event_types values.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param events Event notification types.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_fd_group_add_for_events(struct spdk_fd_group *fgrp, int efd, uint32_t events,
				 spdk_fd_fn fn, void *arg,  const char *name);

/**
 * Register one event type stated in spdk_event_handler_opts agrument to the specified fgrp.
 *
 * spdk_event_handler_opts argument consists of event which is a bit mask composed by ORing
 * together enum spdk_interrupt_event_types values. It also consists of fd_type, which can be
 * used by event handler to perform extra checks during the spdk_fd_group_wait call.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 * \param opts Extended event handler option.
 *
 * \return 0 if success or -errno if failed
 */
int spdk_fd_group_add_ext(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn, void *arg,
			  const char *name, struct spdk_event_handler_opts *opts);

/*
 * \brief Register an event source with the name set to the string of the
 * callback function.
 */
#define SPDK_FD_GROUP_ADD(fgrp, efd, fn, arg) \
	spdk_fd_group_add(fgrp, efd, fn, arg, #fn)

/*
 * \brief Register an event source provided in opts with the name set to the string of the
 * callback function.
 */
#define SPDK_FD_GROUP_ADD_EXT(fgrp, efd, fn, arg, opts) \
	spdk_fd_group_add_ext(fgrp, efd, fn, arg, #fn, opts)

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
 * \return 0 on success, negated errno on failure.
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
 * \param event pointer to an epoll(7) event to copy to.
 * \return 0 on success, negated errno on failure.
 */
int spdk_fd_group_get_epoll_event(struct epoll_event *event);

typedef int (*spdk_fd_group_wrapper_fn)(void *wrapper_ctx, spdk_fd_fn cb_fn, void *cb_ctx);

/**
 * Set a wrapper function to be called when an epoll(7) event is received.  The callback associated
 * with that event is passed to the wrapper, which is responsible for executing it.  Only one
 * wrapper can be assigned to an fd_group at a time.
 *
 * \param fgrp fd group.
 * \param cb_fn Wrapper callback.
 * \param cb_ctx Wrapper callback's context.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_fd_group_set_wrapper(struct spdk_fd_group *fgrp, spdk_fd_group_wrapper_fn cb_fn,
			      void *cb_ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FD_GROUP_H */
