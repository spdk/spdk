/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vmd.h"

#include <libaio.h>

#ifdef SPDK_CONFIG_URING
#include <liburing.h>
#endif

#define TIMESPEC_TO_MS(time) ((time.tv_sec * 1000) + (time.tv_nsec / 1000000))
#define STATUS_POLLER_PERIOD_SEC 1

struct spdk_dd_opts {
	char		*input_file;
	char		*output_file;
	char		*input_file_flags;
	char		*output_file_flags;
	char		*input_bdev;
	char		*output_bdev;
	uint64_t	input_offset;
	uint64_t	output_offset;
	int64_t		io_unit_size;
	int64_t		io_unit_count;
	uint32_t	queue_depth;
	bool		aio;
	bool		sparse;
};

static struct spdk_dd_opts g_opts = {
	.io_unit_size = 4096,
	.queue_depth = 2,
};

enum dd_submit_type {
	DD_POPULATE,
	DD_READ,
	DD_WRITE,
};

struct dd_io {
	uint64_t		offset;
	uint64_t		length;
	struct iocb		iocb;
	enum dd_submit_type	type;
#ifdef SPDK_CONFIG_URING
	int			idx;
#endif
	void			*buf;
	STAILQ_ENTRY(dd_io)	link;
};

enum dd_target_type {
	DD_TARGET_TYPE_FILE,
	DD_TARGET_TYPE_BDEV,
};

struct dd_target {
	enum dd_target_type	type;

	union {
		struct {
			struct spdk_bdev *bdev;
			struct spdk_bdev_desc *desc;
			struct spdk_io_channel *ch;
		} bdev;

#ifdef SPDK_CONFIG_URING
		struct {
			int fd;
			int idx;
		} uring;
#endif
		struct {
			int fd;
		} aio;
	} u;

	/* Block size of underlying device. */
	uint32_t	block_size;

	/* Position of next I/O in bytes */
	uint64_t	pos;

	/* Total size of target in bytes */
	uint64_t	total_size;

	bool open;
};

struct dd_job {
	struct dd_target	input;
	struct dd_target	output;

	struct dd_io		*ios;

	union {
#ifdef SPDK_CONFIG_URING
		struct {
			struct io_uring ring;
			bool active;
			struct spdk_poller *poller;
		} uring;
#endif
		struct {
			io_context_t io_ctx;
			struct spdk_poller *poller;
		} aio;
	} u;

	uint32_t		outstanding;
	uint64_t		copy_size;
	STAILQ_HEAD(, dd_io)	seek_queue;

	struct timespec		start_time;
	uint64_t		total_bytes;
	uint64_t		incremental_bytes;
	struct spdk_poller	*status_poller;
};

struct dd_flags {
	char *name;
	int flag;
};

static struct dd_flags g_flags[] = {
	{"append", O_APPEND},
	{"direct", O_DIRECT},
	{"directory", O_DIRECTORY},
	{"dsync", O_DSYNC},
	{"noatime", O_NOATIME},
	{"noctty", O_NOCTTY},
	{"nofollow", O_NOFOLLOW},
	{"nonblock", O_NONBLOCK},
	{"sync", O_SYNC},
	{NULL, 0}
};

static struct dd_job g_job = {};
static int g_error = 0;
static bool g_interrupt;

static void dd_target_seek(struct dd_io *io);
static void _dd_bdev_seek_hole_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void
dd_cleanup_bdev(struct dd_target io)
{
	/* This can be only on the error path.
	 * To prevent the SEGV, need add checks here.
	 */
	if (io.u.bdev.ch) {
		spdk_put_io_channel(io.u.bdev.ch);
	}

	spdk_bdev_close(io.u.bdev.desc);
}

static void
dd_exit(int rc)
{
	if (g_job.input.type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			close(g_job.input.u.uring.fd);
		} else
#endif
		{
			close(g_job.input.u.aio.fd);
		}
	} else if (g_job.input.type == DD_TARGET_TYPE_BDEV && g_job.input.open) {
		dd_cleanup_bdev(g_job.input);
	}

	if (g_job.output.type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			close(g_job.output.u.uring.fd);
		} else
#endif
		{
			close(g_job.output.u.aio.fd);
		}
	} else if (g_job.output.type == DD_TARGET_TYPE_BDEV && g_job.output.open) {
		dd_cleanup_bdev(g_job.output);
	}

	if (g_job.input.type == DD_TARGET_TYPE_FILE || g_job.output.type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			spdk_poller_unregister(&g_job.u.uring.poller);
		} else
#endif
		{
			spdk_poller_unregister(&g_job.u.aio.poller);
		}
	}

	spdk_poller_unregister(&g_job.status_poller);

	spdk_app_stop(rc);
}

