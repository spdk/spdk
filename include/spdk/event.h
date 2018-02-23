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

/**
 * \file
 * Event framework public API.
 *
 * See @ref event_components for an overview of the SPDK event framework API.
 */

#ifndef SPDK_EVENT_H
#define SPDK_EVENT_H

#include "spdk/stdinc.h"

#include "spdk/cpuset.h"
#include "spdk/queue.h"
#include "spdk/log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spdk_event_fn)(void *arg1, void *arg2);

/**
 * \brief An event is a function that is passed to and called on an lcore.
 */
struct spdk_event;

/**
 * \brief A poller is a function that is repeatedly called on an lcore.
 */
struct spdk_poller;

typedef void (*spdk_app_shutdown_cb)(void);
typedef void (*spdk_sighandler_t)(int);

#define SPDK_DEFAULT_RPC_ADDR "/var/tmp/spdk.sock"

/**
 * \brief Event framework initialization options
 */
struct spdk_app_opts {
	const char *name;
	const char *config_file;
	const char *rpc_addr; /* Can be UNIX domain socket path or IP address + TCP port */
	const char *reactor_mask;
	const char *tpoint_group_mask;

	int shm_id;

	spdk_app_shutdown_cb	shutdown_cb;
	spdk_sighandler_t	usr1_handler;

	bool			enable_coredump;
	int			mem_channel;
	int	 		master_core;
	int			mem_size;
	bool			no_pci;
	bool			hugepage_single_segments;
	enum spdk_log_level	print_level;

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
 * \brief Start the framework. Once started, the framework will call start_fn on the master
 * core with the arguments provided. This call will block until \ref spdk_app_stop is called.
 */
int spdk_app_start(struct spdk_app_opts *opts, spdk_event_fn start_fn,
		   void *arg1, void *arg2);

/**
 * \brief Perform final shutdown operations on an application using the event framework.
 */
void spdk_app_fini(void);

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
 * \brief Return the shared memory id for this application.
 */
int spdk_app_get_shm_id(void);

/**
 * \brief Convert a string containing a CPU core mask into a bitmask
 */
int spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask);

/**
 * \brief Return a mask of the CPU cores active for this application
 */
struct spdk_cpuset *spdk_app_get_core_mask(void);

/**
 * \brief Return the number of CPU cores utilized by this application
 */
int spdk_app_get_core_count(void) __attribute__((deprecated));

/**
 * \brief Return the lcore of the current thread.
 */
uint32_t spdk_app_get_current_core(void) __attribute__((deprecated));

#define SPDK_APP_GETOPT_STRING "c:de:fhi:m:n:p:qr:s:t:"

/**
 * \brief Helper function for parsing arguments and printing usage messages.
 *
 * \param argc Count of arguments in argv parameter array.
 * \param argv Array of command line arguments.
 * \param opts Default options for the application.
 * \param getopt_str String representing the app-specific command line parameters.
 *		     Characters in this string must not conflict with characters in
 *		     SPDK_APP_GETOPT_STRING.
 * \param parse Function pointer to call if an argument in getopt_str is found.
 * \param usage Function pointer to print usage messages for app-specific command
 *		line parameters.
 */
int spdk_app_parse_args(int argc, char **argv, struct spdk_app_opts *opts,
			const char *getopt_str, void (*parse)(int ch, char *arg),
			void (*usage)(void));

/**
 * \brief Allocate an event to be passed to \ref spdk_event_call
 */
struct spdk_event *spdk_event_allocate(uint32_t lcore, spdk_event_fn fn,
				       void *arg1, void *arg2);

/**
 * \brief Pass the given event to the associated lcore and call the function.
 */
void spdk_event_call(struct spdk_event *event);

/**
 * \brief Enable or disable monitoring of context switches.
 */
void spdk_reactor_enable_context_switch_monitor(bool enabled);

/**
 * \brief Return whether context switch monitoring is enabled.
 */
bool spdk_reactor_context_switch_monitor_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
