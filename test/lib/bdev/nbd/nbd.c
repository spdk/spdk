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

#include <linux/nbd.h>

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/io_channel.h"

static char *g_bdev_name;
static char *g_nbd_name = "/dev/nbd0";

#include "../common.c"

struct nbd_io {
	enum spdk_bdev_io_type	type;
	void			*payload;

	/* NOTE: for TRIM, this represents number of bytes to trim. */
	uint32_t		payload_size;

	bool			payload_in_progress;

	struct nbd_request	req;
	bool			req_in_progress;

	struct nbd_reply	resp;
	bool			resp_in_progress;

	struct spdk_scsi_unmap_bdesc	unmap;

	/*
	 * Tracks current progress on reading/writing a request,
	 * response, or payload from the nbd socket.
	 */
	uint32_t		offset;
};

struct nbd_disk {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch;
	int			fd;
	struct spdk_poller	*poller;
	struct nbd_io		io;
};

struct nbd_disk g_nbd_disk = {};

static bool
is_read(enum spdk_bdev_io_type io_type)
{
	if (io_type == SPDK_BDEV_IO_TYPE_READ) {
		return true;
	} else {
		return false;
	}
}

static bool
is_write(enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;
	default:
		return false;
	}
}

static void
nbd_shutdown(void)
{
	spdk_put_io_channel(g_nbd_disk.ch);
	spdk_bdev_close(g_nbd_disk.bdev_desc);
	close(g_nbd_disk.fd);
	spdk_app_stop(0);
}

static uint64_t
read_from_socket(int fd, void *buf, size_t length)
{
	ssize_t bytes_read;

	bytes_read = read(fd, buf, length);
	if (bytes_read == 0) {
		spdk_app_stop(-1);
		return 0;
	} else if (bytes_read == -1) {
		if (errno != EAGAIN) {
			spdk_app_stop(-1);
		}
		return 0;
	} else {
		return bytes_read;
	}
}

static uint64_t
write_to_socket(int fd, void *buf, size_t length)
{
	ssize_t bytes_written;

	bytes_written = write(fd, buf, length);
	if (bytes_written == 0) {
		spdk_app_stop(-1);
		return 0;
	} else if (bytes_written == -1) {
		if (errno != EAGAIN) {
			spdk_app_stop(-1);
		}
		return 0;
	} else {
		return bytes_written;
	}
}

static void
nbd_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct nbd_io	*io = cb_arg;

	if (success) {
		io->resp.error = 0;
	} else {
		to_be32(&io->resp.error, EIO);
	}
	io->resp_in_progress = true;
	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}
}

static void
nbd_submit_bdev_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		   struct spdk_io_channel *ch, struct nbd_io *io)
{
	int rc;

	switch (io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rc = spdk_bdev_read(desc, ch, io->payload, from_be64(&io->req.from),
				    io->payload_size, nbd_io_done, io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_write(desc, ch, io->payload, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		to_be64(&io->unmap.lba, from_be64(&io->req.from) / spdk_bdev_get_block_size(bdev));
		to_be32(&io->unmap.block_count, io->payload_size / spdk_bdev_get_block_size(bdev));
		rc = spdk_bdev_unmap(desc, ch, &io->unmap, 1, nbd_io_done, io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush(desc, ch, 0, spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev),
				     nbd_io_done, io);
		break;
	default:
		rc = -1;
		break;
	}

	if (rc == -1) {
		nbd_io_done(NULL, false, io);
	}
}

static void
process_request(struct nbd_disk *nbd)
{
	struct nbd_io	*io = &nbd->io;

	memcpy(&io->resp.handle, &io->req.handle, sizeof(io->resp.handle));
	io->resp.error = 0;
	io->offset = 0;

	io->payload_size = from_be32(&io->req.len);
	spdk_dma_free(io->payload);
	io->payload = spdk_dma_malloc(io->payload_size, 64, NULL);
	if (io->payload == NULL) {
		SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
		spdk_app_stop(-1);
		return;
	}

	assert(from_be32(&io->req.magic) == NBD_REQUEST_MAGIC);

	switch (from_be32(&io->req.type)) {
	case NBD_CMD_READ:
		io->type = SPDK_BDEV_IO_TYPE_READ;
		nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
		break;
	case NBD_CMD_WRITE:
		io->type = SPDK_BDEV_IO_TYPE_WRITE;
		io->payload_in_progress = true;
		break;
	case NBD_CMD_DISC:
		nbd_shutdown();
		return;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		io->type = SPDK_BDEV_IO_TYPE_FLUSH;
		nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		io->type = SPDK_BDEV_IO_TYPE_UNMAP;
		nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
		break;
#endif
	}
}

static void
nbd_poll(void *arg)
{
	struct nbd_disk *nbd = arg;
	struct nbd_io	*io = &nbd->io;
	int		fd = nbd->fd;
	uint64_t	ret;

	if (io->req_in_progress) {
		ret = read_from_socket(fd, (char *)&io->req + io->offset, sizeof(io->req) - io->offset);
		if (ret == 0) {
			return;
		}
		io->offset += ret;
		if (io->offset == sizeof(io->req)) {
			io->req_in_progress = false;
			process_request(nbd);
		}
	}

	if (io->payload_in_progress && is_write(io->type)) {
		ret = read_from_socket(fd, io->payload + io->offset, io->payload_size - io->offset);
		if (ret == 0) {
			return;
		}
		io->offset += ret;
		if (io->offset == io->payload_size) {
			io->payload_in_progress = false;
			nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
			io->offset = 0;
		}
	}

	if (io->resp_in_progress) {
		ret = write_to_socket(fd, (char *)&io->resp + io->offset, sizeof(io->resp) - io->offset);
		if (ret == 0) {
			return;
		}
		io->offset += ret;
		if (io->offset == sizeof(io->resp)) {
			io->resp_in_progress = false;
			if (is_read(io->type)) {
				io->payload_in_progress = true;
			} else {
				io->req_in_progress = true;
			}
			io->offset = 0;
		}
	}

	if (io->payload_in_progress && is_read(io->type)) {
		ret = write_to_socket(fd, io->payload + io->offset, io->payload_size - io->offset);
		if (ret == 0) {
			return;
		}
		io->offset += ret;
		if (io->offset == io->payload_size) {
			io->payload_in_progress = false;
			io->req_in_progress = true;
			io->offset = 0;
		}
	}
}

static void
nbd_start_kernel(int nbd_fd, int *sp)
{
	int rc;

	close(sp[0]);

	rc = ioctl(nbd_fd, NBD_SET_SOCK, sp[1]);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", strerror(errno));
		exit(-1);
	}

#ifdef NBD_FLAG_SEND_TRIM
	rc = ioctl(nbd_fd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_FLAGS) failed: %s\n", strerror(errno));
		exit(-1);
	}
#endif

	/* This will block in the kernel until the client disconnects. */
	ioctl(nbd_fd, NBD_DO_IT);

	ioctl(nbd_fd, NBD_CLEAR_QUE);
	ioctl(nbd_fd, NBD_CLEAR_SOCK);

	exit(0);
}

