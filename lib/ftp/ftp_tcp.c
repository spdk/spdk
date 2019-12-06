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

#include "spdk/ftp.h"
#include "spdk/sock.h"
#include "ftp_internal.h"
#include "spdk_internal/log.h"
#include "spdk/assert.h"
#include "spdk/util.h"
#include "ftp_commons.h"
#include "spdk_internal/sock.h"
#include "spdk/string.h"
#include "spdk/blobfs.h"
extern struct spdk_filesystem *g_fs;
#define FTP_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16
#define N 4096 

struct spdk_my_sock {
	struct spdk_sock base;
	int fd;
};
struct spdk_ftp_tcp_poll_group {
	struct spdk_ftp_server_poll_group group;
	struct spdk_sock_group *sock_group;

	TAILQ_HEAD(, spdk_ftp_tcp_conn)
	conns;
};
struct spdk_ftp_tcp_port {
	struct spdk_sock *listen_sock;
	uint32_t ref;
	TAILQ_ENTRY(spdk_ftp_tcp_port)
	link;
};

struct spdk_ftp_tcp_server {
	struct spdk_ftp_server server;

	pthread_mutex_t lock;

	TAILQ_HEAD(, spdk_ftp_tcp_port)
	ports;
};
struct spdk_ftp_tcp_conn {
	struct spdk_ftp_conn conn;

	struct spdk_ftp_tcp_poll_group *group;
	struct spdk_ftp_tcp_port *port;

	/* controller connection  */
	struct spdk_sock *ctrl_sock;
	char cmdline[MAX_COMMAND_LINE];
	char cmd[MAX_COMMAND];
	char arg[MAX_ARG];

	/* data connection  */
	/* struct sockaddr_in *port_addr; */
	struct spdk_sock *pasv_listen_scok;
	struct spdk_sock *data_sock;
	int data_process;

	/* FTP status  */
	int is_ascii;
	long long restart_pos;
	char *rnfr_name;
	int abor_received;

	/* IP address */
	char initiator_addr[SPDK_FTP_TRADDR_MAX_LEN];
	char target_addr[SPDK_FTP_TRADDR_MAX_LEN];

	/* IP port */
	uint16_t initiator_port;
	uint16_t target_port;

	int kernelfile;
	void (*cmd_handler)(struct spdk_ftp_tcp_conn *tconn);

