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
#include "dlfcn.h"

typedef int (*openat_t)(int dirfd, const char *pathname, int flags, ...);
typedef int (*open_t)(const char *pathname, int flags, ...);
typedef FILE *(*fopen_t)(const char *pathname, const char *mode);
typedef DIR *(*opendir_t)(const char *pathname);
typedef int (*access_t)(const char *pathname, int mode);

#define SPDK_PATH "/tmp/lsblk"

__thread char path[PATH_MAX];

static const char *
get_path(const char *pathname)
{
	if (strncmp(pathname, SPDK_PATH, strlen(SPDK_PATH)) == 0) {
		return pathname;
	}

	if (strncmp(pathname, "/sys/", 5) == 0) {
		snprintf(path, sizeof(path), SPDK_PATH"%s", pathname);
		pathname = path;
	}

	return pathname;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	pathname = get_path(pathname);
	return ((openat_t)dlsym(RTLD_NEXT, "openat"))(dirfd, pathname, flags);
}

int open(const char *pathname, int flags, ...)
{
	pathname = get_path(pathname);
	return ((open_t)dlsym(RTLD_NEXT, "open"))(pathname, flags);
}

FILE *fopen(const char *pathname, const char *mode)
{
	pathname = get_path(pathname);
	return ((fopen_t)dlsym(RTLD_NEXT, "fopen"))(pathname, mode);
}

DIR *opendir(const char *pathname)
{
	pathname = get_path(pathname);
	return ((opendir_t)dlsym(RTLD_NEXT, "opendir"))(pathname);
}

int access(const char *pathname, int mode)
{
	pathname = get_path(pathname);
	return ((access_t)dlsym(RTLD_NEXT, "access"))(pathname, mode);
}
