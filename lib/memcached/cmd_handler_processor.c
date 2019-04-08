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

#include <sys/queue.h>

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"
#include "spdk/env.h"

#include "spdk/hashtable.h"
#include "spdk/slab.h"

#include "memcached/memcached.h"
#include "memcached/memcached_def.h"
#include "memcached/cmd_handler.h"
#include "memcached/diskitem.h"
#include "memcached/memcached_cmd.h"

#if 0
struct spdk_memcached_cmd_add_cb_args {
	struct spdk_slot_item *ditem;
	struct hashitem *mitem;
	int pending_existed_num;
};

struct spdk_memcached_cmd_delete_cb_args {
	int pending_existed_num;
};

struct spdk_memcached_cmd_get_cb_args {
	int pending_existed_num;
};
#endif

static void
memcached_execute_cmd_done(struct spdk_memcached_cmd *cmd)
{
	spdk_memcached_cmd_done(cmd);
}

#if 1 /* process add cmd */
static void
process_add_cmd_cpl(struct spdk_memcached_cmd *cmd)
{
	int rc;

	if (cmd->status == MEMCACHED_ITEM_EXISTS) {
		rc = snprintf(cmd->response, strlen(STR_EXISTS) + 1, STR_EXISTS);
	} else if (cmd->status == MEMCACHED_ITEM_NOT_STORED) {
		rc = snprintf(cmd->response, strlen(STR_STORED) + 1, STR_STORED);
	} else {
		assert(false);
	}

	cmd->send_len = rc;
	cmd->send_off = 0;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);

	memcached_execute_cmd_done(cmd);
}

static void
process_add_nonexisted_store_cb(void *cb_arg, int err)
{
	struct spdk_memcached_cmd *cmd = cb_arg;

	assert(err == 0);
	cmd->status = MEMCACHED_ITEM_NOT_STORED;
	process_add_cmd_cpl(cmd);
}

static int
process_add_cmd_add_item(struct spdk_memcached_cmd *cmd)
{
	struct spdk_slot_item *sitem;
	struct hashitem *mitem;
	int totel_dsize;
	int rc;

	rc = spdk_hashtable_locate_new_items(cmd->key_hash, &mitem, NULL, 0);
	assert(mitem);

	totel_dsize = memcached_diskitem_required_size(&cmd->cmd_hd);
	rc = spdk_slab_get_item(totel_dsize, &sitem);
	assert(rc == 0);

	spdk_hashtable_item_set_info(mitem, (struct slot_item *)sitem, totel_dsize);

	rc = spdk_slab_item_store(sitem, cmd->mobj_write->buf, totel_dsize,
				  process_add_nonexisted_store_cb, cmd);
	assert(rc == 0);

	return rc;
}

static int process_add_cmd_existed_item(struct spdk_memcached_cmd *cmd);

static void
process_add_existed_obtain_cb(void *cb_arg, int err)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	struct spdk_memcached_diskitem *ditem;
	char *existed_key;

	assert(err == 0);

	ditem = (struct spdk_memcached_diskitem *)cmd->mobj_read->buf;
	existed_key = memcached_diskitem_get_key(ditem);

	if (strcmp(existed_key, cmd->cmd_hd.key) == 0) {
		cmd->status = MEMCACHED_ITEM_EXISTS;
		/* for add, it should directly returned if item existed */
		process_add_cmd_cpl(cmd);
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Collision hash on 0x%lx\n", cmd->key_hash);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "existed key is %s; expected key is %s\n", existed_key, cmd->cmd_hd.key);
	process_add_cmd_existed_item(cmd);
}

static int
process_add_cmd_existed_item(struct spdk_memcached_cmd *cmd)
{
	struct hashitem *mitem;
	struct hashitem *existed_items[8];
	int existed_num = 8;
	struct spdk_slot_item *sitem_existed;
	int rc;
	int existed_dsize;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;

	existed_num = spdk_hashtable_locate_existed_items(cmd->key_hash, existed_items, existed_num);
	assert(existed_num > 0);
	assert(existed_num < 8);

	if (existed_num == args->existed_step) {
		/* All existed mitems are checked. item is not existed */
		rc = process_add_cmd_add_item(cmd);
		return rc;
	}

	mitem = existed_items[args->existed_step];
	args->existed_step++;

	/* Read out existed ditem to verify key */
	spdk_hashtable_item_get_info(mitem, (struct slot_item **)&sitem_existed, &existed_dsize);

	/* TODO: mobj_read should be gotten by ddate length */
	if (cmd->mobj_read) {
		spdk_mempool_put(g_spdk_memcached.diskdata_pool, cmd->mobj_read);
	}
	assert(existed_dsize < SPDK_MEMCACHED_MAX_DISKDATA_LENGTH);
	cmd->mobj_read = spdk_mempool_get(g_spdk_memcached.diskdata_pool);
	assert(cmd->mobj_read);

	args->mitem = mitem;
	args->sitem = sitem_existed;

	rc = spdk_slab_item_obtain(sitem_existed, cmd->mobj_read->buf, existed_dsize,
				   process_add_existed_obtain_cb, cmd);
	assert(rc == 0);

	return rc;
}

