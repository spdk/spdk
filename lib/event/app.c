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

#include "spdk_internal/event.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/conf.h"
#include "spdk/trace.h"
#include "spdk/string.h"
#include "spdk/rpc.h"

#define SPDK_APP_DEFAULT_LOG_LEVEL		SPDK_LOG_NOTICE
#define SPDK_APP_DEFAULT_LOG_PRINT_LEVEL	SPDK_LOG_INFO

#define SPDK_APP_DPDK_DEFAULT_MEM_SIZE		-1
#define SPDK_APP_DPDK_DEFAULT_MASTER_CORE	-1
#define SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL	-1
#define SPDK_APP_DPDK_DEFAULT_CORE_MASK		"0x1"

struct spdk_app {
	struct spdk_conf		*config;
	int				shm_id;
	spdk_app_shutdown_cb		shutdown_cb;
	int				rc;
};

static struct spdk_app g_spdk_app;
static struct spdk_event *g_app_start_event = NULL;
static struct spdk_event *g_shutdown_event = NULL;
static int g_init_lcore;
static bool g_delay_subsystem_init = false;
static bool g_shutdown_sig_received = false;

int
spdk_app_get_shm_id(void)
{
	return g_spdk_app.shm_id;
}

/* Global section */
#define GLOBAL_CONFIG_TMPL \
"# Configuration file\n" \
"#\n" \
"# Please write all parameters using ASCII.\n" \
"# The parameter must be quoted if it includes whitespace.\n" \
"#\n" \
"# Configuration syntax:\n" \
"# Spaces at head of line are deleted, other spaces are as separator\n" \
"# Lines starting with '#' are comments and not evaluated.\n" \
"# Lines ending with '\\' are concatenated with the next line.\n" \
"# Bracketed keys are section keys grouping the following value keys.\n" \
"# Number of section key is used as a tag number.\n" \
"#  Ex. [TargetNode1] = TargetNode section key with tag number 1\n" \
"[Global]\n" \
"  Comment \"Global section\"\n" \
"\n" \
"  # Users can restrict work items to only run on certain cores by\n" \
"  #  specifying a ReactorMask.  Default is to allow work items to run\n" \
"  #  on all cores.  Core 0 must be set in the mask if one is specified.\n" \
"  # Default: 0xFFFF (cores 0-15)\n" \
"  ReactorMask \"0x%s\"\n" \
"\n" \
"  # Tracepoint group mask for spdk trace buffers\n" \
"  # Default: 0x0 (all tracepoint groups disabled)\n" \
"  # Set to 0xFFFFFFFFFFFFFFFF to enable all tracepoint groups.\n" \
"  TpointGroupMask \"0x%" PRIX64 "\"\n" \
"\n" \

static void
spdk_app_config_dump_global_section(FILE *fp)
{
	struct spdk_cpuset *coremask;

	if (NULL == fp) {
		return;
	}

	coremask = spdk_app_get_core_mask();

	fprintf(fp, GLOBAL_CONFIG_TMPL, spdk_cpuset_fmt(coremask),
		spdk_trace_get_tpoint_group_mask());
}

int
spdk_app_get_running_config(char **config_str, char *name)
{
	FILE *fp = NULL;
	int fd = -1;
	long length = 0, ret = 0;
	char vbuf[BUFSIZ];
	char config_template[64];

	snprintf(config_template, sizeof(config_template), "/tmp/%s.XXXXXX", name);
	/* Create temporary file to hold config */
	fd = mkstemp(config_template);
	if (fd == -1) {
		SPDK_ERRLOG("mkstemp failed\n");
		return -1;
	}
	fp = fdopen(fd, "wb+");
	if (NULL == fp) {
		SPDK_ERRLOG("error opening tmpfile fd = %d\n", fd);
		return -1;
	}

	/* Buffered IO */
	setvbuf(fp, vbuf, _IOFBF, BUFSIZ);

	spdk_app_config_dump_global_section(fp);
	spdk_subsystem_config(fp);

	length = ftell(fp);

	*config_str = malloc(length + 1);
	if (!*config_str) {
		SPDK_ERRLOG("out-of-memory for config\n");
		fclose(fp);
		return -1;
	}
	fseek(fp, 0, SEEK_SET);
	ret = fread(*config_str, sizeof(char), length, fp);
	if (ret < length) {
		SPDK_ERRLOG("short read\n");
	}
	fclose(fp);
	(*config_str)[length] = '\0';

	return 0;
}