static void
dd_show_progress(bool finish)
{
	char *unit_str[5] = {"", "k", "M", "G", "T"};
	char *speed_type_str[2] = {"", "average "};
	char *size_unit_str = "";
	char *speed_unit_str = "";
	char *speed_type = "";
	uint64_t size_unit = 1;
	uint64_t speed_unit = 1;
	uint64_t speed, tmp_speed;
	int i = 0;
	uint64_t milliseconds;
	uint64_t size, tmp_size;

	size = g_job.incremental_bytes;

	g_job.incremental_bytes = 0;
	g_job.total_bytes += size;

	if (finish) {
		struct timespec time_now;

		clock_gettime(CLOCK_REALTIME, &time_now);

		milliseconds = spdk_max(1, TIMESPEC_TO_MS(time_now) - TIMESPEC_TO_MS(g_job.start_time));
		size = g_job.total_bytes;
	} else {
		milliseconds = STATUS_POLLER_PERIOD_SEC * 1000;
	}

	/* Find the right unit for size displaying (B vs kB vs MB vs GB vs TB) */
	tmp_size = size;
	while (tmp_size > 1024 * 10) {
		tmp_size >>= 10;
		size_unit <<= 10;
		size_unit_str = unit_str[++i];
		if (i == 4) {
			break;
		}
	}

	speed_type = finish ? speed_type_str[1] : speed_type_str[0];
	speed = (size * 1000) / milliseconds;

	i = 0;

	/* Find the right unit for speed displaying (Bps vs kBps vs MBps vs GBps vs TBps) */
	tmp_speed = speed;
	while (tmp_speed > 1024 * 10) {
		tmp_speed >>= 10;
		speed_unit <<= 10;
		speed_unit_str = unit_str[++i];
		if (i == 4) {
			break;
		}
	}

	printf("\33[2K\rCopying: %" PRIu64 "/%" PRIu64 " [%sB] (%s%" PRIu64 " %sBps)",
	       g_job.total_bytes / size_unit, g_job.copy_size / size_unit, size_unit_str, speed_type,
	       speed / speed_unit, speed_unit_str);
	fflush(stdout);
}

static int
dd_status_poller(void *ctx)
{
	dd_show_progress(false);
	return SPDK_POLLER_BUSY;
}

static void
dd_finalize_output(void)
{
	off_t curr_offset;
	int rc = 0;

	if (g_job.outstanding > 0) {
		return;
	}

	if (g_opts.output_file) {
		curr_offset = lseek(g_job.output.u.aio.fd, 0, SEEK_END);
		if (curr_offset == (off_t) -1) {
			SPDK_ERRLOG("Could not seek output file for finalize: %s\n", strerror(errno));
			g_error = errno;
		} else if ((uint64_t)curr_offset < g_job.copy_size + g_job.output.pos) {
			rc = ftruncate(g_job.output.u.aio.fd, g_job.copy_size + g_job.output.pos);
			if (rc != 0) {
				SPDK_ERRLOG("Could not truncate output file for finalize: %s\n", strerror(errno));
				g_error = errno;
			}
		}
	}

	if (g_error == 0) {
		dd_show_progress(true);
		printf("\n\n");
	}
	dd_exit(g_error);
}

#ifdef SPDK_CONFIG_URING
static void
dd_uring_submit(struct dd_io *io, struct dd_target *target, uint64_t length, uint64_t offset)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&g_job.u.uring.ring);
	if (io->type == DD_READ || io->type == DD_POPULATE) {
		io_uring_prep_read_fixed(sqe, target->u.uring.idx, io->buf, length, offset, io->idx);
	} else {
		io_uring_prep_write_fixed(sqe, target->u.uring.idx, io->buf, length, offset, io->idx);
	}
	sqe->flags |= IOSQE_FIXED_FILE;
	io_uring_sqe_set_data(sqe, io);
	io_uring_submit(&g_job.u.uring.ring);
}
#endif

static void
_dd_write_bdev_done(struct spdk_bdev_io *bdev_io,
		    bool success,
		    void *cb_arg)
{
	struct dd_io *io = cb_arg;

	assert(g_job.outstanding > 0);
	g_job.outstanding--;
	spdk_bdev_free_io(bdev_io);
	dd_target_seek(io);
}

static void
dd_target_write(struct dd_io *io)
{
	struct dd_target *target = &g_job.output;
	uint64_t length = SPDK_CEIL_DIV(io->length, target->block_size) * target->block_size;
	uint64_t read_region_start = g_opts.input_offset * g_opts.io_unit_size;
	uint64_t read_offset = io->offset - read_region_start;
	uint64_t write_region_start = g_opts.output_offset * g_opts.io_unit_size;
	uint64_t write_offset = write_region_start + read_offset;
	int rc = 0;

	if (g_error != 0 || g_interrupt == true) {
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	g_job.incremental_bytes += io->length;
	g_job.outstanding++;
	io->type = DD_WRITE;

	if (target->type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			dd_uring_submit(io, target, length, write_offset);
		} else
#endif
		{
			struct iocb *iocb = &io->iocb;

			io_prep_pwrite(iocb, target->u.aio.fd, io->buf, length, write_offset);
			iocb->data = io;
			if (io_submit(g_job.u.aio.io_ctx, 1, &iocb) < 0) {
				rc = -errno;
			}
		}
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		rc = spdk_bdev_write(target->u.bdev.desc, target->u.bdev.ch, io->buf, write_offset, length,
				     _dd_write_bdev_done, io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		assert(g_job.outstanding > 0);
		g_job.outstanding--;
		g_error = rc;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		}
		return;
	}
}

