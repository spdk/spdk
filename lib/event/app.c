/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/version.h"

#include "spdk_internal/event.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#define SPDK_APP_DEFAULT_LOG_LEVEL		SPDK_LOG_NOTICE
#define SPDK_APP_DEFAULT_LOG_PRINT_LEVEL	SPDK_LOG_INFO
#define SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES	SPDK_DEFAULT_NUM_TRACE_ENTRIES

#define SPDK_APP_DPDK_DEFAULT_MEM_SIZE		-1
#define SPDK_APP_DPDK_DEFAULT_MASTER_CORE	-1
#define SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL	-1
#define SPDK_APP_DPDK_DEFAULT_CORE_MASK		"0x1"
#define SPDK_APP_DPDK_DEFAULT_BASE_VIRTADDR	0x200000000000
#define SPDK_APP_DEFAULT_CORE_LIMIT		0x140000000 /* 5 GiB */

struct spdk_app {
	const char			*json_config_file;
	bool				json_config_ignore_errors;
	const char			*rpc_addr;
	int				shm_id;
	spdk_app_shutdown_cb		shutdown_cb;
	int				rc;
};

static struct spdk_app g_spdk_app;
static spdk_msg_fn g_start_fn = NULL;
static void *g_start_arg = NULL;
static struct spdk_thread *g_app_thread = NULL;
static bool g_delay_subsystem_init = false;
static bool g_shutdown_sig_received = false;
static char *g_executable_name;
static struct spdk_app_opts g_default_opts;

int
spdk_app_get_shm_id(void)
{
	return g_spdk_app.shm_id;
}

/* append one empty option to indicate the end of the array */
static const struct option g_cmdline_options[] = {
#define CONFIG_FILE_OPT_IDX	'c'
	{"config",			required_argument,	NULL, CONFIG_FILE_OPT_IDX},
#define LIMIT_COREDUMP_OPT_IDX 'd'
	{"limit-coredump",		no_argument,		NULL, LIMIT_COREDUMP_OPT_IDX},
#define TPOINT_GROUP_MASK_OPT_IDX 'e'
	{"tpoint-group-mask",		required_argument,	NULL, TPOINT_GROUP_MASK_OPT_IDX},
#define SINGLE_FILE_SEGMENTS_OPT_IDX 'g'
	{"single-file-segments",	no_argument,		NULL, SINGLE_FILE_SEGMENTS_OPT_IDX},
#define HELP_OPT_IDX		'h'
	{"help",			no_argument,		NULL, HELP_OPT_IDX},
#define SHM_ID_OPT_IDX		'i'
	{"shm-id",			required_argument,	NULL, SHM_ID_OPT_IDX},
#define CPUMASK_OPT_IDX		'm'
	{"cpumask",			required_argument,	NULL, CPUMASK_OPT_IDX},
#define MEM_CHANNELS_OPT_IDX	'n'
	{"mem-channels",		required_argument,	NULL, MEM_CHANNELS_OPT_IDX},
#define MASTER_CORE_OPT_IDX	'p'
	{"master-core",			required_argument,	NULL, MASTER_CORE_OPT_IDX},
#define RPC_SOCKET_OPT_IDX	'r'
	{"rpc-socket",			required_argument,	NULL, RPC_SOCKET_OPT_IDX},
#define MEM_SIZE_OPT_IDX	's'
	{"mem-size",			required_argument,	NULL, MEM_SIZE_OPT_IDX},
#define NO_PCI_OPT_IDX		'u'
	{"no-pci",			no_argument,		NULL, NO_PCI_OPT_IDX},
#define VERSION_OPT_IDX		'v'
	{"version",			no_argument,		NULL, VERSION_OPT_IDX},
#define PCI_BLACKLIST_OPT_IDX	'B'
	{"pci-blacklist",		required_argument,	NULL, PCI_BLACKLIST_OPT_IDX},
#define LOGFLAG_OPT_IDX		'L'
	{"logflag",			required_argument,	NULL, LOGFLAG_OPT_IDX},
#define HUGE_UNLINK_OPT_IDX	'R'
	{"huge-unlink",			no_argument,		NULL, HUGE_UNLINK_OPT_IDX},
#define PCI_WHITELIST_OPT_IDX	'W'
	{"pci-whitelist",		required_argument,	NULL, PCI_WHITELIST_OPT_IDX},
#define SILENCE_NOTICELOG_OPT_IDX 257
	{"silence-noticelog",		no_argument,		NULL, SILENCE_NOTICELOG_OPT_IDX},
#define WAIT_FOR_RPC_OPT_IDX	258
	{"wait-for-rpc",		no_argument,		NULL, WAIT_FOR_RPC_OPT_IDX},
#define HUGE_DIR_OPT_IDX	259
	{"huge-dir",			required_argument,	NULL, HUGE_DIR_OPT_IDX},
#define NUM_TRACE_ENTRIES_OPT_IDX	260
	{"num-trace-entries",		required_argument,	NULL, NUM_TRACE_ENTRIES_OPT_IDX},
#define MAX_REACTOR_DELAY_OPT_IDX	261
	{"max-delay",			required_argument,	NULL, MAX_REACTOR_DELAY_OPT_IDX},
#define JSON_CONFIG_OPT_IDX		262
	{"json",			required_argument,	NULL, JSON_CONFIG_OPT_IDX},
#define JSON_CONFIG_IGNORE_INIT_ERRORS_IDX	263
	{"json-ignore-init-errors",	no_argument,		NULL, JSON_CONFIG_IGNORE_INIT_ERRORS_IDX},
#define IOVA_MODE_OPT_IDX	264
	{"iova-mode",			required_argument,	NULL, IOVA_MODE_OPT_IDX},
#define BASE_VIRTADDR_OPT_IDX	265
	{"base-virtaddr",		required_argument,	NULL, BASE_VIRTADDR_OPT_IDX},
};