static int
process_add_cmd(struct spdk_memcached_cmd *cmd)
{
	int rc;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;
	bool is_existed;

	is_existed = spdk_hashtable_is_existed_item(cmd->key_hash);
	if (is_existed) {
		args->existed_step = 0;
		rc = process_add_cmd_existed_item(cmd);
	} else {
		rc = process_add_cmd_add_item(cmd);
	}

	return rc;
}
#endif


#if 1 /* process_get_cmd */
static void
process_get_cmd_cpl(struct spdk_memcached_cmd *cmd)
{
	struct spdk_memcached_diskitem *ditem;
	int rc;

	rc = snprintf(cmd->response, strlen(STR_END) + 1, STR_END);
	cmd->send_len = rc;

	if (cmd->status == MEMCACHED_ITEM_EXISTS) {
		uint32_t datalen;

		ditem = (struct spdk_memcached_diskitem *)cmd->mobj_read->buf;

		datalen = memcached_diskitem_get_data_len(ditem);

		rc = snprintf(cmd->outbuf, strlen(STR_VALUE_1) + 1, STR_VALUE_1);
		rc += snprintf(cmd->outbuf + rc, cmd->cmd_hd.key_len + 1, "%s", cmd->cmd_hd.key);
		rc += snprintf(cmd->outbuf + rc, sizeof(cmd->outbuf) - rc - 1, STR_VALUE_3, 0, datalen);
		cmd->send_len = rc;
		cmd->send_len += datalen;

		/* add \n between data and "END" */
		cmd->response[0] = '\r';
		cmd->response[1] = '\n';
		rc = snprintf(cmd->response + 2, strlen(STR_END) + 1, STR_END);
		cmd->send_len += rc + 2;
	} else if (cmd->status == MEMCACHED_ITEM_NOT_FOUND) {
		rc = snprintf(cmd->response, strlen(STR_END) + 1, STR_END);
		cmd->send_len = rc;
	} else {
		assert(false);
	}

	cmd->send_off = 0;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);
}

static int process_get_cmd_existed_item(struct spdk_memcached_cmd *cmd);

static void
process_get_existed_obtain_cb(void *cb_arg, int err)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	struct spdk_memcached_diskitem *ditem;
	char *existed_key;

	assert(err == 0);

	ditem = (struct spdk_memcached_diskitem *)cmd->mobj_read->buf;
	existed_key = memcached_diskitem_get_key(ditem);

	if (strcmp(existed_key, cmd->cmd_hd.key) == 0) {
		cmd->status = MEMCACHED_ITEM_EXISTS;
		/* for add, it should directly returned if item existed */
		process_get_cmd_cpl(cmd);
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Collision hash on 0x%lx\n", cmd->key_hash);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "existed key is %s; expected key is %s\n", existed_key, cmd->cmd_hd.key);
	process_get_cmd_existed_item(cmd);
}

static int
process_get_cmd_existed_item(struct spdk_memcached_cmd *cmd)
{
	struct hashitem *mitem;
	struct hashitem *existed_items[8];
	int existed_num = 8;
	struct spdk_slot_item *sitem_existed;
	int rc;
	int existed_dsize;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;

	existed_num = spdk_hashtable_locate_existed_items(cmd->key_hash, existed_items, existed_num);
	assert(existed_num > 0);
	assert(existed_num < 8);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "key_hash 0x%lx existed number is %d\n", cmd->key_hash, existed_num);

	if (existed_num == args->existed_step) {
		/* All existed mitems are checked. item is not existed */
		cmd->status = MEMCACHED_ITEM_NOT_FOUND;

		process_get_cmd_cpl(cmd);
		return 0;
	}

	mitem = existed_items[args->existed_step];
	args->existed_step++;

	/* Read out existed ditem to verify key */
	spdk_hashtable_item_get_info(mitem, (struct slot_item **)&sitem_existed, &existed_dsize);

	/* TODO: mobj_read should be gotten by ddate length */
	if (cmd->mobj_read) {
		spdk_mempool_put(g_spdk_memcached.diskdata_pool, cmd->mobj_read);
	}
	assert(existed_dsize < SPDK_MEMCACHED_MAX_DISKDATA_LENGTH);
	cmd->mobj_read = spdk_mempool_get(g_spdk_memcached.diskdata_pool);
	assert(cmd->mobj_read);

	args->mitem = mitem;
	args->sitem = sitem_existed;

	rc = spdk_slab_item_obtain(sitem_existed, cmd->mobj_read->buf, existed_dsize,
				   process_get_existed_obtain_cb, cmd);
	assert(rc == 0);

	return rc;
}

static int
process_get_cmd(struct spdk_memcached_cmd *cmd)
{
	int rc;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;
	bool is_existed;

	is_existed = spdk_hashtable_is_existed_item(cmd->key_hash);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "key_hash 0x%lx is existed: %s\n", cmd->key_hash, is_existed ? "Yes" : "No");
	if (is_existed) {
		args->existed_step = 0;
		rc = process_get_cmd_existed_item(cmd);
	} else {
		cmd->status = MEMCACHED_ITEM_NOT_FOUND;

		process_get_cmd_cpl(cmd);
		rc = 0;
	}

	return rc;
}
#endif

