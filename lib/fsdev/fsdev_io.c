/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/fsdev.h"
#include "spdk/fsdev_module.h"
#include "fsdev_internal.h"

#define CALL_USR_CLB(_fsdev_io, ch, type, ...) \
	do { \
		type *usr_cb_fn = _fsdev_io->internal.usr_cb_fn; \
		usr_cb_fn(_fsdev_io->internal.usr_cb_arg, ch, _fsdev_io->internal.status, ## __VA_ARGS__); \
	} while (0)

#define CALL_USR_NO_STATUS_CLB(_fsdev_io, ch, type, ...) \
	do { \
		type *usr_cb_fn = _fsdev_io->internal.usr_cb_fn; \
		usr_cb_fn(_fsdev_io->internal.usr_cb_arg, ch, ## __VA_ARGS__); \
	} while (0)

static struct spdk_fsdev_io *
fsdev_io_get_and_fill(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      void *usr_cb_fn, void *usr_cb_arg, spdk_fsdev_io_completion_cb cb_fn, void *cb_arg,
		      enum spdk_fsdev_io_type type)
{
	struct spdk_fsdev_io *fsdev_io;
	struct spdk_fsdev_channel *channel = __io_ch_to_fsdev_ch(ch);

	fsdev_io = fsdev_channel_get_io(channel);
	if (!fsdev_io) {
		return NULL;
	}

	fsdev_io->fsdev = spdk_fsdev_desc_get_fsdev(desc);
	fsdev_io->internal.ch = channel;
	fsdev_io->internal.desc = desc;
	fsdev_io->internal.type = type;
	fsdev_io->internal.unique = unique;
	fsdev_io->internal.usr_cb_fn = usr_cb_fn;
	fsdev_io->internal.usr_cb_arg = usr_cb_arg;
	fsdev_io->internal.cb_arg = cb_arg;
	fsdev_io->internal.cb_fn = cb_fn;
	fsdev_io->internal.status = -ENOSYS;
	fsdev_io->internal.in_submit_request = false;

	return fsdev_io;
}

static inline void
fsdev_io_free(struct spdk_fsdev_io *fsdev_io)
{
	spdk_fsdev_free_io(fsdev_io);
}

static void
_spdk_fsdev_mount_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_mount_cpl_cb, &fsdev_io->u_out.mount.opts,
		     fsdev_io->u_out.mount.root_fobject);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_mount(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		 uint64_t unique, const struct spdk_fsdev_mount_opts *opts,
		 spdk_fsdev_mount_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_mount_cb, ch,
					 SPDK_FSDEV_IO_MOUNT);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.mount.opts = *opts;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_umount_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_NO_STATUS_CLB(fsdev_io, ch, spdk_fsdev_umount_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_umount(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		  uint64_t unique, spdk_fsdev_umount_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_umount_cb, ch,
					 SPDK_FSDEV_IO_UMOUNT);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io_submit(fsdev_io);
	return 0;

}

static void
_spdk_fsdev_lookup_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_lookup_cpl_cb, fsdev_io->u_out.lookup.fobject,
		     &fsdev_io->u_out.lookup.attr);

	free(fsdev_io->u_in.lookup.name);
	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_lookup(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		  struct spdk_fsdev_file_object *parent_fobject, const char *name,
		  spdk_fsdev_lookup_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_lookup_cb, ch,
					 SPDK_FSDEV_IO_LOOKUP);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.lookup.name = strdup(name);
	if (!fsdev_io->u_in.lookup.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.lookup.parent_fobject = parent_fobject;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_forget_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_forget_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_forget(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		  struct spdk_fsdev_file_object *fobject, uint64_t nlookup,
		  spdk_fsdev_forget_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_forget_cb, ch,
					 SPDK_FSDEV_IO_FORGET);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.forget.fobject = fobject;
	fsdev_io->u_in.forget.nlookup = nlookup;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_getattr_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_getattr_cpl_cb, &fsdev_io->u_out.getattr.attr);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_getattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		   struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		   spdk_fsdev_getattr_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_getattr_cb, ch,
					 SPDK_FSDEV_IO_GETATTR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.getattr.fobject = fobject;
	fsdev_io->u_in.getattr.fhandle = fhandle;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_setattr_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_setattr_cpl_cb, &fsdev_io->u_out.setattr.attr);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_setattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		   struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		   const struct spdk_fsdev_file_attr *attr, uint32_t to_set,
		   spdk_fsdev_setattr_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_setattr_cb, ch,
					 SPDK_FSDEV_IO_SETATTR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.setattr.fobject = fobject;
	fsdev_io->u_in.setattr.fhandle = fhandle;
	fsdev_io->u_in.setattr.attr = *attr;
	fsdev_io->u_in.setattr.to_set = to_set;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_readlink_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_readlink_cpl_cb, fsdev_io->u_out.readlink.linkname);

	free(fsdev_io->u_out.readlink.linkname);
	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_readlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, spdk_fsdev_readlink_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_readlink_cb, ch,
					 SPDK_FSDEV_IO_READLINK);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.readlink.fobject = fobject;
	fsdev_io->u_out.readlink.linkname = NULL;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_symlink_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_symlink_cpl_cb, fsdev_io->u_out.symlink.fobject,
		     &fsdev_io->u_out.symlink.attr);

	free(fsdev_io->u_in.symlink.target);
	free(fsdev_io->u_in.symlink.linkpath);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_symlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		   struct spdk_fsdev_file_object *parent_fobject, const char *target, const char *linkpath,
		   uid_t euid, gid_t egid, spdk_fsdev_symlink_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_symlink_cb, ch,
					 SPDK_FSDEV_IO_SYMLINK);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.symlink.target = strdup(target);
	if (!fsdev_io->u_in.symlink.target) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.symlink.linkpath = strdup(linkpath);
	if (!fsdev_io) {
		fsdev_io_free(fsdev_io);
		free(fsdev_io->u_in.symlink.target);
		return -ENOMEM;
	}

	fsdev_io->u_in.symlink.parent_fobject = parent_fobject;
	fsdev_io->u_in.symlink.euid = euid;
	fsdev_io->u_in.symlink.egid = egid;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_mknod_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_mknod_cpl_cb, fsdev_io->u_out.mknod.fobject,
		     &fsdev_io->u_out.mknod.attr);

	free(fsdev_io->u_in.mknod.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_mknod(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode, dev_t rdev,
		 uid_t euid, gid_t egid, spdk_fsdev_mknod_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_mknod_cb, ch,
					 SPDK_FSDEV_IO_MKNOD);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.mknod.name = strdup(name);
	if (!fsdev_io->u_in.mknod.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.mknod.parent_fobject = parent_fobject;
	fsdev_io->u_in.mknod.mode = mode;
	fsdev_io->u_in.mknod.rdev = rdev;
	fsdev_io->u_in.mknod.euid = euid;
	fsdev_io->u_in.mknod.egid = egid;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_mkdir_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_mkdir_cpl_cb, fsdev_io->u_out.mkdir.fobject,
		     &fsdev_io->u_out.mkdir.attr);

	free(fsdev_io->u_in.mkdir.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_mkdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode,
		 uid_t euid, gid_t egid, spdk_fsdev_mkdir_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_mkdir_cb, ch,
					 SPDK_FSDEV_IO_MKDIR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.mkdir.name = strdup(name);
	if (!fsdev_io->u_in.mkdir.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.mkdir.parent_fobject = parent_fobject;
	fsdev_io->u_in.mkdir.mode = mode;
	fsdev_io->u_in.mkdir.euid = euid;
	fsdev_io->u_in.mkdir.egid = egid;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_unlink_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_unlink_cpl_cb);

	free(fsdev_io->u_in.unlink.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_unlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		  struct spdk_fsdev_file_object *parent_fobject, const char *name,
		  spdk_fsdev_unlink_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_unlink_cb, ch,
					 SPDK_FSDEV_IO_UNLINK);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.unlink.name = strdup(name);
	if (!fsdev_io->u_in.unlink.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.unlink.parent_fobject = parent_fobject;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_rmdir_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_rmdir_cpl_cb);

	free(fsdev_io->u_in.rmdir.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_rmdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *parent_fobject, const char *name,
		 spdk_fsdev_rmdir_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_rmdir_cb, ch,
					 SPDK_FSDEV_IO_RMDIR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.rmdir.name = strdup(name);
	if (!fsdev_io->u_in.rmdir.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.rmdir.parent_fobject = parent_fobject;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_rename_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_rename_cpl_cb);

	free(fsdev_io->u_in.rename.name);
	free(fsdev_io->u_in.rename.new_name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_rename(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		  struct spdk_fsdev_file_object *parent_fobject, const char *name,
		  struct spdk_fsdev_file_object *new_parent_fobject, const char *new_name,
		  uint32_t flags, spdk_fsdev_rename_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_rename_cb, ch,
					 SPDK_FSDEV_IO_RENAME);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.rename.name = strdup(name);
	if (!fsdev_io->u_in.rename.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.rename.new_name = strdup(new_name);
	if (!fsdev_io->u_in.rename.new_name) {
		free(fsdev_io->u_in.rename.name);
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.rename.parent_fobject = parent_fobject;
	fsdev_io->u_in.rename.new_parent_fobject = new_parent_fobject;
	fsdev_io->u_in.rename.flags = flags;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_link_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_link_cpl_cb, fsdev_io->u_out.link.fobject,
		     &fsdev_io->u_out.link.attr);

	free(fsdev_io->u_in.link.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_link(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_object *new_parent_fobject,
		const char *name, spdk_fsdev_link_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_link_cb, ch,
					 SPDK_FSDEV_IO_LINK);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.link.name = strdup(name);
	if (!fsdev_io->u_in.link.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.link.fobject = fobject;
	fsdev_io->u_in.link.new_parent_fobject = new_parent_fobject;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_fopen_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_fopen_cpl_cb, fsdev_io->u_out.open.fhandle);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_fopen(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *fobject, uint32_t flags,
		 spdk_fsdev_fopen_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_fopen_cb, ch,
					 SPDK_FSDEV_IO_OPEN);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.open.fobject = fobject;
	fsdev_io->u_in.open.flags = flags;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_read_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_read_cpl_cb, fsdev_io->u_out.read.data_size);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_read(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		size_t size, uint64_t offs, uint32_t flags,
		struct iovec *iov, uint32_t iovcnt, struct spdk_fsdev_io_opts *opts,
		spdk_fsdev_read_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_read_cb, ch,
					 SPDK_FSDEV_IO_READ);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.read.fobject = fobject;
	fsdev_io->u_in.read.fhandle = fhandle;
	fsdev_io->u_in.read.size = size;
	fsdev_io->u_in.read.offs = offs;
	fsdev_io->u_in.read.flags = flags;
	fsdev_io->u_in.read.iov = iov;
	fsdev_io->u_in.read.iovcnt = iovcnt;
	fsdev_io->u_in.read.opts = opts;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_write_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_write_cpl_cb, fsdev_io->u_out.write.data_size);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_write(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		 size_t size, uint64_t offs, uint64_t flags,
		 const struct iovec *iov, uint32_t iovcnt, struct spdk_fsdev_io_opts *opts,
		 spdk_fsdev_write_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_write_cb, ch,
					 SPDK_FSDEV_IO_WRITE);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.write.fobject = fobject;
	fsdev_io->u_in.write.fhandle = fhandle;
	fsdev_io->u_in.write.size = size;
	fsdev_io->u_in.write.offs = offs;
	fsdev_io->u_in.write.flags = flags;
	fsdev_io->u_in.write.iov = iov;
	fsdev_io->u_in.write.iovcnt = iovcnt;
	fsdev_io->u_in.write.opts = opts;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_statfs_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_statfs_cpl_cb, &fsdev_io->u_out.statfs.statfs);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_statfs(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		  struct spdk_fsdev_file_object *fobject, spdk_fsdev_statfs_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_statfs_cb, ch,
					 SPDK_FSDEV_IO_STATFS);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.statfs.fobject = fobject;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_release_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_release_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_release(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		   struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		   spdk_fsdev_release_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_release_cb, ch,
					 SPDK_FSDEV_IO_RELEASE);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.release.fobject = fobject;
	fsdev_io->u_in.release.fhandle = fhandle;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_fsync_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_fsync_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_fsync(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, bool datasync,
		 spdk_fsdev_fsync_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_fsync_cb, ch,
					 SPDK_FSDEV_IO_FSYNC);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.fsync.fobject = fobject;
	fsdev_io->u_in.fsync.fhandle = fhandle;
	fsdev_io->u_in.fsync.datasync = datasync;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_setxattr_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_setxattr_cpl_cb);

	free(fsdev_io->u_in.setxattr.value);
	free(fsdev_io->u_in.setxattr.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_setxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, const char *name, const char *value, size_t size,
		    uint32_t flags, spdk_fsdev_setxattr_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_setxattr_cb, ch,
					 SPDK_FSDEV_IO_SETXATTR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.setxattr.name = strdup(name);
	if (!fsdev_io->u_in.setxattr.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.setxattr.value = malloc(size);
	if (!fsdev_io->u_in.setxattr.value) {
		free(fsdev_io->u_in.setxattr.name);
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	memcpy(fsdev_io->u_in.setxattr.value, value, size);
	fsdev_io->u_in.setxattr.fobject = fobject;
	fsdev_io->u_in.setxattr.size = size;
	fsdev_io->u_in.setxattr.flags = flags;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_getxattr_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_getxattr_cpl_cb, fsdev_io->u_out.getxattr.value_size);

	free(fsdev_io->u_in.getxattr.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_getxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, const char *name, void *buffer, size_t size,
		    spdk_fsdev_getxattr_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_getxattr_cb, ch,
					 SPDK_FSDEV_IO_GETXATTR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.getxattr.name = strdup(name);
	if (!fsdev_io->u_in.getxattr.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.getxattr.fobject = fobject;
	fsdev_io->u_in.getxattr.buffer = buffer;
	fsdev_io->u_in.getxattr.size = size;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_listxattr_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_listxattr_cpl_cb, fsdev_io->u_out.listxattr.data_size,
		     fsdev_io->u_out.listxattr.size_only);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_listxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, char *buffer, size_t size,
		     spdk_fsdev_listxattr_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_listxattr_cb, ch,
					 SPDK_FSDEV_IO_LISTXATTR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.listxattr.fobject = fobject;
	fsdev_io->u_in.listxattr.buffer = buffer;
	fsdev_io->u_in.listxattr.size = size;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_removexattr_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_removexattr_cpl_cb);

	free(fsdev_io->u_in.removexattr.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_removexattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *fobject, const char *name,
		       spdk_fsdev_removexattr_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_removexattr_cb, ch,
					 SPDK_FSDEV_IO_REMOVEXATTR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.removexattr.name = strdup(name);
	if (!fsdev_io->u_in.removexattr.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.removexattr.fobject = fobject;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_flush_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_flush_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_flush(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		 spdk_fsdev_flush_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_flush_cb, ch,
					 SPDK_FSDEV_IO_FLUSH);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.flush.fobject = fobject;
	fsdev_io->u_in.flush.fhandle = fhandle;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_opendir_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_opendir_cpl_cb, fsdev_io->u_out.opendir.fhandle);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_opendir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		   struct spdk_fsdev_file_object *fobject, uint32_t flags,
		   spdk_fsdev_opendir_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_opendir_cb, ch,
					 SPDK_FSDEV_IO_OPENDIR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.opendir.fobject = fobject;
	fsdev_io->u_in.opendir.flags = flags;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static int
_spdk_fsdev_readdir_entry_clb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	spdk_fsdev_readdir_entry_cb *usr_entry_cb_fn = fsdev_io->u_in.readdir.usr_entry_cb_fn;
	struct spdk_io_channel *ch = cb_arg;

	return usr_entry_cb_fn(fsdev_io->internal.usr_cb_arg, ch, fsdev_io->u_out.readdir.name,
			       fsdev_io->u_out.readdir.fobject, &fsdev_io->u_out.readdir.attr, fsdev_io->u_out.readdir.offset);
}

static void
_spdk_fsdev_readdir_emum_clb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_readdir_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_readdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		   struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, uint64_t offset,
		   spdk_fsdev_readdir_entry_cb entry_cb_fn, spdk_fsdev_readdir_cpl_cb cpl_cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cpl_cb_fn, cb_arg,
					 _spdk_fsdev_readdir_emum_clb, ch, SPDK_FSDEV_IO_READDIR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.readdir.fobject = fobject;
	fsdev_io->u_in.readdir.fhandle = fhandle;
	fsdev_io->u_in.readdir.offset = offset;
	fsdev_io->u_in.readdir.entry_cb_fn = _spdk_fsdev_readdir_entry_clb;
	fsdev_io->u_in.readdir.usr_entry_cb_fn = entry_cb_fn;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_releasedir_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_releasedir_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_releasedir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		      spdk_fsdev_releasedir_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_releasedir_cb, ch,
					 SPDK_FSDEV_IO_RELEASEDIR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.releasedir.fobject = fobject;
	fsdev_io->u_in.releasedir.fhandle = fhandle;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_fsyncdir_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_fsyncdir_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_fsyncdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, bool datasync,
		    spdk_fsdev_fsyncdir_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_fsyncdir_cb, ch,
					 SPDK_FSDEV_IO_FSYNCDIR);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.fsyncdir.fobject = fobject;
	fsdev_io->u_in.fsyncdir.fhandle = fhandle;
	fsdev_io->u_in.fsyncdir.datasync = datasync;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_flock_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_flock_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_flock(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		 struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, int operation,
		 spdk_fsdev_flock_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_flock_cb, ch,
					 SPDK_FSDEV_IO_FLOCK);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.flock.fobject = fobject;
	fsdev_io->u_in.flock.fhandle = fhandle;
	fsdev_io->u_in.flock.operation = operation;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_create_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_create_cpl_cb, fsdev_io->u_out.create.fobject,
		     &fsdev_io->u_out.create.attr, fsdev_io->u_out.create.fhandle);

	free(fsdev_io->u_in.create.name);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_create(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		  struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode, uint32_t flags,
		  mode_t umask, uid_t euid, gid_t egid, spdk_fsdev_create_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_create_cb, ch,
					 SPDK_FSDEV_IO_CREATE);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.create.name = strdup(name);
	if (!fsdev_io->u_in.create.name) {
		fsdev_io_free(fsdev_io);
		return -ENOMEM;
	}

	fsdev_io->u_in.create.parent_fobject = parent_fobject;
	fsdev_io->u_in.create.mode = mode;
	fsdev_io->u_in.create.flags = flags;
	fsdev_io->u_in.create.umask = umask;
	fsdev_io->u_in.create.euid = euid;
	fsdev_io->u_in.create.egid = egid;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_interrupt_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_abort_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_abort(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		 uint64_t unique_to_abort, spdk_fsdev_abort_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, 0, cb_fn, cb_arg, _spdk_fsdev_interrupt_cb, ch,
					 SPDK_FSDEV_IO_ABORT);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.abort.unique_to_abort = unique_to_abort;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_fallocate_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_fallocate_cpl_cb);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_fallocate(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		     int mode, off_t offset, off_t length,
		     spdk_fsdev_fallocate_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_fallocate_cb, ch,
					 SPDK_FSDEV_IO_FALLOCATE);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.fallocate.fobject = fobject;
	fsdev_io->u_in.fallocate.fhandle = fhandle;
	fsdev_io->u_in.fallocate.mode = mode;
	fsdev_io->u_in.fallocate.offset = offset;
	fsdev_io->u_in.fallocate.length = length;

	fsdev_io_submit(fsdev_io);
	return 0;
}

