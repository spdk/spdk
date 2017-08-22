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

#include "nvmf_tgt.h"

#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

#define MAX_LISTEN_ADDRESSES 255
#define MAX_HOSTS 255
#define MAX_NAMESPACES 255

#define ACCEPT_TIMEOUT_US		10000 /* 10ms */

struct spdk_nvmf_probe_ctx {
	struct spdk_nvmf_subsystem	*subsystem;
	bool				any;
	bool				found;
	struct spdk_nvme_transport_id	trid;
};

#define MAX_STRING_LEN 255

struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf;

static int
spdk_add_nvmf_discovery_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = nvmf_tgt_create_subsystem(SPDK_NVMF_DISCOVERY_NQN, SPDK_NVMF_SUBTYPE_DISCOVERY, 0);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		return -1;
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);

	return 0;
}

static void
spdk_nvmf_read_config_file_params(struct spdk_conf_section *sp,
				  struct spdk_nvmf_tgt_opts *opts)
{
	int max_queue_depth;
	int max_queues_per_sess;
	int in_capsule_data_size;
	int max_io_size;
	int acceptor_poll_rate;

	max_queue_depth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (max_queue_depth >= 0) {
		opts->max_queue_depth = max_queue_depth;
	}

	max_queues_per_sess = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (max_queues_per_sess >= 0) {
		opts->max_qpairs_per_ctrlr = max_queues_per_sess;
	}

	in_capsule_data_size = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (in_capsule_data_size >= 0) {
		opts->in_capsule_data_size = in_capsule_data_size;
	}

	max_io_size = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (max_io_size >= 0) {
		opts->max_io_size = max_io_size;
	}

	acceptor_poll_rate = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (acceptor_poll_rate >= 0) {
		g_spdk_nvmf_tgt_conf.acceptor_poll_rate = acceptor_poll_rate;
	}
}

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	struct spdk_nvmf_tgt_opts opts;
	int rc;

	spdk_nvmf_tgt_opts_init(&opts);
	g_spdk_nvmf_tgt_conf.acceptor_poll_rate = ACCEPT_TIMEOUT_US;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp != NULL) {
		spdk_nvmf_read_config_file_params(sp, &opts);
	}

	g_tgt.tgt = spdk_nvmf_tgt_create(&opts);
	if (!g_tgt.tgt) {
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

static int
spdk_nvmf_parse_subsystem(struct spdk_conf_section *sp)
{
	const char *nqn, *mode;
	size_t i;
	int ret;
	int lcore;
	int num_listen_addrs;
	struct rpc_listen_address listen_addrs[MAX_LISTEN_ADDRESSES] = {};
	char *listen_addrs_str[MAX_LISTEN_ADDRESSES] = {};
	int num_hosts;
	char *hosts[MAX_HOSTS];
	bool allow_any_host;
	const char *sn;
	size_t num_ns;
	struct spdk_nvmf_ns_params ns_list[MAX_NAMESPACES] = {};
	struct spdk_nvmf_subsystem *subsystem;

	nqn = spdk_conf_section_get_val(sp, "NQN");
	mode = spdk_conf_section_get_val(sp, "Mode");
	lcore = spdk_conf_section_get_intval(sp, "Core");

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

	/* Parse Listen sections */
	num_listen_addrs = 0;
	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		listen_addrs[num_listen_addrs].transport =
			spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (!listen_addrs[num_listen_addrs].transport) {
			break;
		}

		listen_addrs_str[i] = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (!listen_addrs_str[i]) {
			break;
		}

		listen_addrs_str[i] = strdup(listen_addrs_str[i]);

		ret = spdk_parse_ip_addr(listen_addrs_str[i], &listen_addrs[num_listen_addrs].traddr,
					 &listen_addrs[num_listen_addrs].trsvcid);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse listen address '%s'\n", listen_addrs_str[i]);
			free(listen_addrs_str[i]);
			listen_addrs_str[i] = NULL;
			continue;
		}

		if (strchr(listen_addrs[num_listen_addrs].traddr, ':')) {
			listen_addrs[num_listen_addrs].adrfam = "IPv6";
		} else {
			listen_addrs[num_listen_addrs].adrfam = "IPv4";
		}

		num_listen_addrs++;
	}

	/* Parse Host sections */
	for (i = 0; i < MAX_HOSTS; i++) {
		hosts[i] = spdk_conf_section_get_nval(sp, "Host", i);
		if (!hosts[i]) {
			break;
		}
	}
	num_hosts = i;

	allow_any_host = spdk_conf_section_get_boolval(sp, "AllowAnyHost", false);

	sn = spdk_conf_section_get_val(sp, "SN");

	num_ns = 0;
	for (i = 0; i < SPDK_COUNTOF(ns_list); i++) {
		char *nsid_str;

		ns_list[i].bdev_name = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
		if (!ns_list[i].bdev_name) {
			break;
		}

		nsid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 1);
		if (nsid_str) {
			char *end;
			unsigned long nsid_ul = strtoul(nsid_str, &end, 0);

			if (*end != '\0' || nsid_ul == 0 || nsid_ul >= UINT32_MAX) {
				SPDK_ERRLOG("Invalid NSID %s\n", nsid_str);
				return -1;
			}

			ns_list[i].nsid = (uint32_t)nsid_ul;
		} else {
			/* Automatically assign the next available NSID. */
			ns_list[i].nsid = 0;
		}

		num_ns++;
	}

	subsystem = spdk_nvmf_construct_subsystem(nqn,
			num_listen_addrs, listen_addrs,
			num_hosts, hosts, allow_any_host,
			sn,
			num_ns, ns_list);

	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		free(listen_addrs_str[i]);
	}

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

