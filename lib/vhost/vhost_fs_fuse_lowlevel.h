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

#ifndef SPDK_VHOST_FUSE_LOWLEVEL_H
#define SPDK_VHOST_FUSE_LOWLEVEL_H

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs.h"

#define FUSE_USE_VERSION 30
#include <linux/fuse.h>
#include <fuse3/fuse_lowlevel.h>

/* internal ctx for blobfs async operation */
struct spdk_fuse_blobfs_op_args {
	union {
		struct {
			char *filepath;
			void *dir_or_file;
		} lookup, create, unlink;
		struct {
			int nlookup;
			int fserrno;
		} forget;
		struct {
			size_t size;
		} read, write;
		struct {
			char *ori_name;
			char *new_name;
		} rename;
	} op;
	/* In order to align with SPDK FUSE app, vhost-fs stores files with "/" as a prefix to their name */
	char *ori_name; /* used by unlink and rename */
	char *new_name; /* used by rename */
};


/**
 * Redefine SPDK's fuse lowlevel functions.
 *
 * This is aimed to avoid function collision with libfuse.
 * Function notes can be found in fuse_lowlevel.h
 */

size_t spdk_fuse_add_direntry(char *buf, size_t bufsize,
			 const char *name, const struct stat *stbuf, off_t off);
size_t spdk_fuse_add_direntry_plus(char *buf, size_t bufsize,
			      const char *name, const struct fuse_entry_param *e, off_t off);

int spdk_fuse_reply_err(fuse_req_t req, int err);

void spdk_fuse_reply_none(fuse_req_t req);

int spdk_fuse_reply_entry(fuse_req_t req, const struct fuse_entry_param *e);

int spdk_fuse_reply_create(fuse_req_t req, const struct fuse_entry_param *e,
		      const struct fuse_file_info *fi);

int spdk_fuse_reply_attr(fuse_req_t req, const struct stat *attr,
		    double attr_timeout);

int spdk_fuse_reply_open(fuse_req_t req, const struct fuse_file_info *fi);

int spdk_fuse_reply_write(fuse_req_t req, size_t count);

int spdk_fuse_reply_read(fuse_req_t req, size_t count);

int spdk_fuse_reply_buf(fuse_req_t req, const char *buf, size_t size);

int spdk_fuse_reply_data(fuse_req_t req, struct fuse_bufvec *bufv,
		    enum fuse_buf_copy_flags flags);

int spdk_fuse_reply_iov(fuse_req_t req, const struct iovec *iov, int count);

int spdk_fuse_reply_statfs(fuse_req_t req, const struct statvfs *stbuf);


struct spdk_filesystem *spdk_fuse_req_get_fs(fuse_req_t req);
struct spdk_io_channel *spdk_fuse_req_get_io_channel(fuse_req_t req);
struct spdk_fuse_blobfs_op_args *spdk_fuse_req_get_dummy_args(fuse_req_t req);
struct fuse_file_info *spdk_fuse_req_get_fi(fuse_req_t req);
int spdk_fuse_req_get_read_iov(fuse_req_t req, struct iovec **iov);
int spdk_fuse_req_get_write_iov(fuse_req_t req, struct iovec **iov);

void *spdk_fuse_req_userdata(fuse_req_t req);
const struct fuse_ctx *spdk_fuse_req_ctx(fuse_req_t req);

#endif /* SPDK_VHOST_FUSE_LOWLEVEL_H */