	TAILQ_ENTRY(spdk_ftp_tcp_conn)
	link;
};
#define SPDK_FTP_TCP_DEFAULT_IO_UNIT_SIZE 131072
#define SPDK_FTP_TCP_DEFAULT_MAX_IO_SIZE 131072
#define FTPD_CONNECT_TIMEOUT_S 60
#define FTPD_IDLE_SESSION_TIMEOUT_S 300
static void do_user(struct spdk_ftp_tcp_conn *tconn);
static void do_pass(struct spdk_ftp_tcp_conn *tconn);
static void do_cwd(struct spdk_ftp_tcp_conn *tconn);
static void do_pasv(struct spdk_ftp_tcp_conn *tconn);
static void do_list(struct spdk_ftp_tcp_conn *tconn);
static void do_syst(struct spdk_ftp_tcp_conn *tconn);
static void do_type(struct spdk_ftp_tcp_conn *tconn);
static void do_stor(struct spdk_ftp_tcp_conn *tconn);
static void do_quit(struct spdk_ftp_tcp_conn *tconn);
typedef struct ftpcmd {
	const char *cmd;
	void (*cmd_handler)(struct spdk_ftp_tcp_conn *tconn);
} ftpcmd_t;
static ftpcmd_t ctrl_cmds[] = {
	/* 访问控制命令 */
	{"USER", do_user},
	{"PASS", do_pass},
	{"CWD", do_cwd},
	{"QUIT", do_quit},

	/* 传输参数命令 */
	{"PASV", do_pasv},
	{"TYPE", do_type},

	/* 服务命令   */
	{"LIST", do_list},
	{"SYST", do_syst},
	{"STOR", do_stor},

};
static void
spdk_ftp_reply(struct spdk_ftp_tcp_conn *tconn, int status, const char *text)
{
	char buf[1024] = {0};
	struct iovec iov;
	tconn->cmd_handler = NULL;
	snprintf(buf, sizeof(buf), "%d %s\r\n", status, text);
	iov.iov_base = buf;
	iov.iov_len = strlen(buf);
	if (spdk_sock_writev(tconn->ctrl_sock, &iov, 1) < 0) {
		SPDK_ERRLOG("Write Error!At commd_get 1\n");
		exit(1);
	}
}
static void
spdk_ftp_tcp_opts_init(struct spdk_ftp_server_opts *opts)
{
	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "tcp init done\n");
	opts->io_unit_size = SPDK_FTP_TCP_DEFAULT_IO_UNIT_SIZE;
	opts->max_io_size = SPDK_FTP_TCP_DEFAULT_MAX_IO_SIZE;
	opts->spdk_ftpd_anonymous_enable = 1;
	opts->spdk_ftpd_pasv_enable = 1;
	opts->spdk_ftpd_connect_timeout = FTPD_CONNECT_TIMEOUT_S;
	opts->spdk_ftpd_idle_session_timeout = FTPD_IDLE_SESSION_TIMEOUT_S;
}
static struct spdk_ftp_server *
spdk_ftp_tcp_create(struct spdk_ftp_server_opts *opts)
{
	struct spdk_ftp_tcp_server *tserver;

	tserver = calloc(1, sizeof(*tserver));
	if (!tserver) {
		return NULL;
	}

	TAILQ_INIT(&tserver->ports);

	tserver->server.ops = &spdk_ftp_server_tcp;

	SPDK_NOTICELOG("*** FTP TCP Server Init ***\n");

	pthread_mutex_init(&tserver->lock, NULL);
	return &tserver->server;
}
static struct spdk_ftp_server_poll_group *
spdk_ftp_tcp_poll_group_create(struct spdk_ftp_server *server)
{

	struct spdk_ftp_tcp_poll_group *tgroup;
	tgroup = calloc(1, sizeof(*tgroup));

	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create(&tgroup->group);
	if (!tgroup->sock_group) {
		goto cleanup;
	}

	/* RUI TODO:
	 need to init qpairs and pending_data_buf_queue  */
	TAILQ_INIT(&tgroup->conns);
	return &tgroup->group;
cleanup:
	free(tgroup);
	return NULL;
}

static int
spdk_ftp_tcp_destroy(struct spdk_ftp_server *server)
{
	struct spdk_ftp_tcp_server *tserver;

	assert(server != NULL);
	tserver = SPDK_CONTAINEROF(server, struct spdk_ftp_tcp_server, server);

	pthread_mutex_destroy(&tserver->lock);
	free(tserver);
	return 0;
}
static int
spdk_ftp_tcp_poll_group_poll(struct spdk_ftp_server_poll_group *group)
{

	struct spdk_ftp_tcp_poll_group *tgroup;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_ftp_tcp_poll_group, group);
	rc = spdk_sock_group_poll(tgroup->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", tgroup->sock_group);
		return rc;
	}

	return 0;
}

static int
spdk_ftp_tcp_listen(struct spdk_ftp_server *server)
{
	struct spdk_ftp_tcp_server *tserver;
	struct spdk_ftp_tcp_port *port;

	tserver = SPDK_CONTAINEROF(server, struct spdk_ftp_tcp_server, server);

	pthread_mutex_lock(&tserver->lock);

	/* RUI TODO: find existing port  */

	port = calloc(1, sizeof(*port));

	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		free(port);
		pthread_mutex_unlock(&tserver->lock);
		return -ENOMEM;
	}

	port->ref = 1;

	port->listen_sock = spdk_sock_listen(server->opts.ipaddr, server->opts.spdk_ftpd_listen_port);

	if (port->listen_sock == NULL) {
		SPDK_ERRLOG("spdk_sock_listen failed:\n");
		free(port);
		pthread_mutex_unlock(&tserver->lock);
		return -errno;
	}

	SPDK_NOTICELOG("*** FTP-TCP Target Listening on %s port %d g_fs is %p***\n",
		       server->opts.ipaddr, server->opts.spdk_ftpd_listen_port, g_fs);
	TAILQ_INSERT_TAIL(&tserver->ports, port, link);
	pthread_mutex_unlock(&tserver->lock);

	return 0;
}
static void
spdk_ftp_tcp_conn_destroy(struct spdk_ftp_tcp_conn *tconn)
{

	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "enter\n");
	spdk_sock_group_remove_sock(tconn->group->sock_group, tconn->ctrl_sock);
	spdk_sock_close(&tconn->ctrl_sock);
	free(tconn);
	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "Leave\n");
}