static void
app_start_shutdown(void *ctx)
{
	if (g_spdk_app.shutdown_cb) {
		g_spdk_app.shutdown_cb();
		g_spdk_app.shutdown_cb = NULL;
	} else {
		spdk_app_stop(0);
	}
}

void
spdk_app_start_shutdown(void)
{
	spdk_thread_send_critical_msg(g_app_thread, app_start_shutdown);
}

static void
__shutdown_signal(int signo)
{
	if (!g_shutdown_sig_received) {
		g_shutdown_sig_received = true;
		spdk_app_start_shutdown();
	}
}

static int
app_opts_validate(const char *app_opts)
{
	int i = 0, j;

	for (i = 0; app_opts[i] != '\0'; i++) {
		/* ignore getopt control characters */
		if (app_opts[i] == ':' || app_opts[i] == '+' || app_opts[i] == '-') {
			continue;
		}

		for (j = 0; SPDK_APP_GETOPT_STRING[j] != '\0'; j++) {
			if (app_opts[i] == SPDK_APP_GETOPT_STRING[j]) {
				return app_opts[i];
			}
		}
	}
	return 0;
}

void
spdk_app_opts_init(struct spdk_app_opts *opts)
{
	if (!opts) {
		return;
	}

	memset(opts, 0, sizeof(*opts));

	opts->enable_coredump = true;
	opts->shm_id = -1;
	opts->mem_size = SPDK_APP_DPDK_DEFAULT_MEM_SIZE;
	opts->master_core = SPDK_APP_DPDK_DEFAULT_MASTER_CORE;
	opts->mem_channel = SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL;
	opts->reactor_mask = SPDK_APP_DPDK_DEFAULT_CORE_MASK;
	opts->base_virtaddr = SPDK_APP_DPDK_DEFAULT_BASE_VIRTADDR;
	opts->print_level = SPDK_APP_DEFAULT_LOG_PRINT_LEVEL;
	opts->rpc_addr = SPDK_DEFAULT_RPC_ADDR;
	opts->num_entries = SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES;
	opts->delay_subsystem_init = false;
}

