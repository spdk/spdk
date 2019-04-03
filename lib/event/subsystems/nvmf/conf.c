/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018 Mellanox Technologies LTD. All rights reserved.
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

#include "event_nvmf.h"

#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

#define SPDK_NVMF_MAX_NAMESPACES (1 << 14)

struct spdk_nvmf_tgt_conf *g_spdk_nvmf_tgt_conf = NULL;
uint32_t g_spdk_nvmf_tgt_max_subsystems = 0;

static int
spdk_add_nvmf_discovery_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_create(g_spdk_nvmf_tgt, SPDK_NVMF_DISCOVERY_NQN,
					       SPDK_NVMF_SUBTYPE_DISCOVERY, 0);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		return -1;
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);

	return 0;
}

static void
spdk_nvmf_read_config_file_tgt_max_subsystems(struct spdk_conf_section *sp,
		int *deprecated_values)
{
	int tgt_max_subsystems;
	int deprecated;

	tgt_max_subsystems = spdk_conf_section_get_intval(sp, "MaxSubsystems");
	if (tgt_max_subsystems >= 0) {
		g_spdk_nvmf_tgt_max_subsystems = tgt_max_subsystems;
	}

	deprecated = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (deprecated >= 0) {
		*deprecated_values = -1;
	}

	deprecated = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (deprecated >= 0) {
		*deprecated_values = -1;
	}

	deprecated = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (deprecated >= 0) {
		*deprecated_values = -1;
	}

	deprecated = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (deprecated >= 0) {
		*deprecated_values = -1;
	}

	deprecated = spdk_conf_section_get_intval(sp, "IOUnitSize");
	if (deprecated >= 0) {
		*deprecated_values = -1;
	}
}

static void
spdk_nvmf_read_config_file_tgt_conf(struct spdk_conf_section *sp,
				    struct spdk_nvmf_tgt_conf *conf)
{
	int acceptor_poll_rate;

	acceptor_poll_rate = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (acceptor_poll_rate >= 0) {
		conf->acceptor_poll_rate = acceptor_poll_rate;
	}
}

static int
spdk_nvmf_parse_tgt_max_subsystems(void)
{
	struct spdk_conf_section *sp;
	int deprecated_values = 0;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp != NULL) {
		spdk_nvmf_read_config_file_tgt_max_subsystems(sp, &deprecated_values);
	}

	return deprecated_values;
}

static struct spdk_nvmf_tgt_conf *
spdk_nvmf_parse_tgt_conf(void)
{
	struct spdk_nvmf_tgt_conf *conf;
	struct spdk_conf_section *sp;

	conf = calloc(1, sizeof(*conf));
	if (!conf) {
		SPDK_ERRLOG("calloc() failed for target conf\n");
		return NULL;
	}

	conf->acceptor_poll_rate = ACCEPT_TIMEOUT_US;
	conf->conn_sched = DEFAULT_CONN_SCHED;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp != NULL) {
		spdk_nvmf_read_config_file_tgt_conf(sp, conf);
	}

	return conf;
}

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	int rc;
	int using_deprecated_options;

	if (!g_spdk_nvmf_tgt_max_subsystems) {
		using_deprecated_options = spdk_nvmf_parse_tgt_max_subsystems();
		if (using_deprecated_options < 0) {
			SPDK_ERRLOG("Deprecated options detected for the NVMe-oF target.\n"
				    "The following options are no longer controlled by the target\n"
				    "and should be set in the transport on a per-transport basis:\n"
				    "MaxQueueDepth, MaxQueuesPerSession, InCapsuleDataSize, MaxIOSize, IOUnitSize\n"
				    "This can be accomplished by setting the options through the create_nvmf_transport RPC.\n"
				    "You may also continue to configure these options in the conf file under each transport.");
		}
	}

	if (!g_spdk_nvmf_tgt_conf) {
		g_spdk_nvmf_tgt_conf = spdk_nvmf_parse_tgt_conf();
		if (!g_spdk_nvmf_tgt_conf) {
			SPDK_ERRLOG("spdk_nvmf_parse_tgt_conf() failed\n");
			return -1;
		}
	}

	g_spdk_nvmf_tgt = spdk_nvmf_tgt_create(g_spdk_nvmf_tgt_max_subsystems);

	g_spdk_nvmf_tgt_max_subsystems = 0;

	if (!g_spdk_nvmf_tgt) {
		SPDK_ERRLOG("spdk_nvmf_tgt_create() failed\n");
		return -1;
	}

	rc = spdk_add_nvmf_discovery_subsystem();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_add_nvmf_discovery_subsystem failed\n");
		return rc;
	}

	return 0;
}

