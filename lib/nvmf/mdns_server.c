/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright (c) 2022 Dell Inc, or its subsidiaries.
 *  Copyright (c) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 *  All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "nvmf_internal.h"
#include "spdk/log.h"
#include "spdk/config.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

#ifdef SPDK_CONFIG_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#define NVMF_MAX_DNS_NAME_LENGTH 255

static AvahiSimplePoll *g_avahi_publish_simple_poll = NULL;
static AvahiClient *g_avahi_publish_client = NULL;
static AvahiEntryGroup *g_avahi_entry_group = NULL;

struct mdns_publish_ctx {
	struct spdk_poller		*poller;
	struct spdk_nvmf_subsystem	*subsystem;
	struct spdk_nvmf_tgt		*tgt;
};

static struct mdns_publish_ctx *g_mdns_publish_ctx = NULL;

static void
nvmf_avahi_publish_destroy(struct mdns_publish_ctx *ctx)
{
	if (g_avahi_entry_group) {
		avahi_entry_group_free(g_avahi_entry_group);
		g_avahi_entry_group = NULL;
	}

	if (g_avahi_publish_client) {
		avahi_client_free(g_avahi_publish_client);
		g_avahi_publish_client = NULL;
	}

	if (g_avahi_publish_simple_poll) {
		avahi_simple_poll_free(g_avahi_publish_simple_poll);
		g_avahi_publish_simple_poll = NULL;
	}

	g_mdns_publish_ctx = NULL;
	free(ctx);
}

static int
nvmf_avahi_publish_iterate(void *arg)
{
	struct mdns_publish_ctx *ctx = arg;
	int rc;

	if (ctx == NULL) {
		assert(false);
		return SPDK_POLLER_IDLE;
	}

	rc = avahi_simple_poll_iterate(g_avahi_publish_simple_poll, 0);
	if (rc && rc != -EAGAIN) {
		SPDK_ERRLOG("avahi publish poll returned error\n");
		spdk_poller_unregister(&ctx->poller);
		nvmf_avahi_publish_destroy(ctx);
		return SPDK_POLLER_BUSY;
	}

	return SPDK_POLLER_BUSY;
}

static void
nvmf_ctx_stop_mdns_prr(struct mdns_publish_ctx *ctx)
{
	SPDK_INFOLOG(nvmf, "Stopping avahi publish poller\n");
	spdk_poller_unregister(&ctx->poller);
	nvmf_avahi_publish_destroy(ctx);
}

static bool
nvmf_tgt_is_mdns_running(struct spdk_nvmf_tgt *tgt)
{
	if (g_mdns_publish_ctx && g_mdns_publish_ctx->tgt == tgt) {
		return true;
	}
	return false;
}

void
nvmf_tgt_stop_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	if (nvmf_tgt_is_mdns_running(tgt) == true) {
		nvmf_ctx_stop_mdns_prr(g_mdns_publish_ctx);
		return;
	}
}

static void
avahi_entry_group_add_listeners(AvahiEntryGroup *avahi_entry_group,
				struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_listener *listener;
	const char *name_base = "spdk";
	const char *type_base = "_nvme-disc";
	const char *domain = "local";
	char *protocol;
	char name[NVMF_MAX_DNS_NAME_LENGTH];
	char type[NVMF_MAX_DNS_NAME_LENGTH];
	char txt_protocol[NVMF_MAX_DNS_NAME_LENGTH];
	char txt_nqn[NVMF_MAX_DNS_NAME_LENGTH];
	AvahiStringList *txt = NULL;
	uint16_t port;
	uint16_t id = 0;

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (listener->trid->trtype == SPDK_NVME_TRANSPORT_TCP) {
			protocol = "tcp";
		} else if (listener->trid->trtype == SPDK_NVME_TRANSPORT_RDMA) {
			SPDK_ERRLOG("Current SPDK doesn't distinguish RoCE(udp) and iWARP(tcp). Skip adding listener id %d to avahi entry",
				    listener->id);
			continue;
		} else {
			SPDK_ERRLOG("mDNS PRR does not support trtype %d", listener->trid->trtype);
			continue;
		}

		snprintf(type, sizeof(type), "%s._%s", type_base, protocol);
		snprintf(name, sizeof(name), "%s%d", name_base, id++);
		snprintf(txt_protocol, sizeof(txt_protocol), "p=%s", protocol);
		snprintf(txt_nqn, sizeof(txt_nqn), "nqn=%s", SPDK_NVMF_DISCOVERY_NQN);
		txt = avahi_string_list_add(txt, txt_protocol);
		txt = avahi_string_list_add(txt, txt_nqn);
		port = spdk_strtol(listener->trid->trsvcid, 10);

		if (avahi_entry_group_add_service_strlst(avahi_entry_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				0, name, type, domain, NULL, port, txt) < 0) {
			SPDK_ERRLOG("Failed to add avahi service name: %s, type: %s, domain: %s, port: %d",
				    name, type, domain, port);
		}
		avahi_string_list_free(txt);
		txt = NULL;
	}

	avahi_entry_group_commit(avahi_entry_group);
}