static void
_spdk_ftp_tcp_handle_connect(struct spdk_ftp_server *server,
			     struct spdk_ftp_tcp_port *port,
			     struct spdk_sock *sock, new_conn_fn cb_fn)
{
	struct spdk_ftp_tcp_conn *tconn;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "New connection accepted on port %d\n",
		      server->opts.spdk_ftpd_listen_port);

	/* 判断sock_priority  */
	tconn = calloc(1, sizeof(struct spdk_ftp_tcp_conn));
	if (tconn == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);
		return;
	}
	tconn->ctrl_sock = sock;
	tconn->port = port;
	tconn->conn.server = server;
	tconn->kernelfile = -1;
	tconn->cmd_handler = NULL;
	/* RUI TODO: init other para   */

	rc = spdk_sock_getaddr(tconn->ctrl_sock, tconn->target_addr,
			       sizeof(tconn->target_addr), &tconn->target_port,
			       tconn->initiator_addr, sizeof(tconn->initiator_addr),
			       &tconn->initiator_port);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed of tconn=%p\n", tconn);
		spdk_ftp_tcp_conn_destroy(tconn);
		return;
	}
	spdk_ftp_reply(tconn, FTP_GREET, "(miniftpd 0.1)");
	cb_fn(&tconn->conn);
}

static void
spdk_ftp_tcp_port_accept(struct spdk_ftp_server *server, struct spdk_ftp_tcp_port *port,
			 new_conn_fn cb_fn)
{
	struct spdk_sock *sock;
	int i;

	for (i = 0; i < FTP_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock) {
			SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "sock accepted\n");
			_spdk_ftp_tcp_handle_connect(server, port, sock, cb_fn);
		}
	}
}

static void
spdk_ftp_tcp_accept(struct spdk_ftp_server *server, new_conn_fn cb_fn)
{
	struct spdk_ftp_tcp_server *tserver;
	struct spdk_ftp_tcp_port *port;

	tserver = SPDK_CONTAINEROF(server, struct spdk_ftp_tcp_server, server);

	TAILQ_FOREACH(port, &tserver->ports, link) {
		spdk_ftp_tcp_port_accept(server, port, cb_fn);
	}
}


static void *
parsecmd(char *commd)
{
	int i, cmdlist_size;
	cmdlist_size = sizeof(ctrl_cmds) / sizeof(ctrl_cmds[0]);
	for (i = 0; i < cmdlist_size; i++) {
		if (strcmp(ctrl_cmds[i].cmd, commd) == 0) {
			return ctrl_cmds[i].cmd_handler;
		}
	}
	if (i == cmdlist_size) {
		SPDK_ERRLOG("parse error, cmd id %s\n", commd);
		return NULL;
	}
	return NULL;
}
static void str_trim_crlf(char *str)
{
	char *p = &str[strlen(str) - 1];
	while (*p == '\r' || *p == '\n') {
		*p-- = '\0';
	}
}

