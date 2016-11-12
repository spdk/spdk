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

/** \file
* Event framework public API.
*
* This is a framework for writing asynchronous, polled-mode, shared-nothing
* server applications. The framework relies on DPDK for much of its underlying
* architecture. The framework defines several concepts - reactors, events, pollers,
* and subsystems - that are described in the following sections.
*
* The framework runs one thread per core (the user provides a core mask), where
* each thread is a tight loop. The threads never block for any reason. These threads
* are called reactors and their main responsibility is to process incoming events
* from a queue.
*
* An event, defined by \ref spdk_event is a bundled function pointer and arguments that
* can be sent to a different core and executed. The function pointer is executed only once,
* and then the entire event is freed. These functions should never block and preferably
* should execute very quickly. Events also have a pointer to a 'next' event that will be
* executed upon completion of the given event, which allows chaining. This is
* very much a simplified version of futures, promises, and continuations designed within
* the constraints of the C programming language.
*
* The framework also defines another type of function called a poller. Pollers are also
* functions with arguments that can be bundled and sent to a different core to be executed,
* but they are instead executed repeatedly on that core until unregistered. The reactor
* will handle interspersing calls to the pollers with other event processing automatically.
* Pollers are intended to poll hardware as a replacement for interrupts and they should not
* generally be used for any other purpose.
*
* The framework also defines an interface for subsystems, which are libraries of code that
* depend on this framework. A library can register itself as a subsystem and provide
* pointers to initialize and destroy itself which will be called at the appropriate time.
* This is purely for sequencing initialization code in a convenient manner within the
* framework.
*
* The framework itself is bundled into a higher level abstraction called an "app". Once
* \ref spdk_app_start is called it will block the current thread until the application
* terminates (by calling \ref spdk_app_stop).
*/

#ifndef SPDK_EVENT_H
#define SPDK_EVENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spdk/queue.h"

#define SPDK_APP_DEFAULT_LOG_FACILITY	"local7"
#define SPDK_APP_DEFAULT_LOG_PRIORITY	"info"

typedef struct spdk_event *spdk_event_t;
typedef void (*spdk_event_fn)(spdk_event_t);

/**
 * \brief An event is a function that is passed to and called on an lcore.
 */
struct spdk_event {
	uint32_t		lcore;
	spdk_event_fn		fn;
	void			*arg1;
	void			*arg2;
	struct spdk_event	*next;
};

typedef void (*spdk_poller_fn)(void *arg);

/**
 * \brief A poller is a function that is repeatedly called on an lcore.
 */
struct spdk_poller;

typedef void (*spdk_app_shutdown_cb)(void);
typedef void (*spdk_sighandler_t)(int);

#define SPDK_APP_DPDK_DEFAULT_MEM_SIZE		2048
#define SPDK_APP_DPDK_DEFAULT_MASTER_CORE	0
#define SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL	4
#define SPDK_APP_DPDK_DEFAULT_CORE_MASK		"0x1"

/**
 * \brief Event framework initialization options
 */
struct spdk_app_opts {
	const char *name;
	const char *config_file;
	const char *reactor_mask;
	const char *log_facility;
	const char *tpoint_group_mask;

	int instance_id;

	spdk_app_shutdown_cb	shutdown_cb;
	spdk_sighandler_t	usr1_handler;

	bool			enable_coredump;
	uint32_t		dpdk_mem_channel;
	uint32_t 		dpdk_master_core;
	int			dpdk_mem_size;

	/* The maximum latency allowed when passing an event
	 * from one core to another. A value of 0
	 * means all cores continually poll. This is
	 * specified in microseconds.
	 */
	uint64_t		max_delay_us;
};

/**
 * \brief Initialize the default value of opts
*/
void spdk_app_opts_init(struct spdk_app_opts *opts);

/**
 * \brief Initialize DPDK via opts.
*/
void spdk_dpdk_framework_init(struct spdk_app_opts *opts);

/**
 * \brief Initialize an application to use the event framework. This must be called prior to using
 * any other functions in this library.
*/
void spdk_app_init(struct spdk_app_opts *opts);

/**
 * \brief Perform final shutdown operations on an application using the event framework.
*/
int spdk_app_fini(void);

