/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/fsdev.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk_internal/fuse_dispatcher.h"
#include "linux/fuse_kernel.h"

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/* TODO: values, see https://libfuse.github.io/doxygen/structfuse__conn__info.html */
#define DEFAULT_TIME_GRAN 1
#define DEFAULT_MAX_BACKGROUND 1024
#define DEFAULT_CONGESTION_THRESHOLD 1024
#define DEFAULT_MAX_READAHEAD 0x00020000
#define OFFSET_MAX 0x7fffffffffffffffLL

/*
 * NOTE: It appeared that the open flags have different values on the different HW architechtures.
 *
 * This code handles the open flags translation in case they're originated from a platform with
 * a different HW architecture.
 *
 * Currently supported:
 *  - X86
 *  - X86_64
 *  - ARM
 *  - ARM64
 */
/* See https://lxr.missinglinkelectronics.com/linux/arch/arm/include/uapi/asm/fcntl.h */
#define ARM_O_DIRECTORY      040000 /* must be a directory */
#define ARM_O_NOFOLLOW      0100000 /* don't follow links */
#define ARM_O_DIRECT        0200000 /* direct disk access hint - currently ignored */
#define ARM_O_LARGEFILE     0400000

/* See https://lxr.missinglinkelectronics.com/linux/include/uapi/asm-generic/fcntl.h */
#define X86_O_DIRECT        00040000        /* direct disk access hint */
#define X86_O_LARGEFILE     00100000
#define X86_O_DIRECTORY     00200000        /* must be a directory */
#define X86_O_NOFOLLOW      00400000        /* don't follow links */

static inline bool
fsdev_d2h_open_flags(enum spdk_fuse_arch fuse_arch, uint32_t flags, uint32_t *translated_flags)
{
	bool res = true;

	*translated_flags = flags;

	/* NOTE: we always check the original flags to avoid situation where the arch and the native flags
	 * overlap and previously set native flag could be interpreted as original arch flag.
	 */
#define REPLACE_FLAG(arch_flag, native_flag) \
	do { \
		if (flags & (arch_flag)) { \
			*translated_flags &= ~(arch_flag); \
			*translated_flags |= (native_flag); \
		} \
	} while(0)

	switch (fuse_arch) {
	case SPDK_FUSE_ARCH_NATIVE:
#if defined(__x86_64__) || defined(__i386__)
	case SPDK_FUSE_ARCH_X86:
	case SPDK_FUSE_ARCH_X86_64:
#endif
#if defined(__aarch64__) || defined(__arm__)
	case SPDK_FUSE_ARCH_ARM:
	case SPDK_FUSE_ARCH_ARM64:
#endif
		/* No translation required */
		break;
#if defined(__x86_64__) || defined(__i386__)
	case SPDK_FUSE_ARCH_ARM:
	case SPDK_FUSE_ARCH_ARM64:
		/* Relace the ARM-specific flags with the native ones */
		REPLACE_FLAG(ARM_O_DIRECTORY, O_DIRECTORY);
		REPLACE_FLAG(ARM_O_NOFOLLOW, O_NOFOLLOW);
		REPLACE_FLAG(ARM_O_DIRECT, O_DIRECT);
		REPLACE_FLAG(ARM_O_LARGEFILE, O_LARGEFILE);
		break;
#endif
#if defined(__aarch64__) || defined(__arm__)
	case SPDK_FUSE_ARCH_X86:
	case SPDK_FUSE_ARCH_X86_64:
		/* Relace the X86-specific flags with the native ones */
		REPLACE_FLAG(X86_O_DIRECTORY, O_DIRECTORY);
		REPLACE_FLAG(X86_O_NOFOLLOW, O_NOFOLLOW);
		REPLACE_FLAG(X86_O_DIRECT, O_DIRECT);
		REPLACE_FLAG(X86_O_LARGEFILE, O_LARGEFILE);
		break;
#endif
	default:
		SPDK_ERRLOG("Unsupported FUSE arch: %d\n", fuse_arch);
		assert(0);
		res = false;
		break;
	}

#undef REPLACE_FLAG

	return res;
}

struct spdk_fuse_mgr {
	struct spdk_mempool *fuse_io_pool;
	uint32_t ref_cnt;
	pthread_mutex_t lock;
};

static struct spdk_fuse_mgr g_fuse_mgr = {
	.fuse_io_pool = NULL,
	.ref_cnt = 0,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

struct fuse_forget_data {
	uint64_t ino;
	uint64_t nlookup;
};

struct iov_offs {
	size_t iov_offs;
	size_t buf_offs;
};

struct fuse_io {
	/** For SG buffer cases, array of iovecs for input. */
	struct iovec *in_iov;

	/** For SG buffer cases, number of iovecs in in_iov array. */
	int in_iovcnt;

	/** For SG buffer cases, array of iovecs for output. */
	struct iovec *out_iov;

	/** For SG buffer cases, number of iovecs in out_iov array. */
	int out_iovcnt;

	struct iov_offs in_offs;
	struct iov_offs out_offs;

	spdk_fuse_dispatcher_submit_cpl_cb cpl_cb;
	void *cpl_cb_arg;
	struct spdk_io_channel *ch;
	struct spdk_fuse_dispatcher *disp;

	struct fuse_in_header hdr;
	bool in_hdr_with_data;

	union {
		struct {
			struct spdk_thread *thread;
			struct fuse_init_in *in;
			bool legacy_in;
			struct spdk_fsdev_mount_opts opts;
			size_t out_len;
			int error;
		} init;
		struct {
			bool plus;
			uint32_t size;
			char *writep;
			uint32_t bytes_written;
		} readdir;
		struct {
			uint32_t to_forget;
			int status;
		} batch_forget;

		struct {
			int status;
		} fsdev_close;
	} u;
};

struct spdk_fuse_dispatcher {
	/**
	 * fsdev descriptor
	 */
	struct spdk_fsdev_desc *desc;

	/**
	 * fsdev thread
	 */
	struct spdk_thread *fsdev_thread;

	/**
	 * Major version of the protocol (read-only)
	 */
	unsigned proto_major;

	/**
	 * Minor version of the protocol (read-only)
	 */
	unsigned proto_minor;

	/**
	 * FUSE request source's architecture
	 */
	enum spdk_fuse_arch fuse_arch;

	/**
	 * Root file object
	 */
	struct spdk_fsdev_file_object *root_fobject;

	/**
	 * Event callback
	 */
	spdk_fuse_dispatcher_event_cb event_cb;

	/**
	 * Event callback's context
	 */
	void *event_ctx;