static int
spdk_ftp_tcp_sock_process(struct spdk_ftp_tcp_conn *tconn)
{
	int ret;


	memset(tconn->cmdline, 0, sizeof(tconn->cmdline));
	memset(tconn->cmd, 0, sizeof(tconn->cmd));
	memset(tconn->arg, 0, sizeof(tconn->arg));
	ret = spdk_sock_recv(tconn->ctrl_sock, tconn->cmdline, MAX_COMMAND_LINE);

	if (ret > 0) {
		str_trim_crlf(tconn->cmdline);
		str_split(tconn->cmdline, tconn->cmd, tconn->arg, ' ');
		str_upper(tconn->cmd);
		SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "cmd is %s, arg is %s\n", tconn->cmd, tconn->arg);
		tconn->cmd_handler = parsecmd(tconn->cmd);
		if (tconn->cmd_handler) {
			tconn->cmd_handler(tconn);
		} else {
			return 0;
		}
		return 1;
	}
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		SPDK_ERRLOG("error\n");
	}


	return 0;
}
static void
spdk_ftp_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_ftp_tcp_conn *tconn = arg;
	int rc;
	assert(tconn != NULL);
	rc = spdk_ftp_tcp_sock_process(tconn);

	/* check the following two factors:
	 * rc: The socket is closed
	 * State of tqpair: The tqpair is in EXITING state due to internal error
	 */
	if (rc < 0) {
		spdk_ftp_conn_disconnect(&tconn->conn, NULL, NULL);
	}
}
static int
spdk_ftp_tcp_poll_group_add(struct spdk_ftp_server_poll_group *group,
			    struct spdk_ftp_conn *conn)
{
	struct spdk_ftp_tcp_poll_group *tgroup;
	struct spdk_ftp_tcp_conn *tconn;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_ftp_tcp_poll_group, group);
	tconn = SPDK_CONTAINEROF(conn, struct spdk_ftp_tcp_conn, conn);

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tconn->ctrl_sock, spdk_ftp_tcp_sock_cb, tconn);
	if (rc != 0) {
		SPDK_ERRLOG("Could not add sock to sock_group\n");
		spdk_ftp_tcp_conn_destroy(tconn);
		return -1;
	}
	/* RUI TODO : init other things  */

	tconn->group = tgroup;
	TAILQ_INSERT_TAIL(&tgroup->conns, tconn, link);

	return 0;
}
static int
get_pasv_fd(struct spdk_ftp_tcp_conn *tconn)
{
	struct spdk_sock *sock;
	int ret;
	sock = spdk_sock_accept(tconn->pasv_listen_scok);
	ret = spdk_sock_close(&tconn->pasv_listen_scok);
	if (ret < 0) {
		SPDK_ERRLOG("spdk_sock_close failed in function get_pasv_fd");
		return 0;
	}
	tconn->pasv_listen_scok = NULL;
	tconn->data_sock = sock;
	return 1;
}
static int
get_transfer_fd(struct spdk_ftp_tcp_conn *tconn)
{
	/* RUI TODO: Need to check if passive mode or active mode   */
	int ret = 1;
	if (get_pasv_fd(tconn) == 0) {
		ret = 0;
	}
	return ret;
}
static void
do_user(struct spdk_ftp_tcp_conn *tconn)
{
	/* RUI TODO: Do something certified  */
	spdk_ftp_reply(tconn, FTP_GIVEPWORD, "Please specify the password.");
}
static void do_pass(struct spdk_ftp_tcp_conn *tconn)
{
	/* RUI TODO: Do something certified  */
	spdk_ftp_reply(tconn, FTP_LOGINERR, "Login incorrect.");
}
static void
do_cwd(struct spdk_ftp_tcp_conn *tconn)
{
}
static void
do_syst(struct spdk_ftp_tcp_conn *tconn)
{
	spdk_ftp_reply(tconn, FTP_SYSTOK, "UNIX Type: L8");
}