static void
_dd_read_bdev_done(struct spdk_bdev_io *bdev_io,
		   bool success,
		   void *cb_arg)
{
	struct dd_io *io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	assert(g_job.outstanding > 0);
	g_job.outstanding--;
	dd_target_write(io);
}

static void
dd_target_read(struct dd_io *io)
{
	struct dd_target *target = &g_job.input;
	int rc = 0;

	if (g_error != 0 || g_interrupt == true) {
		if (g_job.outstanding == 0) {
			dd_exit(g_error);
		}
		return;
	}

	g_job.outstanding++;
	io->type = DD_READ;

	if (target->type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			dd_uring_submit(io, target, io->length, io->offset);
		} else
#endif
		{
			struct iocb *iocb = &io->iocb;

			io_prep_pread(iocb, target->u.aio.fd, io->buf, io->length, io->offset);
			iocb->data = io;
			if (io_submit(g_job.u.aio.io_ctx, 1, &iocb) < 0) {
				rc = -errno;
			}
		}
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		rc = spdk_bdev_read(target->u.bdev.desc, target->u.bdev.ch, io->buf, io->offset, io->length,
				    _dd_read_bdev_done, io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		assert(g_job.outstanding > 0);
		g_job.outstanding--;
		g_error = rc;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		}
		return;
	}
}

static void
_dd_target_populate_buffer_done(struct spdk_bdev_io *bdev_io,
				bool success,
				void *cb_arg)
{
	struct dd_io *io = cb_arg;

	assert(g_job.outstanding > 0);
	g_job.outstanding--;
	spdk_bdev_free_io(bdev_io);
	dd_target_read(io);
}

static void
dd_target_populate_buffer(struct dd_io *io)
{
	struct dd_target *target = &g_job.output;
	uint64_t read_region_start = g_opts.input_offset * g_opts.io_unit_size;
	uint64_t read_offset = g_job.input.pos - read_region_start;
	uint64_t write_region_start = g_opts.output_offset * g_opts.io_unit_size;
	uint64_t write_offset = write_region_start + read_offset;
	uint64_t length;
	int rc = 0;

	io->offset = g_job.input.pos;
	io->length = spdk_min(io->length, g_job.copy_size - read_offset);

	if (io->length == 0 || g_error != 0 || g_interrupt == true) {
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	g_job.input.pos += io->length;

	if ((io->length % target->block_size) == 0) {
		dd_target_read(io);
		return;
	}

	/* Read whole blocks from output to combine buffers later */
	g_job.outstanding++;
	io->type = DD_POPULATE;

	length = SPDK_CEIL_DIV(io->length, target->block_size) * target->block_size;

	if (target->type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			dd_uring_submit(io, target, length, write_offset);
		} else
#endif
		{
			struct iocb *iocb = &io->iocb;

			io_prep_pread(iocb, target->u.aio.fd, io->buf, length, write_offset);
			iocb->data = io;
			if (io_submit(g_job.u.aio.io_ctx, 1, &iocb) < 0) {
				rc = -errno;
			}
		}
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		rc = spdk_bdev_read(target->u.bdev.desc, target->u.bdev.ch, io->buf, write_offset, length,
				    _dd_target_populate_buffer_done, io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		assert(g_job.outstanding > 0);
		g_job.outstanding--;
		g_error = rc;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		}
		return;
	}
}

static off_t
dd_file_seek_data(void)
{
	off_t next_data_offset = (off_t) -1;

	next_data_offset = lseek(g_job.input.u.aio.fd, g_job.input.pos, SEEK_DATA);

	if (next_data_offset == (off_t) -1) {
		/* NXIO with SEEK_DATA means there are no more data to read.
		 * But in case of input and output files, we may have to finalize output file
		 * inserting a hole to the end of the file.
		 */
		if (errno == ENXIO) {
			dd_finalize_output();
		} else if (g_job.outstanding == 0) {
			SPDK_ERRLOG("Could not seek input file for data: %s\n", strerror(errno));
			g_error = errno;
			dd_exit(g_error);
		}
	}

	return next_data_offset;
}

static off_t
dd_file_seek_hole(void)
{
	off_t next_hole_offset = (off_t) -1;

	next_hole_offset = lseek(g_job.input.u.aio.fd, g_job.input.pos, SEEK_HOLE);

	if (next_hole_offset == (off_t) -1 && g_job.outstanding == 0) {
		SPDK_ERRLOG("Could not seek input file for hole: %s\n", strerror(errno));
		g_error = errno;
		dd_exit(g_error);
	}

	return next_hole_offset;
}