static void
spdk_nvmf_tgt_listen_done(void *cb_arg, int status)
{
	/* TODO: Config parsing should wait for this operation to finish. */

	if (status) {
		SPDK_ERRLOG("Failed to listen on transport address\n");
	}
}

static int
spdk_nvmf_parse_subsystem(struct spdk_conf_section *sp)
{
	const char *nqn, *mode;
	size_t i;
	int ret;
	int lcore;
	bool allow_any_host;
	const char *sn;
	struct spdk_nvmf_subsystem *subsystem;
	int num_ns;

	nqn = spdk_conf_section_get_val(sp, "NQN");
	if (nqn == NULL) {
		SPDK_ERRLOG("Subsystem missing NQN\n");
		return -1;
	}

	mode = spdk_conf_section_get_val(sp, "Mode");
	lcore = spdk_conf_section_get_intval(sp, "Core");
	num_ns = spdk_conf_section_get_intval(sp, "MaxNamespaces");

	if (num_ns < 1) {
		num_ns = 0;
	} else if (num_ns > SPDK_NVMF_MAX_NAMESPACES) {
		num_ns = SPDK_NVMF_MAX_NAMESPACES;
	}

	/* Mode is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (mode) {
		SPDK_NOTICELOG("Mode present in the [Subsystem] section of the config file.\n"
			       "Mode was removed as a valid parameter.\n");
		if (strcasecmp(mode, "Virtual") == 0) {
			SPDK_NOTICELOG("Your mode value is 'Virtual' which is now the only possible mode.\n"
				       "Your configuration file will work as expected.\n");
		} else {
			SPDK_NOTICELOG("Please remove Mode from your configuration file.\n");
			return -1;
		}
	}

	/* Core is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (lcore >= 0) {
		SPDK_NOTICELOG("Core present in the [Subsystem] section of the config file.\n"
			       "Core was removed as an option. Subsystems can now run on all available cores.\n");
		SPDK_NOTICELOG("Please remove Core from your configuration file. Ignoring it and continuing.\n");
	}

	sn = spdk_conf_section_get_val(sp, "SN");
	if (sn == NULL) {
		SPDK_ERRLOG("Subsystem %s: missing serial number\n", nqn);
		return -1;
	}

	subsystem = spdk_nvmf_subsystem_create(g_spdk_nvmf_tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, num_ns);
	if (subsystem == NULL) {
		goto done;
	}

	if (spdk_nvmf_subsystem_set_sn(subsystem, sn)) {
		SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", nqn, sn);
		spdk_nvmf_subsystem_destroy(subsystem);
		subsystem = NULL;
		goto done;
	}

	for (i = 0; ; i++) {
		struct spdk_nvmf_ns_opts ns_opts;
		struct spdk_bdev *bdev;
		const char *bdev_name;
		const char *uuid_str;
		char *nsid_str;

		bdev_name = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
		if (!bdev_name) {
			break;
		}

		bdev = spdk_bdev_get_by_name(bdev_name);
		if (bdev == NULL) {
			SPDK_ERRLOG("Could not find namespace bdev '%s'\n", bdev_name);
			spdk_nvmf_subsystem_destroy(subsystem);
			subsystem = NULL;
			goto done;
		}

		spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

		nsid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 1);
		if (nsid_str) {
			char *end;
			unsigned long nsid_ul = strtoul(nsid_str, &end, 0);

			if (*end != '\0' || nsid_ul == 0 || nsid_ul >= UINT32_MAX) {
				SPDK_ERRLOG("Invalid NSID %s\n", nsid_str);
				spdk_nvmf_subsystem_destroy(subsystem);
				subsystem = NULL;
				goto done;
			}

			ns_opts.nsid = (uint32_t)nsid_ul;
		}

		uuid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 2);
		if (uuid_str) {
			if (spdk_uuid_parse(&ns_opts.uuid, uuid_str)) {
				SPDK_ERRLOG("Invalid UUID %s\n", uuid_str);
				spdk_nvmf_subsystem_destroy(subsystem);
				subsystem = NULL;
				goto done;
			}
		}

		if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts)) == 0) {
			SPDK_ERRLOG("Unable to add namespace\n");
			spdk_nvmf_subsystem_destroy(subsystem);
			subsystem = NULL;
			goto done;
		}

		SPDK_INFOLOG(SPDK_LOG_NVMF, "Attaching block device %s to subsystem %s\n",
			     spdk_bdev_get_name(bdev), spdk_nvmf_subsystem_get_nqn(subsystem));
	}

	/* Parse Listen sections */
	for (i = 0; ; i++) {
		struct spdk_nvme_transport_id trid = {0};
		const char *transport;
		const char *address;
		char *address_dup;
		char *host;
		char *port;

		transport = spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (!transport) {
			break;
		}

		if (spdk_nvme_transport_id_parse_trtype(&trid.trtype, transport)) {
			SPDK_ERRLOG("Invalid listen address transport type '%s'\n", transport);
			continue;
		}

		address = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (!address) {
			break;
		}

		address_dup = strdup(address);
		if (!address_dup) {
			break;
		}

		ret = spdk_parse_ip_addr(address_dup, &host, &port);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse listen address '%s'\n", address);
			free(address_dup);
			continue;
		}

		if (strchr(host, ':')) {
			trid.adrfam = SPDK_NVMF_ADRFAM_IPV6;
		} else {
			trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
		}

		snprintf(trid.traddr, sizeof(trid.traddr), "%s", host);
		if (port) {
			snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", port);
		}
		free(address_dup);

		spdk_nvmf_tgt_listen(g_spdk_nvmf_tgt, &trid, spdk_nvmf_tgt_listen_done, NULL);

		spdk_nvmf_subsystem_add_listener(subsystem, &trid);
	}

	/* Parse Host sections */
	for (i = 0; ; i++) {
		const char *host = spdk_conf_section_get_nval(sp, "Host", i);

		if (!host) {
			break;
		}

		spdk_nvmf_subsystem_add_host(subsystem, host);
	}

	allow_any_host = spdk_conf_section_get_boolval(sp, "AllowAnyHost", false);
	spdk_nvmf_subsystem_set_allow_any_host(subsystem, allow_any_host);

