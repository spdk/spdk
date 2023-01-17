/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright (c) 2022 Dell Inc, or its subsidiaries.
 *  All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/version.h"

#include "spdk_internal/event.h"

#include "spdk/assert.h"
#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/string.h"
#include "spdk/scheduler.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/nvme.h"
#include "bdev_nvme.h"

#ifdef SPDK_CONFIG_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

static AvahiSimplePoll *g_avahi_simple_poll = NULL;
static AvahiClient *g_avahi_client = NULL;

struct mdns_discovery_entry_ctx {
	char                                            name[256];
	struct spdk_nvme_transport_id                   trid;
	struct spdk_nvme_ctrlr_opts                     drv_opts;
	TAILQ_ENTRY(mdns_discovery_entry_ctx)           tailq;
	struct mdns_discovery_ctx                       *ctx;
};

struct mdns_discovery_ctx {
	char                                    *name;
	char                                    *svcname;
	char                                    *hostnqn;
	AvahiServiceBrowser                     *sb;
	struct spdk_poller                      *poller;
	struct spdk_nvme_ctrlr_opts             drv_opts;
	struct nvme_ctrlr_opts                  bdev_opts;
	uint32_t                                seqno;
	bool                                    stop;
	struct spdk_thread                      *calling_thread;
	TAILQ_ENTRY(mdns_discovery_ctx)         tailq;
	TAILQ_HEAD(, mdns_discovery_entry_ctx)  mdns_discovery_entry_ctxs;
};

TAILQ_HEAD(mdns_discovery_ctxs, mdns_discovery_ctx);
static struct mdns_discovery_ctxs g_mdns_discovery_ctxs = TAILQ_HEAD_INITIALIZER(
			g_mdns_discovery_ctxs);

static struct mdns_discovery_entry_ctx *
create_mdns_discovery_entry_ctx(struct mdns_discovery_ctx *ctx, struct spdk_nvme_transport_id *trid)
{
	struct mdns_discovery_entry_ctx *new_ctx;

	assert(ctx);
	assert(trid);
	new_ctx = calloc(1, sizeof(*new_ctx));
	if (new_ctx == NULL) {
		SPDK_ERRLOG("could not allocate new mdns_entry_ctx\n");
		return NULL;
	}

	new_ctx->ctx = ctx;
	memcpy(&new_ctx->trid, trid, sizeof(struct spdk_nvme_transport_id));
	snprintf(new_ctx->name, sizeof(new_ctx->name), "%s%u_nvme", ctx->name, ctx->seqno);
	memcpy(&new_ctx->drv_opts, &ctx->drv_opts, sizeof(ctx->drv_opts));
	snprintf(new_ctx->drv_opts.hostnqn, sizeof(ctx->drv_opts.hostnqn), "%s", ctx->hostnqn);
	ctx->seqno = ctx->seqno + 1;
	return new_ctx;
}

static void
mdns_bdev_nvme_start_discovery(void *_entry_ctx)
{
	int status;
	struct mdns_discovery_entry_ctx *entry_ctx = _entry_ctx;

	assert(_entry_ctx);
	status = bdev_nvme_start_discovery(&entry_ctx->trid, entry_ctx->name,
					   &entry_ctx->ctx->drv_opts,
					   &entry_ctx->ctx->bdev_opts,
					   0, true, NULL, NULL);
	if (status) {
		SPDK_ERRLOG("Error starting discovery for name %s addr %s port %s subnqn %s &trid %p\n",
			    entry_ctx->ctx->name, entry_ctx->trid.traddr, entry_ctx->trid.trsvcid,
			    entry_ctx->trid.subnqn, &entry_ctx->trid);
	}
}

static void
free_mdns_discovery_entry_ctx(struct mdns_discovery_ctx *ctx)
{
	struct mdns_discovery_entry_ctx *entry_ctx = NULL;

	if (!ctx) {
		return;
	}

	TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
		free(entry_ctx);
	}
}

static void
free_mdns_discovery_ctx(struct mdns_discovery_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	free(ctx->name);
	free(ctx->svcname);
	free(ctx->hostnqn);
	avahi_service_browser_free(ctx->sb);
	free_mdns_discovery_entry_ctx(ctx);
	free(ctx);
}