static void
_dd_bdev_seek_data_done(struct spdk_bdev_io *bdev_io,
			bool success,
			void *cb_arg)
{
	struct dd_io *io = cb_arg;
	uint64_t next_data_offset_blocks = UINT64_MAX;
	struct dd_target *target = &g_job.input;
	int rc = 0;

	if (g_error != 0 || g_interrupt == true) {
		STAILQ_REMOVE_HEAD(&g_job.seek_queue, link);
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	assert(g_job.outstanding > 0);
	g_job.outstanding--;

	next_data_offset_blocks = spdk_bdev_io_get_seek_offset(bdev_io);
	spdk_bdev_free_io(bdev_io);

	/* UINT64_MAX means there are no more data to read.
	 * But in case of input and output files, we may have to finalize output file
	 * inserting a hole to the end of the file.
	 */
	if (next_data_offset_blocks == UINT64_MAX) {
		STAILQ_REMOVE_HEAD(&g_job.seek_queue, link);
		dd_finalize_output();
		return;
	}

	g_job.input.pos = next_data_offset_blocks * g_job.input.block_size;

	g_job.outstanding++;
	rc = spdk_bdev_seek_hole(target->u.bdev.desc, target->u.bdev.ch,
				 g_job.input.pos / g_job.input.block_size,
				 _dd_bdev_seek_hole_done, io);

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		STAILQ_REMOVE_HEAD(&g_job.seek_queue, link);
		assert(g_job.outstanding > 0);
		g_job.outstanding--;
		g_error = rc;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		}
	}
}

