/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_aio.h"

#include "spdk/stdinc.h"

#include "spdk/barrier.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/bit_array.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk/log.h"

#include <sys/eventfd.h>

#ifndef __FreeBSD__
#include <libaio.h>
#endif

#define MAX_PATH_FILE 100

// enum spdk_task_state {
// 	/* The blob in-memory version does not match the on-disk
// 	 * version.
// 	 */
// 	SPDK_FILE_ONE_,

// 	/* The blob in memory version of the blob matches the on disk
// 	 * version.
// 	 */
// 	SPDK_FILE_STATE_OPENED,

// 	/* The in-memory state being synchronized with the on-disk
// 	 * blob state. */
// 	SPDK_FILE_STATE_CLOSED,
	
// 	SPDK_FILE_STATE_DELETED,
// };








struct _file_md {
	int fd;
	char filename[MAX_PATH_FILE];
	uint64_t start_offset;
	uint64_t end_offset;
	enum spdk_file_state state;
	struct spdk_bit_array *used_blocks;
	// bs->used_md_pages = spdk_bit_array_create(1);
	// spdk_bit_array_get(blob->bs->used_md_pages, pages[i - 1].next)
};


struct bdev_aio_io_channel {
	uint64_t				io_inflight;
#ifdef __FreeBSD__
	int					kqfd;
#else
	io_context_t				io_ctx;
#endif
	struct bdev_aio_group_channel		*group_ch;
	TAILQ_ENTRY(bdev_aio_io_channel)	link;
};

struct bdev_aio_group_channel {
	/* eventfd for io completion notification in interrupt mode.
	 * Negative value like '-1' indicates it is invalid or unused.
	 */
	int					efd;
	struct spdk_interrupt			*intr;
	struct spdk_poller			*poller;
	TAILQ_HEAD(, bdev_aio_io_channel)	io_ch_head;
};

struct bdev_aio_task {
#ifdef __FreeBSD__
	struct aiocb			aiocb;
#else
	struct iocb			iocb[2]; //we need two iocb for splited IO
#endif
	uint32_t			idx_iovcnt;
	enum spdk_iov_state	mode;
	struct iovec 		*iov;
	void 				*iovbase_hotspot;
	uint32_t			iovlen_hotspot;	
	uint64_t			len;
	uint64_t			first_len;
	bool				first_part_done;
	uint64_t			second_len;
	bool				second_part_done;
	bool		 		splite_io;
	int					first_fid;
	int					second_fid;
	bool				response_sent;
	bool				write_zero;
	struct bdev_aio_io_channel	*ch;
};

struct file_disk {
	struct bdev_aio_task	*reset_task;
	struct spdk_poller	*reset_retry_timer;
	struct spdk_bdev	disk;
	char			*filename;
	int			fd;
	uint32_t    filecnt;
	uint64_t	size_per_file;
	bool		filled;
	struct _file_md *file_md_array;
	struct spdk_bit_array *used_file;
	struct spdk_spinlock 	used_lock;
	// void *zero_buf;
	TAILQ_ENTRY(file_disk)  link;
	bool			block_size_override;
	bool			readonly;
	bool			fallocate;
};

/* For user space reaping of completions */
struct spdk_aio_ring {
	uint32_t id;
	uint32_t size;
	uint32_t head;
	uint32_t tail;

	uint32_t version;
	uint32_t compat_features;
	uint32_t incompat_features;
	uint32_t header_length;
};

#define SPDK_AIO_RING_VERSION	0xa10a10a1

static int bdev_aio_initialize(void);
static void bdev_aio_fini(void);
static void aio_free_disk(struct file_disk *fdisk);
static TAILQ_HEAD(, file_disk) g_aio_disk_head = TAILQ_HEAD_INITIALIZER(g_aio_disk_head);

#define SPDK_AIO_QUEUE_DEPTH 128
#define MAX_EVENTS_PER_POLL 32


// static void
// fdisk_claim_file_bit(struct file_disk *fdisk, uint32_t idx)
// {
// 	assert(spdk_spin_held(&fdisk->used_lock));
// 	assert(idx < spdk_bit_array_capacity(fdisk->used_file));
// 	assert(spdk_bit_array_get(fdisk->used_file, idx) == false);

// 	spdk_bit_array_set(fdisk->used_file, idx);
// }


// static void
// fdisk_clear_file_bit(struct file_disk *fdisk, uint32_t idx)
// {
// 	assert(spdk_spin_held(&fdisk->used_lock));
// 	assert(idx < spdk_bit_array_capacity(fdisk->used_file));
// 	assert(spdk_bit_array_get(fdisk->used_file, idx) == true);

// 	spdk_bit_array_clear(fdisk->used_file, idx);
// }

static int
bdev_aio_get_ctx_size(void)
{
	return sizeof(struct bdev_aio_task);
}

