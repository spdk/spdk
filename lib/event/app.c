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

#include "spdk_internal/event.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>

#include <rte_config.h>
#include <rte_lcore.h>

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/conf.h"
#include "spdk/trace.h"

#define SPDK_APP_DEFAULT_LOG_FACILITY	"local7"
#define SPDK_APP_DEFAULT_LOG_PRIORITY	"info"

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
static struct spdk_event *g_shutdown_event = NULL;

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
"  ReactorMask \"0x%" PRIX64 "\"\n" \
"\n" \
"  # Tracepoint group mask for spdk trace buffers\n" \
"  # Default: 0x0 (all tracepoint groups disabled)\n" \
"  # Set to 0xFFFFFFFFFFFFFFFF to enable all tracepoint groups.\n" \
"  TpointGroupMask \"0x%" PRIX64 "\"\n" \
"\n" \
"  # syslog facility\n" \
"  LogFacility \"%s\"\n" \
"\n"

static void
spdk_app_config_dump_global_section(FILE *fp)
{
	const char *log_facility;

	if (NULL == fp)
		return;

	log_facility = spdk_get_log_facility();
	assert(log_facility != NULL);

	fprintf(fp, GLOBAL_CONFIG_TMPL,
		spdk_app_get_core_mask(), spdk_trace_get_tpoint_group_mask(),
		log_facility);
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
		fprintf(stderr, "mkstemp failed\n");
		return -1;
	}
	fp = fdopen(fd, "wb+");
	if (NULL == fp) {
		fprintf(stderr, "error opening tmpfile fd = %d\n", fd);
		return -1;
	}

	/* Buffered IO */
	setvbuf(fp, vbuf, _IOFBF, BUFSIZ);

	spdk_app_config_dump_global_section(fp);
	spdk_subsystem_config(fp);

	length = ftell(fp);

	*config_str = malloc(length + 1);
	if (!*config_str) {
		perror("config_str");
		fclose(fp);
		return -1;
	}
	fseek(fp, 0, SEEK_SET);
	ret = fread(*config_str, sizeof(char), length, fp);
	if (ret < length)
		fprintf(stderr, "%s: warning - short read\n", __func__);
	fclose(fp);
	(*config_str)[length] = '\0';

	return 0;
}

static const char *
spdk_app_get_log_facility(struct spdk_conf *config)
{
	struct spdk_conf_section *sp;
	const char *logfacility;

	sp = spdk_conf_find_section(config, "Global");
	if (sp == NULL) {
		return SPDK_APP_DEFAULT_LOG_FACILITY;
	}

	logfacility = spdk_conf_section_get_val(sp, "LogFacility");
	if (logfacility == NULL) {
		return SPDK_APP_DEFAULT_LOG_FACILITY;
	}

	return logfacility;
}

void
spdk_app_start_shutdown(void)
{
	if (g_shutdown_event != NULL) {
		spdk_event_call(g_shutdown_event);
		g_shutdown_event = NULL;
	}
}

static void
__shutdown_signal(int signo)
{
	spdk_app_start_shutdown();
}

static void
__shutdown_event_cb(void *arg1, void *arg2)
{
	g_spdk_app.shutdown_cb();
}

void
spdk_app_opts_init(struct spdk_app_opts *opts)
{
	if (!opts)
		return;

	memset(opts, 0, sizeof(*opts));

	opts->log_facility = SPDK_APP_DEFAULT_LOG_FACILITY;
	opts->enable_coredump = true;
	opts->shm_id = -1;
	opts->dpdk_mem_size = SPDK_APP_DPDK_DEFAULT_MEM_SIZE;
	opts->dpdk_master_core = SPDK_APP_DPDK_DEFAULT_MASTER_CORE;
	opts->dpdk_mem_channel = SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL;
	opts->reactor_mask = NULL;
	opts->max_delay_us = 0;
}