static void
_dd_bdev_seek_hole_done(struct spdk_bdev_io *bdev_io,
			bool success,
			void *cb_arg)
{
	struct dd_io *io = cb_arg;
	struct dd_target *target = &g_job.input;
	uint64_t next_hole_offset_blocks = UINT64_MAX;
	struct dd_io *seek_io;
	int rc = 0;

	/* First seek operation is the one in progress, i.e. this one just ended */
	STAILQ_REMOVE_HEAD(&g_job.seek_queue, link);

	if (g_error != 0 || g_interrupt == true) {
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	assert(g_job.outstanding > 0);
	g_job.outstanding--;

	next_hole_offset_blocks = spdk_bdev_io_get_seek_offset(bdev_io);
	spdk_bdev_free_io(bdev_io);

	/* UINT64_MAX means there are no more holes. */
	if (next_hole_offset_blocks == UINT64_MAX) {
		io->length = g_opts.io_unit_size;
	} else {
		io->length = spdk_min((uint64_t)g_opts.io_unit_size,
				      next_hole_offset_blocks * g_job.input.block_size - g_job.input.pos);
	}

	dd_target_populate_buffer(io);

	/* If input reading is not at the end, start following seek operation in the queue */
	if (!STAILQ_EMPTY(&g_job.seek_queue) && g_job.input.pos < g_job.input.total_size) {
		seek_io = STAILQ_FIRST(&g_job.seek_queue);
		assert(seek_io != NULL);
		g_job.outstanding++;
		rc = spdk_bdev_seek_data(target->u.bdev.desc, target->u.bdev.ch,
					 g_job.input.pos / g_job.input.block_size,
					 _dd_bdev_seek_data_done, seek_io);

		if (rc != 0) {
			SPDK_ERRLOG("%s\n", strerror(-rc));
			assert(g_job.outstanding > 0);
			g_job.outstanding--;
			g_error = rc;
			if (g_job.outstanding == 0) {
				dd_exit(rc);
			}
		}
	}
}

static void
dd_target_seek(struct dd_io *io)
{
	struct dd_target *target = &g_job.input;
	uint64_t read_region_start = g_opts.input_offset * g_opts.io_unit_size;
	uint64_t read_offset = g_job.input.pos - read_region_start;
	off_t next_data_offset = (off_t) -1;
	off_t next_hole_offset = (off_t) -1;
	int rc = 0;

	if (!g_opts.sparse) {
		dd_target_populate_buffer(io);
		return;
	}

	if (g_job.copy_size - read_offset == 0 || g_error != 0 || g_interrupt == true) {
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	if (target->type == DD_TARGET_TYPE_FILE) {
		next_data_offset = dd_file_seek_data();
		if (next_data_offset < 0) {
			return;
		} else if ((uint64_t)next_data_offset > g_job.input.pos) {
			g_job.input.pos = next_data_offset;
		}

		next_hole_offset = dd_file_seek_hole();
		if (next_hole_offset < 0) {
			return;
		} else if ((uint64_t)next_hole_offset > g_job.input.pos) {
			io->length = spdk_min((uint64_t)g_opts.io_unit_size,
					      (uint64_t)(next_hole_offset - g_job.input.pos));
		} else {
			io->length = g_opts.io_unit_size;
		}

		dd_target_populate_buffer(io);
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		/* Check if other seek operation is in progress */
		if (STAILQ_EMPTY(&g_job.seek_queue)) {
			g_job.outstanding++;
			rc = spdk_bdev_seek_data(target->u.bdev.desc, target->u.bdev.ch,
						 g_job.input.pos / g_job.input.block_size,
						 _dd_bdev_seek_data_done, io);

		}

		STAILQ_INSERT_TAIL(&g_job.seek_queue, io, link);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		assert(g_job.outstanding > 0);
		g_job.outstanding--;
		g_error = rc;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		}
		return;
	}
}

static void
dd_complete_poll(struct dd_io *io)
{
	assert(g_job.outstanding > 0);
	g_job.outstanding--;

	switch (io->type) {
	case DD_POPULATE:
		dd_target_read(io);
		break;
	case DD_READ:
		dd_target_write(io);
		break;
	case DD_WRITE:
		dd_target_seek(io);
		break;
	default:
		assert(false);
		break;
	}
}

#ifdef SPDK_CONFIG_URING
static int
dd_uring_poll(void *ctx)
{
	struct io_uring_cqe *cqe;
	struct dd_io *io;
	int rc = 0;
	int i;

	for (i = 0; i < (int)g_opts.queue_depth; i++) {
		rc = io_uring_peek_cqe(&g_job.u.uring.ring, &cqe);
		if (rc == 0) {
			if (cqe->res == -EAGAIN) {
				continue;
			} else if (cqe->res < 0) {
				SPDK_ERRLOG("%s\n", strerror(-cqe->res));
				g_error = cqe->res;
			}

			io = io_uring_cqe_get_data(cqe);
			io_uring_cqe_seen(&g_job.u.uring.ring, cqe);

			dd_complete_poll(io);
		} else if (rc != - EAGAIN) {
			SPDK_ERRLOG("%s\n", strerror(-rc));
			g_error = rc;
		}
	}

	return rc;
}
#endif

static int
dd_aio_poll(void *ctx)
{
	struct io_event events[32];
	int rc = 0;
	int i;
	struct timespec timeout;
	struct dd_io *io;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	rc = io_getevents(g_job.u.aio.io_ctx, 0, 32, events, &timeout);

	if (rc < 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		dd_exit(rc);
	}

	for (i = 0; i < rc; i++) {
		io = events[i].data;
		if (events[i].res != io->length) {
			g_error = rc = -ENOSPC;
		}

		dd_complete_poll(io);
	}

	return rc;
}

static int
dd_open_file(struct dd_target *target, const char *fname, int flags, uint64_t skip_blocks,
	     bool input)
{
	int *fd;

#ifdef SPDK_CONFIG_URING
	if (g_opts.aio == false) {
		fd = &target->u.uring.fd;
	} else
#endif
	{
		fd = &target->u.aio.fd;
	}

	flags |= O_RDWR;

	if (input == false && ((flags & O_DIRECTORY) == 0)) {
		flags |= O_CREAT;
	}

	if (input == false && ((flags & O_APPEND) == 0)) {
		flags |= O_TRUNC;
	}

	target->type = DD_TARGET_TYPE_FILE;
	*fd = open(fname, flags, 0600);
	if (*fd < 0) {
		SPDK_ERRLOG("Could not open file %s: %s\n", fname, strerror(errno));
		return *fd;
	}

	target->block_size = spdk_max(spdk_fd_get_blocklen(*fd), 1);

	target->total_size = spdk_fd_get_size(*fd);
	if (target->total_size == 0) {
		target->total_size = g_opts.io_unit_size * g_opts.io_unit_count;
	}

	if (input == true) {
		g_opts.queue_depth = spdk_min(g_opts.queue_depth,
					      (target->total_size / g_opts.io_unit_size) - skip_blocks + 1);
	}

	if (g_opts.io_unit_count != 0) {
		g_opts.queue_depth = spdk_min(g_opts.queue_depth, g_opts.io_unit_count);
	}

	return 0;
}

static void
dd_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		 void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static int
dd_open_bdev(struct dd_target *target, const char *bdev_name, uint64_t skip_blocks)
{
	int rc;

	target->type = DD_TARGET_TYPE_BDEV;

	rc = spdk_bdev_open_ext(bdev_name, true, dd_bdev_event_cb, NULL, &target->u.bdev.desc);
	if (rc < 0) {
		SPDK_ERRLOG("Could not open bdev %s: %s\n", bdev_name, strerror(-rc));
		return rc;
	}

	target->u.bdev.bdev = spdk_bdev_desc_get_bdev(target->u.bdev.desc);
	target->open = true;

	target->u.bdev.ch = spdk_bdev_get_io_channel(target->u.bdev.desc);
	if (target->u.bdev.ch == NULL) {
		spdk_bdev_close(target->u.bdev.desc);
		SPDK_ERRLOG("Could not get I/O channel: %s\n", strerror(ENOMEM));
		return -ENOMEM;
	}

	target->block_size = spdk_bdev_get_block_size(target->u.bdev.bdev);
	target->total_size = spdk_bdev_get_num_blocks(target->u.bdev.bdev) * target->block_size;

	g_opts.queue_depth = spdk_min(g_opts.queue_depth,
				      (target->total_size / g_opts.io_unit_size) - skip_blocks + 1);

	if (g_opts.io_unit_count != 0) {
		g_opts.queue_depth = spdk_min(g_opts.queue_depth, g_opts.io_unit_count);
	}

	return 0;
}

static void
dd_finish(void)
{
	/* Interrupt operation */
	g_interrupt = true;
}

static int
parse_flags(char *file_flags)
{
	char *input_flag;
	int flags = 0;
	int i;
	bool found = false;

	/* Translate input flags to file open flags */
	while ((input_flag = strsep(&file_flags, ","))) {
		for (i = 0; g_flags[i].name != NULL; i++) {
			if (!strcmp(input_flag, g_flags[i].name)) {
				flags |= g_flags[i].flag;
				found = true;
				break;
			}
		}

		if (found == false) {
			SPDK_ERRLOG("Unknown file flag: %s\n", input_flag);
			return -EINVAL;
		}

		found = false;
	}

	return flags;
}

#ifdef SPDK_CONFIG_URING
static bool
dd_is_blk(int fd)
{
	struct stat st;

	if (fstat(fd, &st) != 0) {
		return false;
	}

	return S_ISBLK(st.st_mode);
}

static int
dd_register_files(void)
{
	int fds[2];
	unsigned count = 0;

	if (g_opts.input_file) {
		fds[count] = g_job.input.u.uring.fd;
		g_job.input.u.uring.idx = count;
		count++;
	}

	if (g_opts.output_file) {
		fds[count] = g_job.output.u.uring.fd;
		g_job.output.u.uring.idx = count;
		count++;
	}

	return io_uring_register_files(&g_job.u.uring.ring, fds, count);

}

static int
dd_register_buffers(void)
{
	struct iovec *iovs;
	int i, rc;

	iovs = calloc(g_opts.queue_depth, sizeof(struct iovec));
	if (iovs == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < (int)g_opts.queue_depth; i++) {
		iovs[i].iov_base = g_job.ios[i].buf;
		iovs[i].iov_len = g_opts.io_unit_size;
		g_job.ios[i].idx = i;
	}

	rc = io_uring_register_buffers(&g_job.u.uring.ring, iovs, g_opts.queue_depth);

	free(iovs);
	return rc;
}
#endif

static void
dd_run(void *arg1)
{
	uint64_t write_size;
	uint32_t i;
	int rc, flags = 0;

	if (g_opts.input_file) {
		if (g_opts.input_file_flags) {
			flags = parse_flags(g_opts.input_file_flags);
		}

		if (dd_open_file(&g_job.input, g_opts.input_file, flags, g_opts.input_offset, true) < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.input_file, strerror(errno));
			dd_exit(-errno);
			return;
		}
	} else if (g_opts.input_bdev) {
		rc = dd_open_bdev(&g_job.input, g_opts.input_bdev, g_opts.input_offset);
		if (rc < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.input_bdev, strerror(-rc));
			dd_exit(rc);
			return;
		}
	}

	write_size = g_opts.io_unit_count * g_opts.io_unit_size;
	g_job.input.pos = g_opts.input_offset * g_opts.io_unit_size;

	/* We cannot check write size for input files because /dev/zeros, /dev/random, etc would not work.
	 * We will handle that during copying */
	if (g_opts.input_bdev && g_job.input.pos > g_job.input.total_size) {
		SPDK_ERRLOG("--skip value too big (%" PRIu64 ") - only %" PRIu64 " blocks available in input\n",
			    g_opts.input_offset, g_job.input.total_size / g_opts.io_unit_size);
		dd_exit(-ENOSPC);
		return;
	}

	if (g_opts.io_unit_count != 0 && g_opts.input_bdev &&
	    write_size + g_job.input.pos > g_job.input.total_size) {
		SPDK_ERRLOG("--count value too big (%" PRIu64 ") - only %" PRIu64 " blocks available from input\n",
			    g_opts.io_unit_count, (g_job.input.total_size - g_job.input.pos) / g_opts.io_unit_size);
		dd_exit(-ENOSPC);
		return;
	}

	if (g_opts.io_unit_count != 0) {
		g_job.copy_size = write_size;
	} else {
		g_job.copy_size = g_job.input.total_size - g_job.input.pos;
	}

	g_job.output.pos = g_opts.output_offset * g_opts.io_unit_size;

	if (g_opts.output_file) {
		flags = 0;

		if (g_opts.output_file_flags) {
			flags = parse_flags(g_opts.output_file_flags);
		}

		if (dd_open_file(&g_job.output, g_opts.output_file, flags, g_opts.output_offset, false) < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.output_file, strerror(errno));
			dd_exit(-errno);
			return;
		}
	} else if (g_opts.output_bdev) {
		rc = dd_open_bdev(&g_job.output, g_opts.output_bdev, g_opts.output_offset);
		if (rc < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.output_bdev, strerror(-rc));
			dd_exit(rc);
			return;
		}

		if (g_job.output.pos > g_job.output.total_size) {
			SPDK_ERRLOG("--seek value too big (%" PRIu64 ") - only %" PRIu64 " blocks available in output\n",
				    g_opts.output_offset, g_job.output.total_size / g_opts.io_unit_size);
			dd_exit(-ENOSPC);
			return;
		}

		if (g_opts.io_unit_count != 0 && write_size + g_job.output.pos > g_job.output.total_size) {
			SPDK_ERRLOG("--count value too big (%" PRIu64 ") - only %" PRIu64 " blocks available in output\n",
				    g_opts.io_unit_count, (g_job.output.total_size - g_job.output.pos) / g_opts.io_unit_size);
			dd_exit(-ENOSPC);
			return;
		}
	}

	if ((g_job.output.block_size > g_opts.io_unit_size) ||
	    (g_job.input.block_size > g_opts.io_unit_size)) {
		SPDK_ERRLOG("--bs value cannot be less than input (%d) neither output (%d) native block size\n",
			    g_job.input.block_size, g_job.output.block_size);
		dd_exit(-EINVAL);
		return;
	}

	if (g_opts.input_bdev && g_opts.io_unit_size % g_job.input.block_size != 0) {
		SPDK_ERRLOG("--bs value must be a multiple of input native block size (%d)\n",
			    g_job.input.block_size);
		dd_exit(-EINVAL);
		return;
	}

	g_job.ios = calloc(g_opts.queue_depth, sizeof(struct dd_io));
	if (g_job.ios == NULL) {
		SPDK_ERRLOG("%s\n", strerror(ENOMEM));
		dd_exit(-ENOMEM);
		return;
	}

	for (i = 0; i < g_opts.queue_depth; i++) {
		g_job.ios[i].buf = spdk_malloc(g_opts.io_unit_size, 0x1000, NULL, 0, SPDK_MALLOC_DMA);
		if (g_job.ios[i].buf == NULL) {
			SPDK_ERRLOG("%s - try smaller block size value\n", strerror(ENOMEM));
			dd_exit(-ENOMEM);
			return;
		}
		g_job.ios[i].length = (uint64_t)g_opts.io_unit_size;
	}

	if (g_opts.input_file || g_opts.output_file) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			unsigned int io_uring_flags = IORING_SETUP_SQPOLL;
			int flags = parse_flags(g_opts.input_file_flags) & parse_flags(g_opts.output_file_flags);

			if ((flags & O_DIRECT) != 0 &&
			    dd_is_blk(g_job.input.u.uring.fd) &&
			    dd_is_blk(g_job.output.u.uring.fd)) {
				io_uring_flags = IORING_SETUP_IOPOLL;
			}

			g_job.u.uring.poller = SPDK_POLLER_REGISTER(dd_uring_poll, NULL, 0);
			rc = io_uring_queue_init(g_opts.queue_depth * 2, &g_job.u.uring.ring, io_uring_flags);
			if (rc) {
				SPDK_ERRLOG("Failed to create io_uring: %d (%s)\n", rc, spdk_strerror(-rc));
				dd_exit(rc);
				return;
			}
			g_job.u.uring.active = true;

			/* Register the files */
			rc = dd_register_files();
			if (rc) {
				SPDK_ERRLOG("Failed to register files with io_uring: %d (%s)\n", rc, spdk_strerror(-rc));
				dd_exit(rc);
				return;
			}

			/* Register the buffers */
			rc = dd_register_buffers();
			if (rc) {
				SPDK_ERRLOG("Failed to register buffers with io_uring: %d (%s)\n", rc, spdk_strerror(-rc));
				dd_exit(rc);
				return;
			}

		} else