static struct spdk_bdev_module aio_if = {
	.name		= "aio",
	.module_init	= bdev_aio_initialize,
	.module_fini	= bdev_aio_fini,
	.get_ctx_size	= bdev_aio_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(aio, &aio_if)

static int
bdev_aio_open(struct file_disk *disk)
{
	int fd;
	int io_flag = disk->readonly ? O_RDONLY : O_RDWR;

	fd = open(disk->filename, io_flag | O_DIRECT);
	if (fd < 0) {
		/* Try without O_DIRECT for non-disk files */
		fd = open(disk->filename, io_flag);
		if (fd < 0) {
			SPDK_ERRLOG("open() failed (file:%s), errno %d: %s\n",
				    disk->filename, errno, spdk_strerror(errno));
			disk->fd = -1;
			return -1;
		}
	}

	disk->fd = fd;

	return 0;
}

static int
bdev_aio_open_part_file(struct file_disk *disk, struct _file_md *file_md)
{
	int fd;
	int io_flag = disk->readonly ? O_RDONLY : O_RDWR;

	/* Try without O_DIRECT for non-disk files */
	fd = open(file_md->filename, io_flag | O_CREAT);
	if (fd < 0) {
		SPDK_ERRLOG("open() failed (file:%s), errno %d: %s\n",
				file_md->filename, errno, spdk_strerror(errno));
		disk->fd = -1;
		return -1;
	}

	if((file_md->state == SPDK_FILE_STATE_DELETED || file_md->state == SPDK_FILE_STATE_CLEAN)) {
		
		// Set the file size to 1 GB
		if (ftruncate(fd, disk->size_per_file) != 0) {
			perror("Failed to set file size");
			close(fd);
			return 1;
		}
		// Reset the file offset to the beginning
		if (lseek(fd, 0, SEEK_SET) == -1) {
			perror("Failed to reset file offset");
			close(fd);
			return 1;
		}
		
		if(disk->filled) {
			char buffer[1048576] = {0};  // 1 MB buffer filled with zeros
			for (uint64_t i = 0; i < disk->size_per_file; i += sizeof(buffer)) {
				if (write(fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
					perror("Failed to write zeros to file");
					close(fd);
					return 1;
				}
			}
		}

		fsync(fd);
	}

	file_md->fd = fd;
	return 0;
}



static int
bdev_aio_close(struct file_disk *disk)
{
	int rc;

	if (disk->fd == -1) {
		return 0;
	}

	rc = close(disk->fd);
	if (rc < 0) {
		SPDK_ERRLOG("close() failed (fd=%d), errno %d: %s\n",
			    disk->fd, errno, spdk_strerror(errno));
		return -1;
	}

	disk->fd = -1;

	return 0;
}

#ifdef __FreeBSD__
static int
bdev_aio_submit_io(enum spdk_bdev_io_type type, struct file_disk *fdisk,
		   struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct aiocb *aiocb = &aio_task->aiocb;
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);

	memset(aiocb, 0, sizeof(struct aiocb));
	aiocb->aio_fildes = fdisk->fd;
	aiocb->aio_iov = iov;
	aiocb->aio_iovcnt = iovcnt;
	aiocb->aio_offset = offset;
	aiocb->aio_sigevent.sigev_notify_kqueue = aio_ch->kqfd;
	aiocb->aio_sigevent.sigev_value.sival_ptr = aio_task;
	aiocb->aio_sigevent.sigev_notify = SIGEV_KEVENT;

	aio_task->len = nbytes;
	aio_task->ch = aio_ch;

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		return aio_readv(aiocb);
	}

	return aio_writev(aiocb);
}
#else
static int
bdev_aio_submit_io(enum spdk_bdev_io_type type, struct file_disk *fdisk,
		   struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb[0];
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		io_prep_preadv(iocb, fdisk->fd, iov, iovcnt, offset);
	} else {
		io_prep_pwritev(iocb, fdisk->fd, iov, iovcnt, offset);
	}

	if (aio_ch->group_ch->efd >= 0) {
		io_set_eventfd(iocb, aio_ch->group_ch->efd);
	}
	iocb->data = aio_task;
	aio_task->len = nbytes;
	aio_task->splite_io = false;
	aio_task->mode = SPDK_IOVS_ONE_FILE;
	aio_task->ch = aio_ch;

	return io_submit(aio_ch->io_ctx, 1, &iocb);
}


static int
bdev_aio_submit_io_multifile_one(enum spdk_bdev_io_type type, struct file_disk *fdisk,
		   struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);	
	struct iocb *iocb = &aio_task->iocb[0];
	uint32_t idx = offset / fdisk->size_per_file;
	// spdk_spin_lock(&fdisk->used_lock);
	// fdisk_claim_file(fdisk, idx);
	// spdk_spin_unlock(&fdisk->used_lock);
	if(fdisk->file_md_array[idx].state != SPDK_FILE_STATE_OPENED) {
		if (bdev_aio_open_part_file(fdisk, &fdisk->file_md_array[idx])) {
			SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", fdisk->file_md_array[idx].filename, fdisk->file_md_array[idx].fd, errno);
			return -1;
		}
		fdisk->file_md_array[idx].state = SPDK_FILE_STATE_OPENED;	
	}
	struct _file_md md = fdisk->file_md_array[idx];

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		io_prep_preadv(iocb, md.fd, iov, iovcnt, offset - md.start_offset);
	} else {
		io_prep_pwritev(iocb, md.fd, iov, iovcnt, offset - md.start_offset);
	}

	if (aio_ch->group_ch->efd >= 0) {
		io_set_eventfd(iocb, aio_ch->group_ch->efd);
	}
	iocb->data = aio_task;
	aio_task->len = nbytes;
	aio_task->mode = SPDK_IOVS_ONE_FILE;
	aio_task->first_len = 0;
	aio_task->second_len = 0;
	aio_task->splite_io = false;
	aio_task->ch = aio_ch;

	return io_submit(aio_ch->io_ctx, 1, &iocb);
}


static int
bdev_aio_submit_io_multifile_two(enum spdk_bdev_io_type type, struct file_disk *fdisk,
		   struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset, int index)
{
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);	
	uint32_t idx = offset / fdisk->size_per_file;			
	struct iocb *iocb = &aio_task->iocb[index];

	// spdk_spin_lock(&fdisk->used_lock);
	// fdisk_claim_file(fdisk, idx);
	// spdk_spin_unlock(&fdisk->used_lock);
	if(fdisk->file_md_array[idx].state != SPDK_FILE_STATE_OPENED) {
		if (bdev_aio_open_part_file(fdisk, &fdisk->file_md_array[idx])) {
			SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", fdisk->file_md_array[idx].filename, fdisk->file_md_array[idx].fd, errno);
			return -1;
		}
		fdisk->file_md_array[idx].state = SPDK_FILE_STATE_OPENED;	
	}
	struct _file_md md = fdisk->file_md_array[idx];

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		io_prep_preadv(iocb, md.fd, iov, iovcnt, offset - md.start_offset);
	} else {
		io_prep_pwritev(iocb, md.fd, iov, iovcnt, offset - md.start_offset);
	}

	if (aio_ch->group_ch->efd >= 0) {
		io_set_eventfd(iocb, aio_ch->group_ch->efd);
	}
		
	iocb->data = aio_task;
	aio_task->ch = aio_ch;

	return io_submit(aio_ch->io_ctx, 1, &iocb);
}