void
spdk_app_start_shutdown(void)
{
	if (g_shutdown_event != NULL) {
		spdk_event_call(g_shutdown_event);
		g_shutdown_event = NULL;
	} else {
		spdk_app_stop(0);
	}
}

static void
__shutdown_signal(int signo)
{
	if (!g_shutdown_sig_received) {
		g_shutdown_sig_received = true;
		spdk_app_start_shutdown();
	}
}

static void
__shutdown_event_cb(void *arg1, void *arg2)
{
	g_spdk_app.shutdown_cb();
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
	opts->reactor_mask = NULL;
	opts->max_delay_us = 0;
	opts->print_level = SPDK_APP_DEFAULT_LOG_PRINT_LEVEL;
	opts->rpc_addr = SPDK_DEFAULT_RPC_ADDR;
	opts->delay_subsystem_init = false;
}

static int
spdk_app_setup_signal_handlers(struct spdk_app_opts *opts)
{
	struct sigaction	sigact;
	sigset_t		sigmask;
	int			rc;

	/* Set up custom shutdown handling if the user requested it. */
	if (opts->shutdown_cb != NULL) {
		g_shutdown_event = spdk_event_allocate(spdk_env_get_current_core(),
						       __shutdown_event_cb,
						       NULL, NULL);
	}

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

	if (opts->usr1_handler != NULL) {
		sigact.sa_handler = opts->usr1_handler;
		rc = sigaction(SIGUSR1, &sigact, NULL);
		if (rc < 0) {
			SPDK_ERRLOG("sigaction(SIGUSR1) failed\n");
			return rc;
		}
		sigaddset(&sigmask, SIGUSR1);
	}

	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	return 0;
}

static void
spdk_app_start_application(void)
{
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
	spdk_event_call(g_app_start_event);
}

static void
spdk_app_start_rpc(void *arg1, void *arg2)
{
	const char *rpc_addr = arg1;

	spdk_rpc_initialize(rpc_addr);
	if (!g_delay_subsystem_init) {
		spdk_app_start_application();
	}
}

static struct spdk_conf *
spdk_app_setup_conf(const char *config_file)
{
	struct spdk_conf *config;
	int rc;

	config = spdk_conf_allocate();
	assert(config != NULL);
	if (config_file) {
		rc = spdk_conf_read(config, config_file);
		if (rc != 0) {
			SPDK_ERRLOG("Could not read config file %s\n", config_file);
			goto error;
		}
		if (spdk_conf_first_section(config) == NULL) {
			SPDK_ERRLOG("Invalid config file %s\n", config_file);
			goto error;
		}
	}
	spdk_conf_set_as_default(config);
	return config;

error:
	spdk_conf_free(config);
	return NULL;
}

static int
spdk_app_opts_add_pci_addr(struct spdk_app_opts *opts, struct spdk_pci_addr **list, char *bdf)
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
spdk_app_read_config_file_global_params(struct spdk_app_opts *opts)
{
	struct spdk_conf_section *sp;
	char *bdf;
	int i, rc = 0;

	sp = spdk_conf_find_section(NULL, "Global");

	if (opts->shm_id == -1) {
		if (sp != NULL) {
			opts->shm_id = spdk_conf_section_get_intval(sp, "SharedMemoryID");
		}
	}

	if (opts->reactor_mask == NULL) {
		if (sp && spdk_conf_section_get_val(sp, "ReactorMask")) {
			opts->reactor_mask = spdk_conf_section_get_val(sp, "ReactorMask");
		} else {
			opts->reactor_mask = SPDK_APP_DPDK_DEFAULT_CORE_MASK;
		}
	}

	if (!opts->no_pci && sp) {
		opts->no_pci = spdk_conf_section_get_boolval(sp, "NoPci", false);
	}

	if (opts->tpoint_group_mask == NULL) {
		if (sp != NULL) {
			opts->tpoint_group_mask = spdk_conf_section_get_val(sp, "TpointGroupMask");
		}
	}

	if (sp == NULL) {
		return 0;
	}

	for (i = 0; ; i++) {
		bdf = spdk_conf_section_get_nmval(sp, "PciBlacklist", i, 0);
		if (!bdf) {
			break;
		}

		rc = spdk_app_opts_add_pci_addr(opts, &opts->pci_blacklist, bdf);
		if (rc != 0) {
			free(opts->pci_blacklist);
			return rc;
		}
	}

	for (i = 0; ; i++) {
		bdf = spdk_conf_section_get_nmval(sp, "PciWhitelist", i, 0);
		if (!bdf) {
			break;
		}

		if (opts->pci_blacklist != NULL) {
			SPDK_ERRLOG("PciBlacklist and PciWhitelist cannot be used at the same time\n");
			free(opts->pci_blacklist);
			return -EINVAL;
		}

		rc = spdk_app_opts_add_pci_addr(opts, &opts->pci_whitelist, bdf);
		if (rc != 0) {
			free(opts->pci_whitelist);
			return rc;
		}
	}
	return 0;
}