#endif
		{
			g_job.u.aio.poller = SPDK_POLLER_REGISTER(dd_aio_poll, NULL, 0);
			io_setup(g_opts.queue_depth, &g_job.u.aio.io_ctx);
		}
	}

	clock_gettime(CLOCK_REALTIME, &g_job.start_time);

	g_job.status_poller = SPDK_POLLER_REGISTER(dd_status_poller, NULL,
			      STATUS_POLLER_PERIOD_SEC * SPDK_SEC_TO_USEC);

	STAILQ_INIT(&g_job.seek_queue);

	for (i = 0; i < g_opts.queue_depth; i++) {
		dd_target_seek(&g_job.ios[i]);
	}

}

enum dd_cmdline_opts {
	DD_OPTION_IF = 0x1000,
	DD_OPTION_OF,
	DD_OPTION_IFLAGS,
	DD_OPTION_OFLAGS,
	DD_OPTION_IB,
	DD_OPTION_OB,
	DD_OPTION_SKIP,
	DD_OPTION_SEEK,
	DD_OPTION_BS,
	DD_OPTION_QD,
	DD_OPTION_COUNT,
	DD_OPTION_AIO,
	DD_OPTION_SPARSE,
};

static struct option g_cmdline_opts[] = {
	{
		.name = "if",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IF,
	},
	{
		.name = "of",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OF,
	},
	{
		.name = "iflag",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IFLAGS,
	},
	{
		.name = "oflag",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OFLAGS,
	},
	{
		.name = "ib",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IB,
	},
	{
		.name = "ob",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OB,
	},
	{
		.name = "skip",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_SKIP,
	},
	{
		.name = "seek",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_SEEK,
	},
	{
		.name = "bs",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_BS,
	},
	{
		.name = "qd",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_QD,
	},
	{
		.name = "count",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_COUNT,
	},
	{
		.name = "aio",
		.has_arg = 0,
		.flag = NULL,
		.val = DD_OPTION_AIO,
	},
	{
		.name = "sparse",
		.has_arg = 0,
		.flag = NULL,
		.val = DD_OPTION_SPARSE,
	},
	{
		.name = NULL
	}
};