static void
_spdk_fsdev_copy_file_range_cb(struct spdk_fsdev_io *fsdev_io, void *cb_arg)
{
	struct spdk_io_channel *ch = cb_arg;

	CALL_USR_CLB(fsdev_io, ch, spdk_fsdev_copy_file_range_cpl_cb,
		     fsdev_io->u_out.copy_file_range.data_size);

	fsdev_io_free(fsdev_io);
}

int
spdk_fsdev_copy_file_range(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t unique,
			   struct spdk_fsdev_file_object *fobject_in, struct spdk_fsdev_file_handle *fhandle_in, off_t off_in,
			   struct spdk_fsdev_file_object *fobject_out, struct spdk_fsdev_file_handle *fhandle_out,
			   off_t off_out, size_t len, uint32_t flags,
			   spdk_fsdev_copy_file_range_cpl_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_io *fsdev_io;

	fsdev_io = fsdev_io_get_and_fill(desc, ch, unique, cb_fn, cb_arg, _spdk_fsdev_copy_file_range_cb,
					 ch,
					 SPDK_FSDEV_IO_COPY_FILE_RANGE);
	if (!fsdev_io) {
		return -ENOBUFS;
	}

	fsdev_io->u_in.copy_file_range.fobject_in = fobject_in;
	fsdev_io->u_in.copy_file_range.fhandle_in = fhandle_in;
	fsdev_io->u_in.copy_file_range.off_in = off_in;
	fsdev_io->u_in.copy_file_range.fobject_out = fobject_out;
	fsdev_io->u_in.copy_file_range.fhandle_out = fhandle_out;
	fsdev_io->u_in.copy_file_range.off_out = off_out;
	fsdev_io->u_in.copy_file_range.len = len;
	fsdev_io->u_in.copy_file_range.flags = flags;

	fsdev_io_submit(fsdev_io);
	return 0;
}
