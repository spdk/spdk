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

#include "memcached/memcached.h"
#include "memcached/memcached_def.h"
#include "memcached/cmd_handler.h"
#include "memcached/diskrecord.h"
#include "memcached/hashtable.h"

/* Avoid warnings on solaris, where isspace() is an index into an array, and gcc uses signed chars */
#define xisspace(c) isspace((unsigned char)c)

#if 1 // API mock functions


static void
process_update_cmd_cb(void *cb_arg, int err)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	int rc;

	assert(err == 0);

	cmd->status = MEMCACHED_ITEM_STORED;
	rc = snprintf(cmd->send_buf, strlen(STR_STORED) + 1, STR_STORED);
	cmd->send_len = rc;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);
}

static int
process_update_cmd(struct spdk_memcached_cmd *cmd)
{
//	struct spdk_memcached_conn *conn;

	int opcode = cmd->cmd_hd.opcode;
	struct mem_item *mitem;
	struct disk_item *ditem;
	int rc;
	bool valid;


	rc = spdk_memcached_get_memitem(cmd->key_hash, &mitem);
	assert(rc == 0);

	valid = spdk_memcached_memitem_is_valid(mitem);

	if (valid == true) {
		if (opcode == MEMCACHED_CMD_ADD) {
			cmd->status = MEMCACHED_ITEM_EXISTS;
			rc = snprintf(cmd->send_buf, strlen(STR_STORED) + 1, STR_STORED);
			goto exit;
		}

		ditem = spdk_memcached_memitem_get_record(mitem);
	} else {
		if (opcode == MEMCACHED_CMD_REPLACE) {
			cmd->status = MEMCACHED_ITEM_NOT_STORED;
			rc = snprintf(cmd->send_buf, strlen(STR_NOT_STROED) + 1, STR_NOT_STROED);
			goto exit;
		}

		int size = memcached_cmd_store_buf_len(cmd);
		// get a new ditem for this key
		rc = spdk_memcached_get_diskitem(size, &ditem);
		assert(rc == 0);

		spdk_memcached_memitem_set_record(mitem, ditem);
	}

	spdk_memcached_diskitem_store(ditem, cmd->store_buf, cmd->store_len, process_update_cmd_cb, cmd);
	return 0;

exit:
	cmd->send_len = rc;
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);

	return 0;
}


static void
process_get_cmd_cb(void *cb_arg, int err)
{
	struct spdk_memcached_cmd *cmd = cb_arg;
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;
	struct spdk_memcached_diskrecord *record;
	int rc;

	assert(err == 0);

	record = (struct spdk_memcached_diskrecord *)cmd->obtain_buf;

	printf("disk key len is %d, request key len is %d\n", record->key_len, hd->key_len);
	assert(record->key_len == hd->key_len);
	rc = strncmp(record->key, hd->key, record->key_len);
	assert(rc == 0);

	cmd->status = MEMCACHED_ITEM_EXISTS;
	rc = snprintf(cmd->send_buf, strlen(STR_VALUE_1) + 1, STR_VALUE_1);
	rc += snprintf(cmd->send_buf + rc, record->key_len + 1, "%s", record->key);
	rc += snprintf(cmd->send_buf + rc, strlen(STR_VALUE_3) + 1, STR_VALUE_3, 0, record->data_len);
	cmd->send_len = rc;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);
}

static int
process_get_cmd(struct spdk_memcached_cmd *cmd)
{
//	int opcode = cmd->cmd_hd.opcode;
	struct mem_item *mitem;
	struct disk_item *ditem;
	int rc;
	bool valid;

	rc = spdk_memcached_get_memitem(cmd->key_hash, &mitem);
	assert(rc == 0);

	valid = spdk_memcached_memitem_is_valid(mitem);
	if (valid == true) {

		ditem = spdk_memcached_memitem_get_record(mitem);
		assert(spdk_memcached_diskitem_is_valid(ditem));
		cmd->obtain_len = spdk_memcached_diskitem_get_data_size(ditem);

		// prepare buffer to contain the data in disk
		struct spdk_mempool *pool;
		pool = g_spdk_memcached.item_store_pool;
		cmd->mobj = spdk_mempool_get(pool);
		if (cmd->mobj == NULL) {
			assert(false);
			return SPDK_SUCCESS;
		}
		cmd->obtain_buf = cmd->mobj->buf;


		rc = spdk_memcached_diskitem_obtain(ditem, cmd->obtain_buf, cmd->obtain_len, process_get_cmd_cb,
						    cmd);
		return 0;
	}


	cmd->status = MEMCACHED_ITEM_NOT_FOUND;
	rc = snprintf(cmd->send_buf, strlen(STR_NOT_FOUND) + 1, STR_NOT_FOUND);
	cmd->send_len = rc;
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);

	return 0;

}

//static void
//process_delete_cmd_cb(void *cb_arg)
//{
//	struct spdk_memcached_cmd *cmd = cb_arg;
//
//
//	snprintf(cmd->send_buf, );
//
//
//	_memcached_process_cmd_cb(cmd);
//}

static int
process_delete_cmd(struct spdk_memcached_cmd *cmd)
{
//	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;
//	int opcode = cmd->cmd_hd.opcode;
	struct mem_item *mitem;
	struct disk_item *ditem;
	int rc;
	bool valid;

	rc = spdk_memcached_get_memitem(cmd->key_hash, &mitem);
	assert(rc == 0);

	valid = spdk_memcached_memitem_is_valid(mitem);
	if (valid == false) {
		cmd->status = MEMCACHED_ITEM_NOT_FOUND;
		rc = snprintf(cmd->send_buf, strlen(STR_NOT_FOUND) + 1, STR_NOT_FOUND);

	} else {

		ditem = spdk_memcached_memitem_get_record(mitem);
		spdk_memcached_invalid_memitem(mitem);


		assert(spdk_memcached_diskitem_is_valid(ditem));
		rc = spdk_memcached_put_diskitem(ditem);
		assert(rc == 0);

		cmd->status = MEMCACHED_ITEM_EXISTS;
		rc = snprintf(cmd->send_buf, strlen(STR_DELETED) + 1, STR_DELETED);
	}


	cmd->send_len = rc;
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);

	return 0;
}

#endif

static int
process_invalid_cmd(struct spdk_memcached_cmd *cmd)
{
	int rc;

	rc = snprintf(cmd->send_buf, strlen(STR_ERR_NONEXIST_CMD) + 1, STR_ERR_NONEXIST_CMD);
	cmd->send_len = rc;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "send len is %d\n", cmd->send_len);
	memcached_execute_cmd_done(cmd);

	return 0;
}

struct memcached_cmd_methods_processor cmd_processors[MEMCACHED_CMD_NUM] = {
	{"get",	MEMCACHED_CMD_GET, process_get_cmd},
	{"set", MEMCACHED_CMD_SET, process_update_cmd},
	{"add", MEMCACHED_CMD_ADD, process_update_cmd},
	{"replace", MEMCACHED_CMD_REPLACE, process_update_cmd},
	{"delete", MEMCACHED_CMD_DELETE, process_delete_cmd},
	{"invalid_cmd", MEMCACHED_CMD_INVALID_CMD, process_invalid_cmd},
};