#endif
static int
bdev_aio_rw_split(enum spdk_bdev_io_type type, struct file_disk *fdisk,
	    struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
	    struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct iovec *tmp_iov;
	uint32_t idx = offset / fdisk->size_per_file;
	uint32_t first_len = fdisk->file_md_array[idx].end_offset - offset;
	uint32_t second_len = nbytes - first_len;
	uint32_t tmp_len = 0, inside_vec_fp = 0, inside_vec_sp = 0;
	uint8_t mode = 0;
	uint32_t idx_iovcnt = 0;
	int rc = 0;
	aio_task->splite_io = true;
	aio_task->response_sent = false;
	aio_task->first_len = first_len;
	aio_task->second_len = second_len;
	aio_task->first_part_done = false;
	aio_task->second_part_done = false;
	aio_task->iov = iov;
	aio_task->first_fid = fdisk->file_md_array[idx].fd;
	aio_task->second_fid = fdisk->file_md_array[idx + 1].fd;


	if(iovcnt == 1) {
		mode = SPDK_IOV_SINGLE_IOVCNT;
		iov[0].iov_len = first_len;
		//sent the fisrt IO
		aio_task->mode = mode;
		aio_task->iovbase_hotspot = iov[0].iov_base;
		rc = bdev_aio_submit_io_multifile_two(type, fdisk, ch, aio_task, iov, iovcnt, first_len, offset, 0);
		if (spdk_unlikely(rc < 0)) {
			return rc;
		}

		iov[0].iov_base = iov[0].iov_base + first_len;
		iov[0].iov_len = second_len;
		//send the second IO
		rc = bdev_aio_submit_io_multifile_two(type, fdisk, ch, aio_task, iov, iovcnt, second_len, offset + first_len, 1);
		if (spdk_unlikely(rc < 0)) {
			return rc;
		}
		return rc;
		
	} else {
		for (int i = 0; i < iovcnt; i++) {
			tmp_len += iov[i].iov_len;
			if(tmp_len == first_len) {
				idx_iovcnt = i;
				mode = SPDK_IOVS_SPLIT_IOVCNT;				
				break;
			}

			if(tmp_len > first_len) {
				idx_iovcnt = i;
				inside_vec_fp = iov[i].iov_len - (tmp_len - first_len);
				inside_vec_sp = (iov[i].iov_len - inside_vec_fp);
				mode = SPDK_IOVS_SPLIT_IOV;
				break;
			}
		}

		aio_task->mode = mode;
		if(mode == SPDK_IOVS_SPLIT_IOVCNT) {						
			//sent the first IO
			rc = bdev_aio_submit_io_multifile_two(type, fdisk, ch, aio_task, iov, idx_iovcnt, first_len, offset, 0);
			if (spdk_unlikely(rc < 0)) {
				return rc;
			}

			struct iovec *tmp_iov = &iov[idx_iovcnt + 1];
			iovcnt = iovcnt - idx_iovcnt;	
			//send the second IO
			rc = bdev_aio_submit_io_multifile_two(type, fdisk, ch, aio_task, tmp_iov, iovcnt, second_len, offset + first_len, 1);
			if (spdk_unlikely(rc < 0)) {
				return rc;
			}
			return rc;
		} else if(mode == SPDK_IOVS_SPLIT_IOV) {
			aio_task->idx_iovcnt = idx_iovcnt;
			aio_task->iovlen_hotspot = iov[idx_iovcnt].iov_len;			
			aio_task->iovbase_hotspot = iov[idx_iovcnt].iov_base;
			iov[idx_iovcnt].iov_len = inside_vec_fp;
			//sent the first IO
			rc = bdev_aio_submit_io_multifile_two(type, fdisk, ch, aio_task, iov, idx_iovcnt, first_len, offset, 0);
			if (spdk_unlikely(rc < 0)) {
				return rc;
			}
			iov[idx_iovcnt].iov_base = iov[idx_iovcnt].iov_base + inside_vec_fp;
			iov[idx_iovcnt].iov_len = inside_vec_sp;
			tmp_iov = &iov[idx_iovcnt];
			iovcnt = iovcnt - idx_iovcnt + 1;
			//send the second IO
			rc = bdev_aio_submit_io_multifile_two(type, fdisk, ch, aio_task, tmp_iov, iovcnt, second_len, offset + first_len, 1);
			if (spdk_unlikely(rc < 0)) {
				return rc;
			}
			return rc;
		}
	}
	return -1;

}


static void
bdev_aio_rw(enum spdk_bdev_io_type type, struct file_disk *fdisk,
	    struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
	    struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	bool split_io = false;
	int rc;
	// TODO: Implement function to check if the file is opened or exist
	// TODO: Implement function for claim used blocks in files with bit array
	if (type == SPDK_BDEV_IO_TYPE_READ) {
		SPDK_NOTICELOG("AIO_BDEV: read %d iovs size %lu to off: %#lx\n",
			      iovcnt, nbytes, offset);
	} else {
		SPDK_NOTICELOG("AIO_BDEV: write %d iovs size %lu from off: %#lx\n",
			      iovcnt, nbytes, offset);
	}   

	if(fdisk->filecnt <= 1) {
		rc = bdev_aio_submit_io(type, fdisk, ch, aio_task, iov, iovcnt, nbytes, offset);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EAGAIN) {
				spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
			} else {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), rc);
				SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
			}
		} else {
			aio_ch->io_inflight++;
		}
		return;
	}

	uint64_t idx = offset / fdisk->size_per_file;
	if(fdisk->file_md_array[idx].end_offset < (offset + nbytes)) {
		split_io = true;
	}

	if(split_io) {
		rc = bdev_aio_rw_split(type, fdisk, ch, aio_task, iov, iovcnt, nbytes, offset);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EAGAIN) {
				spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
			} else {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), rc);
				SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
			}
		} else {
			aio_ch->io_inflight+=2;
		}
		return;
	} else {
		rc = bdev_aio_submit_io_multifile_one(type, fdisk, ch, aio_task, iov, iovcnt, nbytes, offset);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EAGAIN) {
				spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
			} else {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), rc);
				SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
			}
		} else {
			aio_ch->io_inflight++;
		}
		return;
	}

}

