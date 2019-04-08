/*
 * memcached_def.h
 *
 *  Created on: Apr 17, 2019
 *      Author: root
 */

#ifndef LIB_MEMCACHED_MEMCACHED_DEF_H_
#define LIB_MEMCACHED_MEMCACHED_DEF_H_


#define REALTIME_MAXDELTA 60*60*24*30

/** Maximum length of a key. */
#define KEY_MAX_LENGTH 250

//TODO: only support 4096 at first
#define MEMCACHED_MAX_STORE_LENGTH 4096

enum memcached_protocol {
	ASCII_PROT = 3, /* arbitrary value. */
	BINARY_PROT = 4,
	NEGOTIATING_PROT = 5 /* Discovering the protocol */
};

/**
 * Definition of the legal "magic" values used in a packet.
 * See section 3.1 Magic byte
 */
typedef enum {
	PROTOCOL_BINARY_REQ = 0x80,
	PROTOCOL_BINARY_RES = 0x81
} protocol_binary_magic;

enum memcached_cmd_opcode {
	MEMCACHED_CMD_GET = 0,
	MEMCACHED_CMD_SET,
	MEMCACHED_CMD_ADD,
	MEMCACHED_CMD_REPLACE,
	MEMCACHED_CMD_DELETE,
	MEMCACHED_CMD_INVALID_CMD,
	MEMCACHED_CMD_NUM,
};

enum store_item_type {
	MEMCACHED_ITEM_NOT_STORED = 0,
	MEMCACHED_ITEM_STORED,
	MEMCACHED_ITEM_EXISTS,
	MEMCACHED_ITEM_NOT_FOUND,
	MEMCACHED_ITEM_TOO_LARGE,
	MEMCACHED_ITEM_NO_MEMORY,
};


#define STR_ERR_NONEXIST_CMD	"ERROR\r\n"

#define STR_STORED		"STORED\r\n"		// to indicate success.
#define STR_NOT_STROED		"NOT_STORED\r\n"	//to indicate the data was not stored, but not because of an error.
#define STR_EXISTS		"EXISTS\r\n"		// to indicate that the item with a "cas" command has been modified.
#define STR_NOT_FOUND		"NOT_FOUND\r\n"		// to indicate that the item with a "cas"  or delete command did not exist.
#define STR_DELETED		"DELETED\r\n"		// to indicate success

#define STR_END			"END\r\n"		// After all the items have been transmitted, the server sends the string
#define STR_VALUE		"VALUE %s %d %d\r\n"		// "<key> <flags> <bytes> [<cas unique>]\r\n" "<data block>\r\n"
#define STR_VALUE_1		"VALUE "
#define STR_VALUE_2		"%s"
#define STR_VALUE_3		" %d %d\r\n"


#endif /* LIB_MEMCACHED_MEMCACHED_DEF_H_ */
