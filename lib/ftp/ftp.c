/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/ftp.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "ftp_internal.h"
#include "spdk/log.h"


static int
spdk_ftp_tgt_create_poll_group(void *io_device, void *ctx_buf)
{

	struct spdk_ftp_tgt *tgt = io_device;
	struct spdk_ftp_poll_group *group = ctx_buf;
	struct spdk_ftp_server *temp_ftpd;
	struct spdk_ftp_server_poll_group *fpg;

	TAILQ_INIT(&group->ftpd_pgs);
	group->thread = spdk_get_thread();

	TAILQ_FOREACH(temp_ftpd, &tgt->ftpds, link) {
		fpg = spdk_ftp_server_poll_group_create(temp_ftpd);
		TAILQ_INSERT_TAIL(&group->ftpd_pgs, fpg, link);
	}

	return 0;
}


static void
spdk_ftp_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_ftp_poll_group *group = ctx_buf;
	struct spdk_ftp_server_poll_group *temp_fpg;

	TAILQ_FOREACH(temp_fpg, &group->ftpd_pgs, link) {
		TAILQ_REMOVE(&group->ftpd_pgs, temp_fpg, link);
		spdk_ftp_server_poll_group_destroy(temp_fpg);
	}

}


void
spdk_ftp_poll_group_destroy(struct spdk_ftp_poll_group *group)
{
	/* destroy io_channel invoke
	 * this will call spdk_ftp_tgt_destroy_poll_group , because io_channel begin to be destroy
	 */
	spdk_put_io_channel(spdk_io_channel_from_ctx(group));
}


void
spdk_ftp_tgt_destroy_server(struct spdk_ftp_tgt *tgt)
{
	struct spdk_ftp_server *temp_ftpd;

	TAILQ_FOREACH(temp_ftpd, &tgt->ftpds, link) {
		TAILQ_REMOVE(&tgt->ftpds, temp_ftpd, link);
		spdk_ftp_server_destroy(temp_ftpd);
	}

}


struct spdk_ftp_tgt *
spdk_ftp_tgt_create(void)
{

	struct spdk_ftp_tgt *tgt;

	tgt = calloc(1, sizeof(*tgt));
	if (!tgt) {
		return NULL;
	}

	TAILQ_INIT(&tgt->ftpds);

	/* register io_channel */
	spdk_io_device_register(tgt,
				spdk_ftp_tgt_create_poll_group,	/* io_channel create function */
				spdk_ftp_tgt_destroy_poll_group,	/* io_channel destroy function */
				sizeof(struct spdk_ftp_poll_group),
				"ftp_tgt");
	return tgt;
}