static void
bdev_aio_flush(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{	
	int rc = 0;
	if(fdisk->filecnt == 1) {		
		rc = fsync(fdisk->fd);
	} else {
		for(uint32_t i = 0; i < fdisk->filecnt; i++) {
			if(fdisk->file_md_array[i].state == SPDK_FILE_STATE_OPENED) {
				rc = fsync(fdisk->file_md_array[i].fd);
				if(rc < 0) break;
			}
		}
	}

	if (rc == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
	}
}

static int delete_file(struct _file_md *md) {
	if(md->state == SPDK_FILE_STATE_OPENED) {
		close(md->fd);		
		// Unlink the file
		if (unlink(md->filename) == 0) {
			printf("File '%s' deleted successfully.\n", md->filename);
		} else {
			perror("Failed to delete the file");
		}
		md->state = SPDK_FILE_STATE_DELETED;
		// clear this bit array
		spdk_bit_array_clear_mask(md->used_blocks);
	}
    return 0;
}

#ifndef __FreeBSD__

static void
bdev_aio_unmap_multi_file_mode(struct spdk_bdev_io *bdev_io, int mode)
{
	struct file_disk *fdisk = (struct file_disk *)bdev_io->bdev->ctxt;
	struct bdev_aio_task *aio_task = (struct bdev_aio_task *)bdev_io->driver_ctx;
	uint64_t offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	uint64_t nbytes = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	uint64_t idx = offset / fdisk->size_per_file;
	struct _file_md md = fdisk->file_md_array[idx];
	bool split_io = false;
	int filecnt = 0;
	uint64_t diff = nbytes > (md.end_offset - offset) ? nbytes - (md.end_offset - offset) : (md.end_offset - offset) - nbytes;
	int rc;

	SPDK_NOTICELOG("AIO_BDEV: Unmap or zero wirte %lu iovs size %lu to off: %#lx\n",
			      bdev_io->u.bdev.num_blocks, nbytes, offset);
	
	if(md.end_offset < (offset + nbytes)) {
		split_io = true;
	}

	if(!split_io) {
		// aio_task->write_zero = true;;
		// one file to be deleted 
		if(md.start_offset == offset && nbytes == fdisk->size_per_file) {
			delete_file(&fdisk->file_md_array[idx]);
		} else {			
			// send the write zero or unmap as ordinary write IO
			if(fdisk->file_md_array[idx].state != SPDK_FILE_STATE_OPENED) {
				if (bdev_aio_open_part_file(fdisk, &fdisk->file_md_array[idx])) {
					SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", fdisk->file_md_array[idx].filename, fdisk->file_md_array[idx].fd, errno);
					return -1;
				}
				fdisk->file_md_array[idx].state = SPDK_FILE_STATE_OPENED;	
			}
			md = fdisk->file_md_array[idx];
			rc = fallocate(md.fd, mode, (offset - md.start_offset), nbytes);
			if(rc == 0) {
				fsync(md.fd);
			} else {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
				return;
			}	
		}
	} else {
		filecnt =  diff / fdisk->size_per_file;
		bool last_part = diff % fdisk->size_per_file ? true : false;
		bool first_part = md.start_offset == offset ? false : true;

		if(!first_part) {
			delete_file(&fdisk->file_md_array[idx]);
		}

		for(int i = 1; i <= filecnt; i++) {	
			delete_file(&fdisk->file_md_array[idx + i]);
		}

		if(first_part) {			
			// take care of first part in first file

			if(fdisk->file_md_array[idx].state != SPDK_FILE_STATE_OPENED) {
				if (bdev_aio_open_part_file(fdisk, &fdisk->file_md_array[idx])) {
					SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", fdisk->file_md_array[idx].filename, fdisk->file_md_array[idx].fd, errno);
					return -1;
				}
				fdisk->file_md_array[idx].state = SPDK_FILE_STATE_OPENED;	
			}
			md = fdisk->file_md_array[idx];

			rc = fallocate(md.fd, mode, (offset - md.start_offset), (md.end_offset - offset));
			// rc = write_zeros(fdisk, md.fd, (offset - md.start_offset), (md.end_offset - offset));
			if(rc == 0) {
				fsync(md.fd);
			} else {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
				return;
			}
		}

		if(last_part) {

			// take care of last part in last file
			struct _file_md *tmp_md = &fdisk->file_md_array[idx + filecnt + 1];			
			if(tmp_md->state != SPDK_FILE_STATE_OPENED) {
				if (bdev_aio_open_part_file(fdisk, tmp_md)) {
					SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", tmp_md->filename, tmp_md->fd, errno);
					return -1;
				}
				tmp_md->state = SPDK_FILE_STATE_OPENED;	
			}
			md = fdisk->file_md_array[idx + filecnt + 1];		
			uint64_t remain_byte = (nbytes - (fdisk->file_md_array[idx].end_offset - offset)) % fdisk->size_per_file;
			rc = fallocate(md.fd, mode, 0, remain_byte);
			// rc = write_zeros(fdisk, md.fd, 0, remain_byte);
			if(rc == 0) {
				fsync(md.fd);
			} else {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
				return;
			}
		}
	}

	if (rc == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
	}
}

static void
bdev_aio_fallocate(struct spdk_bdev_io *bdev_io, int mode)
{
	struct file_disk *fdisk = (struct file_disk *)bdev_io->bdev->ctxt;
	struct bdev_aio_task *aio_task = (struct bdev_aio_task *)bdev_io->driver_ctx;
	uint64_t offset_bytes = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	uint64_t length_bytes = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	int rc;

	if (!fdisk->fallocate) {
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -ENOTSUP);
		return;
	}

	rc = fallocate(fdisk->fd, mode, offset_bytes, length_bytes);
	if (rc == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
	}
}

static void
bdev_aio_unmap(struct spdk_bdev_io *bdev_io)
{
	int mode = FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE;
	struct file_disk *fdisk = (struct file_disk *)bdev_io->bdev->ctxt;
	if(fdisk->filecnt > 1) {	
		bdev_aio_unmap_multi_file_mode(bdev_io, mode);
	} else {
		bdev_aio_fallocate(bdev_io, mode);
	}
}


static void
bdev_aio_write_zeros(struct spdk_bdev_io *bdev_io)
{
	int mode = FALLOC_FL_ZERO_RANGE;
	struct file_disk *fdisk = (struct file_disk *)bdev_io->bdev->ctxt;
	if(fdisk->filecnt > 1) {	
		bdev_aio_unmap_multi_file_mode(bdev_io, mode);
	} else {
		bdev_aio_fallocate(bdev_io, mode);
	}
}
#endif

static void
bdev_aio_destruct_cb(void *io_device)
{
	struct file_disk *fdisk = io_device;
	int rc = 0;

	TAILQ_REMOVE(&g_aio_disk_head, fdisk, link);
	rc = bdev_aio_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("bdev_aio_close() failed\n");
	}
	aio_free_disk(fdisk);
}

