/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.  All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/init.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Event handler function.
 *
 * \param arg1 Argument 1.
 * \param arg2 Argument 2.
 */
typedef void (*spdk_event_fn)(void *arg1, void *arg2);

/**
 * \brief An event is a function that is passed to and called on an lcore.
 */
struct spdk_event;

/**
 * \brief A poller is a function that is repeatedly called on an lcore.
 */
struct spdk_poller;

/**
 * Callback function for customized shutdown handling of application.
 */
typedef void (*spdk_app_shutdown_cb)(void);

/**
 * Signal handler function.
 *
 * \param signal Signal number.
 */
typedef void (*spdk_sighandler_t)(int signal);

/**
 * \brief Event framework initialization options
 */
struct spdk_app_opts {
	const char *name;
	const char *json_config_file;
	bool json_config_ignore_errors;

	/* Hole at bytes 17-23. */
	uint8_t	reserved17[7];

	const char *rpc_addr; /* Can be UNIX domain socket path or IP address + TCP port */
	const char *reactor_mask;
	const char *tpoint_group_mask;

	int shm_id;

	/* Hole at bytes 52-55. */
	uint8_t			reserved52[4];

	spdk_app_shutdown_cb	shutdown_cb;

	bool			enable_coredump;

	/* Hole at bytes 65-67. */
	uint8_t			reserved65[3];

	int			mem_channel;
	int			main_core;
	int			mem_size;
	bool			no_pci;
	bool			hugepage_single_segments;
	bool			unlink_hugepage;

	/* Hole at bytes 83-85. */
	uint8_t			reserved83[5];

	const char		*hugedir;
	enum spdk_log_level	print_level;

	/* Hole at bytes 100-103. */
	uint8_t			reserved100[4];

	size_t			num_pci_addr;
	struct spdk_pci_addr	*pci_blocked;
	struct spdk_pci_addr	*pci_allowed;
	const char		*iova_mode;

	/* Wait for the associated RPC before initializing subsystems
	 * when this flag is enabled.
	 */
	bool			delay_subsystem_init;

	/* Hole at bytes 137-143. */
	uint8_t			reserved137[7];

	/* Number of trace entries allocated for each core */
	uint64_t		num_entries;

	/** Opaque context for use of the env implementation. */
	void			*env_context;

	/**
	 * for passing user-provided log call
	 */
	logfunc         *log;

	uint64_t		base_virtaddr;

	/**
	 * The size of spdk_app_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * After that, new added fields should be put after opts_size.
	 */
	size_t opts_size;

	/**
	 * Disable default signal handlers.
	 * If set to `true`, the shutdown process is not started implicitly by
	 * process signals, hence the application is responsible for calling
	 * spdk_app_start_shutdown().
	 *
	 * Default is `false`.
	 */
	bool disable_signal_handlers;

	/* Hole at bytes 185-191. */
	uint8_t reserved185[7];

	/**
	 * The allocated size for the message pool used by the threading library.
	 *
	 * Default is `SPDK_DEFAULT_MSG_MEMPOOL_SIZE`.
	 */
	size_t msg_mempool_size;

	/*
	 *  If non-NULL, a string array of allowed RPC methods.
	 */
	const char **rpc_allowlist;

	/**
	 * Used to pass vf_token to vfio_pci driver through DPDK.
	 * The vf_token is an UUID that shared between SR-IOV PF and VF.
	 */
	const char		*vf_token;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_app_opts) == 216, "Incorrect size");

/**
 * Initialize the default value of opts
 *
 * \param opts Data structure where SPDK will initialize the default options.
 * \param opts_size Must be set to sizeof(struct spdk_app_opts).
 */
void spdk_app_opts_init(struct spdk_app_opts *opts, size_t opts_size);

/**
 * Start the framework.
 *
 * Before calling this function, opts must be initialized by
 * spdk_app_opts_init(). Once started, the framework will call start_fn on
 * an spdk_thread running on the current system thread with the
 * argument provided.
 *
 * If opts->delay_subsystem_init is set
 * (e.g. through --wait-for-rpc flag in spdk_app_parse_args())
 * this function will only start a limited RPC server accepting
 * only a few RPC commands - mostly related to pre-initialization.
 * With this option, the framework won't be started and start_fn
 * won't be called until the user sends an `rpc_framework_start_init`
 * RPC command, which marks the pre-initialization complete and
 * allows start_fn to be finally called.
 *
 * This call will block until spdk_app_stop() is called. If an error
 * condition occurs during the initialization code within spdk_app_start(),
 * this function will immediately return before invoking start_fn.
 *
 * \param opts_user Initialization options used for this application. It should not be
 *             NULL. And the opts_size value inside the opts structure should not be zero.
 * \param start_fn Entry point that will execute on an internally created thread
 *                 once the framework has been started.
 * \param ctx Argument passed to function start_fn.
 *
 * \return 0 on success or non-zero on failure.
 */