	/**
	 * Name of the underlying fsdev
	 *
	 * NOTE: must be last
	 */
	char fsdev_name[];
};

struct spdk_fuse_dispatcher_channel {
	struct spdk_io_channel *fsdev_io_ch;
};

#define __disp_to_io_dev(disp)	(((char *)disp) + 1)
#define __disp_from_io_dev(io_dev)	((struct spdk_fuse_dispatcher *)(((char *)io_dev) - 1))
#define __disp_ch_from_io_ch(io_ch)	((struct spdk_fuse_dispatcher_channel *)spdk_io_channel_get_ctx(io_ch))

static inline const char *
fuse_dispatcher_name(struct spdk_fuse_dispatcher *disp)
{
	return disp->fsdev_name;
}

static inline uint64_t
file_ino(struct fuse_io *fuse_io, const struct spdk_fsdev_file_object *fobject)
{
	return (fuse_io->disp->root_fobject == fobject) ? FUSE_ROOT_ID : (uint64_t)(uintptr_t)fobject;
}

static struct spdk_fsdev_file_object *
ino_to_object(struct fuse_io *fuse_io, uint64_t ino)
{
	return (ino == FUSE_ROOT_ID) ?
	       fuse_io->disp->root_fobject :
	       (struct spdk_fsdev_file_object *)(uintptr_t)ino;
}

static struct spdk_fsdev_file_object *
file_object(struct fuse_io *fuse_io)
{
	return ino_to_object(fuse_io, fuse_io->hdr.nodeid);
}

static inline uint64_t
file_fh(const struct spdk_fsdev_file_handle *fhandle)
{
	return (uint64_t)(uintptr_t)fhandle;
}

static struct spdk_fsdev_file_handle *
file_handle(uint64_t fh)
{
	return (struct spdk_fsdev_file_handle *)(uintptr_t)fh;
}

static inline uint16_t
fsdev_io_d2h_u16(struct fuse_io *fuse_io, uint16_t v)
{
	return v;
}

static inline uint16_t
fsdev_io_h2d_u16(struct fuse_io *fuse_io, uint16_t v)
{
	return v;
}

static inline uint32_t
fsdev_io_d2h_u32(struct fuse_io *fuse_io, uint32_t v)
{
	return v;
}

static inline uint32_t
fsdev_io_h2d_u32(struct fuse_io *fuse_io, uint32_t v)
{
	return v;
}

static inline int32_t
fsdev_io_h2d_i32(struct fuse_io *fuse_io, int32_t v)
{
	return v;
}

static inline uint64_t
fsdev_io_d2h_u64(struct fuse_io *fuse_io, uint64_t v)
{
	return v;
}

static inline uint64_t
fsdev_io_h2d_u64(struct fuse_io *fuse_io, uint64_t v)
{
	return v;
}

static inline unsigned
fsdev_io_proto_minor(struct fuse_io *fuse_io)
{
	return fuse_io->disp->proto_minor;
}

static inline void *
_iov_arr_get_buf_info(struct iovec *iovs, size_t cnt, struct iov_offs *offs, size_t *size)
{
	struct iovec *iov;

	assert(offs->iov_offs <= cnt);

	if (offs->iov_offs == cnt) {
		assert(!offs->buf_offs);
		*size = 0;
		return NULL;
	}

	iov = &iovs[offs->iov_offs];

	assert(offs->buf_offs < iov->iov_len);

	*size = iov->iov_len - offs->buf_offs;

	return ((char *)iov->iov_base) + offs->buf_offs;
}

static inline void *
_iov_arr_get_buf(struct iovec *iovs, size_t cnt, struct iov_offs *offs, size_t size,
		 const char *direction)
{
	char *arg_buf;
	size_t arg_size;

	arg_buf = _iov_arr_get_buf_info(iovs, cnt, offs, &arg_size);
	if (!arg_buf) {
		SPDK_INFOLOG(fuse_dispatcher, "No %s arg header attached at %zu:%zu\n", direction, offs->iov_offs,
			     offs->buf_offs);
		return NULL;
	}

	if (!arg_size) {
		SPDK_INFOLOG(fuse_dispatcher, "%s arg of zero length attached at %zu:%zu\n", direction,
			     offs->iov_offs, offs->buf_offs);
		return NULL;
	}

	if (size > arg_size) {
		SPDK_INFOLOG(fuse_dispatcher, "%s arg is too small (%zu > %zu) at %zu:%zu\n", direction, size,
			     arg_size, offs->iov_offs, offs->buf_offs);
		return NULL;
	}

	if (size == arg_size) {
		offs->iov_offs++;
		offs->buf_offs = 0;
	} else {
		offs->buf_offs += size;
	}

	return arg_buf;
}

static inline const char *
_fsdev_io_in_arg_get_str(struct fuse_io *fuse_io)
{
	char *arg_buf;
	size_t arg_size, len;

	arg_buf = _iov_arr_get_buf_info(fuse_io->in_iov, fuse_io->in_iovcnt, &fuse_io->in_offs,
					&arg_size);
	if (!arg_buf) {
		SPDK_ERRLOG("No IN arg header attached at %zu:%zu\n", fuse_io->in_offs.iov_offs,
			    fuse_io->in_offs.buf_offs);
		return NULL;
	}

	len = strnlen(arg_buf, arg_size);
	if (len == arg_size) {
		SPDK_ERRLOG("no string or bad string attached at %zu:%zu\n", fuse_io->in_offs.iov_offs,
			    fuse_io->in_offs.buf_offs);
		return NULL;
	}

	fuse_io->in_offs.buf_offs += len + 1;

	if (len + 1 == arg_size) {
		fuse_io->in_offs.iov_offs++;
		fuse_io->in_offs.buf_offs = 0;
	}

	return arg_buf;
}

static inline void *
_fsdev_io_in_arg_get_buf(struct fuse_io *fuse_io, size_t size)
{
	return _iov_arr_get_buf(fuse_io->in_iov, fuse_io->in_iovcnt, &fuse_io->in_offs, size, "IN");
}


static inline void *
_fsdev_io_out_arg_get_buf(struct fuse_io *fuse_io, size_t size)
{
	return _iov_arr_get_buf(fuse_io->out_iov, fuse_io->out_iovcnt, &fuse_io->out_offs, size,
				"OUT");
}

static bool
_fuse_op_requires_reply(uint32_t opcode)
{
	switch (opcode) {
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
		return false;
	default:
		return true;
	}
}

static void
convert_stat(struct fuse_io *fuse_io, struct spdk_fsdev_file_object *fobject,
	     const struct spdk_fsdev_file_attr *attr, struct fuse_attr *fattr)
{
	fattr->ino	= fsdev_io_h2d_u64(fuse_io, attr->ino);
	fattr->mode	= fsdev_io_h2d_u32(fuse_io, attr->mode);
	fattr->nlink	= fsdev_io_h2d_u32(fuse_io, attr->nlink);
	fattr->uid	= fsdev_io_h2d_u32(fuse_io, attr->uid);
	fattr->gid	= fsdev_io_h2d_u32(fuse_io, attr->gid);
	fattr->rdev	= fsdev_io_h2d_u32(fuse_io, attr->rdev);
	fattr->size	= fsdev_io_h2d_u64(fuse_io, attr->size);
	fattr->blksize	= fsdev_io_h2d_u32(fuse_io, attr->blksize);
	fattr->blocks	= fsdev_io_h2d_u64(fuse_io, attr->blocks);
	fattr->atime	= fsdev_io_h2d_u64(fuse_io, attr->atime);
	fattr->mtime	= fsdev_io_h2d_u64(fuse_io, attr->mtime);
	fattr->ctime	= fsdev_io_h2d_u64(fuse_io, attr->ctime);
	fattr->atimensec = fsdev_io_h2d_u32(fuse_io, attr->atimensec);
	fattr->mtimensec = fsdev_io_h2d_u32(fuse_io, attr->mtimensec);
	fattr->ctimensec = fsdev_io_h2d_u32(fuse_io, attr->ctimensec);
}

static uint32_t
calc_timeout_sec(uint32_t ms)
{
	return ms / 1000;
}

static uint32_t
calc_timeout_nsec(uint32_t ms)
{
	return (ms % 1000) * 1000000;
}

static void
fill_entry(struct fuse_io *fuse_io, struct fuse_entry_out *arg,
	   struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	arg->nodeid = fsdev_io_h2d_u64(fuse_io, file_ino(fuse_io, fobject));
	arg->generation = 0;
	arg->entry_valid = fsdev_io_h2d_u64(fuse_io, calc_timeout_sec(attr->valid_ms));
	arg->entry_valid_nsec = fsdev_io_h2d_u32(fuse_io, calc_timeout_nsec(attr->valid_ms));
	arg->attr_valid = fsdev_io_h2d_u64(fuse_io, calc_timeout_sec(attr->valid_ms));
	arg->attr_valid_nsec = fsdev_io_h2d_u32(fuse_io, calc_timeout_nsec(attr->valid_ms));
	convert_stat(fuse_io, fobject, attr, &arg->attr);
}

static void
fill_open(struct fuse_io *fuse_io, struct fuse_open_out *arg,
	  struct spdk_fsdev_file_handle *fhandle)
{
	arg->fh = fsdev_io_h2d_u64(fuse_io, file_fh(fhandle));
	arg->open_flags = fsdev_io_h2d_u64(fuse_io, FOPEN_DIRECT_IO);
}

static void
convert_statfs(struct fuse_io *fuse_io, const struct spdk_fsdev_file_statfs *statfs,
	       struct fuse_kstatfs *kstatfs)
{
	kstatfs->bsize	 = fsdev_io_h2d_u32(fuse_io, statfs->bsize);
	kstatfs->frsize	 = fsdev_io_h2d_u32(fuse_io, statfs->frsize);
	kstatfs->blocks	 = fsdev_io_h2d_u64(fuse_io, statfs->blocks);
	kstatfs->bfree	 = fsdev_io_h2d_u64(fuse_io, statfs->bfree);
	kstatfs->bavail	 = fsdev_io_h2d_u64(fuse_io, statfs->bavail);
	kstatfs->files	 = fsdev_io_h2d_u64(fuse_io, statfs->files);
	kstatfs->ffree	 = fsdev_io_h2d_u64(fuse_io, statfs->ffree);
	kstatfs->namelen = fsdev_io_h2d_u32(fuse_io, statfs->namelen);
}

static struct fuse_out_header *
fuse_dispatcher_fill_out_hdr(struct fuse_io *fuse_io, size_t out_len, int error)
{
	struct fuse_out_header *hdr;
	struct iovec *out;
	uint32_t len;

	assert(fuse_io->out_iovcnt >= 1);
	assert(error <= 0);

	out = fuse_io->out_iov;

	if (out->iov_len < sizeof(*hdr)) {
		SPDK_ERRLOG("Bad out header len: %zu < %zu\n", out->iov_len, sizeof(*hdr));
		return NULL;
	}

	if (error < -1000) {
		SPDK_ERRLOG("Bad completion error value: %" PRIu32 "\n", error);
		return NULL;
	}

	len = sizeof(*hdr) + out_len;

	hdr = out->iov_base;
	memset(hdr, 0, sizeof(*hdr));

	hdr->unique = fsdev_io_h2d_u64(fuse_io, fuse_io->hdr.unique);
	hdr->error = fsdev_io_h2d_i32(fuse_io, error);
	hdr->len = fsdev_io_h2d_u32(fuse_io, len);

	return hdr;
}

static void
fuse_dispatcher_io_complete_final(struct fuse_io *fuse_io, int error)
{
	spdk_fuse_dispatcher_submit_cpl_cb cpl_cb = fuse_io->cpl_cb;
	void *cpl_cb_arg = fuse_io->cpl_cb_arg;

	/* NOTE: it's important to free fuse_io before the completion callback,
	 * as the callback can destroy the dispatcher
	 */
	spdk_mempool_put(g_fuse_mgr.fuse_io_pool, fuse_io);

	cpl_cb(cpl_cb_arg, error);
}

static void
fuse_dispatcher_io_complete(struct fuse_io *fuse_io, uint32_t out_len, int error)
{
	struct fuse_out_header *hdr = fuse_dispatcher_fill_out_hdr(fuse_io, out_len, error);

	assert(_fuse_op_requires_reply(fuse_io->hdr.opcode));

	if (!hdr) {
		SPDK_ERRLOG("Completion failed: cannot fill out header\n");
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher,
		      "Completing IO#%" PRIu64 " (err=%d, out_len=%" PRIu32 ")\n",
		      fuse_io->hdr.unique, error, out_len);

	fuse_dispatcher_io_complete_final(fuse_io, error);
}

static void
fuse_dispatcher_io_copy_and_complete(struct fuse_io *fuse_io, const void *out, uint32_t out_len,
				     int error)
{
	if (out && out_len) {
		void *buf = _fsdev_io_out_arg_get_buf(fuse_io, out_len);
		if (buf) {
			memcpy(buf, out, out_len);
		} else {
			SPDK_ERRLOG("Completion failed: cannot get buf to copy %" PRIu32 " bytes\n", out_len);
			error = EINVAL;
			out_len = 0;
		}
	}

	fuse_dispatcher_io_complete(fuse_io, out_len, error);
}

static void
fuse_dispatcher_io_complete_none(struct fuse_io *fuse_io, int err)
{
	SPDK_DEBUGLOG(fuse_dispatcher, "Completing IO#%" PRIu64 "(err=%d)\n",
		      fuse_io->hdr.unique, err);
	fuse_dispatcher_io_complete_final(fuse_io, err);
}

static void
fuse_dispatcher_io_complete_ok(struct fuse_io *fuse_io, uint32_t out_len)
{
	fuse_dispatcher_io_complete(fuse_io, out_len, 0);
}

static void
fuse_dispatcher_io_complete_err(struct fuse_io *fuse_io, int err)
{
	fuse_dispatcher_io_complete(fuse_io, 0, err);
}

static void
fuse_dispatcher_io_complete_entry(struct fuse_io *fuse_io, struct spdk_fsdev_file_object *fobject,
				  const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_entry_out arg;
	size_t size = fsdev_io_proto_minor(fuse_io) < 9 ?
		      FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(arg);

	memset(&arg, 0, sizeof(arg));
	fill_entry(fuse_io, &arg, fobject, attr);

	fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

static void
fuse_dispatcher_io_complete_open(struct fuse_io *fuse_io, struct spdk_fsdev_file_handle *fhandle)
{
	struct fuse_open_out *arg;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_open_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fill_open(fuse_io, arg, fhandle);

	fuse_dispatcher_io_complete_ok(fuse_io, sizeof(*arg));
}

static void
fuse_dispatcher_io_complete_create(struct fuse_io *fuse_io, struct spdk_fsdev_file_object *fobject,
				   const struct spdk_fsdev_file_attr *attr,
				   struct spdk_fsdev_file_handle *fhandle)
{
	char buf[sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out)];
	size_t entrysize = fsdev_io_proto_minor(fuse_io) < 9 ?
			   FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(struct fuse_entry_out);
	struct fuse_entry_out *earg = (struct fuse_entry_out *) buf;
	struct fuse_open_out *oarg = (struct fuse_open_out *)(buf + entrysize);

	memset(buf, 0, sizeof(buf));
	fill_entry(fuse_io, earg, fobject, attr);
	fill_open(fuse_io, oarg, fhandle);

	fuse_dispatcher_io_copy_and_complete(fuse_io, buf, entrysize + sizeof(struct fuse_open_out), 0);
}

static void
fuse_dispatcher_io_complete_xattr(struct fuse_io *fuse_io, uint32_t count)
{
	struct fuse_getxattr_out *arg;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_getxattr_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	arg->size = fsdev_io_h2d_i32(fuse_io, count);

	fuse_dispatcher_io_complete_ok(fuse_io, sizeof(*arg));
}

static void
fuse_dispatcher_io_complete_write(struct fuse_io *fuse_io, uint32_t data_size, int error)
{
	struct fuse_write_out *arg;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_write_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	arg->size = fsdev_io_d2h_u32(fuse_io, data_size);

	fuse_dispatcher_io_complete(fuse_io, sizeof(*arg), error);
}

static void
fuse_dispatcher_io_complete_statfs(struct fuse_io *fuse_io,
				   const struct spdk_fsdev_file_statfs *statfs)
{
	struct fuse_statfs_out arg;
	size_t size = fsdev_io_proto_minor(fuse_io) < 4 ?
		      FUSE_COMPAT_STATFS_SIZE : sizeof(arg);

	memset(&arg, 0, sizeof(arg));
	convert_statfs(fuse_io, statfs, &arg.st);

	return fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

static void
fuse_dispatcher_io_complete_attr(struct fuse_io *fuse_io, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_attr_out arg;
	size_t size = fsdev_io_proto_minor(fuse_io) < 9 ?
		      FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(arg);

	memset(&arg, 0, sizeof(arg));
	arg.attr_valid = fsdev_io_h2d_u64(fuse_io, calc_timeout_sec(attr->valid_ms));
	arg.attr_valid_nsec = fsdev_io_h2d_u32(fuse_io, calc_timeout_nsec(attr->valid_ms));
	convert_stat(fuse_io, file_object(fuse_io), attr, &arg.attr);

	fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
static size_t
fuse_dispatcher_add_direntry(struct fuse_io *fuse_io, char *buf, size_t bufsize,
			     const char *name, struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
			     off_t off)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;
	struct fuse_dirent *dirent;

	namelen = strlen(name);
	entlen = FUSE_NAME_OFFSET + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);

	if ((buf == NULL) || (entlen_padded > bufsize)) {
		return entlen_padded;
	}

	dirent = (struct fuse_dirent *) buf;
	dirent->ino = file_ino(fuse_io, fobject);
	dirent->off = fsdev_io_h2d_u64(fuse_io, off);
	dirent->namelen = fsdev_io_h2d_u32(fuse_io, namelen);
	dirent->type = fsdev_io_h2d_u32(fuse_io, (attr->mode & 0170000) >> 12);
	memcpy(dirent->name, name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return entlen_padded;
}

/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
static size_t
fuse_dispatcher_add_direntry_plus(struct fuse_io *fuse_io, char *buf, size_t bufsize,
				  const char *name, struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
				  off_t off)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;

	namelen = strlen(name);
	entlen = FUSE_NAME_OFFSET_DIRENTPLUS + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);
	if ((buf == NULL) || (entlen_padded > bufsize)) {
		return entlen_padded;
	}