static int
bdev_aio_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;

	spdk_io_device_unregister(fdisk, bdev_aio_destruct_cb);

	return 0;
}

#ifdef __FreeBSD__
static int
bdev_user_io_getevents(int kq, unsigned int max, struct kevent *events)
{
	struct timespec ts;
	int count;

	memset(events, 0, max * sizeof(struct kevent));
	memset(&ts, 0, sizeof(ts));

	count = kevent(kq, NULL, 0, events, max, &ts);
	if (count < 0) {
		SPDK_ERRLOG("failed to get kevents: %s.\n", spdk_strerror(errno));
		return -errno;
	}

	return count;
}

static int
bdev_aio_io_channel_poll(struct bdev_aio_io_channel *io_ch)
{
	int nr, i, res = 0;
	struct bdev_aio_task *aio_task;
	struct kevent events[SPDK_AIO_QUEUE_DEPTH];

	nr = bdev_user_io_getevents(io_ch->kqfd, SPDK_AIO_QUEUE_DEPTH, events);
	if (nr < 0) {
		return 0;
	}

	for (i = 0; i < nr; i++) {
		aio_task = events[i].udata;
		aio_task->ch->io_inflight--;
		if (aio_task == NULL) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
			break;
		} else if ((uint64_t)aio_return(&aio_task->aiocb) == aio_task->len) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			SPDK_ERRLOG("failed to complete aio: rc %d\n", aio_error(&aio_task->aiocb));
			res = aio_error(&aio_task->aiocb);
			if (res != 0) {
				spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), res);
			} else {
				spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
			}
		}
	}

	return nr;
}
#else
static int
bdev_user_io_getevents(io_context_t io_ctx, unsigned int max, struct io_event *uevents)
{
	uint32_t head, tail, count;
	struct spdk_aio_ring *ring;
	struct timespec timeout;
	struct io_event *kevents;

	ring = (struct spdk_aio_ring *)io_ctx;

	if (spdk_unlikely(ring->version != SPDK_AIO_RING_VERSION || ring->incompat_features != 0)) {
		timeout.tv_sec = 0;
		timeout.tv_nsec = 0;

		return io_getevents(io_ctx, 0, max, uevents, &timeout);
	}

	/* Read the current state out of the ring */
	head = ring->head;
	tail = ring->tail;

	/* This memory barrier is required to prevent the loads above
	 * from being re-ordered with stores to the events array
	 * potentially occurring on other threads. */
	spdk_smp_rmb();

	/* Calculate how many items are in the circular ring */
	count = tail - head;
	if (tail < head) {
		count += ring->size;
	}

	/* Reduce the count to the limit provided by the user */
	count = spdk_min(max, count);

	/* Grab the memory location of the event array */
	kevents = (struct io_event *)((uintptr_t)ring + ring->header_length);

	/* Copy the events out of the ring. */
	if ((head + count) <= ring->size) {
		/* Only one copy is required */
		memcpy(uevents, &kevents[head], count * sizeof(struct io_event));
	} else {
		uint32_t first_part = ring->size - head;
		/* Two copies are required */
		memcpy(uevents, &kevents[head], first_part * sizeof(struct io_event));
		memcpy(&uevents[first_part], &kevents[0], (count - first_part) * sizeof(struct io_event));
	}

	/* Update the head pointer. On x86, stores will not be reordered with older loads,
	 * so the copies out of the event array will always be complete prior to this
	 * update becoming visible. On other architectures this is not guaranteed, so
	 * add a barrier. */
#if defined(__i386__) || defined(__x86_64__)
	spdk_compiler_barrier();
#else
	spdk_smp_mb();
#endif
	ring->head = (head + count) % ring->size;

	return count;
}

static int
reasmble_io(struct bdev_aio_task *aio_task)
{	
	struct iovec *tmp_iov = aio_task->iov;	

	switch (aio_task->mode){
		case SPDK_IOVS_ONE_FILE:
			return 0;
		case SPDK_IOV_SINGLE_IOVCNT:			
			tmp_iov[0].iov_base = aio_task->iovbase_hotspot;
			tmp_iov[0].iov_len = aio_task->first_len + aio_task->second_len;
			break;
		case SPDK_IOVS_SPLIT_IOVCNT:			
			break;		
		case SPDK_IOVS_SPLIT_IOV:
			tmp_iov[aio_task->idx_iovcnt].iov_base = aio_task->iovbase_hotspot;
			tmp_iov[aio_task->idx_iovcnt].iov_len = aio_task->iovlen_hotspot;
			break;
	}
	return 0;
}

static void
reset_io_task(struct bdev_aio_task *aio_task)
{	
	aio_task->splite_io = false;
	aio_task->first_fid = 0;
	aio_task->second_fid = 0;
	aio_task->first_part_done = false;
	aio_task->second_part_done = false;
	aio_task->first_len = 0;
	aio_task->second_len = 0;	
}


static void
split_io_task_handler_first_part(struct bdev_aio_task *aio_task, struct io_event events, uint8_t idx)
{	
	if(events.res == aio_task->first_len) {
		if(aio_task->second_part_done) {
			aio_task->first_part_done = true;
			reasmble_io(aio_task);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
			reset_io_task(aio_task);
		}
		else if(!aio_task->second_part_done) {						
			aio_task->first_part_done = true;
		}
		if(aio_task->iocb[idx].aio_lio_opcode == IO_CMD_PWRITEV || aio_task->iocb[idx].aio_lio_opcode == IO_CMD_PWRITE){
			fsync(aio_task->iocb[idx].aio_fildes);
		}
		return;
	} else {
		if(aio_task->response_sent) {
				reset_io_task(aio_task);
		}
		else {
			SPDK_ERRLOG("failed to complete aio: rc %"PRId64"\n", events.res);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);			
			aio_task->response_sent = true;
			if(aio_task->second_part_done) {
				reset_io_task(aio_task);
			}
		}		
	}	
}