static int
spdk_app_setup_env(struct spdk_app_opts *opts)
{
	struct spdk_env_opts env_opts = {};
	int rc;

	spdk_env_opts_init(&env_opts);

	env_opts.name = opts->name;
	env_opts.core_mask = opts->reactor_mask;
	env_opts.shm_id = opts->shm_id;
	env_opts.mem_channel = opts->mem_channel;
	env_opts.master_core = opts->master_core;
	env_opts.mem_size = opts->mem_size;
	env_opts.hugepage_single_segments = opts->hugepage_single_segments;
	env_opts.no_pci = opts->no_pci;
	env_opts.num_pci_addr = opts->num_pci_addr;
	env_opts.pci_blacklist = opts->pci_blacklist;
	env_opts.pci_whitelist = opts->pci_whitelist;

	rc = spdk_env_init(&env_opts);
	free(env_opts.pci_blacklist);
	free(env_opts.pci_whitelist);

	if (rc < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
	}

	return rc;
}

static int
spdk_app_setup_trace(struct spdk_app_opts *opts)
{
	char		shm_name[64];
	uint64_t	tpoint_group_mask;
	char		*end;

	if (opts->shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", opts->name, opts->shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", opts->name, (int)getpid());
	}

	if (spdk_trace_init(shm_name) != 0) {
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
			spdk_trace_set_tpoint_group_mask(tpoint_group_mask);
		}
	}

	return 0;
}

