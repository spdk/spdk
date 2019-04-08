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
#include "memcached/memcached_cmd.h"
#include "memcached/cmd_handler.h"

/* Avoid warnings on solaris, where isspace() is an index into an array, and gcc uses signed chars */
#define xisspace(c) isspace((unsigned char)c)

static bool
safe_strtoull(const char *str, uint64_t *out)
{
	assert(out != NULL);
	errno = 0;
	*out = 0;
	char *endptr;
	unsigned long long ull = strtoull(str, &endptr, 10);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (xisspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		if ((long long) ull < 0) {
			/* only check for negative signs in the uncommon case when
			 * the unsigned number is so big that it's negative as a
			 * signed number. */
			if (strchr(str, '-') != NULL) {
				return false;
			}
		}
		*out = ull;
		return true;
	}
	return false;
}

static bool
safe_strtoll(const char *str, int64_t *out)
{
	assert(out != NULL);
	errno = 0;
	*out = 0;
	char *endptr;
	long long ll = strtoll(str, &endptr, 10);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (xisspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		*out = ll;
		return true;
	}
	return false;
}

static bool
safe_strtoul(const char *str, uint32_t *out)
{
	char *endptr = NULL;
	unsigned long l = 0;
	assert(out);
	assert(str);
	*out = 0;
	errno = 0;

	l = strtoul(str, &endptr, 10);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (xisspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		if ((long) l < 0) {
			/* only check for negative signs in the uncommon case when
			 * the unsigned number is so big that it's negative as a
			 * signed number. */
			if (strchr(str, '-') != NULL) {
				return false;
			}
		}
		*out = l;
		return true;
	}

	return false;
}

static bool
safe_strtol(const char *str, int32_t *out)
{
	assert(out != NULL);
	errno = 0;
	*out = 0;
	char *endptr;
	long l = strtol(str, &endptr, 10);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (xisspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		*out = l;
		return true;
	}
	return false;
}

static bool
safe_strtod(const char *str, double *out)
{
	assert(out != NULL);
	errno = 0;
	*out = 0;
	char *endptr;
	double d = strtod(str, &endptr);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (xisspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		*out = d;
		return true;
	}
	return false;
}

static inline bool
noreply_is_set(token_t *tokens, size_t ntokens)
{
	int noreply_index = ntokens - 2;

	/*
	  NOTE: this function is not the first place where we are going to
	  send the reply.  We could send it instead from process_command()
	  if the request line has wrong number of tokens.  However parsing
	  malformed line for "noreply" option is not reliable anyway, so
	  it can't be helped.
	*/
	if (tokens[noreply_index].value
	    && strcmp(tokens[noreply_index].value, "noreply") == 0) {
		return true;
	}
	return false;
}

static int
extract_update_cmd(struct spdk_memcached_cmd *cmd, token_t *tokens, int ntokens)
{
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;

	hd->noreply = noreply_is_set(tokens, ntokens);

	if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
		assert(false); // out_string(c, "CLIENT_ERROR bad command line format");
		return -1;
	}
	hd->key = tokens[KEY_TOKEN].value;
	hd->key_len = tokens[KEY_TOKEN].length;

	if (!(safe_strtoul(tokens[2].value, &hd->flags)
	      && safe_strtol(tokens[3].value, &hd->exptime_int)
	      && safe_strtol(tokens[4].value, (int32_t *)&hd->data_len))) {
		assert(false); //out_string(c, "CLIENT_ERROR bad command line format");
		return -1;
	}

	/* Negative exptimes can underflow and end up immortal. realtime() will
	   immediately expire values that are greater than REALTIME_MAXDELTA, but less
	   than process_started, so lets aim for that. */
	if (hd->exptime_int < 0) {
		hd->exptime_int = REALTIME_MAXDELTA + 1;
	}

	if (hd->data_len > (INT_MAX - 2)) {
		assert(false); //out_string(c, "CLIENT_ERROR bad command line format");
		return -1;
	}

//	hd->data_len += 2;

	// TODO: does cas value exist?
//    if (handle_cas) {
//        if (!safe_strtoull(tokens[5].value, &req_cas_id)) {
//            out_string(c, "CLIENT_ERROR bad command line format");
//            return;
//        }
//    }

	return 0;
}

static int
extract_get_cmd(struct spdk_memcached_cmd *cmd, token_t *tokens, int ntokens)
{
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;
	token_t *key_token = &tokens[KEY_TOKEN];

	(void)key_token;

	if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
		assert(false); // out_string(c, "CLIENT_ERROR bad command line format");
		return -1;
	}

	strncpy(hd->maybe_key, tokens[KEY_TOKEN].value, KEY_MAX_LENGTH);
	hd->key = hd->maybe_key;
	hd->key_len = tokens[KEY_TOKEN].length;

	//TODO: multiple keys check

	//TODO: add should_touch
//    if (should_touch) {
//        // For get and touch commands, use first token as exptime
//        if (!safe_strtol(tokens[1].value, &exptime_int)) {
//            out_string(c, "CLIENT_ERROR invalid exptime argument");
//            return;
//        }
//        key_token++;
//        exptime = realtime(exptime_int);
//    }

	return 0;
}


static int
extract_delete_cmd(struct spdk_memcached_cmd *cmd, token_t *tokens, int ntokens)
{
	struct spdk_memcached_cmd_header *hd = &cmd->cmd_hd;

	if (ntokens > 3) {
		bool hold_is_zero = strcmp(tokens[KEY_TOKEN + 1].value, "0") == 0;
		bool sets_noreply = noreply_is_set(tokens, ntokens);
		bool valid = (ntokens == 4 && (hold_is_zero || sets_noreply))
			     || (ntokens == 5 && hold_is_zero && sets_noreply);
		if (!valid) {
			assert(false); //out_string(c, "CLIENT_ERROR bad command line format.  "
//                       "Usage: delete <key> [noreply]");
			return -1;
		}
	}

	if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
		assert(false); // out_string(c, "CLIENT_ERROR bad command line format");
		return -1;
	}
	strncpy(hd->maybe_key, tokens[KEY_TOKEN].value, KEY_MAX_LENGTH);
	hd->key = hd->maybe_key;
	hd->key_len = tokens[KEY_TOKEN].length;

	return 0;
}

static int
extract_invalid_cmd(struct spdk_memcached_cmd *cmd, token_t *tokens, int ntokens)
{

	return 0;
}

struct memcached_cmd_methods_extracter cmd_extracters[MEMCACHED_CMD_NUM] = {
	{"get",	MEMCACHED_CMD_GET, extract_get_cmd},
	{"set", MEMCACHED_CMD_SET, extract_update_cmd},
	{"add", MEMCACHED_CMD_ADD, extract_update_cmd},
	{"replace", MEMCACHED_CMD_REPLACE, extract_update_cmd},
	{"delete", MEMCACHED_CMD_DELETE, extract_delete_cmd},
	{"invalid_cmd", MEMCACHED_CMD_INVALID_CMD, extract_invalid_cmd},
};