static void
split_io_task_handler_second_part(struct bdev_aio_task *aio_task, struct io_event events, uint8_t idx)
{
	if(events.res == aio_task->second_len) {
		if(aio_task->first_part_done) {
			aio_task->second_part_done = true;
			reasmble_io(aio_task);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
			reset_io_task(aio_task);
		}
		else if(!aio_task->first_part_done) {						
			aio_task->second_part_done = true;
		}
		if(aio_task->iocb[idx].aio_lio_opcode == IO_CMD_PWRITEV || aio_task->iocb[idx].aio_lio_opcode == IO_CMD_PWRITE) {
			fsync(aio_task->iocb[idx].aio_fildes);
		}
		return;
	} else {
		if(aio_task->response_sent) {
				reset_io_task(aio_task);
		}
		else {
			SPDK_ERRLOG("failed to complete aio: rc %"PRId64"\n", events.res);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);			
			aio_task->response_sent = true;
			if(aio_task->first_part_done) {
				reset_io_task(aio_task);
			}
		}
	}
}


static int
bdev_aio_io_channel_poll(struct bdev_aio_io_channel *io_ch)
{
	int nr, i, res = 0;
	struct bdev_aio_task *aio_task;
	struct io_event events[SPDK_AIO_QUEUE_DEPTH];

	nr = bdev_user_io_getevents(io_ch->io_ctx, SPDK_AIO_QUEUE_DEPTH, events);
	if (nr < 0) {
		return 0;
	}
	// TODO: Implement function for check split IO and refactor this area
	for (i = 0; i < nr; i++) {
		aio_task = events[i].data;
		aio_task->ch->io_inflight--;
		if(aio_task->splite_io) {
			if(aio_task->first_fid == events[i].obj->aio_fildes) {
				split_io_task_handler_first_part(aio_task, events[i], 0);
				continue;
			} else if(aio_task->second_fid == events[i].obj->aio_fildes) {
				split_io_task_handler_second_part(aio_task, events[i], 1);
				continue;
			}
		} else {
			if (events[i].res == aio_task->len) {
				if(aio_task->iocb[0].aio_lio_opcode == IO_CMD_PWRITEV || aio_task->iocb[0].aio_lio_opcode == IO_CMD_PWRITE) {
					fsync(aio_task->iocb[0].aio_fildes);
				}
				spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
			} else {
				/* From aio_abi.h, io_event.res is defined __s64, negative errno
				* will be assigned to io_event.res for error situation.
				* But from libaio.h, io_event.res is defined unsigned long, so
				* convert it to signed value for error detection.
				*/
				SPDK_ERRLOG("failed to complete aio: rc %"PRId64"\n", events[i].res);
				res = (int)events[i].res;
				if (res < 0) {
					spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), res);
				} else {
					spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
				}
			}
		}
	}

	return nr;
}
#endif

static int
bdev_aio_group_poll(void *arg)
{
	struct bdev_aio_group_channel *group_ch = arg;
	struct bdev_aio_io_channel *io_ch;
	int nr = 0;

	TAILQ_FOREACH(io_ch, &group_ch->io_ch_head, link) {
		nr += bdev_aio_io_channel_poll(io_ch);
	}

	return nr > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
bdev_aio_group_interrupt(void *arg)
{
	struct bdev_aio_group_channel *group_ch = arg;
	int rc;
	uint64_t num_events;

	assert(group_ch->efd >= 0);

	/* if completed IO number is larger than SPDK_AIO_QUEUE_DEPTH,
	 * io_getevent should be called again to ensure all completed IO are processed.
	 */
	rc = read(group_ch->efd, &num_events, sizeof(num_events));
	if (rc < 0) {
		SPDK_ERRLOG("failed to acknowledge aio group: %s.\n", spdk_strerror(errno));
		return -errno;
	}

	if (num_events > SPDK_AIO_QUEUE_DEPTH) {
		num_events -= SPDK_AIO_QUEUE_DEPTH;
		rc = write(group_ch->efd, &num_events, sizeof(num_events));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify aio group: %s.\n", spdk_strerror(errno));
		}
	}

	return bdev_aio_group_poll(group_ch);
}

static void
_bdev_aio_get_io_inflight(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);

	if (aio_ch->io_inflight) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static int bdev_aio_reset_retry_timer(void *arg);

static void
_bdev_aio_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct file_disk *fdisk = spdk_io_channel_iter_get_ctx(i);

	if (status == -1) {
		fdisk->reset_retry_timer = SPDK_POLLER_REGISTER(bdev_aio_reset_retry_timer, fdisk, 500);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(fdisk->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static int
bdev_aio_reset_retry_timer(void *arg)
{
	struct file_disk *fdisk = arg;

	if (fdisk->reset_retry_timer) {
		spdk_poller_unregister(&fdisk->reset_retry_timer);
	}

	spdk_for_each_channel(fdisk,
			      _bdev_aio_get_io_inflight,
			      fdisk,
			      _bdev_aio_get_io_inflight_done);

	return SPDK_POLLER_BUSY;
}

static void
bdev_aio_reset(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{
	fdisk->reset_task = aio_task;

	bdev_aio_reset_retry_timer(fdisk);
}

static void
bdev_aio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_aio_rw(bdev_io->type,
			    (struct file_disk *)bdev_io->bdev->ctxt,
			    ch,
			    (struct bdev_aio_task *)bdev_io->driver_ctx,
			    bdev_io->u.bdev.iovs,
			    bdev_io->u.bdev.iovcnt,
			    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			    bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		SPDK_ERRLOG("Wrong io type\n");
		break;
	}
}

static int
_bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct file_disk *fdisk = (struct file_disk *)bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_aio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (fdisk->readonly) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		} else {
			spdk_bdev_io_get_buf(bdev_io, bdev_aio_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		}
		return 0;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_aio_flush((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_aio_task *)bdev_io->driver_ctx);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_aio_reset((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_aio_task *)bdev_io->driver_ctx);
		return 0;

#ifndef __FreeBSD__
	case SPDK_BDEV_IO_TYPE_UNMAP:
		// TODO: Implement function for unmap
		bdev_aio_unmap(bdev_io);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_aio_write_zeros(bdev_io);
		return 0;
#endif

	default:
		return -1;
	}
}

static void
bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_aio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_aio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct file_disk *fdisk = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return fdisk->fallocate;

	default:
		return false;
	}
}

#ifdef __FreeBSD__
static int
bdev_aio_create_io(struct bdev_aio_io_channel *ch)
{
	ch->kqfd = kqueue();
	if (ch->kqfd < 0) {
		SPDK_ERRLOG("async I/O context setup failure: %s.\n", spdk_strerror(errno));
		return -1;
	}

	return 0;
}

