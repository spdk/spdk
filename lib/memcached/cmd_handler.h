/*
 * cmd_handler.h
 *
 *  Created on: Apr 29, 2019
 *      Author: root
 */

#ifndef LIB_MEMCACHED_CMD_HANDLER_H_
#define LIB_MEMCACHED_CMD_HANDLER_H_

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 8

typedef struct token_s {
	char *value;
	size_t length;
} token_t;

struct spdk_memcached_cmd;

typedef int (*extract_cmd_handler)(struct spdk_memcached_cmd *cmd, token_t *tokens, int ntokens);
typedef int (*process_cmd_handler)(struct spdk_memcached_cmd *cmd);


struct memcached_cmd_methods_extracter {
	char *cmd_name;
	enum memcached_cmd_opcode opcode;
	extract_cmd_handler extract_fn;
};

struct memcached_cmd_methods_processor {
	char *cmd_name;
	enum memcached_cmd_opcode opcode;
	process_cmd_handler process_fn;
};

struct spdk_memcached_cmd_cb_args {
	struct spdk_slot_item *sitem;
	struct hashitem *mitem;
	int existed_step;
	int existed_mitem_num;
};

extern struct memcached_cmd_methods_extracter cmd_extracters[];
extern struct memcached_cmd_methods_processor cmd_processors[];

#endif /* LIB_MEMCACHED_CMD_HANDLER_H_ */