/* get_key_val_avahi_resolve_txt - Search for the key string in the TXT received
 *                            from Avavi daemon and return its value.
 *   input
 *       txt: TXT returned by Ahavi daemon will be of format
 *            "NQN=nqn.1988-11.com.dell:SFSS:1:20221122170722e8" "p=tcp foo" and the
 *            AvahiStringList txt is a linked list with each node holding a
 *            key-value pair like key:p value:tcp
 *
 *       key: Key string to search in the txt list
 *   output
 *       Returns the value for the key or NULL if key is not present
 *       Returned string needs to be freed with avahi_free()
 */
static char *
get_key_val_avahi_resolve_txt(AvahiStringList *txt, const char *key)
{
	char *k = NULL, *v = NULL;
	AvahiStringList *p = NULL;
	int r;

	if (!txt || !key) {
		return NULL;
	}

	p = avahi_string_list_find(txt, key);
	if (!p) {
		return NULL;
	}

	r = avahi_string_list_get_pair(p, &k, &v, NULL);
	if (r < 0) {
		return NULL;
	}

	avahi_free(k);
	return v;
}

static int
get_spdk_nvme_transport_from_proto_str(char *protocol, enum spdk_nvme_transport_type *trtype)
{
	int status = -1;

	if (!protocol || !trtype) {
		return status;
	}

	if (strcmp("tcp", protocol) == 0) {
		*trtype = SPDK_NVME_TRANSPORT_TCP;
		return 0;
	}

	return status;
}

static enum spdk_nvmf_adrfam
get_spdk_nvme_adrfam_from_avahi_addr(const AvahiAddress *address) {

	if (!address)
	{
		/* Return ipv4 by default */
		return SPDK_NVMF_ADRFAM_IPV4;
	}

	switch (address->proto)
	{
	case AVAHI_PROTO_INET:
		return SPDK_NVMF_ADRFAM_IPV4;
	case AVAHI_PROTO_INET6:
		return SPDK_NVMF_ADRFAM_IPV6;
	default:
		return SPDK_NVMF_ADRFAM_IPV4;
	}
}

static struct mdns_discovery_ctx *
get_mdns_discovery_ctx_by_svcname(const char *svcname)
{
	struct mdns_discovery_ctx *ctx = NULL, *tmp_ctx = NULL;

	if (!svcname) {
		return NULL;
	}

	TAILQ_FOREACH_SAFE(ctx, &g_mdns_discovery_ctxs, tailq, tmp_ctx) {
		if (strcmp(ctx->svcname, svcname) == 0) {
			return ctx;
		}
	}
	return NULL;
}