static void
do_pasv(struct spdk_ftp_tcp_conn *tconn)
{
	int ret;
	uint16_t port = 0;
	tconn->pasv_listen_scok = spdk_sock_listen(tconn->conn.server->opts.ipaddr, 0);
	ret = spdk_sock_getclientport(tconn->pasv_listen_scok, &port);
	if (ret < 0) {
		SPDK_ERRLOG("failed at spdk_sock_getclientport\n");
	}

	unsigned int v[4];
	sscanf(tconn->target_addr, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
	char text[1024] = {0};
	snprintf(text, sizeof(text), "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
		 v[0], v[1], v[2], v[3], port >> 8, port & 0xFF);

	spdk_ftp_reply(tconn, FTP_PASVOK, text);
}
static void do_ls(struct spdk_ftp_tcp_conn *tconn);
static void
do_list(struct spdk_ftp_tcp_conn *tconn)
{
	if (get_transfer_fd(tconn) == 0) {
		spdk_ftp_reply(tconn, 404, "get data conntction errror\n");
		return;
	}
	spdk_ftp_reply(tconn, FTP_DATACONN, "Here comes the directory listing.");

	/* 传输列表  */
	do_ls(tconn);
	/* 关闭数据套接字   */
	spdk_sock_close(&tconn->data_sock);
	tconn->data_sock = NULL;
	spdk_ftp_reply(tconn, FTP_TRANSFEROK, "Directory send OK.");
}
static void
do_type(struct spdk_ftp_tcp_conn *tconn)
{
	if (strcmp(tconn->arg, "A") == 0) {
		tconn->is_ascii = 1;
		spdk_ftp_reply(tconn, FTP_TYPEOK, "Switching to ASCII mode.");
	} else if (strcmp(tconn->arg, "I") == 0) {
		tconn->is_ascii = 0;
		spdk_ftp_reply(tconn, FTP_TYPEOK, "Switching to Binary mode.");
	} else {
		spdk_ftp_reply(tconn, FTP_BADCMD, "Unrecognised TYPE command.");
	}
}
struct blobfs_args{
	struct spdk_file *g_file;
	int g_fserrno;
	int offset;
};
struct spdk_ftp_tcp_getandput_conn_ctx {
	struct spdk_ftp_tcp_conn *tconn;
	bool use_blobfs;
	int is_append;
	int fd;
	struct blobfs_args args;
};
static void
open_cb(void *ctxargs, struct spdk_file *f, int fserrno)
{
	struct spdk_ftp_tcp_getandput_conn_ctx *ctx;
	ctx = (struct spdk_ftp_tcp_getandput_conn_ctx *)ctxargs;
	ctx->args.g_file = f;
	ctx->args.g_fserrno = fserrno;
	ctx->args.offset = 0;
}
static int
upload_init(struct spdk_ftp_tcp_getandput_conn_ctx *ctx)
{
	struct spdk_ftp_tcp_conn *tconn = ctx->tconn;
	ctx->use_blobfs = true;
	long long offset = tconn->restart_pos;
	tconn->restart_pos = 0;
	int ret;

	if (ctx->use_blobfs) {
		/* init it a posive value beause all errno in spdk is negative. */
		ctx->args.g_fserrno = 1; 
		spdk_fs_open_file_async(g_fs, tconn->arg, SPDK_BLOBFS_OPEN_CREATE, open_cb, ctx);
		while(ctx->args.g_fserrno==1)
		{
			printf("wait for g_file to open\n");
		}
		if (ctx->args.g_fserrno<0) {
			spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
			return -1;
		}
		char text[1024] = {0};
		if (tconn->is_ascii) {
			snprintf(text, sizeof(text), "Opening ASCII mode data connection for %s.",
				 tconn->arg);
		} else {
			snprintf(text, sizeof(text), "Opening BINARY mode data connection for %s.",
				 tconn->arg);
		}
		spdk_ftp_reply(tconn, FTP_DATACONN, text);

	} else {
		ctx->fd = open(tconn->arg, O_CREAT | O_WRONLY, 0666);
		if (ctx->fd == -1) {
			spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
			return -1;
		}

		/**
		*		 RUI TODO: need to add block
		*		int ret;
		*		 加写锁
		*		ret = lock_file_write(fd);
		*		if (ret == -1)
		*		{
		*			spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
		*			return;
		*		} */

		/*  STOR   */
		if (!ctx->is_append && offset == 0) {
			ret = ftruncate(ctx->fd, 0);
			if (ret < 0) {
				SPDK_ERRLOG("failed at ftruncate file");
			}
			if (lseek(ctx->fd, 0, SEEK_SET) < 0) {
				spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
				return -1;
			}
		} else if (!ctx->is_append && offset != 0) {
			/*  REST+STOR  */
			if (lseek(ctx->fd, offset, SEEK_SET) < 0) {
				spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
				return -1;
			}
		} else if (ctx->is_append) {
			/*  APPE  */
			if (lseek(ctx->fd, 0, SEEK_END) < 0) {
				spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
				return -1;
			}
		}
		struct stat sbuf;
		ret = fstat(ctx->fd, &sbuf);
		if (ret < 0) {
			SPDK_ERRLOG("fstat error\n");
		}
		if (!S_ISREG(sbuf.st_mode)) {
			spdk_ftp_reply(tconn, FTP_UPLOADFAIL, "Could not create file.");
			return -1;
		}

		char text[1024] = {0};
		if (tconn->is_ascii) {
			snprintf(text, sizeof(text), "Opening ASCII mode data connection for %s (%lld bytes).",
				 tconn->arg, (long long)sbuf.st_size);
		} else {
			snprintf(text, sizeof(text), "Opening BINARY mode data connection for %s (%lld bytes).",
				 tconn->arg, (long long)sbuf.st_size);
		}
		spdk_ftp_reply(tconn, FTP_DATACONN, text);
	}
	return 1;
}
static void
fs_op_complete(void *ctxargs, int fserrno)
{
	struct spdk_ftp_tcp_getandput_conn_ctx *ctx = ctxargs;
	ctx->args.g_fserrno = fserrno;
}
static void
spdk_data_sock_close(struct spdk_ftp_tcp_getandput_conn_ctx *ctx)
{
	int ret;
	spdk_sock_group_remove_sock(ctx->tconn->group->sock_group, ctx->tconn->data_sock);
	ret = spdk_sock_close(&ctx->tconn->data_sock);
	if (ret < 0) {
		SPDK_ERRLOG("close data_sock error\n");
	}
	if(ctx->use_blobfs)
	{
		spdk_file_close_async(ctx->args.g_file, fs_op_complete, NULL);
	}else{
		close(ctx->fd);
	}
	free(ctx);
}

static int
spdk_ftp_tcp_getandput_sock_process(struct spdk_ftp_tcp_getandput_conn_ctx *ctx)
{
	int ret;
	struct spdk_ftp_tcp_conn *tconn = ctx->tconn;
	if (strcmp(tconn->cmd, "STOR") == 0) {
		int flag = -1;
		char buf[N];
		ret = spdk_sock_recv(tconn->data_sock, buf, sizeof(buf));
		if (ret == -1) {
			flag = 2;
			SPDK_ERRLOG("receive data error\n");
		} else if (ret == 0) {
			flag = 0;
		}
		if(ctx->use_blobfs)
		{
			
			spdk_file_write_async(ctx->args.g_file, g_fs->sync_target.sync_io_channel, buf, ctx->args.offset, ret, fs_op_complete, ctx);
			ctx->args.offset+=ret;
			if(ctx->args.g_fserrno<0)
			{
				spdk_ftp_reply(tconn, FTP_BADSENDFILE, "Failure writting to local file.");
				spdk_data_sock_close(ctx);
			}
		}else{
			if (writen(ctx->fd, buf, ret) != ret) {
			flag = 1;
			}
		}
		if (flag == 0) {
			spdk_ftp_reply(tconn, FTP_TRANSFEROK, "Transfer complete.");
			spdk_data_sock_close(ctx);
		} else if (flag == 1) {
			spdk_ftp_reply(tconn, FTP_BADSENDFILE, "Failure writting to local file.");
			spdk_data_sock_close(ctx);
		} else if (flag == 2) {
			spdk_ftp_reply(tconn, FTP_BADSENDNET, "Failure reading from network stream.");
			spdk_data_sock_close(ctx);
		}
		
	} else if (strcmp(tconn->cmd, "RETR") == 0) {

	} else {
		SPDK_ERRLOG("error command in spdk_ftp_tcp_getandput_sock_process\n");
	}
	return 0;
}

static void
spdk_ftp_tcp_upload_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_ftp_tcp_getandput_conn_ctx *tconn_ctx = arg;
	int rc;
	assert(tconn_ctx != NULL);
	rc = spdk_ftp_tcp_getandput_sock_process(tconn_ctx);

	/* check the following two factors:
	 * rc: The socket is closed
	 * State of tqpair: The tqpair is in EXITING state due to internal error
	 */
	if (rc < 0) {
		spdk_ftp_conn_disconnect(&tconn_ctx->tconn->conn, NULL, NULL);
	}
}
static void
upload_common(struct spdk_ftp_tcp_conn *tconn, int is_append)
{
	int rc;
	struct spdk_ftp_tcp_getandput_conn_ctx *ctx;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("coould not allocat memory for spdk_ftp_tcp_getandput_conn_ctx\n ");
		return ;
	}
	ctx->tconn = tconn;
	ctx->is_append = is_append;

	if (get_transfer_fd(tconn) == 0) {
		return;
	}
	upload_init(ctx);
	rc = spdk_sock_group_add_sock(tconn->group->sock_group, tconn->data_sock,
				      spdk_ftp_tcp_upload_sock_cb, ctx);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_add_sock error\n");
	}
}
static void
do_stor(struct spdk_ftp_tcp_conn *tconn)
{
	upload_common(tconn, 0);
}
static void
do_quit(struct spdk_ftp_tcp_conn *tconn)
{
	spdk_ftp_reply(tconn, FTP_GOODBYE, "Goodbye.");
	spdk_sock_group_remove_sock(tconn->group->sock_group, tconn->ctrl_sock);
	spdk_sock_close(&tconn->ctrl_sock);


}