static int
app_setup_signal_handlers(struct spdk_app_opts *opts)
{
	struct sigaction	sigact;
	sigset_t		sigmask;
	int			rc;

	sigemptyset(&sigmask);
	memset(&sigact, 0, sizeof(sigact));
	sigemptyset(&sigact.sa_mask);

	sigact.sa_handler = SIG_IGN;
	rc = sigaction(SIGPIPE, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGPIPE) failed\n");
		return rc;
	}

	/* Install the same handler for SIGINT and SIGTERM */
	g_shutdown_sig_received = false;
	sigact.sa_handler = __shutdown_signal;
	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGINT) failed\n");
		return rc;
	}
	sigaddset(&sigmask, SIGINT);

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGTERM) failed\n");
		return rc;
	}
	sigaddset(&sigmask, SIGTERM);

	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	return 0;
}

static void
app_start_application(void)
{
	assert(spdk_get_thread() == g_app_thread);

	g_start_fn(g_start_arg);
}

static void
app_start_rpc(int rc, void *arg1)
{
	if (rc) {
		spdk_app_stop(rc);
		return;
	}

	spdk_rpc_initialize(g_spdk_app.rpc_addr);
	if (!g_delay_subsystem_init) {
		spdk_rpc_set_state(SPDK_RPC_RUNTIME);
		app_start_application();
	}
}

static int
app_opts_add_pci_addr(struct spdk_app_opts *opts, struct spdk_pci_addr **list, char *bdf)
{
	struct spdk_pci_addr *tmp = *list;
	size_t i = opts->num_pci_addr;

	tmp = realloc(tmp, sizeof(*tmp) * (i + 1));
	if (tmp == NULL) {
		SPDK_ERRLOG("realloc error\n");
		return -ENOMEM;
	}

	*list = tmp;
	if (spdk_pci_addr_parse(*list + i, bdf) < 0) {
		SPDK_ERRLOG("Invalid address %s\n", bdf);
		return -EINVAL;
	}

	opts->num_pci_addr++;
	return 0;
}

static int
app_setup_env(struct spdk_app_opts *opts)
{
	struct spdk_env_opts env_opts = {};
	int rc;

	if (opts == NULL) {
		rc = spdk_env_init(NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to reinitialize SPDK env\n");
		}

		return rc;
	}

	spdk_env_opts_init(&env_opts);

	env_opts.name = opts->name;
	env_opts.core_mask = opts->reactor_mask;
	env_opts.shm_id = opts->shm_id;
	env_opts.mem_channel = opts->mem_channel;
	env_opts.master_core = opts->master_core;
	env_opts.mem_size = opts->mem_size;
	env_opts.hugepage_single_segments = opts->hugepage_single_segments;
	env_opts.unlink_hugepage = opts->unlink_hugepage;
	env_opts.hugedir = opts->hugedir;
	env_opts.no_pci = opts->no_pci;
	env_opts.num_pci_addr = opts->num_pci_addr;
	env_opts.pci_blacklist = opts->pci_blacklist;
	env_opts.pci_whitelist = opts->pci_whitelist;
	env_opts.base_virtaddr = opts->base_virtaddr;
	env_opts.env_context = opts->env_context;
	env_opts.iova_mode = opts->iova_mode;

	rc = spdk_env_init(&env_opts);
	free(env_opts.pci_blacklist);
	free(env_opts.pci_whitelist);

	if (rc < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
	}

	return rc;
}

static int
app_setup_trace(struct spdk_app_opts *opts)
{
	char		shm_name[64];
	uint64_t	tpoint_group_mask;
	char		*end;

	if (opts->shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", opts->name, opts->shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", opts->name, (int)getpid());
	}

	if (spdk_trace_init(shm_name, opts->num_entries) != 0) {
		return -1;
	}

	if (opts->tpoint_group_mask != NULL) {
		errno = 0;
		tpoint_group_mask = strtoull(opts->tpoint_group_mask, &end, 16);
		if (*end != '\0' || errno) {
			SPDK_ERRLOG("invalid tpoint mask %s\n", opts->tpoint_group_mask);
		} else {
			SPDK_NOTICELOG("Tracepoint Group Mask %s specified.\n", opts->tpoint_group_mask);
			SPDK_NOTICELOG("Use 'spdk_trace -s %s %s %d' to capture a snapshot of events at runtime.\n",
				       opts->name,
				       opts->shm_id >= 0 ? "-i" : "-p",
				       opts->shm_id >= 0 ? opts->shm_id : getpid());
#if defined(__linux__)
			SPDK_NOTICELOG("Or copy /dev/shm%s for offline analysis/debug.\n", shm_name);
#endif
			spdk_trace_set_tpoint_group_mask(tpoint_group_mask);
		}
	}

	return 0;
}