static void
nbd_start(void *arg1, void *arg2)
{
	struct spdk_bdev	*bdev;
	int			rc;
	int			sp[2], nbd_fd;

	bdev = spdk_bdev_get_by_name(g_bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev %s exists\n", g_bdev_name);
		spdk_app_stop(-1);
		return;
	}

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &g_nbd_disk.bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", g_bdev_name, rc);
		spdk_app_stop(-1);
		return;
	}

	g_nbd_disk.bdev = bdev;
	g_nbd_disk.ch = spdk_bdev_get_io_channel(g_nbd_disk.bdev_desc);

	rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
	if (rc != 0) {
		SPDK_ERRLOG("socketpair failed\n");
		spdk_app_stop(-1);
		return;
	}

	nbd_fd = open(g_nbd_name, O_RDWR);
	if (nbd_fd == -1) {
		SPDK_ERRLOG("open(\"%s\") failed: %s\n", g_nbd_name, strerror(errno));
		spdk_app_stop(-1);
		return;
	}

	rc = ioctl(nbd_fd, NBD_SET_BLKSIZE, spdk_bdev_get_block_size(bdev));
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_BLKSIZE) failed: %s\n", strerror(errno));
		spdk_app_stop(-1);
		return;
	}

	rc = ioctl(nbd_fd, NBD_SET_SIZE_BLOCKS, spdk_bdev_get_num_blocks(bdev));
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_SIZE_BLOCKS) failed: %s\n", strerror(errno));
		spdk_app_stop(-1);
		return;
	}

	rc = ioctl(nbd_fd, NBD_CLEAR_SOCK);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_CLEAR_SOCK) failed: %s\n", strerror(errno));
		spdk_app_stop(-1);
		return;
	}

	printf("Enabling kernel access to bdev %s via %s\n", g_bdev_name, g_nbd_name);

	rc = fork();

	switch (rc) {
	case 0:
		nbd_start_kernel(nbd_fd, sp);
		break;
	case -1:
		SPDK_ERRLOG("could not fork: %s\n", strerror(errno));
		spdk_app_stop(-1);
		return;
	default:
		break;
	}

	close(sp[1]);

	g_nbd_disk.fd = sp[0];
	fcntl(g_nbd_disk.fd, F_SETFL, O_NONBLOCK);

	to_be32(&g_nbd_disk.io.resp.magic, NBD_REPLY_MAGIC);
	g_nbd_disk.io.req_in_progress = true;

	spdk_poller_register(&g_nbd_disk.poller, nbd_poll, &g_nbd_disk, spdk_env_get_current_core(), 0);

}

static void usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf(" -b bdev    export bdev via NBD (required)\n");
	printf(" -c conf    configuration file (required)\n");
	printf(" -m mask    core mask for distributing I/O submission/completion work\n");
	printf("            (default: 0x1 - use core 0 only)\n");
	printf(" -n dev     nbd device name\n");
	printf("            (default: /dev/nbd0)\n");
	spdk_tracelog_usage(stdout, "-t");
}

int
main(int argc, char **argv)
{
	const char *config_file;
	const char *core_mask;
	int op;
	struct spdk_app_opts opts = {};

	/* default value */
	config_file = NULL;
	core_mask = NULL;

	while ((op = getopt(argc, argv, "b:c:m:n:t:")) != -1) {
		switch (op) {
		case 'b':
			g_bdev_name = optarg;
			break;
		case 'c':
			config_file = optarg;
			break;
		case 'm':
			core_mask = optarg;
			break;
		case 'n':
			g_nbd_name = optarg;
			break;
		case 't':
			if (spdk_log_set_trace_flag(optarg) < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -t flag.\n",
				argv[0]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
#endif
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (!config_file || !g_bdev_name) {
		usage(argv[0]);
		exit(1);
	}

	bdevtest_init(config_file, core_mask, &opts);
	opts.shutdown_cb = nbd_shutdown;

	spdk_app_start(&opts, nbd_start, NULL, NULL);

	spdk_app_fini();
	return 0;
}