done:
	return (subsystem != NULL);
}

static int
spdk_nvmf_parse_subsystems(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			rc = spdk_nvmf_parse_subsystem(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

struct spdk_nvmf_parse_transport_ctx {
	struct spdk_conf_section *sp;
	spdk_nvmf_parse_conf_done_fn cb_fn;
};

static void spdk_nvmf_parse_transport(struct spdk_nvmf_parse_transport_ctx *ctx);

static void
spdk_nvmf_tgt_add_transport_done(void *cb_arg, int status)
{
	struct spdk_nvmf_parse_transport_ctx *ctx = cb_arg;
	int rc;

	if (status < 0) {
		SPDK_ERRLOG("Add transport to target failed (%d).\n", status);
		ctx->cb_fn(status);
		free(ctx);
		return;
	}

	/* find next transport */
	ctx->sp = spdk_conf_next_section(ctx->sp);
	while (ctx->sp) {
		if (spdk_conf_section_match_prefix(ctx->sp, "Transport")) {
			spdk_nvmf_parse_transport(ctx);
			return;
		}
		ctx->sp = spdk_conf_next_section(ctx->sp);
	}

	/* done with transports, parse Subsystem sections */
	rc = spdk_nvmf_parse_subsystems();

	ctx->cb_fn(rc);
	free(ctx);
}

static void
spdk_nvmf_parse_transport(struct spdk_nvmf_parse_transport_ctx *ctx)
{
	const char *type;
	struct spdk_nvmf_transport_opts opts = { 0 };
	enum spdk_nvme_transport_type trtype;
	struct spdk_nvmf_transport *transport;
	int val;

	type = spdk_conf_section_get_val(ctx->sp, "Type");
	if (type == NULL) {
		SPDK_ERRLOG("Transport missing Type\n");
		ctx->cb_fn(-1);
		free(ctx);
		return;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trtype, type)) {
		SPDK_ERRLOG("Invalid transport type '%s'\n", type);
		ctx->cb_fn(-1);
		free(ctx);
		return;
	}

	if (spdk_nvmf_tgt_get_transport(g_spdk_nvmf_tgt, trtype)) {
		SPDK_ERRLOG("Duplicate transport type '%s'\n", type);
		ctx->cb_fn(-1);
		free(ctx);
		return;
	}

	if (!spdk_nvmf_transport_opts_init(trtype, &opts)) {
		ctx->cb_fn(-1);
		free(ctx);
		return;
	}

	val = spdk_conf_section_get_intval(ctx->sp, "MaxQueueDepth");
	if (val >= 0) {
		opts.max_queue_depth = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "MaxQueuesPerSession");
	if (val >= 0) {
		opts.max_qpairs_per_ctrlr = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "InCapsuleDataSize");
	if (val >= 0) {
		opts.in_capsule_data_size = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "MaxIOSize");
	if (val >= 0) {
		opts.max_io_size = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "IOUnitSize");
	if (val >= 0) {
		opts.io_unit_size = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "MaxAQDepth");
	if (val >= 0) {
		opts.max_aq_depth = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "NumSharedBuffers");
	if (val >= 0) {
		opts.num_shared_buffers = val;
	}
	val = spdk_conf_section_get_intval(ctx->sp, "BufCacheSize");
	if (val >= 0) {
		opts.buf_cache_size = val;
	}

	val = spdk_conf_section_get_intval(ctx->sp, "MaxSRQDepth");
	if (val >= 0) {
		if (trtype == SPDK_NVME_TRANSPORT_RDMA) {
			opts.max_srq_depth = val;
		} else {
			SPDK_ERRLOG("MaxSRQDepth is relevant only for RDMA transport '%s'\n", type);
			ctx->cb_fn(-1);
			free(ctx);
			return;
		}
	}

	transport = spdk_nvmf_transport_create(trtype, &opts);
	if (transport) {
		spdk_nvmf_tgt_add_transport(g_spdk_nvmf_tgt, transport, spdk_nvmf_tgt_add_transport_done, ctx);
	} else {
		ctx->cb_fn(-1);
		free(ctx);
		return;
	}
}

static int
spdk_nvmf_parse_transports(spdk_nvmf_parse_conf_done_fn cb_fn)
{
	struct spdk_nvmf_parse_transport_ctx *ctx;

	ctx = calloc(1, sizeof(struct spdk_nvmf_parse_transport_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed alloc of context memory for parse transports\n");
		return -ENOMEM;
	}

	ctx->cb_fn = cb_fn;
	ctx->sp = spdk_conf_first_section(NULL);
	if (ctx->sp == NULL) {
		free(ctx);
		cb_fn(0);

		return 0;
	}

	while (ctx->sp != NULL) {
		if (spdk_conf_section_match_prefix(ctx->sp, "Transport")) {
			spdk_nvmf_parse_transport(ctx);
			return 0;
		}
		ctx->sp = spdk_conf_next_section(ctx->sp);
	}

	/* if we get here, there are no transports defined in conf file */
	free(ctx);
	cb_fn(0);
	return 0;
}

int
spdk_nvmf_parse_conf(spdk_nvmf_parse_conf_done_fn cb_fn)
{
	int rc;

	if (cb_fn == NULL) {
		SPDK_ERRLOG("Callback function is NULL\n");
		return -1;
	}

	/* NVMf section */
	rc = spdk_nvmf_parse_nvmf_tgt();
	if (rc < 0) {
		return rc;
	}

	/* Transport sections */
	rc = spdk_nvmf_parse_transports(cb_fn);
	if (rc < 0) {
		return rc;
	}

	return 0;
}
