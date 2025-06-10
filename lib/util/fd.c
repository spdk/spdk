/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/log.h"

#ifdef __linux__
#include <linux/fs.h>
#endif

#ifdef __FreeBSD__
#include <sys/disk.h>
#endif

static uint64_t
dev_get_size(int fd)
{
#if defined(DIOCGMEDIASIZE) /* FreeBSD */
	off_t size;

	if (ioctl(fd, DIOCGMEDIASIZE, &size) == 0) {
		return size;
	}
#elif defined(__linux__) && defined(BLKGETSIZE64)
	uint64_t size;

	if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
		return size;
	}
#endif

	return 0;
}

uint32_t
spdk_fd_get_blocklen(int fd)
{
#if defined(DIOCGSECTORSIZE) /* FreeBSD */
	uint32_t blocklen;

	if (ioctl(fd, DIOCGSECTORSIZE, &blocklen) == 0) {
		return blocklen;
	}
#elif defined(DKIOCGETBLOCKSIZE)
	uint32_t blocklen;

	if (ioctl(fd, DKIOCGETBLOCKSIZE, &blocklen) == 0) {
		return blocklen;
	}
#elif defined(__linux__) && defined(BLKSSZGET)
	uint32_t blocklen;

	if (ioctl(fd, BLKSSZGET, &blocklen) == 0) {
		return blocklen;
	}
#endif

	return 0;
}

uint64_t
spdk_fd_get_size(int fd)
{
	struct stat st;

	if (fstat(fd, &st) != 0) {
		return 0;
	}

	if (S_ISLNK(st.st_mode)) {
		return 0;
	}

	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		return dev_get_size(fd);
	} else if (S_ISREG(st.st_mode)) {
		return st.st_size;
	}

	/* Not REG, CHR or BLK */
	return 0;
}

/* If set flag is true then set nonblock, clear otherwise. */
static int
fd_update_nonblock(int fd, bool set)
{
	int flag;

	flag = fcntl(fd, F_GETFL);
	if (flag < 0) {
		SPDK_ERRLOG("fcntl can't get file status flag, fd: %d (%s)\n", fd, spdk_strerror(errno));
		return -errno;
	}

	if (set) {
		if (flag & O_NONBLOCK) {
			return 0;
		}

		flag |= O_NONBLOCK;
	} else {
		if (!(flag & O_NONBLOCK)) {
			return 0;
		}

		flag &= ~O_NONBLOCK;
	}

	if (fcntl(fd, F_SETFL, flag) < 0) {
		SPDK_ERRLOG("fcntl can't set %sblocking mode, fd: %d (%s)\n", set ? "non" : "", fd,
			    spdk_strerror(errno));
		return -errno;
	}

	return 0;
}

int
spdk_fd_set_nonblock(int fd)
{
	return fd_update_nonblock(fd, true);
}

int
spdk_fd_clear_nonblock(int fd)
{
	return fd_update_nonblock(fd, false);
}