static void
bdev_aio_destroy_io(struct bdev_aio_io_channel *ch)
{
	close(ch->kqfd);
}
#else
static int
bdev_aio_create_io(struct bdev_aio_io_channel *ch)
{
	if (io_setup(SPDK_AIO_QUEUE_DEPTH, &ch->io_ctx) < 0) {
		SPDK_ERRLOG("Async I/O context setup failure, likely due to exceeding kernel limit.\n");
		SPDK_ERRLOG("This limit may be increased using 'sysctl -w fs.aio-max-nr'.\n");
		return -1;
	}

	return 0;
}

static void
bdev_aio_destroy_io(struct bdev_aio_io_channel *ch)
{
	io_destroy(ch->io_ctx);
}
#endif

static int
bdev_aio_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *ch = ctx_buf;
	int rc;

	rc = bdev_aio_create_io(ch);
	if (rc < 0) {
		return rc;
	}

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&aio_if));
	TAILQ_INSERT_TAIL(&ch->group_ch->io_ch_head, ch, link);

	return 0;
}

static void
bdev_aio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *ch = ctx_buf;

	bdev_aio_destroy_io(ch);

	assert(ch->group_ch);
	TAILQ_REMOVE(&ch->group_ch->io_ch_head, ch, link);

	spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
}

static struct spdk_io_channel *
bdev_aio_get_io_channel(void *ctx)
{
	struct file_disk *fdisk = ctx;

	return spdk_get_io_channel(fdisk);
}


static int
bdev_aio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = ctx;

	spdk_json_write_named_object_begin(w, "aio");

	if(fdisk->filecnt == 1) {
		spdk_json_write_named_string(w, "filename", fdisk->filename);
	} else {
		spdk_json_write_named_string(w, "base_directory", fdisk->filename);
		spdk_json_write_named_int64(w, "split_filecnt", fdisk->filecnt);
		spdk_json_write_named_int64(w, "size_per_file", fdisk->size_per_file);		
	}

	spdk_json_write_named_bool(w, "block_size_override", fdisk->block_size_override);

	spdk_json_write_named_bool(w, "readonly", fdisk->readonly);

	spdk_json_write_named_bool(w, "fallocate", fdisk->fallocate);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_aio_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_aio_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	if (fdisk->block_size_override) {
		spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	}
	spdk_json_write_named_string(w, "filename", fdisk->filename);
	spdk_json_write_named_bool(w, "readonly", fdisk->readonly);
	spdk_json_write_named_bool(w, "fallocate", fdisk->fallocate);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table aio_fn_table = {
	.destruct		= bdev_aio_destruct,
	.submit_request		= bdev_aio_submit_request,
	.io_type_supported	= bdev_aio_io_type_supported,
	.get_io_channel		= bdev_aio_get_io_channel,
	.dump_info_json		= bdev_aio_dump_info_json,
	.write_config_json	= bdev_aio_write_json_config,
};

static void
aio_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL) {
		return;
	}
	free(fdisk->filename);
	free(fdisk->disk.name);
	free(fdisk);
}

static int
bdev_aio_register_interrupt(struct bdev_aio_group_channel *ch)
{
	int efd;

	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (efd < 0) {
		return -1;
	}

	ch->intr = SPDK_INTERRUPT_REGISTER(efd, bdev_aio_group_interrupt, ch);
	if (ch->intr == NULL) {
		close(efd);
		return -1;
	}
	ch->efd = efd;

	return 0;
}

static void
bdev_aio_unregister_interrupt(struct bdev_aio_group_channel *ch)
{
	spdk_interrupt_unregister(&ch->intr);
	close(ch->efd);
	ch->efd = -1;
}

static void
bdev_aio_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	return;
}

static int
bdev_aio_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_group_channel *ch = ctx_buf;
	int rc;

	TAILQ_INIT(&ch->io_ch_head);
	/* Initialize ch->efd to be invalid and unused. */
	ch->efd = -1;
	if (spdk_interrupt_mode_is_enabled()) {
		rc = bdev_aio_register_interrupt(ch);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to prepare intr resource to bdev_aio\n");
			return rc;
		}
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_aio_group_poll, ch, 0);
	spdk_poller_register_interrupt(ch->poller, bdev_aio_poller_set_interrupt_mode, NULL);

	return 0;
}

static void
bdev_aio_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_group_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->io_ch_head)) {
		SPDK_ERRLOG("Group channel of bdev aio has uncleared io channel\n");
	}

	spdk_poller_unregister(&ch->poller);
	if (spdk_interrupt_mode_is_enabled()) {
		bdev_aio_unregister_interrupt(ch);
	}
}


static int
create_md_array(struct file_disk *fdisk, uint32_t file_cnt)
{	
	struct _file_md *md;	
	int rc = 0;;	
	int base_path_length = strlen(fdisk->filename) + strlen(fdisk->disk.name) + 32;	
	uint32_t block_size = fdisk->disk.blocklen;
	uint64_t size_per_file = fdisk->size_per_file;

	md = calloc(file_cnt, sizeof(*md));
	if(!md) {
		rc = -ENOMEM;
		goto error_return;
	}

	if(file_cnt <= 1)  {
		if (bdev_aio_open(fdisk)) {
			SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", fdisk->filename, fdisk->fd, errno);
			rc = -errno;
			goto error_return;
		}
	} else {		
		for(uint32_t i = 0; i < file_cnt; i++) {
			struct _file_md *md_ref = &md[i];
			md_ref->state = SPDK_FILE_STATE_CLEAN;
			if (snprintf(md_ref->filename, base_path_length, "%s/%s.%d", fdisk->filename, fdisk->disk.name, i) >= base_path_length) {
                SPDK_ERRLOG("Filename too long\n");
                rc = -ENAMETOOLONG;
                goto error_return;
            }

			// if (bdev_aio_open_part_file(fdisk, md_ref)) {
			// 	SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", md_ref->filename, md_ref->fd, errno);
			// 	rc = -errno;
			// 	goto error_return;
			// }
			// md_ref->state = SPDK_FILE_STATE_OPENED;			
			md_ref->start_offset = size_per_file * i;
			md_ref->end_offset = size_per_file * (i + 1);
			md_ref->used_blocks = spdk_bit_array_create(size_per_file/block_size);
			if (!md_ref->used_blocks) {
                SPDK_ERRLOG("Failed to create bit array for file %s\n", md_ref->filename);
                rc = -ENOMEM;
                goto error_return;
            }
			
		}
	}
	fdisk->file_md_array = md;
	return rc;

error_return:
	free(md);
	return rc;
}