static void
mdns_resolve_callback(
	AvahiServiceResolver *r,
	AVAHI_GCC_UNUSED AvahiIfIndex interface,
	AVAHI_GCC_UNUSED AvahiProtocol protocol,
	AvahiResolverEvent event,
	const char *name,
	const char *type,
	const char *domain,
	const char *host_name,
	const AvahiAddress *address,
	uint16_t port,
	AvahiStringList *txt,
	AvahiLookupResultFlags flags,
	AVAHI_GCC_UNUSED void *userdata)
{
	assert(r);
	/* Called whenever a service has been resolved successfully or timed out */
	switch (event) {
	case AVAHI_RESOLVER_FAILURE:
		SPDK_ERRLOG("(Resolver) Failed to resolve service '%s' of type '%s' in domain '%s': %s\n",
			    name, type, domain,
			    avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(r))));
		break;
	case AVAHI_RESOLVER_FOUND: {
		char ipaddr[SPDK_NVMF_TRADDR_MAX_LEN + 1], port_str[SPDK_NVMF_TRSVCID_MAX_LEN + 1], *t;
		struct spdk_nvme_transport_id *trid = NULL;
		char *subnqn = NULL, *proto = NULL;
		struct mdns_discovery_ctx *ctx = NULL;
		struct mdns_discovery_entry_ctx *entry_ctx = NULL;
		int status = -1;

		memset(ipaddr, 0, sizeof(ipaddr));
		memset(port_str, 0, sizeof(port_str));
		SPDK_INFOLOG(bdev_nvme, "Service '%s' of type '%s' in domain '%s'\n", name, type, domain);
		avahi_address_snprint(ipaddr, sizeof(ipaddr), address);
		snprintf(port_str, sizeof(port_str), "%d", port);
		t = avahi_string_list_to_string(txt);
		SPDK_INFOLOG(bdev_nvme,
			     "\t%s:%u (%s)\n"
			     "\tTXT=%s\n"
			     "\tcookie is %u\n"
			     "\tis_local: %i\n"
			     "\tour_own: %i\n"
			     "\twide_area: %i\n"
			     "\tmulticast: %i\n"
			     "\tcached: %i\n",
			     host_name, port, ipaddr,
			     t,
			     avahi_string_list_get_service_cookie(txt),
			     !!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
			     !!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
			     !!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
			     !!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
			     !!(flags & AVAHI_LOOKUP_RESULT_CACHED));

		ctx = get_mdns_discovery_ctx_by_svcname(type);
		if (!ctx) {
			SPDK_ERRLOG("Unknown Service '%s'\n", type);
			break;
		}

		trid = (struct spdk_nvme_transport_id *) calloc(1, sizeof(struct spdk_nvme_transport_id));
		if (!trid) {
			SPDK_ERRLOG(" Error allocating memory for trid\n");
			break;
		}
		trid->adrfam = get_spdk_nvme_adrfam_from_avahi_addr(address);
		if (trid->adrfam != SPDK_NVMF_ADRFAM_IPV4) {
			/* TODO: For now process only ipv4 addresses */
			SPDK_INFOLOG(bdev_nvme, "trid family is not IPV4 %d\n", trid->adrfam);
			free(trid);
			break;
		}
		subnqn = get_key_val_avahi_resolve_txt(txt, "NQN");
		if (!subnqn) {
			free(trid);
			SPDK_ERRLOG("subnqn received is empty for service %s\n", ctx->svcname);
			break;
		}
		proto = get_key_val_avahi_resolve_txt(txt, "p");
		if (!proto) {
			free(trid);
			avahi_free(subnqn);
			SPDK_ERRLOG("Protocol not received for service %s\n", ctx->svcname);
			break;
		}
		status = get_spdk_nvme_transport_from_proto_str(proto, &trid->trtype);
		if (status) {
			free(trid);
			avahi_free(subnqn);
			avahi_free(proto);
			SPDK_ERRLOG("Unable to derive nvme transport type  for service %s\n", ctx->svcname);
			break;
		}
		trid->adrfam = get_spdk_nvme_adrfam_from_avahi_addr(address);
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", ipaddr);
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", port_str);
		snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", subnqn);
		TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
			if (!spdk_nvme_transport_id_compare(trid, &entry_ctx->trid)) {
				SPDK_ERRLOG("mDNS discovery entry exists already. trid->traddr: %s trid->trsvcid: %s\n",
					    trid->traddr, trid->trsvcid);
				free(trid);
				avahi_free(subnqn);
				avahi_free(proto);
				break;
			}
		}
		entry_ctx = create_mdns_discovery_entry_ctx(ctx, trid);
		TAILQ_INSERT_TAIL(&ctx->mdns_discovery_entry_ctxs, entry_ctx, tailq);
		spdk_thread_send_msg(ctx->calling_thread, mdns_bdev_nvme_start_discovery, entry_ctx);
		free(trid);
		avahi_free(subnqn);
		avahi_free(proto);
		break;
	}
	default:
		SPDK_ERRLOG("Unknown Avahi resolver event: %d", event);
	}
	avahi_service_resolver_free(r);
}