int
spdk_nvmf_parse_conf(void)
{
	int rc;

	/* NVMf section */
	rc = spdk_nvmf_parse_nvmf_tgt();
	if (rc < 0) {
		return rc;
	}

	/* Subsystem sections */
	rc = spdk_nvmf_parse_subsystems();
	if (rc < 0) {
		return rc;
	}

	return 0;
}

struct spdk_nvmf_subsystem *
	spdk_nvmf_construct_subsystem(const char *name,
			      int num_listen_addresses, struct rpc_listen_address *addresses,
			      int num_hosts, char *hosts[], bool allow_any_host,
			      const char *sn, size_t num_ns, struct spdk_nvmf_ns_params *ns_list)
{
	struct spdk_nvmf_subsystem *subsystem;
	int i, rc;
	size_t j;
	struct spdk_bdev *bdev;

	if (name == NULL) {
		SPDK_ERRLOG("No NQN specified for subsystem\n");
		return NULL;
	}

	if (num_listen_addresses > MAX_LISTEN_ADDRESSES) {
		SPDK_ERRLOG("invalid listen adresses number\n");
		return NULL;
	}

	if (num_hosts > MAX_HOSTS) {
		SPDK_ERRLOG("invalid hosts number\n");
		return NULL;
	}

	subsystem = nvmf_tgt_create_subsystem(name, SPDK_NVMF_SUBTYPE_NVME, num_ns);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		return NULL;
	}

	/* Parse Listen sections */
	for (i = 0; i < num_listen_addresses; i++) {
		struct spdk_nvme_transport_id trid = {};

		if (spdk_nvme_transport_id_parse_trtype(&trid.trtype, addresses[i].transport)) {
			SPDK_ERRLOG("Missing listen address transport type\n");
			goto error;
		}

		if (spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, addresses[i].adrfam)) {
			trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
		}

		snprintf(trid.traddr, sizeof(trid.traddr), "%s", addresses[i].traddr);
		snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", addresses[i].trsvcid);

		rc = spdk_nvmf_tgt_listen(g_tgt.tgt, &trid);
		if (rc) {
			SPDK_ERRLOG("Failed to listen on transport %s, adrfam %s, traddr %s, trsvcid %s\n",
				    addresses[i].transport,
				    addresses[i].adrfam,
				    addresses[i].traddr,
				    addresses[i].trsvcid);
			goto error;
		}
		spdk_nvmf_subsystem_add_listener(subsystem, &trid);
	}

	/* Parse Host sections */
	for (i = 0; i < num_hosts; i++) {
		spdk_nvmf_subsystem_add_host(subsystem, hosts[i]);
	}
	spdk_nvmf_subsystem_set_allow_any_host(subsystem, allow_any_host);

	if (sn == NULL) {
		SPDK_ERRLOG("Subsystem %s: missing serial number\n", name);
		goto error;
	}

	if (spdk_nvmf_subsystem_set_sn(subsystem, sn)) {
		SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", name, sn);
		goto error;
	}

	for (j = 0; j < num_ns; j++) {
		struct spdk_nvmf_ns_params *ns_params = &ns_list[j];
		struct spdk_nvmf_ns_opts ns_opts;

		if (!ns_params->bdev_name) {
			SPDK_ERRLOG("Namespace missing bdev name\n");
			goto error;
		}

		bdev = spdk_bdev_get_by_name(ns_params->bdev_name);
		if (bdev == NULL) {
			SPDK_ERRLOG("Could not find namespace bdev '%s'\n", ns_params->bdev_name);
			goto error;
		}

		spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
		ns_opts.nsid = ns_params->nsid;

		if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts)) == 0) {
			goto error;
		}

		SPDK_NOTICELOG("Attaching block device %s to subsystem %s\n",
			       spdk_bdev_get_name(bdev), spdk_nvmf_subsystem_get_nqn(subsystem));

	}

	return subsystem;

error:
	spdk_nvmf_subsystem_destroy(subsystem);
	return NULL;
}