int
nvmf_tgt_update_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	int rc;

	if (nvmf_tgt_is_mdns_running(tgt) == false || g_avahi_entry_group == NULL) {
		SPDK_INFOLOG(nvmf,
			     "nvmf_tgt_update_mdns_prr is only supported when mDNS servier is running on target\n");
		return 0;
	}

	rc = avahi_entry_group_reset(g_avahi_entry_group);
	if (rc) {
		SPDK_ERRLOG("Failed to reset avahi_entry_group");
		return -EINVAL;
	}

	avahi_entry_group_add_listeners(g_avahi_entry_group, g_mdns_publish_ctx->subsystem);

	return 0;
}

static int
publish_pull_registration_request(AvahiClient *client, struct mdns_publish_ctx *publish_ctx)
{
	struct spdk_nvmf_subsystem *subsystem = publish_ctx->subsystem;

	if (g_avahi_entry_group != NULL) {
		return 0;
	}

	g_avahi_entry_group = avahi_entry_group_new(client, NULL, NULL);
	if (g_avahi_entry_group == NULL) {
		SPDK_ERRLOG("avahi_entry_group_new failure: %s\n", avahi_strerror(avahi_client_errno(client)));
		return -1;
	}

	avahi_entry_group_add_listeners(g_avahi_entry_group, subsystem);

	return 0;
}

static void
publish_client_new_callback(AvahiClient *client, AvahiClientState avahi_state,
			    AVAHI_GCC_UNUSED void *user_data)
{
	int rc;
	struct mdns_publish_ctx *publish_ctx = user_data;

	switch (avahi_state) {
	case AVAHI_CLIENT_S_RUNNING:
		rc = publish_pull_registration_request(client, publish_ctx);
		if (rc) {
			nvmf_ctx_stop_mdns_prr(publish_ctx);
		}
		break;
	case AVAHI_CLIENT_CONNECTING:
		SPDK_INFOLOG(nvmf, "Avahi client waiting for avahi-daemon");
		break;
	case AVAHI_CLIENT_S_REGISTERING:
		SPDK_INFOLOG(nvmf, "Avahi client registering service");
		break;
	case AVAHI_CLIENT_FAILURE:
		SPDK_ERRLOG("Server connection failure: %s\n", avahi_strerror(avahi_client_errno(client)));
		nvmf_ctx_stop_mdns_prr(publish_ctx);
		break;
	case AVAHI_CLIENT_S_COLLISION:
		SPDK_ERRLOG("Avahi client name is already used in the mDNS");
		nvmf_ctx_stop_mdns_prr(publish_ctx);
		break;
	default:
		SPDK_ERRLOG("Avahi client is in unsupported state");
		break;
	}
}

int
nvmf_publish_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	int error;
	struct mdns_publish_ctx *publish_ctx = NULL;
	struct spdk_nvmf_subsystem *subsystem = NULL;

	if (g_mdns_publish_ctx != NULL) {
		if (g_mdns_publish_ctx->tgt == tgt) {
			SPDK_ERRLOG("mDNS server is already running on target %s.\n", tgt->name);
			return -EEXIST;
		}
		SPDK_ERRLOG("mDNS server does not support publishing multiple targets simultaneously.");
		return -EINVAL;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, SPDK_NVMF_DISCOVERY_NQN);
	if (TAILQ_EMPTY(&subsystem->listeners)) {
		SPDK_ERRLOG("Discovery subsystem has no listeners.\n");
		return -EINVAL;
	}

	publish_ctx = calloc(1, sizeof(*publish_ctx));
	if (publish_ctx == NULL) {
		SPDK_ERRLOG("Error creating mDNS publish ctx\n");
		return -ENOMEM;
	}
	publish_ctx->subsystem = subsystem;
	publish_ctx->tgt = tgt;
	/* Allocate main loop object */
	g_avahi_publish_simple_poll = avahi_simple_poll_new();
	if (g_avahi_publish_simple_poll == NULL) {
		SPDK_ERRLOG("Failed to create poll object for mDNS publish.\n");
		nvmf_avahi_publish_destroy(publish_ctx);
		return -ENOMEM;
	}

	assert(g_avahi_publish_client == NULL);

	/* Allocate a new client */
	g_avahi_publish_client = avahi_client_new(avahi_simple_poll_get(g_avahi_publish_simple_poll),
				 0, publish_client_new_callback, publish_ctx, &error);
	/* Check whether creating the client object succeeded */
	if (g_avahi_publish_client == NULL) {
		SPDK_ERRLOG("Failed to create mDNS client Error: %s\n", avahi_strerror(error));
		nvmf_avahi_publish_destroy(publish_ctx);
		return -ENOMEM;
	}

	g_mdns_publish_ctx = publish_ctx;
	publish_ctx->poller = SPDK_POLLER_REGISTER(nvmf_avahi_publish_iterate, publish_ctx, 100 * 1000);
	return 0;
}
#endif