#if 1 /* process_delete_cmd */
static void
process_delete_cmd_cpl(struct spdk_memcached_cmd *cmd)
{
	int rc;

	if (cmd->status == MEMCACHED_ITEM_EXISTS) {
		rc = snprintf(cmd->response, strlen(STR_DELETED) + 1, STR_DELETED);
	} else if (cmd->status == MEMCACHED_ITEM_NOT_FOUND) {
		rc = snprintf(cmd->response, strlen(STR_NOT_FOUND) + 1, STR_NOT_FOUND);
	}

	cmd->send_len = rc;
	cmd->send_off = 0;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);

	memcached_execute_cmd_done(cmd);
}

static int process_delete_cmd_existed_item(struct spdk_memcached_cmd *cmd);

static void
process_delete_existed_obtain_cb(void *cb_arg, int err)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;
	struct spdk_memcached_diskitem *ditem;
	char *existed_key;
	int rc;

	assert(err == 0);

	ditem = (struct spdk_memcached_diskitem *)cmd->mobj_read->buf;
	existed_key = memcached_diskitem_get_key(ditem);

	if (strcmp(existed_key, cmd->cmd_hd.key) == 0) {
		cmd->status = MEMCACHED_ITEM_EXISTS;

		rc = spdk_slab_put_item(args->sitem);
		assert(rc == 0);

		rc = spdk_hashtable_release_item(args->mitem);
		assert(rc == 0);

		process_delete_cmd_cpl(cmd);
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Collision hash on 0x%lx\n", cmd->key_hash);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "existed key is %s; expected key is %s\n", existed_key, cmd->cmd_hd.key);
	process_delete_cmd_existed_item(cmd);
}

static int
process_delete_cmd_existed_item(struct spdk_memcached_cmd *cmd)
{
	struct hashitem *mitem;
	struct hashitem *existed_items[8];
	int existed_num = 8;
	struct spdk_slot_item *sitem_existed;
	int rc;
	int existed_dsize;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;

	existed_num = spdk_hashtable_locate_existed_items(cmd->key_hash, existed_items, existed_num);
	assert(existed_num > 0);
	assert(existed_num < 8);

	if (existed_num == args->existed_step) {
		/* All existed mitems are checked. item is not existed */
		cmd->status = MEMCACHED_ITEM_NOT_FOUND;

		process_delete_cmd_cpl(cmd);
		return 0;
	}

	mitem = existed_items[args->existed_step];
	args->existed_step++;

	/* Read out existed ditem to verify key */
	spdk_hashtable_item_get_info(mitem, (struct slot_item **)&sitem_existed, &existed_dsize);

	/* TODO: mobj_read should be gotten by ddate length */
	assert(existed_dsize < SPDK_MEMCACHED_MAX_DISKDATA_LENGTH);
	cmd->mobj_read = spdk_mempool_get(g_spdk_memcached.diskdata_pool);
	assert(cmd->mobj_read);

	args->mitem = mitem;
	args->sitem = sitem_existed;

	rc = spdk_slab_item_obtain(sitem_existed, cmd->mobj_read->buf, existed_dsize,
				   process_delete_existed_obtain_cb, cmd);
	assert(rc == 0);

	return rc;
}

static int
process_delete_cmd(struct spdk_memcached_cmd *cmd)
{
	int rc;
	struct spdk_memcached_cmd_cb_args *args = &cmd->args;
	bool is_existed;

	is_existed = spdk_hashtable_is_existed_item(cmd->key_hash);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "key_hash 0x%lx is existed: %s\n", cmd->key_hash, is_existed ? "Yes" : "No");
	if (is_existed) {
		args->existed_step = 0;
		rc = process_delete_cmd_existed_item(cmd);
	} else {
		cmd->status = MEMCACHED_ITEM_NOT_FOUND;

		process_delete_cmd_cpl(cmd);
		rc = 0;
	}

	return rc;
}
#endif

static int
process_invalid_cmd(struct spdk_memcached_cmd *cmd)
{
	int rc;

	rc = snprintf(cmd->response, strlen(STR_ERR_NONEXIST_CMD) + 1, STR_ERR_NONEXIST_CMD);
	cmd->send_len = rc;
	cmd->send_off = 0;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Cmd response is %s\n", cmd->response);
	memcached_execute_cmd_done(cmd);

	return 0;
}

struct memcached_cmd_methods_processor cmd_processors[MEMCACHED_CMD_NUM] = {
	{"get",	MEMCACHED_CMD_GET, process_get_cmd},
	{"set", MEMCACHED_CMD_SET, process_invalid_cmd},
	{"add", MEMCACHED_CMD_ADD, process_add_cmd},
	{"replace", MEMCACHED_CMD_REPLACE, process_invalid_cmd},
	{"delete", MEMCACHED_CMD_DELETE, process_delete_cmd},
	{"invalid_cmd", MEMCACHED_CMD_INVALID_CMD, process_invalid_cmd},
};