static void
do_ls(struct spdk_ftp_tcp_conn *tconn)
{
	int detail = 1, ret;
	DIR *dir = opendir(".");
	if (dir == NULL) {
		return;
	}

	struct dirent *dt;
	struct stat sbuf;
	struct iovec iov;
	while ((dt = readdir(dir)) != NULL) {
		if (lstat(dt->d_name, &sbuf) < 0) {
			continue;
		}
		if (dt->d_name[0] == '.') {
			continue;
		}

		char buf[MAX_LINE] = {0};
		if (detail) {
			const char *perms = statbuf_get_perms(&sbuf);

			int off = 0;
			off += snprintf(buf, sizeof(buf), "%s ", perms);
			off += snprintf(buf + off, sizeof(buf), " %3ld %-8d %-8d ", sbuf.st_nlink, sbuf.st_uid,
					sbuf.st_gid);
			off += snprintf(buf + off, sizeof(buf), "%8lu ", (unsigned long)sbuf.st_size);

			const char *datebuf = statbuf_get_date(&sbuf);
			off += snprintf(buf + off, sizeof(buf), "%s ", datebuf);
			if (S_ISLNK(sbuf.st_mode)) {
				char tmp[MAX_ARG] = {0};
				ret = readlink(dt->d_name, tmp, sizeof(tmp));
				if (ret == -1) {
					SPDK_ERRLOG("failed at readlink, errinfo is %s", strerror(errno));
				}
				off += snprintf(buf + off, sizeof(buf), "%s -> %s\r\n", dt->d_name, tmp);
			} else {
				off += snprintf(buf + off, sizeof(buf), "%s\r\n", dt->d_name);
			}
		} else {
			snprintf(buf, sizeof(buf), "%s\r\n", dt->d_name);
		}

		iov.iov_base = buf;
		iov.iov_len = N;
		if (spdk_sock_writev(tconn->data_sock, &iov, 1) < 0) {
			SPDK_ERRLOG("Write Error!At commd_get 1\n");
			exit(1);
		}
	}

	closedir(dir);
}
static int spdk_ftp_tcp_poll_group_remove(struct spdk_ftp_server_poll_group *group,
		struct spdk_ftp_conn *conn)
{

	return 0;
}
static void
spdk_ftp_tcp_close_conn(struct spdk_ftp_conn *conn)
{
	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "enter\n");

	spdk_ftp_tcp_conn_destroy(SPDK_CONTAINEROF(conn, struct spdk_ftp_tcp_conn, conn));
}
const struct spdk_ftp_server_ops spdk_ftp_server_tcp = {
	.type = SPDK_FTP_TCP,
	.opts_init = spdk_ftp_tcp_opts_init,
	.create = spdk_ftp_tcp_create,
	.destroy = spdk_ftp_tcp_destroy,

	.listen = spdk_ftp_tcp_listen,
	.accept = spdk_ftp_tcp_accept,

	.poll_group_create = spdk_ftp_tcp_poll_group_create,
	.poll_group_add = spdk_ftp_tcp_poll_group_add,
	.poll_group_remove = spdk_ftp_tcp_poll_group_remove,
	.poll_group_poll = spdk_ftp_tcp_poll_group_poll,

	.conn_fini = spdk_ftp_tcp_close_conn,
};

SPDK_LOG_REGISTER_COMPONENT("ftp_tcp", SPDK_LOG_FTP_TCP)
