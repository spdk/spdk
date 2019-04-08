/*
 * memcached_rpc.c
 *
 *  Created on: Apr 15, 2019
 *      Author: root
 */


#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"

#include "spdk_internal/log.h"

static const struct spdk_json_object_decoder rpc_set_memcached_opts_decoders[] = {

};

static void
spdk_rpc_memcached_set_opts(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	(void)rpc_set_memcached_opts_decoders;
}

SPDK_RPC_REGISTER("set_memcached_options", spdk_rpc_memcached_set_opts, SPDK_RPC_STARTUP)