static int file_get_blocklen(char *filepath){    
    struct stat st;
    // Get file status
    if (stat(filepath, &st) != 0) {
        perror("stat failed");
        return 1;
    }

    // Print the block size
    printf("File system block size: %ld bytes\n", (long)st.st_blksize);
	if((long)st.st_blksize)
		return (long)st.st_blksize;
    return 0;
}

int
create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
		bool fallocate, uint64_t disk_size_t, uint32_t size_per_file_t, bool filled_t)
{
	struct file_disk *fdisk;
	uint32_t detected_block_size;
	uint64_t file_cnt = 1;
	uint64_t disk_size;
	int rc;
	
	if(disk_size_t && size_per_file_t) {
		file_cnt = disk_size_t / size_per_file_t;
	}
	

#ifdef __FreeBSD__
	if (fallocate) {
		SPDK_ERRLOG("Unable to support fallocate on this platform\n");
		return -ENOTSUP;
	}
#endif

	fdisk = calloc(1, sizeof(*fdisk));
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for aio backend\n");
		return -ENOMEM;
	}
	fdisk->readonly = readonly;
	fdisk->fallocate = fallocate;
	fdisk->filled = filled_t;

	fdisk->filename = strdup(filename);
	if (!fdisk->filename) {
		rc = -ENOMEM;
		goto error_return;
	}

	fdisk->disk.name = strdup(name);
	if (!fdisk->disk.name) {
		rc = -ENOMEM;
		goto error_return;
	}
	fdisk->disk.blocklen = block_size;
	fdisk->size_per_file = size_per_file_t;
	fdisk->filecnt = file_cnt;

	// fdisk->used_file = spdk_bit_array_create(file_cnt);
	// if (!fdisk->used_file) {
	// 	SPDK_ERRLOG("Failed to create bit array for main fdisk struct\n");
	// 	rc = -ENOMEM;
	// 	goto error_return;
	// }
	// spdk_spin_init(&fdisk->used_lock);

	if(create_md_array(fdisk, fdisk->filecnt)) {
		goto error_return;
	}

	if(disk_size_t) {
		disk_size = disk_size_t;
	} else {
		disk_size = spdk_fd_get_size(fdisk->fd);
	}

	fdisk->disk.product_name = "AIO disk";
	fdisk->disk.module = &aio_if;

	fdisk->disk.write_cache = 1;

	if(file_cnt) {
		detected_block_size = file_get_blocklen(fdisk->file_md_array[0].filename);
	} else {
		detected_block_size = spdk_fd_get_blocklen(fdisk->fd);
	}

	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			SPDK_ERRLOG("Block size could not be auto-detected\n");
			rc = -EINVAL;
			goto error_return;
		}
		fdisk->block_size_override = false;
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			SPDK_ERRLOG("Specified block size %" PRIu32 " is smaller than "
				    "auto-detected block size %" PRIu32 "\n",
				    block_size, detected_block_size);
			rc = -EINVAL;
			goto error_return;
		} else if (detected_block_size != 0 && block_size != detected_block_size) {
			SPDK_WARNLOG("Specified block size %" PRIu32 " does not match "
				     "auto-detected block size %" PRIu32 "\n",
				     block_size, detected_block_size);
		}
		fdisk->block_size_override = true;
	}

	if (block_size < 512) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blocklen = block_size;
	if (fdisk->block_size_override && detected_block_size) {
		fdisk->disk.required_alignment = spdk_u32log2(detected_block_size);
	} else {
		fdisk->disk.required_alignment = spdk_u32log2(block_size);
	}

	if (disk_size % fdisk->disk.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    disk_size, fdisk->disk.blocklen);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blockcnt = disk_size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &aio_fn_table;
	// fdisk->zero_buf = spdk_zmalloc(1073741824, 2 * 1024 * 1024, NULL,
	// 					    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	spdk_io_device_register(fdisk, bdev_aio_create_cb, bdev_aio_destroy_cb,
				sizeof(struct bdev_aio_io_channel),
				fdisk->disk.name);
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		spdk_io_device_unregister(fdisk, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_aio_disk_head, fdisk, link);
	return 0;

error_return:
	bdev_aio_close(fdisk);
	aio_free_disk(fdisk);
	return rc;
}

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

int
bdev_aio_rescan(const char *name)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct file_disk *fdisk;
	uint64_t disk_size, blockcnt;
	int rc;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	if (bdev->module != &aio_if) {
		rc = -ENODEV;
		goto exit;
	}

	fdisk = SPDK_CONTAINEROF(bdev, struct file_disk, disk);
	disk_size = spdk_fd_get_size(fdisk->fd);
	blockcnt = disk_size / bdev->blocklen;

	if (bdev->blockcnt != blockcnt) {
		SPDK_NOTICELOG("AIO device is resized: bdev name %s, old block count %" PRIu64 ", new block count %"
			       PRIu64 "\n",
			       fdisk->filename,
			       bdev->blockcnt,
			       blockcnt);
		rc = spdk_bdev_notify_blockcnt_change(bdev, blockcnt);
		if (rc != 0) {
			SPDK_ERRLOG("Could not change num blocks for aio bdev: name %s, errno: %d.\n",
				    fdisk->filename, rc);
			goto exit;
		}
	}

exit:
	spdk_bdev_close(desc);
	return rc;
}

struct delete_aio_bdev_ctx {
	delete_aio_bdev_complete cb_fn;
	void *cb_arg;
};

static void
aio_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_aio_bdev_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	free(ctx);
}

void
bdev_aio_delete(const char *name, delete_aio_bdev_complete cb_fn, void *cb_arg)
{
	struct delete_aio_bdev_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	rc = spdk_bdev_unregister_by_name(name, &aio_if, aio_bdev_unregister_cb, ctx);
	if (rc != 0) {
		aio_bdev_unregister_cb(ctx, rc);
	}
}

static int
bdev_aio_initialize(void)
{
	spdk_io_device_register(&aio_if, bdev_aio_group_create_cb, bdev_aio_group_destroy_cb,
				sizeof(struct bdev_aio_group_channel), "aio_module");

	return 0;
}

static void
bdev_aio_fini(void)
{
	spdk_io_device_unregister(&aio_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(aio)