static void
bootstrap_fn(void *arg1)
{
	if (g_spdk_app.json_config_file) {
		g_delay_subsystem_init = false;
		spdk_app_json_config_load(g_spdk_app.json_config_file, g_spdk_app.rpc_addr, app_start_rpc,
					  NULL, !g_spdk_app.json_config_ignore_errors);
	} else {
		if (!g_delay_subsystem_init) {
			spdk_subsystem_init(app_start_rpc, NULL);
		} else {
			spdk_rpc_initialize(g_spdk_app.rpc_addr);
		}
	}
}

int
spdk_app_start(struct spdk_app_opts *opts, spdk_msg_fn start_fn,
	       void *arg1)
{
	int			rc;
	char			*tty;
	struct spdk_cpuset	tmp_cpumask = {};
	static bool		g_env_was_setup = false;

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return 1;
	}

	if (opts->config_file) {
		SPDK_ERRLOG("opts->config_file is deprecated.  Use opts->json_config_file instead.\n");
		/* For now we will just treat config_file as json_config_file.  But if both were
		 * specified we will return an error here.
		 */
		if (opts->json_config_file) {
			SPDK_ERRLOG("Setting both opts->config_file and opts->json_config_file not allowed.\n");
			return 1;
		}
		opts->json_config_file = opts->config_file;
	}

	if (!start_fn) {
		SPDK_ERRLOG("start_fn should not be NULL\n");
		return 1;
	}

	tty = ttyname(STDERR_FILENO);
	if (opts->print_level > SPDK_LOG_WARN &&
	    isatty(STDERR_FILENO) &&
	    tty &&
	    !strncmp(tty, "/dev/tty", strlen("/dev/tty"))) {
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using --silence-noticelog to disable logging to stderr and\n");
		printf("monitor syslog, or redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	spdk_log_set_print_level(opts->print_level);

#ifndef SPDK_NO_RLIMIT
	if (opts->enable_coredump) {
		struct rlimit core_limits;

		core_limits.rlim_cur = core_limits.rlim_max = SPDK_APP_DEFAULT_CORE_LIMIT;
		setrlimit(RLIMIT_CORE, &core_limits);
	}
#endif

	memset(&g_spdk_app, 0, sizeof(g_spdk_app));
	g_spdk_app.json_config_file = opts->json_config_file;
	g_spdk_app.json_config_ignore_errors = opts->json_config_ignore_errors;
	g_spdk_app.rpc_addr = opts->rpc_addr;
	g_spdk_app.shm_id = opts->shm_id;
	g_spdk_app.shutdown_cb = opts->shutdown_cb;
	g_spdk_app.rc = 0;

	spdk_log_set_level(SPDK_APP_DEFAULT_LOG_LEVEL);

	/* Pass NULL to app_setup_env if SPDK app has been set up, in order to
	 * indicate that this is a reinitialization.
	 */
	if (app_setup_env(g_env_was_setup ? NULL : opts) < 0) {
		return 1;
	}

	spdk_log_open(opts->log);
	SPDK_NOTICELOG("Total cores available: %d\n", spdk_env_get_core_count());

	if ((rc = spdk_reactors_init()) != 0) {
		SPDK_ERRLOG("Reactor Initilization failed: rc = %d\n", rc);
		return 1;
	}

	spdk_cpuset_set_cpu(&tmp_cpumask, spdk_env_get_current_core(), true);

	/* Now that the reactors have been initialized, we can create an
	 * initialization thread. */
	g_app_thread = spdk_thread_create("app_thread", &tmp_cpumask);
	if (!g_app_thread) {
		SPDK_ERRLOG("Unable to create an spdk_thread for initialization\n");
		return 1;
	}

	/*
	 * Disable and ignore trace setup if setting num_entries
	 * to be 0.
	 *
	 * Note the call to app_setup_trace() is located here
	 * ahead of app_setup_signal_handlers().
	 * That's because there is not an easy/direct clean
	 * way of unwinding alloc'd resources that can occur
	 * in app_setup_signal_handlers().
	 */
	if (opts->num_entries != 0 && app_setup_trace(opts) != 0) {
		return 1;
	}

	if (app_setup_signal_handlers(opts) != 0) {
		return 1;
	}

	g_delay_subsystem_init = opts->delay_subsystem_init;
	g_start_fn = start_fn;
	g_start_arg = arg1;

	spdk_thread_send_msg(g_app_thread, bootstrap_fn, NULL);

	/* This blocks until spdk_app_stop is called */
	spdk_reactors_start();

	g_env_was_setup = true;

	return g_spdk_app.rc;
}

void
spdk_app_fini(void)
{
	spdk_trace_cleanup();
	spdk_reactors_fini();
	spdk_env_fini();
	spdk_log_close();
}

static void
app_stop(void *arg1)
{
	spdk_rpc_finish();
	spdk_subsystem_fini(spdk_reactors_stop, NULL);
}

void
spdk_app_stop(int rc)
{
	if (rc) {
		SPDK_WARNLOG("spdk_app_stop'd on non-zero\n");
	}
	g_spdk_app.rc = rc;
	/*
	 * We want to run spdk_subsystem_fini() from the same thread where spdk_subsystem_init()
	 * was called.
	 */
	spdk_thread_send_msg(g_app_thread, app_stop, NULL);
}

static void
usage(void (*app_usage)(void))
{
	printf("%s [options]\n", g_executable_name);
	printf("options:\n");
	printf(" -c, --config <config>     JSON config file (default %s)\n",
	       g_default_opts.json_config_file != NULL ? g_default_opts.json_config_file : "none");
	printf("     --json <config>       JSON config file (default %s)\n",
	       g_default_opts.json_config_file != NULL ? g_default_opts.json_config_file : "none");
	printf("     --json-ignore-init-errors\n");
	printf("                           don't exit on invalid config entry\n");
	printf(" -d, --limit-coredump      do not set max coredump size to RLIM_INFINITY\n");
	printf(" -g, --single-file-segments\n");
	printf("                           force creating just one hugetlbfs file\n");
	printf(" -h, --help                show this usage\n");
	printf(" -i, --shm-id <id>         shared memory ID (optional)\n");
	printf(" -m, --cpumask <mask>      core mask for DPDK\n");
	printf(" -n, --mem-channels <num>  channel number of memory channels used for DPDK\n");
	printf(" -p, --master-core <id>    master (primary) core for DPDK\n");
	printf(" -r, --rpc-socket <path>   RPC listen address (default %s)\n", SPDK_DEFAULT_RPC_ADDR);
	printf(" -s, --mem-size <size>     memory size in MB for DPDK (default: ");
#ifndef __linux__
	if (g_default_opts.mem_size <= 0) {
		printf("all hugepage memory)\n");
	} else
#endif
	{
		printf("%dMB)\n", g_default_opts.mem_size >= 0 ? g_default_opts.mem_size : 0);
	}
	printf("     --silence-noticelog   disable notice level logging to stderr\n");
	printf(" -u, --no-pci              disable PCI access\n");
	printf("     --wait-for-rpc        wait for RPCs to initialize subsystems\n");
	printf("     --max-delay <num>     maximum reactor delay (in microseconds)\n");
	printf(" -B, --pci-blacklist <bdf>\n");
	printf("                           pci addr to blacklist (can be used more than once)\n");
	printf(" -R, --huge-unlink         unlink huge files after initialization\n");
	printf(" -v, --version             print SPDK version\n");
	printf(" -W, --pci-whitelist <bdf>\n");
	printf("                           pci addr to whitelist (-B and -W cannot be used at the same time)\n");
	printf("     --huge-dir <path>     use a specific hugetlbfs mount to reserve memory from\n");
	printf("     --iova-mode <pa/va>   set IOVA mode ('pa' for IOVA_PA and 'va' for IOVA_VA)\n");
	printf("     --base-virtaddr <addr>      the base virtual address for DPDK (default: 0x200000000000)\n");
	printf("     --num-trace-entries <num>   number of trace entries for each core, must be power of 2, setting 0 to disable trace (default %d)\n",
	       SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES);
	spdk_log_usage(stdout, "-L");
	spdk_trace_mask_usage(stdout, "-e");
	if (app_usage) {
		app_usage();
	}
}

spdk_app_parse_args_rvals_t
spdk_app_parse_args(int argc, char **argv, struct spdk_app_opts *opts,
		    const char *app_getopt_str, struct option *app_long_opts,
		    int (*app_parse)(int ch, char *arg),
		    void (*app_usage)(void))
{
	int ch, rc, opt_idx, global_long_opts_len, app_long_opts_len;
	struct option *cmdline_options;
	char *cmdline_short_opts = NULL;
	enum spdk_app_parse_args_rvals retval = SPDK_APP_PARSE_ARGS_FAIL;
	long int tmp;

	memcpy(&g_default_opts, opts, sizeof(g_default_opts));

	if (opts->config_file && access(opts->config_file, R_OK) != 0) {
		SPDK_WARNLOG("Can't read JSON configuration file '%s'\n", opts->config_file);
		opts->config_file = NULL;
	}

	if (opts->json_config_file && access(opts->json_config_file, R_OK) != 0) {
		SPDK_WARNLOG("Can't read JSON configuration file '%s'\n", opts->json_config_file);
		opts->json_config_file = NULL;
	}

	if (app_long_opts == NULL) {
		app_long_opts_len = 0;
	} else {
		for (app_long_opts_len = 0;
		     app_long_opts[app_long_opts_len].name != NULL;
		     app_long_opts_len++);
	}

	global_long_opts_len = SPDK_COUNTOF(g_cmdline_options);

	cmdline_options = calloc(global_long_opts_len + app_long_opts_len + 1, sizeof(*cmdline_options));
	if (!cmdline_options) {
		SPDK_ERRLOG("Out of memory\n");
		return SPDK_APP_PARSE_ARGS_FAIL;
	}

	memcpy(&cmdline_options[0], g_cmdline_options, sizeof(g_cmdline_options));
	if (app_long_opts) {
		memcpy(&cmdline_options[global_long_opts_len], app_long_opts,
		       app_long_opts_len * sizeof(*app_long_opts));
	}

	if (app_getopt_str != NULL) {
		ch = app_opts_validate(app_getopt_str);
		if (ch) {
			SPDK_ERRLOG("Duplicated option '%c' between the generic and application specific spdk opts.\n",
				    ch);
			goto out;
		}
	}

	cmdline_short_opts = spdk_sprintf_alloc("%s%s", app_getopt_str, SPDK_APP_GETOPT_STRING);
	if (!cmdline_short_opts) {
		SPDK_ERRLOG("Out of memory\n");
		goto out;
	}

	g_executable_name = argv[0];

	while ((ch = getopt_long(argc, argv, cmdline_short_opts, cmdline_options, &opt_idx)) != -1) {
		switch (ch) {
		case CONFIG_FILE_OPT_IDX:
		case JSON_CONFIG_OPT_IDX:
			opts->json_config_file = optarg;
			break;
		case JSON_CONFIG_IGNORE_INIT_ERRORS_IDX:
			opts->json_config_ignore_errors = true;
			break;
		case LIMIT_COREDUMP_OPT_IDX:
			opts->enable_coredump = false;
			break;
		case TPOINT_GROUP_MASK_OPT_IDX:
			opts->tpoint_group_mask = optarg;
			break;
		case SINGLE_FILE_SEGMENTS_OPT_IDX:
			opts->hugepage_single_segments = true;
			break;
		case HELP_OPT_IDX:
			usage(app_usage);
			retval = SPDK_APP_PARSE_ARGS_HELP;
			goto out;
		case SHM_ID_OPT_IDX:
			opts->shm_id = spdk_strtol(optarg, 0);
			if (opts->shm_id < 0) {
				SPDK_ERRLOG("Invalid shared memory ID %s\n", optarg);
				goto out;
			}
			break;
		case CPUMASK_OPT_IDX:
			opts->reactor_mask = optarg;
			break;
		case MEM_CHANNELS_OPT_IDX:
			opts->mem_channel = spdk_strtol(optarg, 0);
			if (opts->mem_channel < 0) {
				SPDK_ERRLOG("Invalid memory channel %s\n", optarg);
				goto out;
			}
			break;
		case MASTER_CORE_OPT_IDX:
			opts->master_core = spdk_strtol(optarg, 0);
			if (opts->master_core < 0) {
				SPDK_ERRLOG("Invalid master core %s\n", optarg);
				goto out;
			}
			break;
		case SILENCE_NOTICELOG_OPT_IDX:
			opts->print_level = SPDK_LOG_WARN;
			break;
		case RPC_SOCKET_OPT_IDX:
			opts->rpc_addr = optarg;
			break;
		case MEM_SIZE_OPT_IDX: {
			uint64_t mem_size_mb;
			bool mem_size_has_prefix;

			rc = spdk_parse_capacity(optarg, &mem_size_mb, &mem_size_has_prefix);
			if (rc != 0) {
				SPDK_ERRLOG("invalid memory pool size `-s %s`\n", optarg);
				usage(app_usage);
				goto out;
			}

			if (mem_size_has_prefix) {
				/* the mem size is in MB by default, so if a prefix was
				 * specified, we need to manually convert to MB.
				 */
				mem_size_mb /= 1024 * 1024;
			}

			if (mem_size_mb > INT_MAX) {
				SPDK_ERRLOG("invalid memory pool size `-s %s`\n", optarg);
				usage(app_usage);
				goto out;
			}

			opts->mem_size = (int) mem_size_mb;
			break;
		}
		case NO_PCI_OPT_IDX:
			opts->no_pci = true;
			break;
		case WAIT_FOR_RPC_OPT_IDX:
			opts->delay_subsystem_init = true;
			break;
		case PCI_BLACKLIST_OPT_IDX:
			if (opts->pci_whitelist) {
				free(opts->pci_whitelist);
				opts->pci_whitelist = NULL;
				SPDK_ERRLOG("-B and -W cannot be used at the same time\n");
				usage(app_usage);
				goto out;
			}

			rc = app_opts_add_pci_addr(opts, &opts->pci_blacklist, optarg);
			if (rc != 0) {
				free(opts->pci_blacklist);
				opts->pci_blacklist = NULL;
				goto out;
			}
			break;
		case LOGFLAG_OPT_IDX:
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				SPDK_ERRLOG("unknown flag\n");
				usage(app_usage);
				goto out;
			}
#ifdef DEBUG
			opts->print_level = SPDK_LOG_DEBUG;
#endif
			break;
		case HUGE_UNLINK_OPT_IDX:
			opts->unlink_hugepage = true;
			break;
		case PCI_WHITELIST_OPT_IDX:
			if (opts->pci_blacklist) {
				free(opts->pci_blacklist);
				opts->pci_blacklist = NULL;
				SPDK_ERRLOG("-B and -W cannot be used at the same time\n");
				usage(app_usage);
				goto out;
			}

			rc = app_opts_add_pci_addr(opts, &opts->pci_whitelist, optarg);
			if (rc != 0) {
				free(opts->pci_whitelist);
				opts->pci_whitelist = NULL;
				goto out;
			}
			break;
		case BASE_VIRTADDR_OPT_IDX:
			tmp = spdk_strtoll(optarg, 0);
			if (tmp <= 0) {
				SPDK_ERRLOG("Invalid base-virtaddr %s\n", optarg);
				usage(app_usage);
				goto out;
			}
			opts->base_virtaddr = (uint64_t)tmp;
			break;
		case HUGE_DIR_OPT_IDX:
			opts->hugedir = optarg;
			break;
		case IOVA_MODE_OPT_IDX:
			opts->iova_mode = optarg;
			break;
		case NUM_TRACE_ENTRIES_OPT_IDX:
			tmp = spdk_strtoll(optarg, 0);
			if (tmp < 0) {
				SPDK_ERRLOG("Invalid num-trace-entries %s\n", optarg);
				usage(app_usage);
				goto out;
			}
			opts->num_entries = (uint64_t)tmp;
			if (opts->num_entries > 0 && opts->num_entries & (opts->num_entries - 1)) {
				SPDK_ERRLOG("num-trace-entries must be power of 2\n");
				usage(app_usage);
				goto out;
			}
			break;
		case MAX_REACTOR_DELAY_OPT_IDX:
			SPDK_ERRLOG("Deprecation warning: The maximum allowed latency parameter is no longer supported.\n");
			break;
		case VERSION_OPT_IDX:
			printf(SPDK_VERSION_STRING"\n");
			retval = SPDK_APP_PARSE_ARGS_HELP;
			goto out;
		case '?':
			/*
			 * In the event getopt() above detects an option
			 * in argv that is NOT in the getopt_str,
			 * getopt() will return a '?' indicating failure.
			 */
			usage(app_usage);
			goto out;
		default:
			rc = app_parse(ch, optarg);
			if (rc) {
				SPDK_ERRLOG("Parsing application specific arguments failed: %d\n", rc);
				goto out;
			}
		}
	}

	if (opts->json_config_file && opts->delay_subsystem_init) {
		SPDK_ERRLOG("JSON configuration file can't be used together with --wait-for-rpc.\n");
		goto out;
	}

	retval = SPDK_APP_PARSE_ARGS_SUCCESS;
out:
	if (retval != SPDK_APP_PARSE_ARGS_SUCCESS) {
		free(opts->pci_blacklist);
		opts->pci_blacklist = NULL;
		free(opts->pci_whitelist);
		opts->pci_whitelist = NULL;
	}
	free(cmdline_short_opts);
	free(cmdline_options);
	return retval;
}