int spdk_app_start(struct spdk_app_opts *opts_user, spdk_msg_fn start_fn,
		   void *ctx);

/**
 * Perform final shutdown operations on an application using the event framework.
 */
void spdk_app_fini(void);

/**
 * Start shutting down the framework.
 *
 * Typically this function is not called directly, and the shutdown process is
 * started implicitly by a process signal. But in applications that are using
 * SPDK for a subset of its process threads, this function can be called in lieu
 * of a signal.
 */
void spdk_app_start_shutdown(void);

/**
 * Stop the framework.
 *
 * This does not wait for all threads to exit. Instead, it kicks off the shutdown
 * process and returns. Once the shutdown process is complete, spdk_app_start()
 * will return.
 *
 * \param rc The rc value specified here will be returned to caller of spdk_app_start().
 */
void spdk_app_stop(int rc);

/**
 * Return the shared memory id for this application.
 *
 * \return shared memory id.
 */
int spdk_app_get_shm_id(void);

/**
 * Convert a string containing a CPU core mask into a bitmask
 *
 * \param mask String containing a CPU core mask.
 * \param cpumask Bitmask of CPU cores.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask);

/**
 * Get the mask of the CPU cores active for this application
 *
 * \return the bitmask of the active CPU cores.
 */
const struct spdk_cpuset *spdk_app_get_core_mask(void);

#define SPDK_APP_GETOPT_STRING "c:de:ghi:m:n:p:r:s:uvA:B:L:RW:"

enum spdk_app_parse_args_rvals {
	SPDK_APP_PARSE_ARGS_HELP = 0,
	SPDK_APP_PARSE_ARGS_SUCCESS = 1,
	SPDK_APP_PARSE_ARGS_FAIL = 2
};
typedef enum spdk_app_parse_args_rvals spdk_app_parse_args_rvals_t;

/**
 * Helper function for parsing arguments and printing usage messages.
 *
 * \param argc Count of arguments in argv parameter array.
 * \param argv Array of command line arguments.
 * \param opts Default options for the application.
 * \param getopt_str String representing the app-specific command line parameters.
 * Characters in this string must not conflict with characters in SPDK_APP_GETOPT_STRING.
 * \param app_long_opts Array of full-name parameters. Can be NULL.
 * \param parse Function pointer to call if an argument in getopt_str is found.
 * \param usage Function pointer to print usage messages for app-specific command
 *		line parameters.
 *\return SPDK_APP_PARSE_ARGS_FAIL on failure, SPDK_APP_PARSE_ARGS_SUCCESS on
 *        success, SPDK_APP_PARSE_ARGS_HELP if '-h' passed as an option.
 */
spdk_app_parse_args_rvals_t spdk_app_parse_args(int argc, char **argv,
		struct spdk_app_opts *opts, const char *getopt_str,
		const struct option *app_long_opts, int (*parse)(int ch, char *arg),
		void (*usage)(void));

/**
 * Print usage strings for common SPDK command line options.
 *
 * May only be called after spdk_app_parse_args().
 */
void spdk_app_usage(void);

/**
 * Allocate an event to be passed to spdk_event_call().
 *
 * \param lcore Lcore to run this event.
 * \param fn Function used to execute event.
 * \param arg1 Argument passed to function fn.
 * \param arg2 Argument passed to function fn.
 *
 * \return a pointer to the allocated event.
 */
struct spdk_event *spdk_event_allocate(uint32_t lcore, spdk_event_fn fn,
				       void *arg1, void *arg2);

/**
 * Pass the given event to the associated lcore and call the function.
 *
 * \param event Event to execute.
 */
void spdk_event_call(struct spdk_event *event);

/**
 * Enable or disable monitoring of context switches.
 *
 * \param enabled True to enable, false to disable.
 */
void spdk_framework_enable_context_switch_monitor(bool enabled);

/**
 * Return whether context switch monitoring is enabled.
 *
 * \return true if enabled or false otherwise.
 */
bool spdk_framework_context_switch_monitor_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