static void
usage(void)
{
	printf("[--------- DD Options ---------]\n");
	printf(" --if Input file. Must specify either --if or --ib.\n");
	printf(" --ib Input bdev. Must specifier either --if or --ib\n");
	printf(" --of Output file. Must specify either --of or --ob.\n");
	printf(" --ob Output bdev. Must specify either --of or --ob.\n");
	printf(" --iflag Input file flags.\n");
	printf(" --oflag Output file flags.\n");
	printf(" --bs I/O unit size (default: %" PRId64 ")\n", g_opts.io_unit_size);
	printf(" --qd Queue depth (default: %d)\n", g_opts.queue_depth);
	printf(" --count I/O unit count. The number of I/O units to copy. (default: all)\n");
	printf(" --skip Skip this many I/O units at start of input. (default: 0)\n");
	printf(" --seek Skip this many I/O units at start of output. (default: 0)\n");
	printf(" --aio Force usage of AIO. (by default io_uring is used if available)\n");
	printf(" --sparse Enable hole skipping in input target\n");
	printf(" Available iflag and oflag values:\n");
	printf("  append - append mode\n");
	printf("  direct - use direct I/O for data\n");
	printf("  directory - fail unless a directory\n");
	printf("  dsync - use synchronized I/O for data\n");
	printf("  noatime - do not update access time\n");
	printf("  noctty - do not assign controlling terminal from file\n");
	printf("  nofollow - do not follow symlinks\n");
	printf("  nonblock - use non-blocking I/O\n");
	printf("  sync - use synchronized I/O for data and metadata\n");
}