void
spdk_app_usage(void)
{
	if (g_executable_name == NULL) {
		SPDK_ERRLOG("%s not valid before calling spdk_app_parse_args()\n", __func__);
		return;
	}

	usage(NULL);
}

static void
rpc_framework_start_init_cpl(int rc, void *arg1)
{
	struct spdk_jsonrpc_request *request = arg1;

	assert(spdk_get_thread() == g_app_thread);

	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "framework_initialization failed");
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
	app_start_application();

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_framework_start_init(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_start_init requires no parameters");
		return;
	}

	spdk_subsystem_init(rpc_framework_start_init_cpl, request);
}
SPDK_RPC_REGISTER("framework_start_init", rpc_framework_start_init, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(framework_start_init, start_subsystem_init)

struct subsystem_init_poller_ctx {
	struct spdk_poller *init_poller;
	struct spdk_jsonrpc_request *request;
};

static int
rpc_subsystem_init_poller_ctx(void *ctx)
{
	struct subsystem_init_poller_ctx *poller_ctx = ctx;

	if (spdk_rpc_get_state() == SPDK_RPC_RUNTIME) {
		spdk_jsonrpc_send_bool_response(poller_ctx->request, true);
		spdk_poller_unregister(&poller_ctx->init_poller);
		free(poller_ctx);
	}

	return SPDK_POLLER_BUSY;
}

static void
rpc_framework_wait_init(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct subsystem_init_poller_ctx *ctx;

	if (spdk_rpc_get_state() == SPDK_RPC_RUNTIME) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		ctx = malloc(sizeof(struct subsystem_init_poller_ctx));
		if (ctx == NULL) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to allocate memory for the request context\n");
			return;
		}
		ctx->request = request;
		ctx->init_poller = SPDK_POLLER_REGISTER(rpc_subsystem_init_poller_ctx, ctx, 0);
	}
}
SPDK_RPC_REGISTER("framework_wait_init", rpc_framework_wait_init,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(framework_wait_init, wait_subsystem_init)
