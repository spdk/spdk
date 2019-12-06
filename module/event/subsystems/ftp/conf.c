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

#include "conf.h"

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/ftp.h"

struct spdk_ftp_parse_ftpds_ctx {
	struct spdk_conf_section *sp;
	spdk_ftp_parse_conf_done cb_fn;
};
static int
spdk_ftp_read_config_file_tgt_conf(struct spdk_conf_section *sp,
				   struct spdk_ftp_tgt_conf *conf)
{
	int acceptor_poll_rate;
	int rc = 0;

	acceptor_poll_rate = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (acceptor_poll_rate >= 0) {
		conf->acceptor_poll_rate = acceptor_poll_rate;
	}

	/* todo parse more conf params here */

	return rc;
}

static struct spdk_ftp_tgt_conf *
spdk_ftp_parse_tgt_conf(void)
{
	struct spdk_ftp_tgt_conf *conf;
	struct spdk_conf_section *sp;
	int rc;

	conf = calloc(1, sizeof(*conf));
	if (!conf) {
		SPDK_ERRLOG("calloc() failed for target conf\n");
		return NULL;
	}
	conf->acceptor_poll_rate = ACCEPT_TIMEOUT_US;

	/* begin parse [Ftp] */
	sp = spdk_conf_find_section(NULL, "Ftp");
	if (sp != NULL) {
		rc = spdk_ftp_read_config_file_tgt_conf(sp, conf);
		if (rc) {
			free(conf);
			return NULL;
		}
	}

	return conf;
}


static int
spdk_ftp_parse_ftp_tgt(void)
{
	if (!g_spdk_ftp_tgt_conf) {
		g_spdk_ftp_tgt_conf = spdk_ftp_parse_tgt_conf();
		if (!g_spdk_ftp_tgt_conf) {
			SPDK_ERRLOG("spdk_ftp_parse_ftp_tgt() failed\n");
			return -1;
		}

	}

	/* create the tgt and register io channel method */
	g_spdk_ftp_tgt = spdk_ftp_tgt_create();

	if (!g_spdk_ftp_tgt) {
		SPDK_ERRLOG("spdk_ftp_tgt_create() failed\n");
		return -1;
	}

	/* todo do more thing init */

	return 0;
}


static int
spdk_ftp_read_config_file_ftpd_conf(struct spdk_conf_section *sp,
				    struct spdk_ftp_server *ftpd)
{
	int conn_timeout;
	int session_timeout;
	int rc = 0;

	conn_timeout = spdk_conf_section_get_intval(sp, "ConnectTimeout");
	if (conn_timeout >= 0) {
		ftpd->opts.spdk_ftpd_connect_timeout = conn_timeout;
	}

	session_timeout = spdk_conf_section_get_intval(sp, "SessionTimeout");
	if (session_timeout >= 0) {
		ftpd->opts.spdk_ftpd_idle_session_timeout = session_timeout;
	}

	/* todo parse more conf params here */

	return rc;
}


static void
spdk_ftp_tgt_add_server_done(void *cb_arg, int status)
{
	struct spdk_ftp_parse_ftpds_ctx *ctx = cb_arg;
	if (status < 0) {
		SPDK_ERRLOG("Add server to target failed (%d).\n", status);
		ctx->cb_fn(status);
		free(ctx);
		return ;
	}



	ctx->cb_fn(0);
	free(ctx);

}

static void
spdk_ftp_tgt_listen_done(void *cb_arg, int status)
{
	/* TODO: Config parsing should wait for this operation to finish. */

	if (status) {
		SPDK_ERRLOG("Failed to listen on server address\n");
	}
}
static int
spdk_ftp_parse_ftpd(struct spdk_ftp_parse_ftpds_ctx *ctx)
{
	struct spdk_ftp_server *ftpd;
	int rc = 0;
	int listen_port = 0;
	struct spdk_ftp_server_opts opts;
	const char *type;
	enum spdk_ftp_server_type stype;


	type = spdk_conf_section_get_val(ctx->sp, "Protocol");
	if (type == NULL) {
		SPDK_ERRLOG("FTP missing Protocol, either tcp or rdma\n");
		ctx->cb_fn(-1);
		free(ctx);
		return -1;
	}
	if (spdk_ftp_server_parse_type(&stype, type)) {
		ctx->cb_fn(-1);
		free(ctx);
		return -1;

	}


	if (!spdk_ftp_server_opts_init(stype, &opts)) {
		ctx->cb_fn(-1);
		free(ctx);
		return -1;
	}



	listen_port = spdk_conf_section_get_intval(ctx->sp, "ListenPort");
	if (listen_port == -1) {
		SPDK_ERRLOG("Ftpd missing listen_port\n");
		return -1;
	}
	opts.spdk_ftpd_listen_port = listen_port;
	opts.ipaddr = spdk_conf_section_get_val(ctx->sp, "Listen_address");
	printf("Type is %s, listen_port is %d, addr is %s\n ", type, listen_port, opts.ipaddr);
	ftpd = spdk_ftp_server_create(stype, &opts);
	if (ftpd == NULL) {
		SPDK_ERRLOG("Ftpd create failed\n");
		return -1;
	}


	rc = spdk_ftp_read_config_file_ftpd_conf(ctx->sp, ftpd);
	spdk_ftp_tgt_add_server(g_spdk_ftp_tgt, ftpd, spdk_ftp_tgt_add_server_done, ctx);
	spdk_ftp_tgt_listen(g_spdk_ftp_tgt, stype, spdk_ftp_tgt_listen_done, NULL);
	//TAILQ_INSERT_TAIL(&g_spdk_ftp_tgt->ftpds, ftpd, link);

	return rc;
}


static int
spdk_ftp_parse_ftpds(spdk_ftp_parse_conf_done cb_fn)
{
	int rc = 0;
	struct spdk_ftp_parse_ftpds_ctx *ctx;

	ctx = calloc(1, sizeof(struct spdk_ftp_parse_ftpds_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed alloc of context memory for parse ftpds\n");
		return -ENOMEM;
	}

	ctx->sp = spdk_conf_first_section(NULL);
	ctx->cb_fn = cb_fn;

	/* parse the [Ftpd1] [Ftpd2] ... */
	while (ctx->sp != NULL) {
		if (spdk_conf_section_match_prefix(ctx->sp, "Ftpd")) {
			rc = spdk_ftp_parse_ftpd(ctx);
			if (rc < 0) {
				return -1;
			}
		}
		ctx->sp = spdk_conf_next_section(ctx->sp);
	}
	return 0;
}


int
spdk_ftp_parse_conf(spdk_ftp_parse_conf_done cb_fn)
{
	int rc = 0;
	if (cb_fn == NULL) {
		SPDK_ERRLOG("Callback function is NULL\n");
		return -1;
	}

	rc = spdk_ftp_parse_ftp_tgt();
	if (rc < 0) {
		return rc;
	}

	rc = spdk_ftp_parse_ftpds(cb_fn);
	if (rc < 0) {
		return rc;
	}

	return rc;
}