/**
 * \brief Start the framework. Once started, the framework will call start_fn on the master
 * core with the arguments provided. This call will block until \ref spdk_app_stop is called.
*/
int spdk_app_start(spdk_event_fn start_fn, void *arg1, void *arg2);

/**
 * \brief Start shutting down the framework.  Typically this function is not called directly, and
 * the shutdown process is started implicitly by a process signal.  But in applications that are
 * using SPDK for a subset of its process threads, this function can be called in lieu of a signal.
 */
void spdk_app_start_shutdown(void);

/**
 * \brief Stop the framework. This does not wait for all threads to exit. Instead, it kicks off
 * the shutdown process and returns. Once the shutdown process is complete, \ref spdk_app_start will return.
*/
void spdk_app_stop(int rc);

/**
 * \brief Generate a configuration file that corresponds to the current running state.
*/
int spdk_app_get_running_config(char **config_str, char *name);

/**
 * \brief Return the instance id for this application.
*/
int spdk_app_get_instance_id(void);

/**
 * \brief Convert a string containing a CPU core mask into a bitmask
 */
int spdk_app_parse_core_mask(const char *mask, uint64_t *cpumask);

/**
 * \brief Return a mask of the CPU cores active for this application
 */
uint64_t spdk_app_get_core_mask(void);

/**
 * \brief Return the number of CPU cores utilized by this application
 */
int spdk_app_get_core_count(void);

/**
 * \brief Return the lcore of the current thread.
 */
uint32_t spdk_app_get_current_core(void);

/**
 * \brief Allocate an event to be passed to \ref spdk_event_call
 */
spdk_event_t spdk_event_allocate(uint32_t lcore, spdk_event_fn fn,
				 void *arg1, void *arg2,
				 spdk_event_t next);

/**
 * \brief Pass the given event to the associated lcore and call the function.
 */
void spdk_event_call(spdk_event_t event);

#define spdk_event_get_next(event)	(event)->next
#define spdk_event_get_arg1(event)	(event)->arg1
#define spdk_event_get_arg2(event)	(event)->arg2

/* TODO: This is only used by tests and should be made private */
uint32_t spdk_event_queue_run_batch(uint32_t lcore);

/**
 * \brief Register a poller on the given lcore.
 */
void spdk_poller_register(struct spdk_poller **ppoller,
			  spdk_poller_fn fn,
			  void *arg,
			  uint32_t lcore,
			  struct spdk_event *complete,
			  uint64_t period_microseconds);

/**
 * \brief Unregister a poller on the given lcore.
 */
void spdk_poller_unregister(struct spdk_poller **ppoller,
			    struct spdk_event *complete);

struct spdk_subsystem {
	const char *name;
	int (*init)(void);
	int (*fini)(void);
	void (*config)(FILE *fp);
	TAILQ_ENTRY(spdk_subsystem) tailq;
};

struct spdk_subsystem_depend {
	const char *name;
	const char *depends_on;
	struct spdk_subsystem *depends_on_subsystem;
	TAILQ_ENTRY(spdk_subsystem_depend) tailq;
};

void spdk_add_subsystem(struct spdk_subsystem *subsystem);
void spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend);

/**
 * \brief Register a new subsystem
 */
#define SPDK_SUBSYSTEM_REGISTER(_name, _init, _fini, _config)			\
	struct spdk_subsystem __spdk_subsystem_ ## _name = {			\
	.name = #_name,								\
	.init = _init,								\
	.fini = _fini,								\
	.config = _config,							\
	};									\
	__attribute__((constructor)) static void _name ## _register(void)	\
	{									\
		spdk_add_subsystem(&__spdk_subsystem_ ## _name);		\
	}

/**
 * \brief Declare that a subsystem depends on another subsystem.
 */
#define SPDK_SUBSYSTEM_DEPEND(_name, _depends_on)						\
	extern struct spdk_subsystem __spdk_subsystem_ ## _depends_on;				\
	static struct spdk_subsystem_depend __subsystem_ ## _name ## _depend_on ## _depends_on = { \
	.name = #_name,										\
	.depends_on = #_depends_on,								\
	.depends_on_subsystem = &__spdk_subsystem_ ## _depends_on,				\
	};											\
	__attribute__((constructor)) static void _name ## _depend_on ## _depends_on(void)	\
	{											\
		spdk_add_subsystem_depend(&__subsystem_ ## _name ## _depend_on ## _depends_on); \
	}

#endif
