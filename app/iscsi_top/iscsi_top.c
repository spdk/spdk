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

#include "spdk/event.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "iscsi/conn.h"

static char *exe_name;
static int g_shm_id = 0;

struct spdk_jsonrpc_client *g_rpc_client;

static void usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "   %s <option>\n", exe_name);
	fprintf(stderr, "        option = '-i' to specify the shared memory ID,"
		" (required)\n");
	fprintf(stderr, " -r <path>  RPC listen address (default: /var/tmp/spdk.sock\n");
}

struct rpc_conn_info {
	uint32_t	id;
	uint32_t	cid;
	uint32_t	tsih;
	uint32_t	lcore_id;
	char		*initiator_addr;
	char		*target_addr;
	char		*target_node_name;
};

static struct rpc_conn_info g_conn_info[1024];

static const struct spdk_json_object_decoder rpc_conn_info_decoders[] = {
	{"id", offsetof(struct rpc_conn_info, id), spdk_json_decode_uint32},
	{"cid", offsetof(struct rpc_conn_info, cid), spdk_json_decode_uint32},
	{"tsih", offsetof(struct rpc_conn_info, tsih), spdk_json_decode_uint32},
	{"lcore_id", offsetof(struct rpc_conn_info, lcore_id), spdk_json_decode_uint32},
	{"initiator_addr", offsetof(struct rpc_conn_info, initiator_addr), spdk_json_decode_string},
	{"target_addr", offsetof(struct rpc_conn_info, target_addr), spdk_json_decode_string},
	{"target_node_name", offsetof(struct rpc_conn_info, target_node_name), spdk_json_decode_string},
};

static int
rpc_decode_conn_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_conn_info *info = (struct rpc_conn_info *)out;

	return spdk_json_decode_object(val, rpc_conn_info_decoders,
				       SPDK_COUNTOF(rpc_conn_info_decoders), info);
}

static void
print_connections(void)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;
	int rc;
	size_t conn_count, i;
	struct rpc_conn_info *conn;

	request = spdk_jsonrpc_client_create_request();
	if (request == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_request(request, 1, "iscsi_get_connections");
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(g_rpc_client, request);

	do {
		rc = spdk_jsonrpc_client_poll(g_rpc_client, 1);
	} while (rc == 0 || rc == -ENOTCONN);

	if (rc <= 0) {
		goto end;
	}

	json_resp = spdk_jsonrpc_client_get_response(g_rpc_client);
	if (json_resp == NULL) {
		goto end;
	}

	if (spdk_json_decode_array(json_resp->result, rpc_decode_conn_object, g_conn_info,
				   SPDK_COUNTOF(g_conn_info), &conn_count, sizeof(struct rpc_conn_info))) {
		goto end;
	}

	for (i = 0; i < conn_count; i++) {
		conn = &g_conn_info[i];

		printf("Connection: %u CID: %u TSIH: %u Initiator Address: %s Target Address: %s Target Node Name: %s\n",
		       conn->id, conn->cid, conn->tsih, conn->initiator_addr, conn->target_addr, conn->target_node_name);
	}

end:
	spdk_jsonrpc_client_free_request(request);
}

int main(int argc, char **argv)
{
	void			*history_ptr;
	struct spdk_trace_histories *histories;
	struct spdk_trace_history *history;
	const char *rpc_socket_path = SPDK_DEFAULT_RPC_ADDR;

	uint64_t		tasks_done, last_tasks_done[SPDK_TRACE_MAX_LCORE];
	int			delay, old_delay, history_fd, i, quit, rc;
	int			tasks_done_delta, tasks_done_per_sec;
	int			total_tasks_done_per_sec;
	struct timeval		timeout;
	fd_set			fds;
	char			ch;
	struct termios		oldt, newt;
	char			spdk_trace_shm_name[64];
	int			op;

	exe_name = argv[0];
	while ((op = getopt(argc, argv, "i:r:")) != -1) {
		switch (op) {
		case 'i':
			g_shm_id = spdk_strtol(optarg, 10);
			break;
		case 'r':
			rpc_socket_path = optarg;
			break;
		default:
			usage();
			exit(1);
		}
	}

	g_rpc_client = spdk_jsonrpc_client_connect(rpc_socket_path, AF_UNIX);
	if (!g_rpc_client) {
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		return 1;
	}

	snprintf(spdk_trace_shm_name, sizeof(spdk_trace_shm_name), "/iscsi_trace.%d", g_shm_id);
	history_fd = shm_open(spdk_trace_shm_name, O_RDONLY, 0600);
	if (history_fd < 0) {
		fprintf(stderr, "Unable to open history shm %s\n", spdk_trace_shm_name);
		usage();
		exit(1);
	}

	history_ptr = mmap(NULL, sizeof(*histories), PROT_READ, MAP_SHARED, history_fd, 0);
	if (history_ptr == MAP_FAILED) {
		fprintf(stderr, "Unable to mmap history shm (%d).\n", errno);
		exit(1);
	}

	histories = (struct spdk_trace_histories *)history_ptr;

	memset(last_tasks_done, 0, sizeof(last_tasks_done));

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		history = spdk_get_per_lcore_history(histories, i);
		last_tasks_done[i] = history->tpoint_count[TRACE_ISCSI_TASK_DONE];
	}

	delay = 1;
	quit = 0;

	tcgetattr(0, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ICANON);
	tcsetattr(0, TCSANOW, &newt);

	while (1) {

		FD_ZERO(&fds);
		FD_SET(0, &fds);
		timeout.tv_sec = delay;
		timeout.tv_usec = 0;
		rc = select(2, &fds, NULL, NULL, &timeout);

		if (rc > 0) {
			if (read(0, &ch, 1) != 1) {
				fprintf(stderr, "Read error on stdin\n");
				goto cleanup;
			}

			printf("\b");
			switch (ch) {
			case 'd':
				printf("Enter num seconds to delay (1-10): ");
				old_delay = delay;
				rc = scanf("%d", &delay);
				if (rc != 1) {
					fprintf(stderr, "Illegal delay value\n");
					delay = old_delay;
				} else if (delay < 1 || delay > 10) {
					delay = 1;
				}
				break;
			case 'q':
				quit = 1;
				break;
			default:
				fprintf(stderr, "'%c' not recognized\n", ch);
				break;
			}

			if (quit == 1) {
				break;
			}
		}

		printf("\e[1;1H\e[2J");
		print_connections();
		printf("lcore   tasks\n");
		printf("=============\n");
		total_tasks_done_per_sec = 0;
		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
			history = spdk_get_per_lcore_history(histories, i);
			tasks_done = history->tpoint_count[TRACE_ISCSI_TASK_DONE];
			tasks_done_delta = tasks_done - last_tasks_done[i];
			if (tasks_done_delta == 0) {
				continue;
			}
			last_tasks_done[i] = tasks_done;
			tasks_done_per_sec = tasks_done_delta / delay;
			printf("%5d %7d\n", history->lcore, tasks_done_per_sec);
			total_tasks_done_per_sec += tasks_done_per_sec;
		}
		printf("Total %7d\n", total_tasks_done_per_sec);
	}

cleanup:
	tcsetattr(0, TCSANOW, &oldt);

	munmap(history_ptr, sizeof(*histories));
	close(history_fd);

	spdk_jsonrpc_client_close(g_rpc_client);

	return (0);
}
