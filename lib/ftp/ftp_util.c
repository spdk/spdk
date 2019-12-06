/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "ftp_commons.h"
#include "spdk/ftp.h"


struct spdk_filesystem *g_fs;
uint32_t g_lcore = 0;
const char *statbuf_get_perms(struct stat *sbuf)
{
	static char perms[] = "----------";
	perms[0] = '?';

	mode_t mode = sbuf->st_mode;
	switch (mode & S_IFMT) {
	case S_IFREG:
		perms[0] = '-';
		break;
	case S_IFDIR:
		perms[0] = 'd';
		break;
	case S_IFLNK:
		perms[0] = 'l';
		break;
	case S_IFIFO:
		perms[0] = 'p';
		break;
	case S_IFSOCK:
		perms[0] = 's';
		break;
	case S_IFCHR:
		perms[0] = 'c';
		break;
	case S_IFBLK:
		perms[0] = 'b';
		break;
	}

	if (mode & S_IRUSR) {
		perms[1] = 'r';
	}
	if (mode & S_IWUSR) {
		perms[2] = 'w';
	}
	if (mode & S_IXUSR) {
		perms[3] = 'x';
	}
	if (mode & S_IRGRP) {
		perms[4] = 'r';
	}
	if (mode & S_IWGRP) {
		perms[5] = 'w';
	}
	if (mode & S_IXGRP) {
		perms[6] = 'x';
	}
	if (mode & S_IROTH) {
		perms[7] = 'r';
	}
	if (mode & S_IWOTH) {
		perms[8] = 'w';
	}
	if (mode & S_IXOTH) {
		perms[9] = 'x';
	}
	if (mode & S_ISUID) {
		perms[3] = (perms[3] == 'x') ? 's' : 'S';
	}
	if (mode & S_ISGID) {
		perms[6] = (perms[6] == 'x') ? 's' : 'S';
	}
	if (mode & S_ISVTX) {
		perms[9] = (perms[9] == 'x') ? 't' : 'T';
	}

	return perms;
}

const char *statbuf_get_date(struct stat *sbuf)
{
	static char datebuf[64] = {0};
	const char *p_date_format = "%b %e %H:%M";
	struct timespec tv;

	clock_gettime(CLOCK_REALTIME, &tv);
	time_t local_time = tv.tv_sec;
	if (sbuf->st_mtime > local_time || (local_time - sbuf->st_mtime) > 60 * 60 * 24 * 182) {
		p_date_format = "%b %e  %Y";
	}

	struct tm *p_tm = localtime(&local_time);
	strftime(datebuf, sizeof(datebuf), p_date_format, p_tm);

	return datebuf;
}

ssize_t writen(int fd, const void *buf, size_t count)
{
	size_t nleft = count;
	ssize_t nwritten;
	char *bufp = (char *)buf;

	while (nleft > 0) {
		if ((nwritten = write(fd, bufp, nleft)) < 0) {
			if (errno == EINTR) {
				continue;
			}
			printf("error info is %s\n", strerror(errno));
			return -1;
		} else if (nwritten == 0) {
			continue;
		}

		bufp += nwritten;
		nleft -= nwritten;
	}

	return count;
}

void
str_upper(char *str)
{
	while (*str) {
		*str = toupper(*str);
		str++;
	}
}
void
str_split(const char *str, char *left, char *right, char c)
{
	char *p = strchr(str, c);
	if (p == NULL) {
		memcpy(left, str, strlen(str));
	} else {
		memcpy(left, str, p - str);
		memcpy(right, p + 1, strlen(str) - strlen(left) - 1);
	}
}

static void
__call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}


static void
__send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(g_lcore, __call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

static void
fs_load_cb(__attribute__((unused)) void *ctx,
	   struct spdk_filesystem *fs, int fserrno)
{
	printf("begin load\n");
	if (fserrno == 0) {
		g_fs = fs;
		printf("load success\n");
	} else {
		printf("load failed, error code num is %s\n", strerror(errno));
	}
}



int 
spdk_ftp_load_blobfs(char *bdevname)
{
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *g_bs_dev;
	bdev = spdk_bdev_get_by_name(bdevname);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", bdevname);
		exit(1);
	}

	g_lcore = spdk_env_get_first_core();
	g_bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	printf("using bdev %s\n", bdevname);
	spdk_fs_load(g_bs_dev, __send_request, fs_load_cb, NULL);
	
	
	return 0;
}
static void
fs_unload_cb(__attribute__((unused)) void *ctx,
	     __attribute__((unused)) int fserrno)
{
	assert(fserrno == 0);

	
}
void spdk_ftp_unload_blobfs(void)
{
	printf("spdk_ruiblobfs_shutdown called\n");
	/* if (g_sync_args.channel) {
		spdk_fs_free_thread_ctx(g_sync_args.channel);
		g_sync_args.channel = NULL;
	} */
	if (g_fs != NULL) {
		spdk_fs_unload(g_fs, fs_unload_cb, NULL);
	} else {
		fs_unload_cb(NULL, 0);
	}
}