void
spdk_app_init(struct spdk_app_opts *opts)
{
	struct spdk_conf		*config;
	struct spdk_conf_section	*sp;
	struct sigaction	sigact;
	sigset_t		signew;
	char			shm_name[64];
	int			rc;
	uint64_t		tpoint_group_mask;
	char			*end;
	struct spdk_env_opts env_opts = {};

	if (opts->enable_coredump) {
		struct rlimit core_limits;

		core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;
		setrlimit(RLIMIT_CORE, &core_limits);
	}

	config = spdk_conf_allocate();
	assert(config != NULL);
	if (opts->config_file) {
		rc = spdk_conf_read(config, opts->config_file);
		if (rc != 0) {
			fprintf(stderr, "Could not read config file %s\n", opts->config_file);
			spdk_conf_free(config);
			exit(EXIT_FAILURE);
		}
		if (spdk_conf_first_section(config) == NULL) {
			fprintf(stderr, "Invalid config file %s\n", opts->config_file);
			spdk_conf_free(config);
			exit(EXIT_FAILURE);
		}
	}
	spdk_conf_set_as_default(config);

	if (opts->shm_id == -1) {
		sp = spdk_conf_find_section(config, "Global");
		if (sp != NULL) {
			opts->shm_id = spdk_conf_section_get_intval(sp, "SharedMemoryID");
		}
	}

	memset(&g_spdk_app, 0, sizeof(g_spdk_app));
	g_spdk_app.config = config;
	g_spdk_app.shm_id = opts->shm_id;
	g_spdk_app.shutdown_cb = opts->shutdown_cb;

	/* open log files */
	if (opts->log_facility == NULL) {
		opts->log_facility = spdk_app_get_log_facility(g_spdk_app.config);
		if (opts->log_facility == NULL) {
			fprintf(stderr, "NULL logfacility\n");
			spdk_conf_free(g_spdk_app.config);
			exit(EXIT_FAILURE);
		}
	}
	rc = spdk_set_log_facility(opts->log_facility);
	if (rc < 0) {
		fprintf(stderr, "log facility error\n");
		spdk_conf_free(g_spdk_app.config);
		exit(EXIT_FAILURE);
	}

	rc = spdk_set_log_priority(SPDK_APP_DEFAULT_LOG_PRIORITY);
	if (rc < 0) {
		fprintf(stderr, "log priority error\n");
		spdk_conf_free(g_spdk_app.config);
		exit(EXIT_FAILURE);
	}
	spdk_open_log();

	if (opts->reactor_mask == NULL) {
		sp = spdk_conf_find_section(g_spdk_app.config, "Global");
		if (sp != NULL) {
			if (spdk_conf_section_get_val(sp, "ReactorMask")) {
				opts->reactor_mask = spdk_conf_section_get_val(sp, "ReactorMask");
			} else {
				opts->reactor_mask = SPDK_APP_DPDK_DEFAULT_CORE_MASK;
			}
		} else {
			opts->reactor_mask = SPDK_APP_DPDK_DEFAULT_CORE_MASK;
		}
	}

	spdk_env_opts_init(&env_opts);

	env_opts.name = opts->name;
	env_opts.core_mask = opts->reactor_mask;
	env_opts.shm_id = opts->shm_id;
	env_opts.dpdk_mem_channel = opts->dpdk_mem_channel;
	env_opts.dpdk_master_core = opts->dpdk_master_core;
	env_opts.dpdk_mem_size = opts->dpdk_mem_size;

	spdk_env_init(&env_opts);

	/*
	 * If mask not specified on command line or in configuration file,
	 *  reactor_mask will be 0x1 which will enable core 0 to run one
	 *  reactor.
	 */
	if (spdk_reactors_init(opts->max_delay_us)) {
		fprintf(stderr, "Invalid reactor mask.\n");
		spdk_conf_free(g_spdk_app.config);
		exit(EXIT_FAILURE);
	}

	/* setup signal handler thread */
	pthread_sigmask(SIG_SETMASK, NULL, &signew);

	memset(&sigact, 0, sizeof(sigact));
	sigact.sa_handler = SIG_IGN;
	sigemptyset(&sigact.sa_mask);
	rc = sigaction(SIGPIPE, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGPIPE) failed\n");
		spdk_conf_free(g_spdk_app.config);
		exit(EXIT_FAILURE);
	}

	if (opts->shutdown_cb != NULL) {
		g_shutdown_event = spdk_event_allocate(rte_lcore_id(), __shutdown_event_cb,
						       NULL, NULL);

		sigact.sa_handler = __shutdown_signal;
		sigemptyset(&sigact.sa_mask);
		rc = sigaction(SIGINT, &sigact, NULL);
		if (rc < 0) {
			SPDK_ERRLOG("sigaction(SIGINT) failed\n");
			spdk_conf_free(g_spdk_app.config);
			exit(EXIT_FAILURE);
		}
		sigaddset(&signew, SIGINT);

		sigact.sa_handler = __shutdown_signal;
		sigemptyset(&sigact.sa_mask);
		rc = sigaction(SIGTERM, &sigact, NULL);
		if (rc < 0) {
			SPDK_ERRLOG("sigaction(SIGTERM) failed\n");
			spdk_conf_free(g_spdk_app.config);
			exit(EXIT_FAILURE);
		}
		sigaddset(&signew, SIGTERM);
	}

	if (opts->usr1_handler != NULL) {
		sigact.sa_handler = opts->usr1_handler;
		sigemptyset(&sigact.sa_mask);
		rc = sigaction(SIGUSR1, &sigact, NULL);
		if (rc < 0) {
			SPDK_ERRLOG("sigaction(SIGUSR1) failed\n");
			spdk_conf_free(g_spdk_app.config);
			exit(EXIT_FAILURE);
		}
		sigaddset(&signew, SIGUSR1);
	}

	sigaddset(&signew, SIGQUIT);
	sigaddset(&signew, SIGHUP);
	pthread_sigmask(SIG_SETMASK, &signew, NULL);

	if (opts->shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", opts->name, opts->shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", opts->name, (int)getpid());
	}

	spdk_trace_init(shm_name);

	if (opts->tpoint_group_mask == NULL) {
		sp = spdk_conf_find_section(g_spdk_app.config, "Global");
		if (sp != NULL) {
			opts->tpoint_group_mask = spdk_conf_section_get_val(sp, "TpointGroupMask");
		}
	}

	if (opts->tpoint_group_mask != NULL) {
		errno = 0;
		tpoint_group_mask = strtoull(opts->tpoint_group_mask, &end, 16);
		if (*end != '\0' || errno) {
			SPDK_ERRLOG("invalid tpoint mask %s\n", opts->tpoint_group_mask);
		} else {
			spdk_trace_set_tpoint_group_mask(tpoint_group_mask);
		}
	}

	rc = spdk_subsystem_init();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_subsystem_init() failed\n");
		spdk_conf_free(g_spdk_app.config);
		exit(EXIT_FAILURE);
	}
}

int
spdk_app_fini(void)
{
	int rc;

	rc = spdk_subsystem_fini();
	spdk_trace_cleanup();
	spdk_reactors_fini();
	spdk_conf_free(g_spdk_app.config);
	spdk_close_log();

	return rc;
}

int
spdk_app_start(spdk_event_fn start_fn, void *arg1, void *arg2)
{
	struct spdk_event *event;

	g_spdk_app.rc = 0;

	event = spdk_event_allocate(rte_get_master_lcore(), start_fn,
				    arg1, arg2);
	/* Queues up the event, but can't run it until the reactors start */
	spdk_event_call(event);

	/* This blocks until spdk_app_stop is called */
	spdk_reactors_start();

	return g_spdk_app.rc;
}

void
spdk_app_stop(int rc)
{
	spdk_reactors_stop();
	g_spdk_app.rc = rc;
}