	struct fuse_direntplus *dp = (struct fuse_direntplus *) buf;
	memset(&dp->entry_out, 0, sizeof(dp->entry_out));
	fill_entry(fuse_io, &dp->entry_out, fobject, attr);

	struct fuse_dirent *dirent = &dp->dirent;
	dirent->ino = fsdev_io_h2d_u64(fuse_io, attr->ino);
	dirent->off = fsdev_io_h2d_u64(fuse_io, off);
	dirent->namelen = fsdev_io_h2d_u32(fuse_io, namelen);
	dirent->type = fsdev_io_h2d_u32(fuse_io, (attr->mode & 0170000) >> 12);
	memcpy(dirent->name, name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return entlen_padded;
}

/*
 * Static FUSE commands handlers
 */
static inline struct spdk_fsdev_desc *
fuse_io_desc(struct fuse_io *fuse_io)
{
	return fuse_io->disp->desc;
}

static void
do_lookup_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		  struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_entry(fuse_io, fobject, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_lookup(struct fuse_io *fuse_io)
{
	int err;
	const char *name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("No name or bad name attached\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_lookup(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				file_object(fuse_io), name, do_lookup_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_forget_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_none(fuse_io, status); /* FUSE_FORGET requires no response */
}

static void
do_forget(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_forget_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_forget_in\n");
		fuse_dispatcher_io_complete_none(fuse_io, EINVAL); /* FUSE_FORGET requires no response */
		return;
	}

	err = spdk_fsdev_forget(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				file_object(fuse_io), fsdev_io_d2h_u64(fuse_io, arg->nlookup),
				do_forget_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_getattr_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		   const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_attr(fuse_io, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_getattr(struct fuse_io *fuse_io)
{
	int err;
	uint64_t fh = 0;

	if (fsdev_io_proto_minor(fuse_io) >= 9) {
		struct fuse_getattr_in *arg;

		arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
		if (!arg) {
			SPDK_ERRLOG("Cannot get fuse_getattr_in\n");
			fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
			return;
		}

		if (fsdev_io_d2h_u64(fuse_io, arg->getattr_flags) & FUSE_GETATTR_FH) {
			fh = fsdev_io_d2h_u64(fuse_io, arg->fh);
		}
	}

	err = spdk_fsdev_getattr(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				 file_object(fuse_io), file_handle(fh), do_getattr_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_setattr_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		   const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_attr(fuse_io, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_setattr(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_setattr_in *arg;
	uint32_t valid;
	uint64_t fh = 0;
	struct spdk_fsdev_file_attr attr;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_setattr_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	memset(&attr, 0, sizeof(attr));
	attr.mode      = fsdev_io_d2h_u32(fuse_io, arg->mode);
	attr.uid       = fsdev_io_d2h_u32(fuse_io, arg->uid);
	attr.gid       = fsdev_io_d2h_u32(fuse_io, arg->gid);
	attr.size      = fsdev_io_d2h_u64(fuse_io, arg->size);
	attr.atime     = fsdev_io_d2h_u64(fuse_io, arg->atime);
	attr.mtime     = fsdev_io_d2h_u64(fuse_io, arg->mtime);
	attr.ctime     = fsdev_io_d2h_u64(fuse_io, arg->ctime);
	attr.atimensec = fsdev_io_d2h_u32(fuse_io, arg->atimensec);
	attr.mtimensec = fsdev_io_d2h_u32(fuse_io, arg->mtimensec);
	attr.ctimensec = fsdev_io_d2h_u32(fuse_io, arg->ctimensec);

	valid = fsdev_io_d2h_u64(fuse_io, arg->valid);
	if (valid & FATTR_FH) {
		valid &= ~FATTR_FH;
		fh = fsdev_io_d2h_u64(fuse_io, arg->fh);
	}

	valid &=
		FSDEV_SET_ATTR_MODE |
		FSDEV_SET_ATTR_UID |
		FSDEV_SET_ATTR_GID |
		FSDEV_SET_ATTR_SIZE |
		FSDEV_SET_ATTR_ATIME |
		FSDEV_SET_ATTR_MTIME |
		FSDEV_SET_ATTR_ATIME_NOW |
		FSDEV_SET_ATTR_MTIME_NOW |
		FSDEV_SET_ATTR_CTIME;

	err = spdk_fsdev_setattr(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				 file_object(fuse_io), file_handle(fh), &attr, valid,
				 do_setattr_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_readlink_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status, const char *linkname)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_copy_and_complete(fuse_io, linkname, strlen(linkname) + 1, 0);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_readlink(struct fuse_io *fuse_io)
{
	int err;

	err = spdk_fsdev_readlink(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				  file_object(fuse_io), do_readlink_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_symlink_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		   struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_entry(fuse_io, fobject, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_symlink(struct fuse_io *fuse_io)
{
	int err;
	const char *name, *linkname;

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	linkname = _fsdev_io_in_arg_get_str(fuse_io);
	if (!linkname) {
		SPDK_ERRLOG("Cannot get linkname\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_symlink(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				 file_object(fuse_io), name, linkname, fuse_io->hdr.uid, fuse_io->hdr.gid,
				 do_symlink_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_mknod_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		 struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_entry(fuse_io, fobject, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_mknod(struct fuse_io *fuse_io)
{
	int err;
	bool compat = fsdev_io_proto_minor(fuse_io) < 12;
	struct fuse_mknod_in *arg;
	const char *name;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, compat ? FUSE_COMPAT_MKNOD_IN_SIZE : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_mknod_in (compat=%d)\n", compat);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name (compat=%d)\n", compat);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_mknod(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), name, fsdev_io_d2h_u32(fuse_io, arg->mode),
			       fsdev_io_d2h_u32(fuse_io, arg->rdev), fuse_io->hdr.uid, fuse_io->hdr.gid,
			       do_mknod_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_mkdir_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		 struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_entry(fuse_io, fobject, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_mkdir(struct fuse_io *fuse_io)
{
	int err;
	bool compat = fsdev_io_proto_minor(fuse_io) < 12;
	struct fuse_mkdir_in *arg;
	const char *name;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, compat ? sizeof(uint32_t) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_mkdir_in (compat=%d)\n", compat);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name (compat=%d)\n", compat);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_mkdir(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), name, fsdev_io_d2h_u32(fuse_io, arg->mode),
			       fuse_io->hdr.uid, fuse_io->hdr.gid, do_mkdir_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_unlink_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_unlink(struct fuse_io *fuse_io)
{
	int err;
	const char *name;

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_unlink(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				file_object(fuse_io), name, do_unlink_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_rmdir_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_rmdir(struct fuse_io *fuse_io)
{
	int err;
	const char *name;

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_rmdir(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), name, do_rmdir_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_rename_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_rename_common(struct fuse_io *fuse_io, bool version2)
{
	int err;
	uint64_t newdir;
	const char *oldname;
	const char *newname;
	uint32_t flags = 0;

	if (!version2) {
		struct fuse_rename_in *arg;
		arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
		if (!arg) {
			SPDK_ERRLOG("Cannot get fuse_rename_in\n");
			fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
			return;
		}
		newdir = fsdev_io_d2h_u64(fuse_io, arg->newdir);
	} else {
		struct fuse_rename2_in *arg;
		arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
		if (!arg) {
			SPDK_ERRLOG("Cannot get fuse_rename2_in\n");
			fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
			return;
		}
		newdir = fsdev_io_d2h_u64(fuse_io, arg->newdir);
		flags = fsdev_io_d2h_u64(fuse_io, arg->flags);
	}

	oldname = _fsdev_io_in_arg_get_str(fuse_io);
	if (!oldname) {
		SPDK_ERRLOG("Cannot get oldname\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	newname = _fsdev_io_in_arg_get_str(fuse_io);
	if (!newname) {
		SPDK_ERRLOG("Cannot get newname\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_rename(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				file_object(fuse_io), oldname, ino_to_object(fuse_io, newdir),
				newname, flags, do_rename_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_rename(struct fuse_io *fuse_io)
{
	do_rename_common(fuse_io, false);
}

static void
do_rename2(struct fuse_io *fuse_io)
{
	do_rename_common(fuse_io, true);
}

static void
do_link_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_entry(fuse_io, fobject, attr);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_link(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_link_in *arg;
	const char *name;
	uint64_t oldnodeid;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_link_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	oldnodeid = fsdev_io_d2h_u64(fuse_io, arg->oldnodeid);

	err = spdk_fsdev_link(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			      ino_to_object(fuse_io, oldnodeid), file_object(fuse_io), name,
			      do_link_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_fopen_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		 struct spdk_fsdev_file_handle *fhandle)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_open(fuse_io, fhandle);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_open(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	int err;
	struct fuse_open_in *arg;
	uint32_t flags;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_forget_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	if (!fsdev_d2h_open_flags(disp->fuse_arch, fsdev_io_d2h_u32(fuse_io, arg->flags), &flags)) {
		SPDK_ERRLOG("Cannot translate flags\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_fopen(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), flags,
			       do_fopen_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_read_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete(fuse_io, data_size, status);
}

static void
do_read(struct fuse_io *fuse_io)
{
	int err;
	bool compat = fsdev_io_proto_minor(fuse_io) < 9;
	struct fuse_read_in *arg;
	uint64_t fh;
	uint32_t flags = 0;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? offsetof(struct fuse_read_in, lock_owner) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_read_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}


	if (!compat) {
		flags = fsdev_io_d2h_u32(fuse_io, arg->flags);
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_read(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			      file_object(fuse_io), file_handle(fh),
			      fsdev_io_d2h_u32(fuse_io, arg->size), fsdev_io_d2h_u64(fuse_io, arg->offset),
			      flags, fuse_io->out_iov + 1, fuse_io->out_iovcnt - 1, NULL,
			      do_read_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_write_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_write(fuse_io, data_size, status);
}

static void
do_write(struct fuse_io *fuse_io)
{
	int err;
	bool compat = fsdev_io_proto_minor(fuse_io) < 9;
	struct fuse_write_in *arg;
	uint64_t fh;
	uint64_t flags = 0;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? FUSE_COMPAT_WRITE_IN_SIZE : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_write_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	if (fuse_io->in_offs.buf_offs) {
		SPDK_ERRLOG("Data IOVs should be separate from the header IOV\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	if (!compat) {
		flags = fsdev_io_d2h_u32(fuse_io, arg->flags);
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_write(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), file_handle(fh),
			       fsdev_io_d2h_u32(fuse_io, arg->size), fsdev_io_d2h_u64(fuse_io, arg->offset),
			       flags, fuse_io->in_iov + fuse_io->in_offs.iov_offs, fuse_io->in_iovcnt - fuse_io->in_offs.iov_offs,
			       NULL, do_write_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_statfs_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		  const struct spdk_fsdev_file_statfs *statfs)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_statfs(fuse_io, statfs);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_statfs(struct fuse_io *fuse_io)
{
	int err;

	err = spdk_fsdev_statfs(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				file_object(fuse_io), do_statfs_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_release_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_release(struct fuse_io *fuse_io)
{
	int err;
	bool compat = fsdev_io_proto_minor(fuse_io) < 8;
	struct fuse_release_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? offsetof(struct fuse_release_in, lock_owner) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_release_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_release(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				 file_object(fuse_io), file_handle(fh),
				 do_release_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_fsync_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_fsync(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_fsync_in *arg;
	uint64_t fh;
	bool datasync;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_fsync_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);
	datasync = (fsdev_io_d2h_u32(fuse_io, arg->fsync_flags) & 1) ? true : false;

	err = spdk_fsdev_fsync(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), file_handle(fh), datasync,
			       do_fsync_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_setxattr_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_setxattr(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_setxattr_in *arg;
	const char *name;
	const char *value;
	uint32_t size;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_setxattr_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	size = fsdev_io_d2h_u32(fuse_io, arg->size);
	value = _fsdev_io_in_arg_get_buf(fuse_io, size);
	if (!value) {
		SPDK_ERRLOG("Cannot get value of %" PRIu32 " bytes\n", size);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_setxattr(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				  file_object(fuse_io), name, value, size, fsdev_io_d2h_u32(fuse_io, arg->flags),
				  do_setxattr_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_getxattr_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status, size_t value_size)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_xattr(fuse_io, value_size);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_getxattr(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_getxattr_in *arg;
	const char *name;
	char *buff;
	uint32_t size;
	struct iov_offs out_offs_bu;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_getxattr_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	if (fuse_io->out_iovcnt < 2) {
		SPDK_ERRLOG("No buffer to getxattr\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	size = fsdev_io_d2h_u32(fuse_io, arg->size);

	/* NOTE: we want to avoid an additionl allocation and copy and put the xattr directly to the buffer provided in out_iov.
	 * In order to do so we have to preserve the out_offs, advance it to get the buffer pointer and then restore to allow
	 * the fuse_dispatcher_io_complete_xattr() to fill the fuse_getxattr_out which precedes this buffer.
	 */
	out_offs_bu = fuse_io->out_offs; /* Preserve the out offset */

	/* Skip the fuse_getxattr_out */
	_fsdev_io_out_arg_get_buf(fuse_io, sizeof(struct fuse_getxattr_out));
	size -= sizeof(struct fuse_getxattr_out);

	buff = _fsdev_io_out_arg_get_buf(fuse_io, size); /* Get the buffer for the xattr */
	if (!buff) {
		SPDK_INFOLOG(fuse_dispatcher, "NULL buffer, probably asking for the size\n");
		size = 0;
	}

	fuse_io->out_offs = out_offs_bu; /* Restore the out offset */

	err = spdk_fsdev_getxattr(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				  file_object(fuse_io), name, buff, size,
				  do_getxattr_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_listxattr_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status, size_t size,
		     bool size_only)
{
	struct fuse_io *fuse_io = cb_arg;

	if (status) {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	} else if (size_only) {
		fuse_dispatcher_io_complete_xattr(fuse_io, size);
	} else {
		fuse_dispatcher_io_complete_ok(fuse_io, size);
	}
}

static void
do_listxattr(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_getxattr_in *arg;
	struct iovec *iov;
	uint32_t size;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_getxattr_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	size = fsdev_io_d2h_u32(fuse_io, arg->size);
	iov = fuse_io->out_iov + 1;
	if (iov->iov_len < size) {
		SPDK_ERRLOG("Wrong iov len (%zu < %" PRIu32")\n", iov->iov_len, size);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_listxattr(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				   file_object(fuse_io), iov->iov_base, size,
				   do_listxattr_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_removexattr_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_removexattr(struct fuse_io *fuse_io)
{
	int err;
	const char *name = _fsdev_io_in_arg_get_str(fuse_io);

	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_removexattr(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				     file_object(fuse_io), name, do_removexattr_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_flush_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_flush(struct fuse_io *fuse_io)
{
	int err;
	bool compat = fsdev_io_proto_minor(fuse_io) < 7;
	struct fuse_flush_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? offsetof(struct fuse_flush_in, lock_owner) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_flush_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_flush(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       file_object(fuse_io), file_handle(fh),
			       do_flush_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_mount_rollback_cpl_clb(void *cb_arg, struct spdk_io_channel *ch)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;

	UNUSED(disp);

	SPDK_DEBUGLOG(fuse_dispatcher, "%s unmounted\n", fuse_dispatcher_name(disp));

	/* The IO is FUSE_INIT, so we complete it with the appropriate error */
	fuse_dispatcher_io_complete_err(fuse_io, fuse_io->u.init.error);
}

static void fuse_dispatcher_mount_rollback_msg(void *ctx);

static void
fuse_dispatcher_mount_rollback(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	int rc;

	rc = spdk_fsdev_umount(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			       do_mount_rollback_cpl_clb, fuse_io);
	if (rc) {
		/* It can only fail due to a lack of the IO objects, so we retry until one of them will be available */
		SPDK_WARNLOG("%s: umount cannot be initiated (err=%d). Retrying...\n",
			     fuse_dispatcher_name(disp), rc);
		spdk_thread_send_msg(spdk_get_thread(), fuse_dispatcher_mount_rollback_msg, fuse_io);
	}
}

static void
fuse_dispatcher_mount_rollback_msg(void *ctx)
{
	struct fuse_io *fuse_io = ctx;

	fuse_dispatcher_mount_rollback(fuse_io);
}

static void
fuse_dispatcher_fsdev_remove_put_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *io_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_fuse_dispatcher_channel *ch = __disp_ch_from_io_ch(io_ch);

	assert(ch->fsdev_io_ch);
	spdk_put_io_channel(ch->fsdev_io_ch);
	ch->fsdev_io_ch = NULL;

	spdk_for_each_channel_continue(i, 0);
}

static void
fuse_dispatcher_fsdev_remove_put_channel_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_fuse_dispatcher *disp = spdk_io_channel_iter_get_ctx(i);

	if (status) {
		SPDK_WARNLOG("%s: putting channels failed with %d\n", fuse_dispatcher_name(disp), status);
	}

	disp->event_cb(SPDK_FUSE_DISP_EVENT_FSDEV_REMOVE, disp, disp->event_ctx);
}

static void
fuse_dispatcher_fsdev_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev,
			       void *event_ctx)
{
	struct spdk_fuse_dispatcher *disp = event_ctx;

	SPDK_NOTICELOG("%s received fsdev event %d\n", fuse_dispatcher_name(disp), type);

	switch (type) {
	case SPDK_FSDEV_EVENT_REMOVE:
		SPDK_NOTICELOG("%s received SPDK_FSDEV_EVENT_REMOVE\n", fuse_dispatcher_name(disp));
		/* Put the channels, to prevent the further IO submission */
		spdk_for_each_channel(__disp_to_io_dev(disp),
				      fuse_dispatcher_fsdev_remove_put_channel,
				      disp,
				      fuse_dispatcher_fsdev_remove_put_channel_done);
		break;
	default:
		SPDK_NOTICELOG("%s received an unknown fsdev event %d\n", fuse_dispatcher_name(disp), type);
		break;
	}
}

static int
do_mount_prepare_completion(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	struct fuse_init_out outarg;
	size_t outargsize = sizeof(outarg);
	uint32_t max_readahead = DEFAULT_MAX_READAHEAD;
	uint32_t flags = 0;
	void *out_buf;

	assert(disp->desc);

	memset(&outarg, 0, sizeof(outarg));
	outarg.major = fsdev_io_h2d_u32(fuse_io, FUSE_KERNEL_VERSION);
	outarg.minor = fsdev_io_h2d_u32(fuse_io, FUSE_KERNEL_MINOR_VERSION);

	if (disp->proto_minor < 5) {
		outargsize = FUSE_COMPAT_INIT_OUT_SIZE;
	} else if (disp->proto_minor < 23) {
		outargsize = FUSE_COMPAT_22_INIT_OUT_SIZE;
	}

	if (!fuse_io->u.init.legacy_in) {
		max_readahead = fsdev_io_d2h_u32(fuse_io, fuse_io->u.init.in->max_readahead);
		flags = fsdev_io_d2h_u32(fuse_io, fuse_io->u.init.in->flags);

		SPDK_INFOLOG(fuse_dispatcher, "max_readahead: %" PRIu32 " flags=0x%" PRIx32 "\n",
			     max_readahead, flags);
	}

	/* Always enable big writes, this is superseded by the max_write option */
	outarg.flags = FUSE_BIG_WRITES;

#define LL_SET_DEFAULT(cond, cap) \
	if ((cond) && flags & (cap)) \
		outarg.flags |= (cap)
	LL_SET_DEFAULT(true, FUSE_ASYNC_READ);
	LL_SET_DEFAULT(true, FUSE_AUTO_INVAL_DATA);
	LL_SET_DEFAULT(true, FUSE_ASYNC_DIO);
	LL_SET_DEFAULT(true, FUSE_ATOMIC_O_TRUNC);
	LL_SET_DEFAULT(true, FUSE_FLOCK_LOCKS);
	LL_SET_DEFAULT(true, FUSE_DO_READDIRPLUS);
	LL_SET_DEFAULT(true, FUSE_READDIRPLUS_AUTO);
	LL_SET_DEFAULT(true, FUSE_EXPORT_SUPPORT);
	LL_SET_DEFAULT(fuse_io->u.init.opts.writeback_cache_enabled, FUSE_WRITEBACK_CACHE);

	outarg.flags = fsdev_io_h2d_u32(fuse_io, outarg.flags);
	outarg.max_readahead = fsdev_io_h2d_u32(fuse_io, max_readahead);
	outarg.max_write = fsdev_io_h2d_u32(fuse_io, fuse_io->u.init.opts.max_write);
	if (fsdev_io_proto_minor(fuse_io) >= 13) {
		outarg.max_background = fsdev_io_h2d_u16(fuse_io, DEFAULT_MAX_BACKGROUND);
		outarg.congestion_threshold = fsdev_io_h2d_u16(fuse_io, DEFAULT_CONGESTION_THRESHOLD);
	}

	if (fsdev_io_proto_minor(fuse_io) >= 23) {
		outarg.time_gran = fsdev_io_h2d_u32(fuse_io, DEFAULT_TIME_GRAN);
	}

	SPDK_INFOLOG(fuse_dispatcher, "INIT: %" PRIu32 ".%" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io, outarg.major), fsdev_io_d2h_u32(fuse_io, outarg.minor));
	SPDK_INFOLOG(fuse_dispatcher, "flags: 0x%08" PRIx32 "\n", fsdev_io_d2h_u32(fuse_io, outarg.flags));
	SPDK_INFOLOG(fuse_dispatcher, "max_readahead: %" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io, outarg.max_readahead));
	SPDK_INFOLOG(fuse_dispatcher, "max_write: %" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io, outarg.max_write));
	SPDK_INFOLOG(fuse_dispatcher, "max_background: %" PRIu16 "\n",
		     fsdev_io_d2h_u16(fuse_io, outarg.max_background));
	SPDK_INFOLOG(fuse_dispatcher, "congestion_threshold: %" PRIu16 "\n",
		     fsdev_io_d2h_u16(fuse_io, outarg.congestion_threshold));
	SPDK_INFOLOG(fuse_dispatcher, "time_gran: %" PRIu32 "\n", fsdev_io_d2h_u32(fuse_io,
			outarg.time_gran));

	out_buf = _fsdev_io_out_arg_get_buf(fuse_io, outargsize);
	if (!out_buf) {
		SPDK_ERRLOG("Cannot get buf to copy fuse_init_out of %zu bytes\n", outargsize);
		return -EINVAL;
	}

	memcpy(out_buf, &outarg, outargsize);

	fuse_io->u.init.out_len = outargsize;
	return 0;
}

static void
do_mount_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		 const struct spdk_fsdev_mount_opts *opts, struct spdk_fsdev_file_object *root_fobject)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	int rc;

	if (status) {
		SPDK_ERRLOG("%s: spdk_fsdev_mount failed (err=%d)\n", fuse_dispatcher_name(disp), status);
		fuse_dispatcher_io_complete_err(fuse_io, status);
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: spdk_fsdev_mount succeeded\n", fuse_dispatcher_name(disp));
	disp->root_fobject = root_fobject;
	rc = do_mount_prepare_completion(fuse_io);
	if (rc) {
		SPDK_ERRLOG("%s: mount completion preparation failed with %d\n", fuse_dispatcher_name(disp), rc);
		fuse_io->u.init.error = rc;
		disp->root_fobject = NULL;
		fuse_dispatcher_mount_rollback(fuse_io);
		return;
	}

	fuse_dispatcher_io_complete_ok(fuse_io, fuse_io->u.init.out_len);
}

static void
do_init(struct fuse_io *fuse_io)
{
	size_t compat_size = offsetof(struct fuse_init_in, max_readahead);
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	uint32_t flags = 0;
	int rc;

	/* First try to read the legacy header */
	fuse_io->u.init.in = _fsdev_io_in_arg_get_buf(fuse_io, compat_size);
	if (!fuse_io->u.init.in) {
		SPDK_ERRLOG("Cannot get fuse_init_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EBADR);
		return;
	}

	disp->proto_major = fsdev_io_d2h_u32(fuse_io, fuse_io->u.init.in->major);
	disp->proto_minor = fsdev_io_d2h_u32(fuse_io, fuse_io->u.init.in->minor);

	SPDK_DEBUGLOG(fuse_dispatcher, "Proto version: %" PRIu32 ".%" PRIu32 "\n",
		      disp->proto_major,
		      disp->proto_minor);

	/* Now try to read the whole struct */
	if (disp->proto_major == 7 && disp->proto_minor >= 6) {
		void *arg_extra = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*fuse_io->u.init.in) - compat_size);
		if (!arg_extra) {
			SPDK_ERRLOG("INIT: protocol version: %" PRIu32 ".%" PRIu32 " but legacy data found\n",
				    disp->proto_major, disp->proto_minor);
			fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
			return;
		}
		fuse_io->u.init.legacy_in = false;
	} else {
		fuse_io->u.init.legacy_in = true;
	}

	if (disp->proto_major < 7) {
		SPDK_ERRLOG("INIT: unsupported major protocol version: %" PRIu32 "\n",
			    disp->proto_major);
		fuse_dispatcher_io_complete_err(fuse_io, -EAGAIN);
		return;
	}

	if (disp->proto_major > 7) {
		/* Wait for a second INIT request with a 7.X version */

		struct fuse_init_out outarg;
		size_t outargsize = sizeof(outarg);

		memset(&outarg, 0, sizeof(outarg));
		outarg.major = fsdev_io_h2d_u32(fuse_io, FUSE_KERNEL_VERSION);
		outarg.minor = fsdev_io_h2d_u32(fuse_io, FUSE_KERNEL_MINOR_VERSION);

		fuse_dispatcher_io_copy_and_complete(fuse_io, &outarg, outargsize, 0);
		return;
	}

	if (!fuse_io->u.init.legacy_in) {
		flags = fsdev_io_d2h_u32(fuse_io, fuse_io->u.init.in->flags);

		SPDK_INFOLOG(fuse_dispatcher, "flags=0x%" PRIx32 "\n", flags);
	}

	memset(&fuse_io->u.init.opts, 0, sizeof(fuse_io->u.init.opts));
	fuse_io->u.init.opts.opts_size = sizeof(fuse_io->u.init.opts);
	fuse_io->u.init.opts.max_write = 0;
	fuse_io->u.init.opts.writeback_cache_enabled = flags & FUSE_WRITEBACK_CACHE ? true : false;
	fuse_io->u.init.thread = spdk_get_thread();

	rc = spdk_fsdev_mount(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
			      &fuse_io->u.init.opts, do_mount_cpl_clb, fuse_io);
	if (rc) {
		SPDK_ERRLOG("%s: failed to initiate mount (err=%d)\n", fuse_dispatcher_name(disp), rc);
		fuse_dispatcher_io_complete_err(fuse_io, rc);
	}
}

static void
do_opendir_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		   struct spdk_fsdev_file_handle *fhandle)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_open(fuse_io, fhandle);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_opendir(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_open_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_open_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_opendir(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				 file_object(fuse_io), fsdev_io_d2h_u32(fuse_io, arg->flags),
				 do_opendir_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static int
do_readdir_entry_clb(void *cb_arg, struct spdk_io_channel *ch, const char *name,
		     struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr, off_t offset)
{
	struct fuse_io *fuse_io = cb_arg;
	size_t bytes_remained = fuse_io->u.readdir.size - fuse_io->u.readdir.bytes_written;
	size_t direntry_bytes;

	direntry_bytes = fuse_io->u.readdir.plus ?
			 fuse_dispatcher_add_direntry_plus(fuse_io, fuse_io->u.readdir.writep, bytes_remained,
					 name, fobject, attr, offset) :
			 fuse_dispatcher_add_direntry(fuse_io, fuse_io->u.readdir.writep, bytes_remained,
					 name, fobject, attr, offset);

	if (direntry_bytes > bytes_remained) {
		return EAGAIN;
	}

	fuse_io->u.readdir.writep += direntry_bytes;
	fuse_io->u.readdir.bytes_written += direntry_bytes;

	return 0;
}

static void
do_readdir_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status || (status == EAGAIN && fuse_io->u.readdir.bytes_written == fuse_io->u.readdir.size)) {
		fuse_dispatcher_io_complete_ok(fuse_io, fuse_io->u.readdir.bytes_written);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_readdir_common(struct fuse_io *fuse_io, bool plus)
{
	int err;
	struct fuse_read_in *arg;
	uint64_t fh;
	uint32_t size;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_read_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	size = fsdev_io_d2h_u32(fuse_io, arg->size);

	fuse_io->u.readdir.writep = _fsdev_io_out_arg_get_buf(fuse_io, size);
	if (!fuse_io->u.readdir.writep) {
		SPDK_ERRLOG("Cannot get buffer of %" PRIu32 " bytes\n", size);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fuse_io->u.readdir.plus = plus;
	fuse_io->u.readdir.size = size;
	fuse_io->u.readdir.bytes_written = 0;

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_readdir(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				 file_object(fuse_io), file_handle(fh),
				 fsdev_io_d2h_u64(fuse_io, arg->offset),
				 do_readdir_entry_clb, do_readdir_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_readdir(struct fuse_io *fuse_io)
{
	do_readdir_common(fuse_io, false);
}

static void
do_readdirplus(struct fuse_io *fuse_io)
{
	do_readdir_common(fuse_io, true);
}

static void
do_releasedir_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_releasedir(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_release_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_release_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_releasedir(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				    file_object(fuse_io), file_handle(fh),
				    do_releasedir_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_fsyncdir_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_fsyncdir(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_fsync_in *arg;
	uint64_t fh;
	bool datasync;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_fsync_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);
	datasync = (fsdev_io_d2h_u32(fuse_io, arg->fsync_flags) & 1) ? true : false;

	err = spdk_fsdev_fsyncdir(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				  file_object(fuse_io), file_handle(fh), datasync,
				  do_fsyncdir_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_getlk(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("GETLK is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_setlk_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_setlk_common(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_lk_in *arg;
	uint64_t fh;
	uint32_t lk_flags;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_lk_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	lk_flags = fsdev_io_d2h_u64(fuse_io, arg->lk_flags);

	if (lk_flags & FUSE_LK_FLOCK) {
		int op = 0;

		switch (arg->lk.type) {
		case F_RDLCK:
			op = LOCK_SH;
			break;
		case F_WRLCK:
			op = LOCK_EX;
			break;
		case F_UNLCK:
			op = LOCK_UN;
			break;
		}

		fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

		err = spdk_fsdev_flock(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				       file_object(fuse_io), file_handle(fh), op,
				       do_setlk_cpl_clb, fuse_io);
		if (err) {
			fuse_dispatcher_io_complete_err(fuse_io, err);
		}
	} else {
		SPDK_ERRLOG("SETLK: with no FUSE_LK_FLOCK is not supported\n");
		fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
	}
}

static void
do_setlk(struct fuse_io *fuse_io)
{
	do_setlk_common(fuse_io);
}

static void
do_setlkw(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("SETLKW is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_access(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("ACCESS is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_create_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status,
		  struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
		  struct spdk_fsdev_file_handle *fhandle)
{
	struct fuse_io *fuse_io = cb_arg;

	if (!status) {
		fuse_dispatcher_io_complete_create(fuse_io, fobject, attr, fhandle);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}
}

static void
do_create(struct fuse_io *fuse_io)
{
	int err;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	bool compat = fsdev_io_proto_minor(fuse_io) < 12;
	struct fuse_create_in *arg;
	const char *name;
	uint32_t flags, mode, umask = 0;
	size_t arg_size = compat ? sizeof(struct fuse_open_in) : sizeof(*arg);

	arg = _fsdev_io_in_arg_get_buf(fuse_io, arg_size);
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_create_in (compat=%d)\n", compat);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name (compat=%d)\n", compat);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	mode =  fsdev_io_d2h_u32(fuse_io, arg->mode);
	if (!compat) {
		umask = fsdev_io_d2h_u32(fuse_io, arg->umask);
	}

	if (!fsdev_d2h_open_flags(disp->fuse_arch, fsdev_io_d2h_u32(fuse_io, arg->flags), &flags)) {
		SPDK_ERRLOG("Cannot translate flags\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = spdk_fsdev_create(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				file_object(fuse_io), name, mode, flags, umask, fuse_io->hdr.uid,
				fuse_io->hdr.gid, do_create_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_abort_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_interrupt(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_interrupt_in *arg;
	uint64_t unique;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_access_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	unique = fsdev_io_d2h_u64(fuse_io, arg->unique);

	SPDK_DEBUGLOG(fuse_dispatcher, "INTERRUPT: %" PRIu64 "\n", unique);

	err = spdk_fsdev_abort(fuse_io_desc(fuse_io), fuse_io->ch, unique, do_abort_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_bmap(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("BMAP is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_ioctl(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("IOCTL is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_poll(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("POLL is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_fallocate_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_err(fuse_io, status);
}

static void
do_fallocate(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_fallocate_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_fallocate_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh = fsdev_io_d2h_u64(fuse_io, arg->fh);

	err = spdk_fsdev_fallocate(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
				   file_object(fuse_io), file_handle(fh),
				   fsdev_io_d2h_u32(fuse_io, arg->mode), fsdev_io_d2h_u64(fuse_io, arg->offset),
				   fsdev_io_d2h_u64(fuse_io, arg->length),
				   do_fallocate_cpl_clb, fuse_io);
	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_umount_cpl_clb(void *cb_arg, struct spdk_io_channel *ch)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;

	disp->proto_major = disp->proto_minor = 0;
	disp->root_fobject = NULL;
	SPDK_DEBUGLOG(fuse_dispatcher, "%s unmounted\n", fuse_dispatcher_name(disp));
	fuse_dispatcher_io_complete_err(fuse_io, 0);
}

static void
do_destroy(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	int rc;

	rc = spdk_fsdev_umount(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique, do_umount_cpl_clb,
			       fuse_io);
	if (rc) {
		SPDK_ERRLOG("%s: failed to initiate umount (err=%d)\n", fuse_dispatcher_name(disp), rc);
		fuse_dispatcher_io_complete_err(fuse_io, rc);
		return;
	}
}

static void
do_batch_forget_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fuse_io *fuse_io = cb_arg;

	if (status) {
		fuse_io->u.batch_forget.status = status;
	}

	fuse_io->u.batch_forget.to_forget--;

	if (!fuse_io->u.batch_forget.to_forget) {
		/* FUSE_BATCH_FORGET requires no response */
		fuse_dispatcher_io_complete_none(fuse_io, fuse_io->u.batch_forget.status);
	}
}

static void
do_batch_forget(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_batch_forget_in *arg;
	struct fuse_forget_data *forgets;
	size_t scount;
	uint32_t count, i;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_batch_forget_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	/* Prevent integer overflow.  The compiler emits the following warning
	 * unless we use the scount local variable:
	 *
	 * error: comparison is always false due to limited range of data type
	 * [-Werror=type-limits]
	 *
	 * This may be true on 64-bit hosts but we need this check for 32-bit
	 * hosts.
	 */
	scount = fsdev_io_d2h_u32(fuse_io, arg->count);
	if (scount > SIZE_MAX / sizeof(forgets[0])) {
		SPDK_WARNLOG("Too many forgets (%zu >= %zu)\n", scount,
			     SIZE_MAX / sizeof(forgets[0]));
		/* FUSE_BATCH_FORGET requires no response */
		fuse_dispatcher_io_complete_none(fuse_io, -EINVAL);
		return;
	}

	count = scount;
	if (!count) {
		SPDK_WARNLOG("0 forgets requested\n");
		/* FUSE_BATCH_FORGET requires no response */
		fuse_dispatcher_io_complete_none(fuse_io, -EINVAL);
		return;
	}

	forgets = _fsdev_io_in_arg_get_buf(fuse_io, count * sizeof(forgets[0]));
	if (!forgets) {
		SPDK_WARNLOG("Cannot get expected forgets (%" PRIu32 ")\n", count);
		/* FUSE_BATCH_FORGET requires no response */
		fuse_dispatcher_io_complete_none(fuse_io, -EINVAL);
		return;
	}

	fuse_io->u.batch_forget.to_forget = 0;
	fuse_io->u.batch_forget.status = 0;

	for (i = 0; i < count; i++) {
		uint64_t ino = fsdev_io_d2h_u64(fuse_io, forgets[i].ino);
		uint64_t nlookup = fsdev_io_d2h_u64(fuse_io, forgets[i].nlookup);
		err = spdk_fsdev_forget(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
					ino_to_object(fuse_io, ino), nlookup,
					do_batch_forget_cpl_clb, fuse_io);
		if (!err) {
			fuse_io->u.batch_forget.to_forget++;
		} else {
			fuse_io->u.batch_forget.status = err;
		}
	}

	if (!fuse_io->u.batch_forget.to_forget) {
		/* FUSE_BATCH_FORGET requires no response */
		fuse_dispatcher_io_complete_none(fuse_io, fuse_io->u.batch_forget.status);
	}
}

static void
do_copy_file_range_cpl_clb(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct fuse_io *fuse_io = cb_arg;

	fuse_dispatcher_io_complete_write(fuse_io, data_size, status);
}

static void
do_copy_file_range(struct fuse_io *fuse_io)
{
	int err;
	struct fuse_copy_file_range_in *arg;
	uint64_t fh_in, fh_out, nodeid_out;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_copy_file_range_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fh_in = fsdev_io_d2h_u64(fuse_io, arg->fh_in);
	nodeid_out = fsdev_io_d2h_u64(fuse_io, arg->nodeid_out);
	fh_out = fsdev_io_d2h_u64(fuse_io, arg->fh_out);

	err = spdk_fsdev_copy_file_range(fuse_io_desc(fuse_io), fuse_io->ch, fuse_io->hdr.unique,
					 file_object(fuse_io), file_handle(fh_in), fsdev_io_d2h_u64(fuse_io, arg->off_in),
					 ino_to_object(fuse_io, nodeid_out), file_handle(fh_out), fsdev_io_d2h_u64(fuse_io, arg->off_out),
					 fsdev_io_d2h_u64(fuse_io, arg->len), fsdev_io_d2h_u64(fuse_io, arg->flags),
					 do_copy_file_range_cpl_clb, fuse_io);

	if (err) {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

static void
do_setupmapping(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("SETUPMAPPING is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_removemapping(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("REMOVEMAPPING is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static void
do_syncfs(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("SYNCFS is not supported\n");
	fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
}

static const struct {
	void (*func)(struct fuse_io *fuse_io);
	const char *name;
} fuse_ll_ops[] = {
	[FUSE_LOOKUP]	   = { do_lookup,      "LOOKUP"	     },
	[FUSE_FORGET]	   = { do_forget,      "FORGET"	     },
	[FUSE_GETATTR]	   = { do_getattr,     "GETATTR"     },
	[FUSE_SETATTR]	   = { do_setattr,     "SETATTR"     },
	[FUSE_READLINK]	   = { do_readlink,    "READLINK"    },
	[FUSE_SYMLINK]	   = { do_symlink,     "SYMLINK"     },
	[FUSE_MKNOD]	   = { do_mknod,       "MKNOD"	     },
	[FUSE_MKDIR]	   = { do_mkdir,       "MKDIR"	     },
	[FUSE_UNLINK]	   = { do_unlink,      "UNLINK"	     },
	[FUSE_RMDIR]	   = { do_rmdir,       "RMDIR"	     },
	[FUSE_RENAME]	   = { do_rename,      "RENAME"	     },
	[FUSE_LINK]	   = { do_link,	       "LINK"	     },
	[FUSE_OPEN]	   = { do_open,	       "OPEN"	     },
	[FUSE_READ]	   = { do_read,       "READ"	     },
	[FUSE_WRITE]	   = { do_write,       "WRITE"	     },
	[FUSE_STATFS]	   = { do_statfs,      "STATFS"	     },
	[FUSE_RELEASE]	   = { do_release,     "RELEASE"     },
	[FUSE_FSYNC]	   = { do_fsync,       "FSYNC"	     },
	[FUSE_SETXATTR]	   = { do_setxattr,    "SETXATTR"    },
	[FUSE_GETXATTR]	   = { do_getxattr,    "GETXATTR"    },
	[FUSE_LISTXATTR]   = { do_listxattr,   "LISTXATTR"   },
	[FUSE_REMOVEXATTR] = { do_removexattr, "REMOVEXATTR" },
	[FUSE_FLUSH]	   = { do_flush,       "FLUSH"	     },
	[FUSE_INIT]	   = { do_init,	       "INIT"	     },
	[FUSE_OPENDIR]	   = { do_opendir,     "OPENDIR"     },
	[FUSE_READDIR]	   = { do_readdir,     "READDIR"     },
	[FUSE_RELEASEDIR]  = { do_releasedir,  "RELEASEDIR"  },
	[FUSE_FSYNCDIR]	   = { do_fsyncdir,    "FSYNCDIR"    },
	[FUSE_GETLK]	   = { do_getlk,       "GETLK"	     },
	[FUSE_SETLK]	   = { do_setlk,       "SETLK"	     },
	[FUSE_SETLKW]	   = { do_setlkw,      "SETLKW"	     },
	[FUSE_ACCESS]	   = { do_access,      "ACCESS"	     },
	[FUSE_CREATE]	   = { do_create,      "CREATE"	     },
	[FUSE_INTERRUPT]   = { do_interrupt,   "INTERRUPT"   },
	[FUSE_BMAP]	   = { do_bmap,	       "BMAP"	     },
	[FUSE_IOCTL]	   = { do_ioctl,       "IOCTL"	     },
	[FUSE_POLL]	   = { do_poll,        "POLL"	     },
	[FUSE_FALLOCATE]   = { do_fallocate,   "FALLOCATE"   },
	[FUSE_DESTROY]	   = { do_destroy,     "DESTROY"     },
	[FUSE_NOTIFY_REPLY] = { NULL,    "NOTIFY_REPLY" },
	[FUSE_BATCH_FORGET] = { do_batch_forget, "BATCH_FORGET" },
	[FUSE_READDIRPLUS] = { do_readdirplus,	"READDIRPLUS"},
	[FUSE_RENAME2]     = { do_rename2,      "RENAME2"    },
	[FUSE_COPY_FILE_RANGE] = { do_copy_file_range, "COPY_FILE_RANGE" },
	[FUSE_SETUPMAPPING]  = { do_setupmapping, "SETUPMAPPING" },
	[FUSE_REMOVEMAPPING] = { do_removemapping, "REMOVEMAPPING" },
	[FUSE_SYNCFS] = { do_syncfs, "SYNCFS" },
};

static int
spdk_fuse_dispatcher_handle_fuse_req(struct spdk_fuse_dispatcher *disp, struct fuse_io *fuse_io)
{
	struct fuse_in_header *hdr;

	if (!fuse_io->in_iovcnt || !fuse_io->in_iov) {
		SPDK_ERRLOG("Bad IO: no IN iov (%d, %p)\n", fuse_io->in_iovcnt, fuse_io->in_iov);
		goto exit;
	}

	hdr = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*hdr));
	if (!hdr) {
		SPDK_ERRLOG("Bad IO: cannot get fuse_in_header\n");
		goto exit;
	}

	fuse_io->hdr.opcode = fsdev_io_d2h_u32(fuse_io, hdr->opcode);

	if (spdk_unlikely(!fuse_io->ch)) {
		/* FUSE_INIT is allowed with no channel. It'll open the fsdev and get channels */
		if (fuse_io->hdr.opcode != FUSE_INIT) {
			/* The fsdev is not currently active. Complete this request. */
			SPDK_ERRLOG("IO (%" PRIu32 ") arrived while there's no channel\n", fuse_io->hdr.opcode);
			goto exit;
		}
	}

	if (spdk_likely(_fuse_op_requires_reply(hdr->opcode))) {
		struct fuse_out_header *out_hdr = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*out_hdr));
		if (!out_hdr) {
			SPDK_ERRLOG("Bad IO: cannot get out_hdr\n");
			goto exit;
		}

		UNUSED(out_hdr); /* We don't need it here, we just made a check and a reservation */
	}

	if (fuse_io->hdr.opcode >= SPDK_COUNTOF(fuse_ll_ops)) {
		SPDK_ERRLOG("Bad IO: opt_code is out of range (%" PRIu32 " > %zu)\n", fuse_io->hdr.opcode,
			    SPDK_COUNTOF(fuse_ll_ops));
		fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
		return 0;
	}

	if (!fuse_ll_ops[fuse_io->hdr.opcode].func) {
		SPDK_ERRLOG("Bad IO: no handler for (%" PRIu32 ") %s\n", fuse_io->hdr.opcode,
			    fuse_ll_ops[fuse_io->hdr.opcode].name);
		fuse_dispatcher_io_complete_err(fuse_io, -ENOSYS);
		return 0;
	}

	fuse_io->hdr.len = fsdev_io_d2h_u32(fuse_io, hdr->len);
	fuse_io->hdr.unique = fsdev_io_d2h_u64(fuse_io, hdr->unique);
	fuse_io->hdr.nodeid = fsdev_io_d2h_u64(fuse_io, hdr->nodeid);
	fuse_io->hdr.uid = fsdev_io_d2h_u32(fuse_io, hdr->uid);
	fuse_io->hdr.gid = fsdev_io_d2h_u32(fuse_io, hdr->gid);
	fuse_io->hdr.pid = fsdev_io_d2h_u32(fuse_io, hdr->pid);

	SPDK_DEBUGLOG(fuse_dispatcher, "IO arrived: %" PRIu32 " (%s) len=%" PRIu32 " unique=%" PRIu64
		      " nodeid=%" PRIu64 " uid=%" PRIu32 " gid=%" PRIu32 " pid=%" PRIu32 "\n", fuse_io->hdr.opcode,
		      fuse_ll_ops[fuse_io->hdr.opcode].name, fuse_io->hdr.len, fuse_io->hdr.unique,
		      fuse_io->hdr.nodeid, fuse_io->hdr.uid, fuse_io->hdr.gid, fuse_io->hdr.pid);

	fuse_ll_ops[fuse_io->hdr.opcode].func(fuse_io);
	return 0;

exit:
	spdk_mempool_put(g_fuse_mgr.fuse_io_pool, fuse_io);
	return -EINVAL;
}

static int
fuse_dispatcher_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_fuse_dispatcher	*disp = __disp_from_io_dev(io_device);
	struct spdk_fuse_dispatcher_channel	*ch = ctx_buf;

	if (disp->desc) {
		ch->fsdev_io_ch = spdk_fsdev_get_io_channel(disp->desc);
	}

	return 0;
}

static void
fuse_dispatcher_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_fuse_dispatcher	*disp = __disp_from_io_dev(io_device);
	struct spdk_fuse_dispatcher_channel	*ch = ctx_buf;

	UNUSED(disp);

	if (ch->fsdev_io_ch) {
		assert(disp->desc);
		spdk_put_io_channel(ch->fsdev_io_ch);
		ch->fsdev_io_ch = NULL;
	}
}

struct fuse_dispatcher_create_ctx {
	struct spdk_fuse_dispatcher *disp;
	spdk_fuse_dispatcher_create_cpl_cb cb;
	void *cb_arg;
};

static void
fuse_dispatcher_get_channel_rollback(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *io_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_fuse_dispatcher_channel *ch = __disp_ch_from_io_ch(io_ch);

	if (ch->fsdev_io_ch) {
		spdk_put_io_channel(ch->fsdev_io_ch);
		ch->fsdev_io_ch = NULL;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
fuse_dispatcher_get_channel_rollback_done(struct spdk_io_channel_iter *i, int status)
{
	struct fuse_dispatcher_create_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fuse_dispatcher *disp = ctx->disp;

	if (status) {
		SPDK_WARNLOG("%s: getting channels failed with %d\n", fuse_dispatcher_name(disp), status);
		spdk_fsdev_close(disp->desc);
		free(disp);
		disp = NULL;
	}

	ctx->cb(ctx->cb_arg, disp);
	free(ctx);
}

static void
fuse_dispatcher_undo_create_get_channel(struct fuse_dispatcher_create_ctx *ctx)
{
	spdk_for_each_channel(__disp_to_io_dev(ctx->disp),
			      fuse_dispatcher_get_channel_rollback,
			      ctx,
			      fuse_dispatcher_get_channel_rollback_done);

}

static void
fuse_dispatcher_get_channel(struct spdk_io_channel_iter *i)
{
	struct fuse_dispatcher_create_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *io_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_fuse_dispatcher_channel *ch = __disp_ch_from_io_ch(io_ch);
	struct spdk_fuse_dispatcher *disp = ctx->disp;

	assert(!ch->fsdev_io_ch);

	ch->fsdev_io_ch = spdk_fsdev_get_io_channel(disp->desc);

	spdk_for_each_channel_continue(i, 0);
}

static void
fuse_dispatcher_get_channel_done(struct spdk_io_channel_iter *i, int status)
{
	struct fuse_dispatcher_create_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fuse_dispatcher *disp = ctx->disp;

	if (status) {
		SPDK_ERRLOG("%s: getting channels failed with %d\n", fuse_dispatcher_name(disp), status);
		fuse_dispatcher_undo_create_get_channel(ctx);
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: getting succeeded\n", fuse_dispatcher_name(disp));
	ctx->cb(ctx->cb_arg, disp);
	free(ctx);
}

int
spdk_fuse_dispatcher_create(const char *fsdev_name, spdk_fuse_dispatcher_event_cb event_cb,
			    void *event_ctx, spdk_fuse_dispatcher_create_cpl_cb cb, void *cb_arg)
{
	struct spdk_fuse_dispatcher *disp;
	struct fuse_dispatcher_create_ctx *ctx;
	char *io_dev_name;
	size_t fsdev_name_len;
	int rc;

	if (!fsdev_name || !event_cb || !cb) {
		SPDK_ERRLOG("Invalid params\n");
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("%s: could not alloc context\n", fsdev_name);
		return -ENOMEM;
	}

	io_dev_name = spdk_sprintf_alloc("fuse_disp_%s", fsdev_name);
	if (!io_dev_name) {
		SPDK_ERRLOG("Could not format io_dev name (%s)\n", fsdev_name);
		rc = -ENOMEM;
		goto io_dev_name_failed;
	}

	fsdev_name_len = strlen(fsdev_name);

	disp = calloc(1, sizeof(*disp) + fsdev_name_len + 1);
	if (!disp) {
		SPDK_ERRLOG("Could not allocate spdk_fuse_dispatcher\n");
		rc = -ENOMEM;
		goto disp_alloc_failed;
	}

	rc = spdk_fsdev_open(fsdev_name, fuse_dispatcher_fsdev_event_cb, disp, &disp->desc);
	if (rc) {
		SPDK_ERRLOG("Could not open fsdev %s (err=%d)\n", fsdev_name, rc);
		goto fsdev_open_failed;
	}

	pthread_mutex_lock(&g_fuse_mgr.lock);
	if (!g_fuse_mgr.ref_cnt) {
		struct spdk_fsdev_opts opts;
		spdk_fsdev_get_opts(&opts, sizeof(opts));

		g_fuse_mgr.fuse_io_pool = spdk_mempool_create("FUSE_disp_ios", opts.fsdev_io_pool_size,
					  sizeof(struct fuse_io), opts.fsdev_io_cache_size, SPDK_ENV_NUMA_ID_ANY);
		if (!g_fuse_mgr.fuse_io_pool) {
			pthread_mutex_unlock(&g_fuse_mgr.lock);
			SPDK_ERRLOG("Could not create mempool\n");
			rc = -ENOMEM;
			goto mempool_create_failed;
		}
	}
	g_fuse_mgr.ref_cnt++;
	pthread_mutex_unlock(&g_fuse_mgr.lock);

	spdk_io_device_register(__disp_to_io_dev(disp),
				fuse_dispatcher_channel_create, fuse_dispatcher_channel_destroy,
				sizeof(struct spdk_fuse_dispatcher_channel),
				io_dev_name);

	free(io_dev_name);

	memcpy(disp->fsdev_name, fsdev_name, fsdev_name_len + 1);
	disp->event_cb = event_cb;
	disp->event_ctx = event_ctx;
	disp->fuse_arch = SPDK_FUSE_ARCH_NATIVE;
	disp->fsdev_thread = spdk_get_thread();

	ctx->disp = disp;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(__disp_to_io_dev(disp),
			      fuse_dispatcher_get_channel,
			      ctx,
			      fuse_dispatcher_get_channel_done);

	return 0;

mempool_create_failed:
	spdk_fsdev_close(disp->desc);
fsdev_open_failed:
	free(disp);
disp_alloc_failed:
	free(io_dev_name);
io_dev_name_failed:
	free(ctx);
	return rc;
}

int
spdk_fuse_dispatcher_set_arch(struct spdk_fuse_dispatcher *disp, enum spdk_fuse_arch fuse_arch)
{
	switch (fuse_arch) {
	case SPDK_FUSE_ARCH_NATIVE:
	case SPDK_FUSE_ARCH_X86:
	case SPDK_FUSE_ARCH_X86_64:
	case SPDK_FUSE_ARCH_ARM:
	case SPDK_FUSE_ARCH_ARM64:
		SPDK_NOTICELOG("FUSE arch set to %d\n", fuse_arch);
		disp->fuse_arch = fuse_arch;
		return 0;
	default:
		return -EINVAL;
	}
}

const char *
spdk_fuse_dispatcher_get_fsdev_name(struct spdk_fuse_dispatcher *disp)
{
	return fuse_dispatcher_name(disp);
}

struct spdk_io_channel *
spdk_fuse_dispatcher_get_io_channel(struct spdk_fuse_dispatcher *disp)
{
	return spdk_get_io_channel(__disp_to_io_dev(disp));
}

int
spdk_fuse_dispatcher_submit_request(struct spdk_fuse_dispatcher *disp,
				    struct spdk_io_channel *ch,
				    struct iovec *in_iov, int in_iovcnt,
				    struct iovec *out_iov, int out_iovcnt,
				    spdk_fuse_dispatcher_submit_cpl_cb clb, void *cb_arg)
{
	struct fuse_io *fuse_io;
	struct spdk_fuse_dispatcher_channel *disp_ch = __disp_ch_from_io_ch(ch);

	fuse_io = spdk_mempool_get(g_fuse_mgr.fuse_io_pool);

	if (!fuse_io) {
		SPDK_ERRLOG("We ran out of FUSE IOs\n");
		return -ENOBUFS;
	}

	fuse_io->disp = disp;
	fuse_io->ch = disp_ch->fsdev_io_ch;
	fuse_io->in_iov = in_iov;
	fuse_io->in_iovcnt = in_iovcnt;
	fuse_io->out_iov = out_iov;
	fuse_io->out_iovcnt = out_iovcnt;
	fuse_io->cpl_cb = clb;
	fuse_io->cpl_cb_arg = cb_arg;

	fuse_io->in_offs.iov_offs = 0;
	fuse_io->in_offs.buf_offs = 0;
	fuse_io->out_offs.iov_offs = 0;
	fuse_io->out_offs.buf_offs = 0;

	return spdk_fuse_dispatcher_handle_fuse_req(disp, fuse_io);
}

struct fuse_dispatcher_delete_ctx {
	struct spdk_fuse_dispatcher *disp;
	struct spdk_thread *thread;
	spdk_fuse_dispatcher_delete_cpl_cb cb;
	void *cb_arg;
};

static void
fuse_dispatcher_delete_done(struct fuse_dispatcher_delete_ctx *ctx, int status)
{
	struct spdk_fuse_dispatcher *disp = ctx->disp;

	if (!status) {
		SPDK_DEBUGLOG(fuse_dispatcher, "%s: deletion succeeded\n", fuse_dispatcher_name(disp));

		spdk_io_device_unregister(__disp_to_io_dev(disp), NULL);

		free(disp);

		pthread_mutex_lock(&g_fuse_mgr.lock);
		g_fuse_mgr.ref_cnt--;
		if (!g_fuse_mgr.ref_cnt) {
			spdk_mempool_free(g_fuse_mgr.fuse_io_pool);
			g_fuse_mgr.fuse_io_pool = NULL;
		}
		pthread_mutex_unlock(&g_fuse_mgr.lock);
	} else {
		SPDK_ERRLOG("%s: deletion failed with %d\n", fuse_dispatcher_name(disp), status);
	}

	ctx->cb(ctx->cb_arg, (uint32_t)(-status));
	free(ctx);
}

static void
fuse_dispatcher_delete_put_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *io_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_fuse_dispatcher_channel *ch = __disp_ch_from_io_ch(io_ch);

	if (ch->fsdev_io_ch) {
		spdk_put_io_channel(ch->fsdev_io_ch);
		ch->fsdev_io_ch = NULL;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
fuse_dispatcher_delete_done_msg(void *_ctx)
{
	struct fuse_dispatcher_delete_ctx *ctx = _ctx;

	fuse_dispatcher_delete_done(ctx, 0);
}

static void
fuse_dispatcher_delete_close_fsdev_msg(void *_ctx)
{
	struct fuse_dispatcher_delete_ctx *ctx = _ctx;
	struct spdk_fuse_dispatcher *disp = ctx->disp;

	spdk_fsdev_close(disp->desc);

	spdk_thread_send_msg(ctx->thread, fuse_dispatcher_delete_done_msg, ctx);
}

static void
fuse_dispatcher_delete_put_channel_done(struct spdk_io_channel_iter *i, int status)
{
	struct fuse_dispatcher_delete_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fuse_dispatcher *disp = ctx->disp;

	if (status) {
		SPDK_ERRLOG("%s: putting channels failed with %d\n", fuse_dispatcher_name(disp), status);
		fuse_dispatcher_delete_done(ctx, status);
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: putting channels succeeded. Releasing the fdev\n",
		      fuse_dispatcher_name(disp));

	spdk_thread_send_msg(disp->fsdev_thread, fuse_dispatcher_delete_close_fsdev_msg, ctx);
}

int
spdk_fuse_dispatcher_delete(struct spdk_fuse_dispatcher *disp,
			    spdk_fuse_dispatcher_delete_cpl_cb cb, void *cb_arg)
{
	struct fuse_dispatcher_delete_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("cannot allocate context\n");
		return -ENOMEM;
	}

	ctx->disp = disp;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->thread = spdk_get_thread();

	if (disp->desc) {
		SPDK_DEBUGLOG(fuse_dispatcher, "%s: fsdev still open. Releasing the channels.\n",
			      fuse_dispatcher_name(disp));

		spdk_for_each_channel(__disp_to_io_dev(disp),
				      fuse_dispatcher_delete_put_channel,
				      ctx,
				      fuse_dispatcher_delete_put_channel_done);
	} else {
		fuse_dispatcher_delete_done(ctx, 0);
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(fuse_dispatcher)