static int
parse_args(int argc, char *argv)
{
	switch (argc) {
	case DD_OPTION_IF:
		g_opts.input_file = strdup(argv);
		break;
	case DD_OPTION_OF:
		g_opts.output_file = strdup(argv);
		break;
	case DD_OPTION_IFLAGS:
		g_opts.input_file_flags = strdup(argv);
		break;
	case DD_OPTION_OFLAGS:
		g_opts.output_file_flags = strdup(argv);
		break;
	case DD_OPTION_IB:
		g_opts.input_bdev = strdup(argv);
		break;
	case DD_OPTION_OB:
		g_opts.output_bdev = strdup(argv);
		break;
	case DD_OPTION_SKIP:
		g_opts.input_offset = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_SEEK:
		g_opts.output_offset = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_BS:
		g_opts.io_unit_size = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_QD:
		g_opts.queue_depth = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_COUNT:
		g_opts.io_unit_count = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_AIO:
		g_opts.aio = true;
		break;
	case DD_OPTION_SPARSE:
		g_opts.sparse = true;
		break;
	default:
		usage();
		return 1;
	}
	return 0;
}

static void
dd_free(void)
{
	uint32_t i;

	free(g_opts.input_file);
	free(g_opts.output_file);
	free(g_opts.input_bdev);
	free(g_opts.output_bdev);
	free(g_opts.input_file_flags);
	free(g_opts.output_file_flags);


	if (g_job.input.type == DD_TARGET_TYPE_FILE || g_job.output.type == DD_TARGET_TYPE_FILE) {
#ifdef SPDK_CONFIG_URING
		if (g_opts.aio == false) {
			if (g_job.u.uring.active) {
				io_uring_unregister_files(&g_job.u.uring.ring);
				io_uring_queue_exit(&g_job.u.uring.ring);
			}
		} else
#endif
		{
			io_destroy(g_job.u.aio.io_ctx);
		}
	}

	if (g_job.ios) {
		for (i = 0; i < g_opts.queue_depth; i++) {
			spdk_free(g_job.ios[i].buf);
		}

		free(g_job.ios);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 1;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_dd";
	opts.reactor_mask = "0x1";
	opts.shutdown_cb = dd_finish;
	rc = spdk_app_parse_args(argc, argv, &opts, "", g_cmdline_opts, parse_args, usage);
	if (rc == SPDK_APP_PARSE_ARGS_FAIL) {
		SPDK_ERRLOG("Invalid arguments\n");
		goto end;
	} else if (rc == SPDK_APP_PARSE_ARGS_HELP) {
		goto end;
	}

	if (g_opts.input_file != NULL && g_opts.input_bdev != NULL) {
		SPDK_ERRLOG("You may specify either --if or --ib, but not both.\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file != NULL && g_opts.output_bdev != NULL) {
		SPDK_ERRLOG("You may specify either --of or --ob, but not both.\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.input_file == NULL && g_opts.input_bdev == NULL) {
		SPDK_ERRLOG("You must specify either --if or --ib\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file == NULL && g_opts.output_bdev == NULL) {
		SPDK_ERRLOG("You must specify either --of or --ob\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.io_unit_size <= 0) {
		SPDK_ERRLOG("Invalid --bs value\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.io_unit_count < 0) {
		SPDK_ERRLOG("Invalid --count value\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file == NULL && g_opts.output_file_flags != NULL) {
		SPDK_ERRLOG("--oflags may be used only with --of\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.input_file == NULL && g_opts.input_file_flags != NULL) {
		SPDK_ERRLOG("--iflags may be used only with --if\n");
		rc = EINVAL;
		goto end;
	}

	rc = spdk_app_start(&opts, dd_run, NULL);
	if (rc) {
		SPDK_ERRLOG("Error occurred while performing copy\n");
	}

	dd_free();
	spdk_app_fini();

end:
	return rc;
}