int
spdk_app_start(struct spdk_app_opts *opts, spdk_event_fn start_fn,
	       void *arg1, void *arg2)
{
	struct spdk_conf	*config = NULL;
	int			rc;
	struct spdk_event	*rpc_start_event;

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return 1;
	}

	if (opts->print_level > SPDK_LOG_WARN &&
	    isatty(STDERR_FILENO) &&
	    !strncmp(ttyname(STDERR_FILENO), "/dev/tty", strlen("/dev/tty"))) {
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using -q to disable logging to stderr and monitor syslog, or\n");
		printf("redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	spdk_log_set_print_level(opts->print_level);

#ifndef SPDK_NO_RLIMIT
	if (opts->enable_coredump) {
		struct rlimit core_limits;

		core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;
		setrlimit(RLIMIT_CORE, &core_limits);
	}
#endif

	config = spdk_app_setup_conf(opts->config_file);
	if (config == NULL) {
		goto app_start_setup_conf_err;
	}

	if (spdk_app_read_config_file_global_params(opts) < 0) {
		goto app_start_setup_conf_err;
	}

	spdk_log_set_level(SPDK_APP_DEFAULT_LOG_LEVEL);
	spdk_log_open();

	if (spdk_app_setup_env(opts) < 0) {
		goto app_start_log_close_err;
	}

	SPDK_NOTICELOG("Total cores available: %d\n", spdk_env_get_core_count());

	/*
	 * If mask not specified on command line or in configuration file,
	 *  reactor_mask will be 0x1 which will enable core 0 to run one
	 *  reactor.
	 */
	if ((rc = spdk_reactors_init(opts->max_delay_us)) != 0) {
		SPDK_ERRLOG("Invalid reactor mask.\n");
		goto app_start_log_close_err;
	}

	/*
	 * Note the call to spdk_app_setup_trace() is located here
	 * ahead of spdk_app_setup_signal_handlers().
	 * That's because there is not an easy/direct clean
	 * way of unwinding alloc'd resources that can occur
	 * in spdk_app_setup_signal_handlers().
	 */
	if (spdk_app_setup_trace(opts) != 0) {
		goto app_start_log_close_err;
	}

	if ((rc = spdk_app_setup_signal_handlers(opts)) != 0) {
		goto app_start_trace_cleanup_err;
	}

	memset(&g_spdk_app, 0, sizeof(g_spdk_app));
	g_spdk_app.config = config;
	g_spdk_app.shm_id = opts->shm_id;
	g_spdk_app.shutdown_cb = opts->shutdown_cb;
	g_spdk_app.rc = 0;
	g_init_lcore = spdk_env_get_current_core();
	g_delay_subsystem_init = opts->delay_subsystem_init;
	g_app_start_event = spdk_event_allocate(g_init_lcore, start_fn, arg1, arg2);

	rpc_start_event = spdk_event_allocate(g_init_lcore, spdk_app_start_rpc,
					      (void *)opts->rpc_addr, NULL);

	if (!g_delay_subsystem_init) {
		spdk_subsystem_init(rpc_start_event);
	} else {
		spdk_event_call(rpc_start_event);
	}

	/* This blocks until spdk_app_stop is called */
	spdk_reactors_start();

	return g_spdk_app.rc;

app_start_trace_cleanup_err:
	spdk_trace_cleanup();

app_start_log_close_err:
	spdk_log_close();

app_start_setup_conf_err:
	return 1;
}

void
spdk_app_fini(void)
{
	spdk_trace_cleanup();
	spdk_reactors_fini();
	spdk_conf_free(g_spdk_app.config);
	spdk_log_close();
}

static void
_spdk_app_stop(void *arg1, void *arg2)
{
	struct spdk_event *app_stop_event;

	spdk_rpc_finish();

	app_stop_event = spdk_event_allocate(spdk_env_get_current_core(), spdk_reactors_stop, NULL, NULL);
	spdk_subsystem_fini(app_stop_event);
}

void
spdk_app_stop(int rc)
{
	if (rc) {
		SPDK_WARNLOG("spdk_app_stop'd on non-zero\n");
	}
	g_spdk_app.rc = rc;
	/*
	 * We want to run spdk_subsystem_fini() from the same lcore where spdk_subsystem_init()
	 * was called.
	 */
	spdk_event_call(spdk_event_allocate(g_init_lcore, _spdk_app_stop, NULL, NULL));
}

static void
usage(char *executable_name, struct spdk_app_opts *default_opts, void (*app_usage)(void))
{
	printf("%s [options]\n", executable_name);
	printf("options:\n");
	printf(" -c config  config file (default %s)\n", default_opts->config_file);
	printf(" -d         disable coredump file enabling\n");
	printf(" -e mask    tracepoint group mask for spdk trace buffers (default 0x0)\n");
	printf(" -g         force creating just one hugetlbfs file\n");
	printf(" -h         show this usage\n");
	printf(" -i shared memory ID (optional)\n");
	printf(" -m mask    core mask for DPDK\n");
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -p core    master (primary) core for DPDK\n");
	printf(" -q         disable notice level logging to stderr\n");
	printf(" -r         RPC listen address (default %s)\n", SPDK_DEFAULT_RPC_ADDR);
	printf(" -s size    memory size in MB for DPDK (default: ");
	if (default_opts->mem_size > 0) {
		printf("%dMB)\n", default_opts->mem_size);
	} else {
		printf("all hugepage memory)\n");
	}
	printf(" -u         disable PCI access.\n");
	printf(" -w         wait for RPCs to initialize subsystems\n");
	printf(" -B addr    pci addr to blacklist\n");
	printf(" -W addr    pci addr to whitelist (-B and -W cannot be used at the same time)\n");
	spdk_tracelog_usage(stdout, "-t");
	app_usage();
}

spdk_app_parse_args_rvals_t
spdk_app_parse_args(int argc, char **argv, struct spdk_app_opts *opts,
		    const char *app_getopt_str, void (*app_parse)(int ch, char *arg),
		    void (*app_usage)(void))
{
	int ch, rc;
	struct spdk_app_opts default_opts;
	char *getopt_str;
	spdk_app_parse_args_rvals_t rval = SPDK_APP_PARSE_ARGS_SUCCESS;

	memcpy(&default_opts, opts, sizeof(default_opts));

	if (opts->config_file && access(opts->config_file, F_OK) != 0) {
		opts->config_file = NULL;
	}

	getopt_str = spdk_sprintf_alloc("%s%s", app_getopt_str, SPDK_APP_GETOPT_STRING);
	if (getopt_str == NULL) {
		fprintf(stderr, "Could not allocate getopt_str in %s()\n", __func__);
		rval = SPDK_APP_PARSE_ARGS_FAIL;
		goto parse_early_fail;
	}

	while ((ch = getopt(argc, argv, getopt_str)) != -1) {
		switch (ch) {
		case 'c':
			opts->config_file = optarg;
			break;
		case 'd':
			opts->enable_coredump = false;
			break;
		case 'e':
			opts->tpoint_group_mask = optarg;
			break;
		case 'g':
			opts->hugepage_single_segments = true;
			break;
		case 'h':
			usage(argv[0], &default_opts, app_usage);
			rval = SPDK_APP_PARSE_ARGS_HELP;
			goto parse_done;
		case 'i':
			if (optarg == NULL) {
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}
			opts->shm_id = atoi(optarg);
			break;
		case 'm':
			opts->reactor_mask = optarg;
			break;
		case 'n':
			if (optarg == NULL) {
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}
			opts->mem_channel = atoi(optarg);
			break;
		case 'p':
			if (optarg == NULL) {
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}
			opts->master_core = atoi(optarg);
			break;
		case 'q':
			opts->print_level = SPDK_LOG_WARN;
			break;
		case 'r':
			opts->rpc_addr = optarg;
			break;
		case 's': {
			uint64_t mem_size_mb;
			bool mem_size_has_prefix;

			rc = spdk_parse_capacity(optarg, &mem_size_mb, &mem_size_has_prefix);
			if (rc != 0) {
				fprintf(stderr, "invalid memory pool size `-s %s`\n", optarg);
				usage(argv[0], &default_opts, app_usage);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}

			if (mem_size_has_prefix) {
				/* the mem size is in MB by default, so if a prefix was
				 * specified, we need to manually convert to MB.
				 */
				mem_size_mb /= 1024 * 1024;
			}

			if (mem_size_mb > INT_MAX) {
				fprintf(stderr, "invalid memory pool size `-s %s`\n", optarg);
				usage(argv[0], &default_opts, app_usage);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}

			opts->mem_size = (int) mem_size_mb;
			break;
		}
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0], &default_opts, app_usage);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}
			opts->print_level = SPDK_LOG_DEBUG;
#ifndef DEBUG
			fprintf(stderr, "%s must be built with CONFIG_DEBUG=y for -t flag\n",
				argv[0]);
			usage(argv[0], &default_opts, app_usage);
			rval = SPDK_APP_PARSE_ARGS_FAIL;
			goto parse_done;
#else
			break;
#endif
		case 'u':
			opts->no_pci = true;
			break;
		case 'w':
			opts->delay_subsystem_init = true;
			break;
		case 'B':
			if (opts->pci_whitelist) {
				free(opts->pci_whitelist);
				fprintf(stderr, "-B and -W cannot be used at the same time\n");
				usage(argv[0], &default_opts, app_usage);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}

			rc = spdk_app_opts_add_pci_addr(opts, &opts->pci_blacklist, optarg);
			if (rc != 0) {
				free(opts->pci_blacklist);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}
			break;
		case 'W':
			if (opts->pci_blacklist) {
				free(opts->pci_blacklist);
				fprintf(stderr, "-B and -W cannot be used at the same time\n");
				usage(argv[0], &default_opts, app_usage);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}

			rc = spdk_app_opts_add_pci_addr(opts, &opts->pci_whitelist, optarg);
			if (rc != 0) {
				free(opts->pci_whitelist);
				rval = SPDK_APP_PARSE_ARGS_FAIL;
				goto parse_done;
			}
			break;
		case '?':
			/*
			 * In the event getopt() above detects an option
			 * in argv that is NOT in the getopt_str,
			 * getopt() will return a '?' indicating failure.
			 */
			usage(argv[0], &default_opts, app_usage);
			rval = SPDK_APP_PARSE_ARGS_FAIL;
			goto parse_done;
		default:
			app_parse(ch, optarg);
		}
	}

	/* TBD: Replace warning by failure when RPCs for startup are prepared. */
	if (opts->config_file && opts->delay_subsystem_init) {
		fprintf(stderr,
			"WARNING: -w and config file are used at the same time. "
			"- Please be careful one options might overwrite others.\n");
	}

parse_done:
	free(getopt_str);

parse_early_fail:
	return rval;
}

static void
spdk_rpc_start_subsystem_init_cpl(void *arg1, void *arg2)
{
	struct spdk_jsonrpc_request *request = arg1;
	struct spdk_json_write_ctx *w;

	spdk_app_start_application();

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_start_subsystem_init(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_event *cb_event;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "start_subsystem_init requires no parameters");
		return;
	}

	cb_event = spdk_event_allocate(g_init_lcore, spdk_rpc_start_subsystem_init_cpl,
				       request, NULL);
	spdk_subsystem_init(cb_event);
}
SPDK_RPC_REGISTER("start_subsystem_init", spdk_rpc_start_subsystem_init, SPDK_RPC_STARTUP)