static void
mdns_browse_callback(
	AvahiServiceBrowser *b,
	AvahiIfIndex interface,
	AvahiProtocol protocol,
	AvahiBrowserEvent event,
	const char *name,
	const char *type,
	const char *domain,
	AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
	void *userdata)
{
	AvahiClient *c = userdata;

	assert(b);
	/* Called whenever a new services becomes available on the LAN or is removed from the LAN */
	switch (event) {
	case AVAHI_BROWSER_FAILURE:
		SPDK_ERRLOG("(Browser) Failure: %s\n",
			    avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));
		return;
	case AVAHI_BROWSER_NEW:
		SPDK_DEBUGLOG(bdev_nvme, "(Browser) NEW: service '%s' of type '%s' in domain '%s'\n", name, type,
			      domain);
		/* We ignore the returned resolver object. In the callback
		   function we free it. If the server is terminated before
		   the callback function is called the server will free
		   the resolver for us. */
		if (!(avahi_service_resolver_new(c, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0,
						 mdns_resolve_callback, c))) {
			SPDK_ERRLOG("Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(c)));
		}
		break;
	case AVAHI_BROWSER_REMOVE:
		SPDK_ERRLOG("(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
		/* On remove, we are not doing the automatic cleanup of connections
		 * to the targets that were learnt from the CDC, for which remove event has
		 * been received. If required, user can clear the connections manually by
		 * invoking bdev_nvme_stop_discovery. We can implement the automatic cleanup
		 * later, if there is a requirement in the future.
		 */
		break;
	case AVAHI_BROWSER_ALL_FOR_NOW:
	case AVAHI_BROWSER_CACHE_EXHAUSTED:
		SPDK_INFOLOG(bdev_nvme, "(Browser) %s\n",
			     event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
		break;
	default:
		SPDK_ERRLOG("Unknown Avahi browser event: %d", event);
	}
}

static void
client_callback(AvahiClient *c, AvahiClientState state, AVAHI_GCC_UNUSED void *userdata)
{
	assert(c);
	/* Called whenever the client or server state changes */
	if (state == AVAHI_CLIENT_FAILURE) {
		SPDK_ERRLOG("Server connection failure: %s\n", avahi_strerror(avahi_client_errno(c)));
	}
}

static int
bdev_nvme_avahi_iterate(void *arg)
{
	struct mdns_discovery_ctx *ctx = arg;
	int rc;

	if (ctx->stop) {
		SPDK_INFOLOG(bdev_nvme, "Stopping avahi poller for service %s\n", ctx->svcname);
		spdk_poller_unregister(&ctx->poller);
		TAILQ_REMOVE(&g_mdns_discovery_ctxs, ctx, tailq);
		free_mdns_discovery_ctx(ctx);
		return SPDK_POLLER_IDLE;
	}

	if (g_avahi_simple_poll == NULL) {
		spdk_poller_unregister(&ctx->poller);
		return SPDK_POLLER_IDLE;
	}

	rc = avahi_simple_poll_iterate(g_avahi_simple_poll, 0);
	if (rc && rc != -EAGAIN) {
		SPDK_ERRLOG("avahi poll returned error for service: %s/n", ctx->svcname);
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

static void
start_mdns_discovery_poller(void *arg)
{
	struct mdns_discovery_ctx *ctx = arg;

	assert(arg);
	TAILQ_INSERT_TAIL(&g_mdns_discovery_ctxs, ctx, tailq);
	ctx->poller = SPDK_POLLER_REGISTER(bdev_nvme_avahi_iterate, ctx, 100 * 1000);
}

int
bdev_nvme_start_mdns_discovery(const char *base_name,
			       const char *svcname,
			       struct spdk_nvme_ctrlr_opts *drv_opts,
			       struct nvme_ctrlr_opts *bdev_opts)
{
	AvahiServiceBrowser *sb = NULL;
	int error;
	struct mdns_discovery_ctx *ctx;

	assert(base_name);
	assert(svcname);

	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		if (strcmp(ctx->name, base_name) == 0) {
			SPDK_ERRLOG("mDNS discovery already running with name %s\n", base_name);
			return -EEXIST;
		}

		if (strcmp(ctx->svcname, svcname) == 0) {
			SPDK_ERRLOG("mDNS discovery already running for service %s\n", svcname);
			return -EEXIST;
		}
	}

	if (g_avahi_simple_poll == NULL) {

		/* Allocate main loop object */
		if (!(g_avahi_simple_poll = avahi_simple_poll_new())) {
			SPDK_ERRLOG("Failed to create poll object for mDNS discovery for service: %s.\n", svcname);
			return -ENOMEM;
		}
	}

	if (g_avahi_client == NULL) {

		/* Allocate a new client */
		g_avahi_client = avahi_client_new(avahi_simple_poll_get(g_avahi_simple_poll), 0, client_callback,
						  NULL, &error);
		/* Check whether creating the client object succeeded */
		if (!g_avahi_client) {
			SPDK_ERRLOG("Failed to create mDNS client for service:%s Error: %s\n", svcname,
				    avahi_strerror(error));
			return -ENOMEM;
		}
	}

	/* Create the service browser */
	if (!(sb = avahi_service_browser_new(g_avahi_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, svcname,
					     NULL, 0, mdns_browse_callback, g_avahi_client))) {
		SPDK_ERRLOG("Failed to create service browser for service: %s Error: %s\n", svcname,
			    avahi_strerror(avahi_client_errno(g_avahi_client)));
		return -ENOMEM;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx for service: %s\n", svcname);
		avahi_service_browser_free(sb);
		return -ENOMEM;
	}

	ctx->svcname = strdup(svcname);
	if (ctx->svcname == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx svcname for service: %s\n", svcname);
		free_mdns_discovery_ctx(ctx);
		avahi_service_browser_free(sb);
		return -ENOMEM;
	}
	ctx->name = strdup(base_name);
	if (ctx->name == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx name for service: %s\n", svcname);
		free_mdns_discovery_ctx(ctx);
		avahi_service_browser_free(sb);
		return -ENOMEM;
	}
	memcpy(&ctx->drv_opts, drv_opts, sizeof(*drv_opts));
	memcpy(&ctx->bdev_opts, bdev_opts, sizeof(*bdev_opts));
	ctx->sb = sb;
	ctx->calling_thread = spdk_get_thread();
	TAILQ_INIT(&ctx->mdns_discovery_entry_ctxs);
	/* Even if user did not specify hostnqn, we can still strdup("\0"); */
	ctx->hostnqn = strdup(ctx->drv_opts.hostnqn);
	if (ctx->hostnqn == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx hostnqn for service: %s\n", svcname);
		free_mdns_discovery_ctx(ctx);
		return -ENOMEM;
	}
	/* Start the poller for the Avahi client browser in g_bdev_nvme_init_thread */
	spdk_thread_send_msg(g_bdev_nvme_init_thread, start_mdns_discovery_poller, ctx);
	return 0;
}

static void
mdns_stop_discovery_entry(struct mdns_discovery_ctx *ctx)
{
	struct mdns_discovery_entry_ctx *entry_ctx = NULL;

	assert(ctx);

	TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
		bdev_nvme_stop_discovery(entry_ctx->name, NULL, NULL);
	}
}

int
bdev_nvme_stop_mdns_discovery(const char *name)
{
	struct mdns_discovery_ctx *ctx;

	assert(name);
	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		if (strcmp(name, ctx->name) == 0) {
			if (ctx->stop) {
				return -EALREADY;
			}
			/* set stop to true to stop the mdns poller instance */
			ctx->stop = true;
			mdns_stop_discovery_entry(ctx);
			return 0;
		}
	}

	return -ENOENT;
}

void
bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request)
{
	struct mdns_discovery_ctx *ctx;
	struct mdns_discovery_entry_ctx *entry_ctx;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name", ctx->name);
		spdk_json_write_named_string(w, "svcname", ctx->svcname);

		spdk_json_write_named_array_begin(w, "referrals");
		TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", entry_ctx->name);
			spdk_json_write_named_object_begin(w, "trid");
			nvme_bdev_dump_trid_json(&entry_ctx->trid, w);
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

void
bdev_nvme_mdns_discovery_config_json(struct spdk_json_write_ctx *w)
{
	struct mdns_discovery_ctx *ctx;

	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_nvme_start_mdns_discovery");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", ctx->name);
		spdk_json_write_named_string(w, "svcname", ctx->svcname);
		spdk_json_write_named_string(w, "hostnqn", ctx->hostnqn);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

#else /* SPDK_CONFIG_AVAHI */

int
bdev_nvme_start_mdns_discovery(const char *base_name,
			       const char *svcname,
			       struct spdk_nvme_ctrlr_opts *drv_opts,
			       struct nvme_ctrlr_opts *bdev_opts)
{
	SPDK_ERRLOG("spdk not built with --with-avahi option\n");
	return -ENOTSUP;
}

int
bdev_nvme_stop_mdns_discovery(const char *name)
{
	SPDK_ERRLOG("spdk not built with --with-avahi option\n");
	return -ENOTSUP;
}

void
bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request)
{
	SPDK_ERRLOG("spdk not built with --with-avahi option\n");
	spdk_jsonrpc_send_error_response(request, -ENOTSUP, spdk_strerror(ENOTSUP));
}

void
bdev_nvme_mdns_discovery_config_json(struct spdk_json_write_ctx *w)
{
	/* Empty function to be invoked, when SPDK is built without --with-avahi */
}

#endif
