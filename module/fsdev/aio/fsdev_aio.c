/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/config.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "aio_mgr.h"
#include "fsdev_aio.h"

#define IO_STATUS_ASYNC INT_MIN

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/* See https://libfuse.github.io/doxygen/structfuse__conn__info.html */
#define MAX_BACKGROUND (100)
#define TIME_GRAN (1)
#define MAX_AIOS 256
#define DEFAULT_WRITEBACK_CACHE true
#define DEFAULT_MAX_WRITE 0x00020000
#define DEFAULT_XATTR_ENABLED false
#define DEFAULT_SKIP_RW false
#define DEFAULT_TIMEOUT_MS 0 /* to prevent the attribute caching */

#ifdef SPDK_CONFIG_HAVE_STRUCT_STAT_ST_ATIM
/* Linux */
#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atim.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctim.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtim.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atim.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctim.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtim.tv_nsec = (val)
#elif defined(SPDK_CONFIG_HAVE_STRUCT_STAT_ST_ATIMESPEC)
/* FreeBSD */
#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atimespec.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctimespec.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtimespec.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atimespec.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctimespec.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtimespec.tv_nsec = (val)
#else
#define ST_ATIM_NSEC(stbuf) 0
#define ST_CTIM_NSEC(stbuf) 0
#define ST_MTIM_NSEC(stbuf) 0
#define ST_ATIM_NSEC_SET(stbuf, val) do { } while (0)
#define ST_CTIM_NSEC_SET(stbuf, val) do { } while (0)
#define ST_MTIM_NSEC_SET(stbuf, val) do { } while (0)
#endif

struct lo_cred {
	uid_t euid;
	gid_t egid;
};

/** Inode number type */
typedef uint64_t spdk_ino_t;

struct lo_key {
	ino_t ino;
	dev_t dev;
};

struct spdk_fsdev_file_handle {
	int fd;
	struct {
		DIR *dp;
		struct dirent *entry;
		off_t offset;
	} dir;
	struct spdk_fsdev_file_object *fobject;
	TAILQ_ENTRY(spdk_fsdev_file_handle) link;
};

#define FOBJECT_FMT "ino=%" PRIu64 " dev=%" PRIu64
#define FOBJECT_ARGS(fo) ((uint64_t)(fo)->key.ino), ((uint64_t)(fo)->key.dev)
struct spdk_fsdev_file_object {
	uint32_t is_symlink : 1;
	uint32_t is_dir : 1;
	uint32_t reserved : 30;
	int fd;
	char *fd_str;
	struct lo_key key;
	uint64_t refcount;
	struct spdk_fsdev_file_object *parent_fobject;
	TAILQ_ENTRY(spdk_fsdev_file_object) link;
	TAILQ_HEAD(, spdk_fsdev_file_object) leafs;
	TAILQ_HEAD(, spdk_fsdev_file_handle) handles;
	struct spdk_spinlock lock;
	char name[];
};

struct aio_fsdev {
	struct spdk_fsdev fsdev;
	struct spdk_fsdev_mount_opts mount_opts;
	char *root_path;
	int proc_self_fd;
	pthread_mutex_t mutex;
	struct spdk_fsdev_file_object *root;
	TAILQ_ENTRY(aio_fsdev) tailq;
	bool xattr_enabled;
	bool skip_rw;
};

struct aio_fsdev_io {
	struct spdk_aio_mgr_io *aio;
	struct aio_io_channel *ch;
	TAILQ_ENTRY(aio_fsdev_io) link;
};

struct aio_io_channel {
	struct spdk_poller *poller;
	struct spdk_aio_mgr *mgr;
	TAILQ_HEAD(, aio_fsdev_io) ios_in_progress;
	TAILQ_HEAD(, aio_fsdev_io) ios_to_complete;
};

static TAILQ_HEAD(, aio_fsdev) g_aio_fsdev_head = TAILQ_HEAD_INITIALIZER(
			g_aio_fsdev_head);

static inline struct aio_fsdev *
fsdev_to_aio_fsdev(struct spdk_fsdev *fsdev)
{
	return SPDK_CONTAINEROF(fsdev, struct aio_fsdev, fsdev);
}

static inline struct spdk_fsdev_io *
aio_to_fsdev_io(const struct aio_fsdev_io *aio_io)
{
	return SPDK_CONTAINEROF(aio_io, struct spdk_fsdev_io, driver_ctx);
}

static inline struct aio_fsdev_io *
fsdev_to_aio_io(const struct spdk_fsdev_io *fsdev_io)
{
	return (struct aio_fsdev_io *)fsdev_io->driver_ctx;
}

static inline bool
fsdev_aio_is_valid_fobject(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_object *fobject)
{
	return fobject != NULL;
}

static inline bool
fsdev_aio_is_valid_fhandle(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_handle *fhandle)
{
	return fhandle != NULL;
}

static int
is_dot_or_dotdot(const char *name)
{
	return name[0] == '.' && (name[1] == '\0' ||
				  (name[1] == '.' && name[2] == '\0'));
}

/* Is `path` a single path component that is not "." or ".."? */
static int
is_safe_path_component(const char *path)
{
	if (strchr(path, '/')) {
		return 0;
	}

	return !is_dot_or_dotdot(path);
}

static struct spdk_fsdev_file_object *
lo_find_leaf_unsafe(struct spdk_fsdev_file_object *fobject, ino_t ino, dev_t dev)
{
	struct spdk_fsdev_file_object *leaf_fobject;

	TAILQ_FOREACH(leaf_fobject, &fobject->leafs, link) {
		if (leaf_fobject->key.ino == ino && leaf_fobject->key.dev == dev) {
			return leaf_fobject;
		}
	}

	return NULL;
}

/* This function returns:
 * 1 if the refcount is still non zero
 * a negative  error number if the refcount became zero, the file object was deleted but the defered underlying file deletion failed
 * 0 if the refcount became zero, the file object was deleted and eithr the underlying file deletion wasn't defered or succeeded
 */
static int
file_object_unref(struct spdk_fsdev_file_object *fobject, uint32_t count)
{
	int res = 0;

	spdk_spin_lock(&fobject->lock);
	assert(fobject->refcount >= count);
	fobject->refcount -= count;
	spdk_spin_unlock(&fobject->lock);

	if (!fobject->refcount) {
		struct spdk_fsdev_file_object *parent_fobject = fobject->parent_fobject;

		if (parent_fobject) {
			spdk_spin_lock(&parent_fobject->lock);
			TAILQ_REMOVE(&parent_fobject->leafs, fobject, link);
			spdk_spin_unlock(&parent_fobject->lock);
			file_object_unref(parent_fobject, 1); /* unref by the leaf */
		}

		spdk_spin_destroy(&fobject->lock);
		close(fobject->fd);
		free(fobject->fd_str);
		free(fobject);
	}

	return res;
}

static void
file_object_ref(struct spdk_fsdev_file_object *fobject)
{
	spdk_spin_lock(&fobject->lock);
	fobject->refcount++;
	spdk_spin_unlock(&fobject->lock);
}

static struct spdk_fsdev_file_object *
file_object_create_unsafe(struct spdk_fsdev_file_object *parent_fobject, int fd, ino_t ino,
			  dev_t dev, mode_t mode)
{
	struct spdk_fsdev_file_object *fobject;

	fobject = calloc(1, sizeof(*fobject));
	if (!fobject) {
		SPDK_ERRLOG("Cannot alloc fobject\n");
		return NULL;
	}

	fobject->fd_str = spdk_sprintf_alloc("%d", fd);
	if (!fobject->fd_str) {
		SPDK_ERRLOG("Cannot alloc fd_str\n");
		free(fobject);
		return NULL;
	}

	fobject->fd = fd;
	fobject->key.ino = ino;
	fobject->key.dev = dev;
	fobject->refcount = 1;
	fobject->is_symlink = S_ISLNK(mode) ? 1 : 0;
	fobject->is_dir = S_ISDIR(mode) ? 1 : 0;

	TAILQ_INIT(&fobject->handles);
	TAILQ_INIT(&fobject->leafs);
	spdk_spin_init(&fobject->lock);

	if (parent_fobject) {
		fobject->parent_fobject = parent_fobject;
		TAILQ_INSERT_TAIL(&parent_fobject->leafs, fobject, link);
		parent_fobject->refcount++;
	}

	return fobject;
}

static struct spdk_fsdev_file_handle *
file_handle_create(struct spdk_fsdev_file_object *fobject, int fd)
{
	struct spdk_fsdev_file_handle *fhandle;

	fhandle = calloc(1, sizeof(*fhandle));
	if (!fhandle) {
		SPDK_ERRLOG("Cannot alloc fhandle\n");
		return NULL;
	}

	fhandle->fobject = fobject;
	fhandle->fd = fd;

	spdk_spin_lock(&fobject->lock);
	fobject->refcount++;
	TAILQ_INSERT_TAIL(&fobject->handles, fhandle, link);
	spdk_spin_unlock(&fobject->lock);

	return fhandle;
}

static void
file_handle_delete(struct spdk_fsdev_file_handle *fhandle)
{
	struct spdk_fsdev_file_object *fobject = fhandle->fobject;

	spdk_spin_lock(&fobject->lock);
	fobject->refcount--;
	TAILQ_REMOVE(&fobject->handles, fhandle, link);
	spdk_spin_unlock(&fobject->lock);

	if (fhandle->dir.dp) {
		closedir(fhandle->dir.dp);
	}

	close(fhandle->fd);
	free(fhandle);
}

static int
file_object_fill_attr(struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_attr *attr)
{
	struct stat stbuf;
	int res;

	res = fstatat(fobject->fd, "", &stbuf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatat() failed with %d\n", res);
		return res;
	}

	memset(attr, 0, sizeof(*attr));

	attr->ino = stbuf.st_ino;
	attr->size = stbuf.st_size;
	attr->blocks = stbuf.st_blocks;
	attr->atime = stbuf.st_atime;
	attr->mtime = stbuf.st_mtime;
	attr->ctime = stbuf.st_ctime;
	attr->atimensec = ST_ATIM_NSEC(&stbuf);
	attr->mtimensec = ST_MTIM_NSEC(&stbuf);
	attr->ctimensec = ST_CTIM_NSEC(&stbuf);
	attr->mode = stbuf.st_mode;
	attr->nlink = stbuf.st_nlink;
	attr->uid = stbuf.st_uid;
	attr->gid = stbuf.st_gid;
	attr->rdev = stbuf.st_rdev;
	attr->blksize = stbuf.st_blksize;
	attr->valid_ms = DEFAULT_TIMEOUT_MS;

	return 0;
}

static int
utimensat_empty(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_object *fobject,
		const struct timespec *tv)
{
	int res;

	if (fobject->is_symlink) {
		res = utimensat(fobject->fd, "", tv, AT_EMPTY_PATH);
		if (res == -1 && errno == EINVAL) {
			/* Sorry, no race free way to set times on symlink. */
			errno = EPERM;
		}
	} else {
		res = utimensat(vfsdev->proc_self_fd, fobject->fd_str, tv, 0);
	}

	return res;
}

static void
fsdev_free_leafs(struct spdk_fsdev_file_object *fobject, bool unref_fobject)
{
	while (!TAILQ_EMPTY(&fobject->handles)) {
		struct spdk_fsdev_file_handle *fhandle = TAILQ_FIRST(&fobject->handles);
		file_handle_delete(fhandle);
#ifdef __clang_analyzer__
		/*
		 * scan-build fails to comprehend that file_handle_delete() removes the fhandle
		 * from the queue, so it thinks it's remained accessible and throws the "Use of
		 * memory after it is freed" error here.
		 * The loop below "teaches" the scan-build that the freed fhandle is not on the
		 * list anymore and supresses the error in this way.
		 */
		struct spdk_fsdev_file_handle *tmp;
		TAILQ_FOREACH(tmp, &fobject->handles, link) {
			assert(tmp != fhandle);
		}
#endif
	}

	while (!TAILQ_EMPTY(&fobject->leafs)) {
		struct spdk_fsdev_file_object *leaf_fobject = TAILQ_FIRST(&fobject->leafs);
		fsdev_free_leafs(leaf_fobject, true);
	}

	if (fobject->refcount && unref_fobject) {
		/* if still referenced - zero refcount */
		int res = file_object_unref(fobject, fobject->refcount);
		assert(res == 0);
		UNUSED(res);
	}
}

static int
lo_getattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.getattr.fobject;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = file_object_fill_attr(fobject, &fsdev_io->u_out.getattr.attr);
	if (res) {
		SPDK_ERRLOG("Cannot fill attr for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), res);
		return res;
	}

	SPDK_DEBUGLOG(fsdev_aio, "GETATTR succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
	return 0;
}

static int
lo_opendir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int error;
	int fd;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.opendir.fobject;
	uint32_t flags = fsdev_io->u_in.opendir.flags;
	struct spdk_fsdev_file_handle *fhandle = NULL;

	UNUSED(flags);

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(fobject->fd, ".", O_RDONLY);
	if (fd == -1) {
		error = -errno;
		SPDK_ERRLOG("openat failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), error);
		goto out_err;
	}

	fhandle = file_handle_create(fobject, fd);
	if (fhandle == NULL) {
		error = -ENOMEM;
		SPDK_ERRLOG("file_handle_create failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject),
			    error);
		goto out_err;
	}

	fhandle->dir.dp = fdopendir(fd);
	if (fhandle->dir.dp == NULL) {
		error = -errno;
		SPDK_ERRLOG("fdopendir failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), error);
		goto out_err;
	}

	fhandle->dir.offset = 0;
	fhandle->dir.entry = NULL;

	SPDK_DEBUGLOG(fsdev_aio, "OPENDIR succeeded for " FOBJECT_FMT " (fh=%p)\n",
		      FOBJECT_ARGS(fobject), fhandle);

	fsdev_io->u_out.opendir.fhandle = fhandle;

	return 0;

out_err:
	if (fhandle) {
		file_handle_delete(fhandle);
	} else if (fd != -1) {
		close(fd);
	}

	return error;
}

static int
lo_releasedir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.releasedir.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.releasedir.fhandle;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "RELEASEDIR succeeded for " FOBJECT_FMT " (fh=%p)\n",
		      FOBJECT_ARGS(fobject), fhandle);

	file_handle_delete(fhandle);

	return 0;
}

static int
lo_set_mount_opts(struct aio_fsdev *vfsdev, struct spdk_fsdev_mount_opts *opts)
{
	assert(opts != NULL);
	assert(opts->opts_size != 0);

	UNUSED(vfsdev);

	if (opts->opts_size > offsetof(struct spdk_fsdev_mount_opts, max_write)) {
		/* Set the value the aio fsdev was created with */
		opts->max_write = vfsdev->mount_opts.max_write;
	}

	if (opts->opts_size > offsetof(struct spdk_fsdev_mount_opts, writeback_cache_enabled)) {
		if (vfsdev->mount_opts.writeback_cache_enabled) {
			/* The writeback_cache_enabled was enabled upon creation => we follow the opts */
			vfsdev->mount_opts.writeback_cache_enabled = opts->writeback_cache_enabled;
		} else {
			/* The writeback_cache_enabled was disabled upon creation => we reflect it in the opts */
			opts->writeback_cache_enabled = false;
		}
	}

	/* The AIO doesn't apply any additional restrictions, so we just accept the requested opts */
	SPDK_DEBUGLOG(fsdev_aio,
		      "aio filesystem %s: opts updated: max_write=%" PRIu32 ", writeback_cache=%" PRIu8 "\n",
		      vfsdev->fsdev.name, vfsdev->mount_opts.max_write, vfsdev->mount_opts.writeback_cache_enabled);

	return 0;
}

static int
lo_mount(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_mount_opts *in_opts = &fsdev_io->u_in.mount.opts;

	fsdev_io->u_out.mount.opts = *in_opts;
	lo_set_mount_opts(vfsdev, &fsdev_io->u_out.mount.opts);
	file_object_ref(vfsdev->root);
	fsdev_io->u_out.mount.root_fobject = vfsdev->root;

	return 0;
}

static int
lo_umount(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);

	fsdev_free_leafs(vfsdev->root, false);
	file_object_unref(vfsdev->root, 1); /* reference by mount */

	return 0;
}

static int
lo_do_lookup(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_object *parent_fobject,
	     const char *name, struct spdk_fsdev_file_object **pfobject,
	     struct spdk_fsdev_file_attr *attr)
{
	int newfd;
	int res;
	struct stat stat;
	struct spdk_fsdev_file_object *fobject;

	/* Do not allow escaping root directory */
	if (parent_fobject == vfsdev->root && strcmp(name, "..") == 0) {
		name = ".";
	}

	newfd = openat(parent_fobject->fd, name, O_PATH | O_NOFOLLOW);
	if (newfd == -1) {
		res = -errno;
		SPDK_DEBUGLOG(fsdev_aio, "openat( " FOBJECT_FMT " %s) failed with %d\n",
			      FOBJECT_ARGS(parent_fobject), name, res);
		return res;
	}

	res = fstatat(newfd, "", &stat, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatat(%s) failed with %d\n", name, res);
		close(newfd);
		return res;
	}

	spdk_spin_lock(&parent_fobject->lock);
	fobject = lo_find_leaf_unsafe(parent_fobject, stat.st_ino, stat.st_dev);
	if (fobject) {
		close(newfd);
		newfd = -1;
		file_object_ref(fobject); /* reference by a lo_do_lookup caller */
	} else {
		fobject = file_object_create_unsafe(parent_fobject, newfd, stat.st_ino, stat.st_dev, stat.st_mode);
	}
	spdk_spin_unlock(&parent_fobject->lock);

	if (!fobject) {
		SPDK_ERRLOG("Cannot create file object\n");
		close(newfd);
		return -ENOMEM;
	}

	if (attr) {
		res = file_object_fill_attr(fobject, attr);
		if (res) {
			SPDK_ERRLOG("fill_attr(%s) failed with %d\n", name, res);
			file_object_unref(fobject, 1);
			if (newfd != -1) {
				close(newfd);
			}
			return res;
		}
	}

	*pfobject = fobject;

	SPDK_DEBUGLOG(fsdev_aio, "lookup(%s) in dir " FOBJECT_FMT ": "  FOBJECT_FMT " fd=%d\n",
		      name, FOBJECT_ARGS(parent_fobject), FOBJECT_ARGS(fobject), fobject->fd);
	return 0;
}

static int
lo_lookup(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int err;
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.lookup.parent_fobject;
	char *name = fsdev_io->u_in.lookup.name;

	if (!parent_fobject) {
		err = file_object_fill_attr(vfsdev->root, &fsdev_io->u_out.lookup.attr);
		if (err) {
			SPDK_DEBUGLOG(fsdev_aio, "file_object_fill_attr(root) failed with err=%d\n", err);
			return err;
		}

		file_object_ref(vfsdev->root);
		fsdev_io->u_out.lookup.fobject = vfsdev->root;
		return 0;
	}

	SPDK_DEBUGLOG(fsdev_aio, "  name %s\n", name);

	/* Don't use is_safe_path_component(), allow "." and ".." for NFS export
	 * support.
	 */
	if (strchr(name, '/')) {
		return -EINVAL;
	}

	err = lo_do_lookup(vfsdev, parent_fobject, name, &fsdev_io->u_out.lookup.fobject,
			   &fsdev_io->u_out.lookup.attr);
	if (err) {
		SPDK_DEBUGLOG(fsdev_aio, "lo_do_lookup(%s) failed with err=%d\n", name, err);
		return err;
	}

	return 0;
}

/*
 * Change to uid/gid of caller so that file is created with ownership of caller.
 */
static int
lo_change_cred(const struct lo_cred *new, struct lo_cred *old)
{
	int res;

	old->euid = geteuid();
	old->egid = getegid();

	res = syscall(SYS_setresgid, -1, new->egid, -1);
	if (res == -1) {
		return -errno;
	}

	res = syscall(SYS_setresuid, -1, new->euid, -1);
	if (res == -1) {
		int errno_save = -errno;

		syscall(SYS_setresgid, -1, old->egid, -1);
		return errno_save;
	}

	return 0;
}

/* Regain Privileges */
static void
lo_restore_cred(struct lo_cred *old)
{
	int res;

	res = syscall(SYS_setresuid, -1, old->euid, -1);
	if (res == -1) {
		SPDK_ERRLOG("seteuid(%u)", old->euid);
	}

	res = syscall(SYS_setresgid, -1, old->egid, -1);
	if (res == -1) {
		SPDK_ERRLOG("setegid(%u)", old->egid);
	}
}

static int
lo_readdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.readdir.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.readdir.fhandle;
	uint64_t offset = fsdev_io->u_in.readdir.offset;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	if (((off_t)offset) != fhandle->dir.offset) {
		seekdir(fhandle->dir.dp, offset);
		fhandle->dir.entry = NULL;
		fhandle->dir.offset = offset;
	}

	while (1) {
		off_t nextoff;
		const char *name;
		int res;

		if (!fhandle->dir.entry) {
			errno = 0;
			fhandle->dir.entry = readdir(fhandle->dir.dp);
			if (!fhandle->dir.entry) {
				if (errno) {  /* Error */
					res = -errno;
					SPDK_ERRLOG("readdir failed with err=%d", res);
					return res;
				} else {  /* End of stream */
					break;
				}
			}
		}

		nextoff = fhandle->dir.entry->d_off;
		name = fhandle->dir.entry->d_name;

		/* Hide root's parent directory */
		if (fobject == vfsdev->root && strcmp(name, "..") == 0) {
			goto skip_entry;
		}

		if (is_dot_or_dotdot(name)) {
			fsdev_io->u_out.readdir.fobject = NULL;
			memset(&fsdev_io->u_out.readdir.attr, 0, sizeof(fsdev_io->u_out.readdir.attr));
			fsdev_io->u_out.readdir.attr.ino = fhandle->dir.entry->d_ino;
			fsdev_io->u_out.readdir.attr.mode = DT_DIR << 12;
			goto skip_lookup;
		}

		res = lo_do_lookup(vfsdev, fobject, name, &fsdev_io->u_out.readdir.fobject,
				   &fsdev_io->u_out.readdir.attr);
		if (res) {
			SPDK_DEBUGLOG(fsdev_aio, "lo_do_lookup(%s) failed with err=%d\n", name, res);
			return res;
		}

skip_lookup:
		fsdev_io->u_out.readdir.name = name;
		fsdev_io->u_out.readdir.offset = nextoff;

		res = fsdev_io->u_in.readdir.entry_cb_fn(fsdev_io, fsdev_io->internal.cb_arg);
		if (res) {
			if (fsdev_io->u_out.readdir.fobject) {
				file_object_unref(fsdev_io->u_out.readdir.fobject, 1);
			}
			break;
		}

skip_entry:
		fhandle->dir.entry = NULL;
		fhandle->dir.offset = nextoff;
	}

	SPDK_DEBUGLOG(fsdev_aio, "READDIR succeeded for " FOBJECT_FMT " (fh=%p, offset=%" PRIu64 ")\n",
		      FOBJECT_ARGS(fobject), fhandle, offset);
	return 0;
}

static int
lo_forget(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.readdir.fobject;
	uint64_t nlookup = fsdev_io->u_in.forget.nlookup;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	file_object_unref(fobject, nlookup);

	return 0;
}

static uint32_t
update_open_flags(struct aio_fsdev *vfsdev, uint32_t flags)
{
	/*
	 * With writeback cache, kernel may send read requests even
	 * when userspace opened write-only
	 */
	if (vfsdev->mount_opts.writeback_cache_enabled && (flags & O_ACCMODE) == O_WRONLY) {
		flags &= ~O_ACCMODE;
		flags |= O_RDWR;
	}

	/*
	 * With writeback cache, O_APPEND is handled by the kernel.
	 * This breaks atomicity (since the file may change in the
	 * underlying filesystem, so that the kernel's idea of the
	 * end of the file isn't accurate anymore). In this example,
	 * we just accept that. A more rigorous filesystem may want
	 * to return an error here
	 */
	if (vfsdev->mount_opts.writeback_cache_enabled && (flags & O_APPEND)) {
		flags &= ~O_APPEND;
	}

	/*
	 * O_DIRECT in guest should not necessarily mean bypassing page
	 * cache on host as well. If somebody needs that behavior, it
	 * probably should be a configuration knob in daemon.
	 */
	flags &= ~O_DIRECT;

	return flags;
}

static int
lo_open(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int fd, saverr;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.open.fobject;
	uint32_t flags = fsdev_io->u_in.open.flags;
	struct spdk_fsdev_file_handle *fhandle;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	flags = update_open_flags(vfsdev, flags);

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, flags & ~O_NOFOLLOW);
	if (fd == -1) {
		saverr = -errno;
		SPDK_ERRLOG("openat(%d, %s, 0x%08" PRIx32 ") failed with err=%d\n",
			    vfsdev->proc_self_fd, fobject->fd_str, flags, saverr);
		return saverr;
	}

	fhandle = file_handle_create(fobject, fd);
	if (!fhandle) {
		SPDK_ERRLOG("cannot create a file handle (fd=%d)\n", fd);
		close(fd);
		return -ENOMEM;
	}

	fsdev_io->u_out.open.fhandle = fhandle;

	SPDK_DEBUGLOG(fsdev_aio, "OPEN succeeded for " FOBJECT_FMT " (fh=%p, fd=%d)\n",
		      FOBJECT_ARGS(fobject), fhandle, fd);

	return 0;
}

static int
lo_flush(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.flush.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.flush.fhandle;
	int res, saverr;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	res = close(dup(fhandle->fd));
	if (res) {
		saverr = -errno;
		SPDK_ERRLOG("close(dup(%d)) failed for " FOBJECT_FMT " (fh=%p, err=%d)\n",
			    fhandle->fd, FOBJECT_ARGS(fobject), fhandle, saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FLUSH succeeded for " FOBJECT_FMT " (fh=%p)\n", FOBJECT_ARGS(fobject),
		      fhandle);

	return 0;
}

static int
lo_setattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int saverr;
	int res;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.setattr.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.setattr.fhandle;
	uint32_t to_set = fsdev_io->u_in.setattr.to_set;
	struct spdk_fsdev_file_attr *attr = &fsdev_io->u_in.setattr.attr;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (to_set & FSDEV_SET_ATTR_MODE) {
		if (fhandle) {
			res = fchmod(fhandle->fd, attr->mode);
		} else {
			res = fchmodat(vfsdev->proc_self_fd, fobject->fd_str, attr->mode, 0);
		}
		if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("fchmod failed for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
			return saverr;
		}
	}

	if (to_set & (FSDEV_SET_ATTR_UID | FSDEV_SET_ATTR_GID)) {
		uid_t uid = (to_set & FSDEV_SET_ATTR_UID) ? attr->uid : (uid_t) -1;
		gid_t gid = (to_set & FSDEV_SET_ATTR_GID) ? attr->gid : (gid_t) -1;

		res = fchownat(fobject->fd, "", uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
		if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("fchownat failed for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
			return saverr;
		}
	}

	if (to_set & FSDEV_SET_ATTR_SIZE) {
		int truncfd;

		if (fhandle) {
			truncfd = fhandle->fd;
		} else {
			truncfd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
			if (truncfd < 0) {
				saverr = -errno;
				SPDK_ERRLOG("openat failed for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
				return saverr;
			}
		}

		res = ftruncate(truncfd, attr->size);
		if (!fhandle) {
			saverr = -errno;
			close(truncfd);
			errno = saverr;
		}
		if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("ftruncate failed for " FOBJECT_FMT " (size=%" PRIu64 ")\n", FOBJECT_ARGS(fobject),
				    attr->size);
			return saverr;
		}
	}

	if (to_set & (FSDEV_SET_ATTR_ATIME | FSDEV_SET_ATTR_MTIME)) {
		struct timespec tv[2];

		tv[0].tv_sec = 0;
		tv[1].tv_sec = 0;
		tv[0].tv_nsec = UTIME_OMIT;
		tv[1].tv_nsec = UTIME_OMIT;

		if (to_set & FSDEV_SET_ATTR_ATIME_NOW) {
			tv[0].tv_nsec = UTIME_NOW;
		} else if (to_set & FSDEV_SET_ATTR_ATIME) {
			tv[0].tv_sec = attr->atime;
			tv[0].tv_nsec = attr->atimensec;
		}

		if (to_set & FSDEV_SET_ATTR_MTIME_NOW) {
			tv[1].tv_nsec = UTIME_NOW;
		} else if (to_set & FSDEV_SET_ATTR_MTIME) {
			tv[1].tv_sec = attr->mtime;
			tv[1].tv_nsec = attr->mtimensec;
		}

		if (fhandle) {
			res = futimens(fhandle->fd, tv);
		} else {
			res = utimensat_empty(vfsdev, fobject, tv);
		}
		if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("futimens/utimensat_empty failed for " FOBJECT_FMT "\n",
				    FOBJECT_ARGS(fobject));
			return saverr;
		}
	}

	res = file_object_fill_attr(fobject, &fsdev_io->u_out.setattr.attr);
	if (res) {
		SPDK_ERRLOG("file_object_fill_attr failed for " FOBJECT_FMT "\n",
			    FOBJECT_ARGS(fobject));
		return res;
	}

	SPDK_DEBUGLOG(fsdev_aio, "SETATTR succeeded for " FOBJECT_FMT "\n",
		      FOBJECT_ARGS(fobject));

	return 0;
}

static int
lo_create(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int fd;
	int err;
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.create.parent_fobject;
	const char *name = fsdev_io->u_in.create.name;
	uint32_t mode = fsdev_io->u_in.create.mode;
	uint32_t flags = fsdev_io->u_in.create.flags;
	uint32_t umask = fsdev_io->u_in.create.umask;
	struct lo_cred old_cred, new_cred = {
		.euid = fsdev_io->u_in.create.euid,
		.egid = fsdev_io->u_in.create.egid,
	};
	struct spdk_fsdev_file_object *fobject;
	struct spdk_fsdev_file_handle *fhandle;
	struct spdk_fsdev_file_attr *attr = &fsdev_io->u_out.create.attr;

	if (!fsdev_aio_is_valid_fobject(vfsdev, parent_fobject)) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	UNUSED(umask);

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("CREATE: %s not a safe component\n", name);
		return -EINVAL;
	}

	err = lo_change_cred(&new_cred, &old_cred);
	if (err) {
		SPDK_ERRLOG("CREATE: cannot change credentials\n");
		return err;
	}

	flags = update_open_flags(vfsdev, flags);

	fd = openat(parent_fobject->fd, name, (flags | O_CREAT) & ~O_NOFOLLOW, mode);
	err = fd == -1 ? -errno : 0;
	lo_restore_cred(&old_cred);

	if (err) {
		SPDK_ERRLOG("CREATE: openat failed with %d\n", err);
		return err;
	}

	err = lo_do_lookup(vfsdev, parent_fobject, name, &fobject, attr);
	if (err) {
		SPDK_ERRLOG("CREATE: lookup failed with %d\n", err);
		return err;
	}

	fhandle = file_handle_create(fobject, fd);
	if (!fhandle) {
		SPDK_ERRLOG("cannot create a file handle (fd=%d)\n", fd);
		close(fd);
		file_object_unref(fobject, 1);
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(fsdev_aio, "CREATE: succeeded (name=%s " FOBJECT_FMT " fh=%p)\n",
		      name, FOBJECT_ARGS(fobject), fhandle);

	fsdev_io->u_out.create.fobject = fobject;
	fsdev_io->u_out.create.fhandle = fhandle;

	return 0;
}

static int
lo_release(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.release.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.release.fhandle;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "RELEASE succeeded for " FOBJECT_FMT " fh=%p)\n",
		      FOBJECT_ARGS(fobject), fhandle);

	file_handle_delete(fhandle);

	return 0;
}

static void
lo_read_cb(void *ctx, uint32_t data_size, int error)
{
	struct spdk_fsdev_io *fsdev_io = ctx;
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);

	if (vfsdev_io->aio) {
		TAILQ_REMOVE(&vfsdev_io->ch->ios_in_progress, vfsdev_io, link);
	}

	fsdev_io->u_out.read.data_size = data_size;

	spdk_fsdev_io_complete(fsdev_io, error);
}

static int
lo_read(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.read.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.read.fhandle;
	size_t size = fsdev_io->u_in.read.size;
	uint64_t offs = fsdev_io->u_in.read.offs;
	uint32_t flags = fsdev_io->u_in.read.flags;
	struct iovec *outvec = fsdev_io->u_in.read.iov;
	uint32_t outcnt = fsdev_io->u_in.read.iovcnt;

	/* we don't suport the memory domains at the moment */
	assert(!fsdev_io->u_in.read.opts || !fsdev_io->u_in.read.opts->memory_domain);

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	UNUSED(flags);

	if (!outcnt || !outvec) {
		SPDK_ERRLOG("bad outvec: iov=%p outcnt=%" PRIu32 "\n", outvec, outcnt);
		return -EINVAL;
	}

	if (vfsdev->skip_rw) {
		uint32_t i;

		fsdev_io->u_out.read.data_size = 0;

		for (i = 0; i < outcnt; i++, outvec++) {
			fsdev_io->u_out.read.data_size += outvec->iov_len;
		}

		TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);

		return IO_STATUS_ASYNC;
	}

	vfsdev_io->aio = spdk_aio_mgr_read(ch->mgr, lo_read_cb, fsdev_io, fhandle->fd, offs, size, outvec,
					   outcnt);
	if (vfsdev_io->aio) {
		vfsdev_io->ch = ch;
		TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);
	}

	return IO_STATUS_ASYNC;
}

static void
lo_write_cb(void *ctx, uint32_t data_size, int error)
{
	struct spdk_fsdev_io *fsdev_io = ctx;
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);

	if (vfsdev_io->aio) {
		TAILQ_REMOVE(&vfsdev_io->ch->ios_in_progress, vfsdev_io, link);
	}

	fsdev_io->u_out.write.data_size = data_size;

	spdk_fsdev_io_complete(fsdev_io, error);
}

static int
lo_write(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.write.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.write.fhandle;
	size_t size = fsdev_io->u_in.write.size;
	uint64_t offs = fsdev_io->u_in.write.offs;
	uint32_t flags = fsdev_io->u_in.write.flags;
	const struct iovec *invec = fsdev_io->u_in.write.iov;
	uint32_t incnt =  fsdev_io->u_in.write.iovcnt;

	/* we don't suport the memory domains at the moment */
	assert(!fsdev_io->u_in.write.opts || !fsdev_io->u_in.write.opts->memory_domain);

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	UNUSED(flags);

	if (!incnt || !invec) { /* there should be at least one iovec with data */
		SPDK_ERRLOG("bad invec: iov=%p cnt=%" PRIu32 "\n", invec, incnt);
		return -EINVAL;
	}

	if (vfsdev->skip_rw) {
		uint32_t i;

		fsdev_io->u_out.write.data_size = 0;
		for (i = 0; i < incnt; i++, invec++) {
			fsdev_io->u_out.write.data_size += invec->iov_len;
		}

		TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);

		return IO_STATUS_ASYNC;
	}

	vfsdev_io->aio = spdk_aio_mgr_write(ch->mgr, lo_write_cb, fsdev_io,
					    fhandle->fd, offs, size, invec, incnt);
	if (vfsdev_io->aio) {
		vfsdev_io->ch = ch;
		TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);
	}

	return IO_STATUS_ASYNC;
}

static int
lo_readlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	char *buf;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.readlink.fobject;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	buf = malloc(PATH_MAX + 1);
	if (!buf) {
		SPDK_ERRLOG("malloc(%zu) failed\n", (size_t)(PATH_MAX + 1));
		return -ENOMEM;
	}

	res = readlinkat(fobject->fd, "", buf, PATH_MAX + 1);
	if (res == -1) {
		int saverr = -errno;
		SPDK_ERRLOG("readlinkat failed for " FOBJECT_FMT " with %d\n",
			    FOBJECT_ARGS(fobject), saverr);
		free(buf);
		return saverr;
	}

	if (((uint32_t)res) == PATH_MAX + 1) {
		SPDK_ERRLOG("buffer is too short\n");
		free(buf);
		return -ENAMETOOLONG;
	}

	buf[res] = 0;
	fsdev_io->u_out.readlink.linkname = buf;

	return 0;
}

static int
lo_statfs(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.statfs.fobject;
	struct statvfs stbuf;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = fstatvfs(fobject->fd, &stbuf);
	if (res == -1) {
		int saverr = -errno;
		SPDK_ERRLOG("fstatvfs failed with %d\n", saverr);
		return saverr;
	}

	fsdev_io->u_out.statfs.statfs.blocks = stbuf.f_blocks;
	fsdev_io->u_out.statfs.statfs.bfree = stbuf.f_bfree;
	fsdev_io->u_out.statfs.statfs.bavail = stbuf.f_bavail;
	fsdev_io->u_out.statfs.statfs.files = stbuf.f_files;
	fsdev_io->u_out.statfs.statfs.ffree = stbuf.f_ffree;
	fsdev_io->u_out.statfs.statfs.bsize = stbuf.f_bsize;
	fsdev_io->u_out.statfs.statfs.namelen = stbuf.f_namemax;
	fsdev_io->u_out.statfs.statfs.frsize = stbuf.f_frsize;

	return 0;
}

static int
lo_mknod_symlink(struct spdk_fsdev_io *fsdev_io, struct spdk_fsdev_file_object *parent_fobject,
		 const char *name, mode_t mode, dev_t rdev, const char *link, uid_t euid, gid_t egid,
		 struct spdk_fsdev_file_object **pfobject, struct spdk_fsdev_file_attr *attr)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	int saverr;
	struct lo_cred old_cred, new_cred = {
		.euid = euid,
		.egid = egid,
	};

	if (!fsdev_aio_is_valid_fobject(vfsdev, parent_fobject)) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s isn'h safe\n", name);
		return -EINVAL;
	}

	res = lo_change_cred(&new_cred, &old_cred);
	if (res) {
		SPDK_ERRLOG("cannot change cred (err=%d)\n", res);
		return res;
	}

	if (S_ISDIR(mode)) {
		res = mkdirat(parent_fobject->fd, name, mode);
	} else if (S_ISLNK(mode)) {
		if (link) {
			res = symlinkat(link, parent_fobject->fd, name);
		} else {
			SPDK_ERRLOG("NULL link pointer\n");
			errno = EINVAL;
		}
	} else {
		res = mknodat(parent_fobject->fd, name, mode, rdev);
	}
	saverr = -errno;

	lo_restore_cred(&old_cred);

	if (res == -1) {
		SPDK_ERRLOG("cannot mkdirat/symlinkat/mknodat (err=%d)\n", saverr);
		return saverr;
	}

	res = lo_do_lookup(vfsdev, parent_fobject, name, pfobject, attr);
	if (res) {
		SPDK_ERRLOG("lookup failed (err=%d)\n", res);
		return res;
	}

	SPDK_DEBUGLOG(fsdev_aio, "lo_mknod_symlink(" FOBJECT_FMT "/%s -> " FOBJECT_FMT "\n",
		      FOBJECT_ARGS(parent_fobject), name, FOBJECT_ARGS(*pfobject));

	return 0;
}

static int
lo_mknod(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.mknod.parent_fobject;
	char *name = fsdev_io->u_in.mknod.name;
	mode_t mode = fsdev_io->u_in.mknod.mode;
	dev_t rdev = fsdev_io->u_in.mknod.rdev;
	uid_t euid = fsdev_io->u_in.mknod.euid;
	gid_t egid = fsdev_io->u_in.mknod.egid;

	return lo_mknod_symlink(fsdev_io, parent_fobject, name, mode, rdev, NULL, euid, egid,
				&fsdev_io->u_out.mknod.fobject, &fsdev_io->u_out.mknod.attr);
}

static int
lo_mkdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.mkdir.parent_fobject;
	char *name = fsdev_io->u_in.mkdir.name;
	mode_t mode = fsdev_io->u_in.mkdir.mode;
	uid_t euid = fsdev_io->u_in.mkdir.euid;
	gid_t egid = fsdev_io->u_in.mkdir.egid;

	return lo_mknod_symlink(fsdev_io, parent_fobject, name, S_IFDIR | mode, 0, NULL, euid, egid,
				&fsdev_io->u_out.mkdir.fobject, &fsdev_io->u_out.mkdir.attr);
}

static int
lo_symlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.symlink.parent_fobject;
	char *target = fsdev_io->u_in.symlink.target;
	char *linkpath = fsdev_io->u_in.symlink.linkpath;
	uid_t euid = fsdev_io->u_in.symlink.euid;
	gid_t egid = fsdev_io->u_in.symlink.egid;

	return lo_mknod_symlink(fsdev_io, parent_fobject, target, S_IFLNK, 0, linkpath, euid, egid,
				&fsdev_io->u_out.symlink.fobject, &fsdev_io->u_out.symlink.attr);
}

static int
lo_do_unlink(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_object *parent_fobject,
	     const char *name, bool is_dir)
{
	/* fobject must be initialized to avoid a scan-build false positive */
	struct spdk_fsdev_file_object *fobject = NULL;
	int res;

	if (!fsdev_aio_is_valid_fobject(vfsdev, parent_fobject)) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s isn't safe\n", name);
		return -EINVAL;
	}

	res = lo_do_lookup(vfsdev, parent_fobject, name, &fobject, NULL);
	if (res) {
		SPDK_ERRLOG("can't find '%s' under " FOBJECT_FMT "\n", name, FOBJECT_ARGS(parent_fobject));
		return -EIO;
	}

	res = unlinkat(parent_fobject->fd, name, is_dir ? AT_REMOVEDIR : 0);
	if (res) {
		res = -errno;
		SPDK_WARNLOG("unlinkat(" FOBJECT_FMT " %s) failed (err=%d)\n",
			     FOBJECT_ARGS(parent_fobject), name, res);
	}

	file_object_unref(fobject, 1);
	return res;
}

static int
lo_unlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.unlink.parent_fobject;
	char *name = fsdev_io->u_in.unlink.name;

	return lo_do_unlink(vfsdev, parent_fobject, name, false);
}

static int
lo_rmdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.rmdir.parent_fobject;
	char *name = fsdev_io->u_in.rmdir.name;

	return lo_do_unlink(vfsdev, parent_fobject, name, true);
}

static int
lo_rename(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, saverr;
	/* old_fobject must be initialized to avoid a scan-build false positive */
	struct spdk_fsdev_file_object *old_fobject = NULL;
	struct spdk_fsdev_file_object *parent_fobject = fsdev_io->u_in.rename.parent_fobject;
	char *name = fsdev_io->u_in.rename.name;
	struct spdk_fsdev_file_object *new_parent_fobject = fsdev_io->u_in.rename.new_parent_fobject;
	char *new_name = fsdev_io->u_in.rename.new_name;
	uint32_t flags = fsdev_io->u_in.rename.flags;

	if (!fsdev_aio_is_valid_fobject(vfsdev, parent_fobject)) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fobject(vfsdev, new_parent_fobject)) {
		SPDK_ERRLOG("Invalid new_parent_fobject: %p\n", new_parent_fobject);
		return -EINVAL;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("name '%s' isn't safe\n", name);
		return -EINVAL;
	}

	if (!is_safe_path_component(new_name)) {
		SPDK_ERRLOG("newname '%s' isn't safe\n", new_name);
		return -EINVAL;
	}

	res = lo_do_lookup(vfsdev, parent_fobject, name, &old_fobject, NULL);
	if (res) {
		SPDK_ERRLOG("can't find '%s' under " FOBJECT_FMT "\n", name, FOBJECT_ARGS(parent_fobject));
		return -EIO;
	}

	saverr = 0;
	if (flags) {
#ifndef SYS_renameat2
		SPDK_ERRLOG("flags are not supported\n");
		return -ENOTSUP;
#else
		res = syscall(SYS_renameat2, parent_fobject->fd, name, new_parent_fobject->fd,
			      new_name, flags);
		if (res == -1 && errno == ENOSYS) {
			SPDK_ERRLOG("SYS_renameat2 returned ENOSYS\n");
			saverr = -EINVAL;
		} else if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("SYS_renameat2 failed (err=%d))\n", saverr);
		}
#endif
	} else {
		res = renameat(parent_fobject->fd, name, new_parent_fobject->fd, new_name);
		if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("renameat failed (err=%d)\n", saverr);
		}
	}

	file_object_unref(old_fobject, 1);

	return saverr;
}

static int
linkat_empty_nofollow(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_object *fobject, int dfd,
		      const char *name)
{
	int res;

	if (fobject->is_symlink) {
		res = linkat(fobject->fd, "", dfd, name, AT_EMPTY_PATH);
		if (res == -1 && (errno == ENOENT || errno == EINVAL)) {
			/* Sorry, no race free way to hard-link a symlink. */
			errno = EPERM;
		}
	} else {
		res = linkat(vfsdev->proc_self_fd, fobject->fd_str, dfd, name, AT_SYMLINK_FOLLOW);
	}

	return res;
}

static int
lo_link(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	int saverr;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.link.fobject;
	struct spdk_fsdev_file_object *new_parent_fobject = fsdev_io->u_in.link.new_parent_fobject;
	char *name = fsdev_io->u_in.link.name;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s is not a safe component\n", name);
		return -EINVAL;
	}

	res = linkat_empty_nofollow(vfsdev, fobject, new_parent_fobject->fd, name);
	if (res == -1) {
		saverr = -errno;
		SPDK_ERRLOG("linkat_empty_nofollow failed " FOBJECT_FMT " -> " FOBJECT_FMT " name=%s (err=%d)\n",
			    FOBJECT_ARGS(fobject), FOBJECT_ARGS(new_parent_fobject), name, saverr);
		return saverr;
	}

	res = lo_do_lookup(vfsdev, new_parent_fobject, name, &fsdev_io->u_out.link.fobject,
			   &fsdev_io->u_out.link.attr);
	if (res) {
		SPDK_ERRLOG("lookup failed (err=%d)\n", res);
		return res;
	}

	SPDK_DEBUGLOG(fsdev_aio, "LINK succeeded for " FOBJECT_FMT " -> " FOBJECT_FMT " name=%s\n",
		      FOBJECT_ARGS(fobject), FOBJECT_ARGS(fsdev_io->u_out.link.fobject), name);

	return 0;
}

static int
lo_fsync(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, saverr, fd;
	char *buf;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.fsync.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.fsync.fhandle;
	bool datasync = fsdev_io->u_in.fsync.datasync;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fhandle) {
		res = asprintf(&buf, "%i", fobject->fd);
		if (res == -1) {
			saverr = -errno;
			SPDK_ERRLOG("asprintf failed (errno=%d)\n", saverr);
			return saverr;
		}

		fd = openat(vfsdev->proc_self_fd, buf, O_RDWR);
		saverr = -errno;
		free(buf);
		if (fd == -1) {
			SPDK_ERRLOG("openat failed (errno=%d)\n", saverr);
			return saverr;
		}
	} else {
		fd = fhandle->fd;
	}

	if (datasync) {
		res = fdatasync(fd);
	} else {
		res = fsync(fd);
	}

	saverr = -errno;
	if (!fhandle) {
		close(fd);
	}

	if (res == -1) {
		SPDK_ERRLOG("fdatasync/fsync failed for " FOBJECT_FMT " fh=%p (err=%d)\n",
			    FOBJECT_ARGS(fobject), fhandle, saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FSYNC succeeded for " FOBJECT_FMT " fh=%p\n",
		      FOBJECT_ARGS(fobject), fhandle);

	return 0;
}

static int
lo_setxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t ret;
	int saverr;
	int fd = -1;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.setxattr.fobject;
	char *name = fsdev_io->u_in.setxattr.name;
	char *value = fsdev_io->u_in.setxattr.value;
	uint32_t size = fsdev_io->u_in.setxattr.size;
	uint32_t flags = fsdev_io->u_in.setxattr.flags;

	if (!vfsdev->xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (fobject->is_symlink) {
		/* Sorry, no race free way to removexattr on symlink. */
		SPDK_ERRLOG("cannot set xattr for symlink\n");
		return -EPERM;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd < 0) {
		saverr = -errno;
		SPDK_ERRLOG("openat failed with errno=%d\n", saverr);
		return saverr;
	}

	ret = fsetxattr(fd, name, value, size, flags);
	saverr = -errno;
	close(fd);
	if (ret == -1) {
		if (saverr == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "flistxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("flistxattr failed with errno=%d\n", saverr);
		}
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "SETXATTR succeeded for " FOBJECT_FMT " name=%s value=%s size=%" PRIu32 "flags=0x%x" PRIx32 "\n",
		      FOBJECT_ARGS(fobject), name, value, size, flags);

	return 0;
}

static int
lo_getxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t ret;
	int saverr;
	int fd = -1;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.getxattr.fobject;
	char *name = fsdev_io->u_in.getxattr.name;
	void *buffer = fsdev_io->u_in.getxattr.buffer;
	size_t size = fsdev_io->u_in.getxattr.size;

	if (!vfsdev->xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (fobject->is_symlink) {
		/* Sorry, no race free way to getxattr on symlink. */
		SPDK_ERRLOG("cannot get xattr for symlink\n");
		return -EPERM;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd < 0) {
		saverr = -errno;
		SPDK_ERRLOG("openat failed with errno=%d\n", saverr);
		return saverr;
	}

	ret = fgetxattr(fd, name, buffer, size);
	saverr = -errno;
	close(fd);
	if (ret == -1) {
		if (saverr == -ENODATA) {
			SPDK_INFOLOG(fsdev_aio, "fgetxattr: no extended attribute '%s' found\n", name);
		} else if (saverr == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "fgetxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("fgetxattr failed with errno=%d\n", saverr);
		}
		return saverr;
	}

	fsdev_io->u_out.getxattr.value_size = ret;

	SPDK_DEBUGLOG(fsdev_aio,
		      "GETXATTR succeeded for " FOBJECT_FMT " name=%s value=%s value_size=%zd\n",
		      FOBJECT_ARGS(fobject), name, (char *)buffer, ret);

	return 0;
}

static int
lo_listxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t ret;
	int saverr;
	int fd = -1;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.listxattr.fobject;
	char *buffer = fsdev_io->u_in.listxattr.buffer;
	size_t size = fsdev_io->u_in.listxattr.size;

	if (!vfsdev->xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (fobject->is_symlink) {
		/* Sorry, no race free way to listxattr on symlink. */
		SPDK_ERRLOG("cannot list xattr for symlink\n");
		return -EPERM;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDONLY);
	if (fd < 0) {
		saverr = -errno;
		SPDK_ERRLOG("openat failed with errno=%d\n", saverr);
		return saverr;
	}

	ret = flistxattr(fd, buffer, size);
	saverr = -errno;
	close(fd);
	if (ret == -1) {
		if (saverr == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "flistxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("flistxattr failed with errno=%d\n", saverr);
		}
		return saverr;
	}

	fsdev_io->u_out.listxattr.data_size = ret;
	fsdev_io->u_out.listxattr.size_only = (size == 0);

	SPDK_DEBUGLOG(fsdev_aio, "LISTXATTR succeeded for " FOBJECT_FMT " data_size=%zu\n",
		      FOBJECT_ARGS(fobject), ret);

	return 0;
}

static int
lo_removexattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t ret;
	int saverr;
	int fd = -1;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.removexattr.fobject;
	char *name = fsdev_io->u_in.removexattr.name;

	if (!vfsdev->xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (fobject->is_symlink) {
		/* Sorry, no race free way to setxattr on symlink. */
		SPDK_ERRLOG("cannot list xattr for symlink\n");
		return -EPERM;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDONLY);
	if (fd < 0) {
		saverr = -errno;
		SPDK_ERRLOG("openat failed with errno=%d\n", saverr);
		return saverr;
	}

	ret = fremovexattr(fd, name);
	saverr = -errno;
	close(fd);
	if (ret == -1) {
		if (saverr == -ENODATA) {
			SPDK_INFOLOG(fsdev_aio, "fremovexattr: no extended attribute '%s' found\n", name);
		} else if (saverr == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "fremovexattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("fremovexattr failed with errno=%d\n", saverr);
		}
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "REMOVEXATTR succeeded for " FOBJECT_FMT " name=%s\n",
		      FOBJECT_ARGS(fobject), name);

	return 0;
}

static int
lo_fsyncdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	int saverr = 0;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.fsyncdir.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.fsyncdir.fhandle;
	bool datasync = fsdev_io->u_in.fsyncdir.datasync;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	if (datasync) {
		res = fdatasync(fhandle->fd);
	} else {
		res = fsync(fhandle->fd);
	}

	if (res == -1) {
		saverr = -errno;
		SPDK_ERRLOG("%s failed for fh=%p with err=%d\n",
			    datasync ? "fdatasync" : "fsync", fhandle, saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FSYNCDIR succeeded for " FOBJECT_FMT " fh=%p datasync=%d\n",
		      FOBJECT_ARGS(fobject), fhandle, datasync);

	return 0;
}

static int
lo_flock(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	int saverr = 0;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.flock.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.flock.fhandle;
	int operation = fsdev_io->u_in.flock.operation;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	res = flock(fhandle->fd, operation | LOCK_NB);
	if (res == -1) {
		saverr = -errno;
		SPDK_ERRLOG("flock failed for fh=%p with err=%d\n", fhandle, saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FLOCK succeeded for " FOBJECT_FMT " fh=%p operation=%d\n",
		      FOBJECT_ARGS(fobject), fhandle, operation);

	return 0;
}

static int
lo_fallocate(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int err;
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.fallocate.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.fallocate.fhandle;
	uint32_t mode = fsdev_io->u_in.fallocate.mode;
	uint64_t offset  = fsdev_io->u_in.fallocate.offset;
	uint64_t length = fsdev_io->u_in.fallocate.length;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject)) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle)) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	if (mode) {
		SPDK_ERRLOG("non-zero mode is not suppored\n");
		return -EOPNOTSUPP;
	}

	err = posix_fallocate(fhandle->fd, offset, length);
	if (err) {
		SPDK_ERRLOG("posix_fallocate failed for fh=%p with err=%d\n",
			    fhandle, err);
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "FALLOCATE returns %d for " FOBJECT_FMT " fh=%p offset=%" PRIu64 " length=%" PRIu64 "\n",
		      err, FOBJECT_ARGS(fobject), fhandle, offset, length);
	return err;
}

static int
lo_copy_file_range(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
#ifdef SPDK_CONFIG_COPY_FILE_RANGE
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t res;
	int saverr = 0;
	struct spdk_fsdev_file_object *fobject_in = fsdev_io->u_in.copy_file_range.fobject_in;
	struct spdk_fsdev_file_handle *fhandle_in = fsdev_io->u_in.copy_file_range.fhandle_in;
	off_t off_in = fsdev_io->u_in.copy_file_range.off_in;
	struct spdk_fsdev_file_object *fobject_out = fsdev_io->u_in.copy_file_range.fobject_out;
	struct spdk_fsdev_file_handle *fhandle_out = fsdev_io->u_in.copy_file_range.fhandle_out;
	off_t off_out = fsdev_io->u_in.copy_file_range.off_out;
	size_t len = fsdev_io->u_in.copy_file_range.len;
	uint32_t flags = fsdev_io->u_in.copy_file_range.flags;

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject_in)) {
		SPDK_ERRLOG("Invalid fobject_in: %p\n", fobject_in);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle_in)) {
		SPDK_ERRLOG("Invalid fhandle_in: %p\n", fhandle_in);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fobject(vfsdev, fobject_out)) {
		SPDK_ERRLOG("Invalid fobject_out: %p\n", fobject_out);
		return -EINVAL;
	}

	if (!fsdev_aio_is_valid_fhandle(vfsdev, fhandle_out)) {
		SPDK_ERRLOG("Invalid fhandle_out: %p\n", fhandle_out);
		return -EINVAL;
	}

	res = copy_file_range(fhandle_in->fd, &off_in, fhandle_out->fd, &off_out, len, flags);
	if (res < 0) {
		saverr = -errno;
		SPDK_ERRLOG("copy_file_range failed with err=%d\n", saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "COPY_FILE_RANGE succeeded for " FOBJECT_FMT " fh=%p offset=%" PRIu64 " -> " FOBJECT_FMT
		      " fh=%p offset=%" PRIu64 " (len-%zu flags=0x%" PRIx32 ")\n",
		      FOBJECT_ARGS(fobject_in), fhandle_in, (uint64_t)off_in, FOBJECT_ARGS(fobject_out), fhandle_out,
		      (uint64_t)off_out, len, flags);

	return 0;
#else
	return -ENOSYS;
#endif
}

static int
lo_abort(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io;
	uint64_t unique_to_abort = fsdev_io->u_in.abort.unique_to_abort;

	TAILQ_FOREACH(vfsdev_io, &ch->ios_in_progress, link) {
		struct spdk_fsdev_io *_fsdev_io = aio_to_fsdev_io(vfsdev_io);
		if (spdk_fsdev_io_get_unique(_fsdev_io) == unique_to_abort) {
			spdk_aio_mgr_cancel(ch->mgr, vfsdev_io->aio);
			return 0;
		}
	}

	return 0;
}

static int
aio_io_poll(void *arg)
{
	struct aio_fsdev_io *vfsdev_io, *tmp;
	struct aio_io_channel *ch = arg;
	int res = SPDK_POLLER_IDLE;

	if (spdk_aio_mgr_poll(ch->mgr)) {
		res = SPDK_POLLER_BUSY;
	}

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_to_complete, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		TAILQ_REMOVE(&ch->ios_to_complete, vfsdev_io, link);
		spdk_fsdev_io_complete(fsdev_io, 0);
		res = SPDK_POLLER_BUSY;
	}

	return res;
}

static int
aio_fsdev_create_cb(void *io_device, void *ctx_buf)
{
	struct aio_io_channel *ch = ctx_buf;
	struct spdk_thread *thread = spdk_get_thread();

	ch->mgr = spdk_aio_mgr_create(MAX_AIOS);
	if (!ch->mgr) {
		SPDK_ERRLOG("aoi manager init for failed (thread=%s)\n", spdk_thread_get_name(thread));
		return -ENOMEM;
	}

	ch->poller = SPDK_POLLER_REGISTER(aio_io_poll, ch, 0);
	TAILQ_INIT(&ch->ios_in_progress);
	TAILQ_INIT(&ch->ios_to_complete);

	SPDK_DEBUGLOG(fsdev_aio, "Created aio fsdev IO channel: thread %s, thread id %" PRIu64
		      "\n",
		      spdk_thread_get_name(thread), spdk_thread_get_id(thread));
	return 0;
}

static void
aio_fsdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct aio_io_channel *ch = ctx_buf;
	struct spdk_thread *thread = spdk_get_thread();

	UNUSED(thread);

	spdk_poller_unregister(&ch->poller);
	spdk_aio_mgr_delete(ch->mgr);

	SPDK_DEBUGLOG(fsdev_aio, "Destroyed aio fsdev IO channel: thread %s, thread id %" PRIu64
		      "\n",
		      spdk_thread_get_name(thread), spdk_thread_get_id(thread));
}

static int
fsdev_aio_initialize(void)
{
	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_aio_fsdev_head,
				aio_fsdev_create_cb, aio_fsdev_destroy_cb,
				sizeof(struct aio_io_channel), "aio_fsdev");

	return 0;
}

static void
_fsdev_aio_finish_cb(void *arg)
{
	/* @todo: handle async module fini */
	/* spdk_fsdev_module_fini_done(); */
}

static void
fsdev_aio_finish(void)
{
	spdk_io_device_unregister(&g_aio_fsdev_head, _fsdev_aio_finish_cb);
}

static int
fsdev_aio_get_ctx_size(void)
{
	return sizeof(struct aio_fsdev_io);
}

static struct spdk_fsdev_module aio_fsdev_module = {
	.name = "aio",
	.module_init = fsdev_aio_initialize,
	.module_fini = fsdev_aio_finish,
	.get_ctx_size	= fsdev_aio_get_ctx_size,
};

SPDK_FSDEV_MODULE_REGISTER(aio, &aio_fsdev_module);

static void
fsdev_aio_free(struct aio_fsdev *vfsdev)
{
	if (vfsdev->proc_self_fd != -1) {
		close(vfsdev->proc_self_fd);
	}

	if (vfsdev->root) {
		int destroyed = file_object_unref(vfsdev->root, 1);
		assert(destroyed == 0);
		UNUSED(destroyed);

	}

	free(vfsdev->fsdev.name);
	free(vfsdev->root_path);

	free(vfsdev);
}

static int
fsdev_aio_destruct(void *ctx)
{
	struct aio_fsdev *vfsdev = ctx;

	TAILQ_REMOVE(&g_aio_fsdev_head, vfsdev, tailq);

	fsdev_free_leafs(vfsdev->root, true);
	vfsdev->root = NULL;

	pthread_mutex_destroy(&vfsdev->mutex);

	fsdev_aio_free(vfsdev);
	return 0;
}

typedef int (*fsdev_op_handler_func)(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io);

static fsdev_op_handler_func handlers[] = {
	[SPDK_FSDEV_IO_MOUNT] = lo_mount,
	[SPDK_FSDEV_IO_UMOUNT] = lo_umount,
	[SPDK_FSDEV_IO_LOOKUP] = lo_lookup,
	[SPDK_FSDEV_IO_FORGET] = lo_forget,
	[SPDK_FSDEV_IO_GETATTR] = lo_getattr,
	[SPDK_FSDEV_IO_SETATTR] = lo_setattr,
	[SPDK_FSDEV_IO_READLINK] = lo_readlink,
	[SPDK_FSDEV_IO_SYMLINK] = lo_symlink,
	[SPDK_FSDEV_IO_MKNOD] = lo_mknod,
	[SPDK_FSDEV_IO_MKDIR] = lo_mkdir,
	[SPDK_FSDEV_IO_UNLINK] = lo_unlink,
	[SPDK_FSDEV_IO_RMDIR] = lo_rmdir,
	[SPDK_FSDEV_IO_RENAME] = lo_rename,
	[SPDK_FSDEV_IO_LINK] = lo_link,
	[SPDK_FSDEV_IO_OPEN] = lo_open,
	[SPDK_FSDEV_IO_READ] = lo_read,
	[SPDK_FSDEV_IO_WRITE] = lo_write,
	[SPDK_FSDEV_IO_STATFS] =  lo_statfs,
	[SPDK_FSDEV_IO_RELEASE] = lo_release,
	[SPDK_FSDEV_IO_FSYNC] = lo_fsync,
	[SPDK_FSDEV_IO_SETXATTR] =  lo_setxattr,
	[SPDK_FSDEV_IO_GETXATTR] =  lo_getxattr,
	[SPDK_FSDEV_IO_LISTXATTR] = lo_listxattr,
	[SPDK_FSDEV_IO_REMOVEXATTR] =  lo_removexattr,
	[SPDK_FSDEV_IO_FLUSH] =  lo_flush,
	[SPDK_FSDEV_IO_OPENDIR] =  lo_opendir,
	[SPDK_FSDEV_IO_READDIR] =  lo_readdir,
	[SPDK_FSDEV_IO_RELEASEDIR] = lo_releasedir,
	[SPDK_FSDEV_IO_FSYNCDIR] = lo_fsyncdir,
	[SPDK_FSDEV_IO_FLOCK] = lo_flock,
	[SPDK_FSDEV_IO_CREATE] = lo_create,
	[SPDK_FSDEV_IO_ABORT] = lo_abort,
	[SPDK_FSDEV_IO_FALLOCATE] = lo_fallocate,
	[SPDK_FSDEV_IO_COPY_FILE_RANGE] = lo_copy_file_range,
};

static void
fsdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int status;
	enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);

	assert(type >= 0 && type < __SPDK_FSDEV_IO_LAST);

	status = handlers[type](ch, fsdev_io);
	if (status != IO_STATUS_ASYNC) {
		spdk_fsdev_io_complete(fsdev_io, status);
	}
}

static struct spdk_io_channel *
fsdev_aio_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_aio_fsdev_head);
}

static void
fsdev_aio_write_config_json(struct spdk_fsdev *fsdev, struct spdk_json_write_ctx *w)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fsdev_aio_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", spdk_fsdev_get_name(&vfsdev->fsdev));
	spdk_json_write_named_string(w, "root_path", vfsdev->root_path);
	spdk_json_write_named_bool(w, "enable_xattr", vfsdev->xattr_enabled);
	spdk_json_write_named_bool(w, "enable_writeback_cache",
				   !!vfsdev->mount_opts.writeback_cache_enabled);
	spdk_json_write_named_uint32(w, "max_write", vfsdev->mount_opts.max_write);
	spdk_json_write_named_bool(w, "skip_rw", vfsdev->skip_rw);
	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w);
}

static const struct spdk_fsdev_fn_table aio_fn_table = {
	.destruct		= fsdev_aio_destruct,
	.submit_request		= fsdev_aio_submit_request,
	.get_io_channel		= fsdev_aio_get_io_channel,
	.write_config_json	= fsdev_aio_write_config_json,
};

static int
setup_root(struct aio_fsdev *vfsdev)
{
	int fd, res;
	struct stat stat;

	fd = open(vfsdev->root_path, O_PATH);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot open root %s (err=%d)\n", vfsdev->root_path, res);
		return res;
	}

	res = fstatat(fd, "", &stat, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot get root fstatat of %s (err=%d)\n", vfsdev->root_path, res);
		close(fd);
		return res;
	}

	vfsdev->root = file_object_create_unsafe(NULL, fd, stat.st_ino, stat.st_dev, stat.st_mode);
	if (!vfsdev->root) {
		SPDK_ERRLOG("Cannot alloc root\n");
		close(fd);
		return -ENOMEM;
	}

	SPDK_INFOLOG(fsdev_aio, "root (%s) fd=%d\n", vfsdev->root_path, fd);
	return 0;
}

static int
setup_proc_self_fd(struct aio_fsdev *vfsdev)
{
	vfsdev->proc_self_fd = open("/proc/self/fd", O_PATH);
	if (vfsdev->proc_self_fd == -1) {
		int saverr = -errno;
		SPDK_ERRLOG("Failed to open procfs fd dir with %d\n", saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "procfs fd dir opened (fd=%d)\n", vfsdev->proc_self_fd);
	return 0;
}

void
spdk_fsdev_aio_get_default_opts(struct spdk_fsdev_aio_opts *opts)
{
	assert(opts);

	memset(opts, 0, sizeof(*opts));

	opts->xattr_enabled = DEFAULT_XATTR_ENABLED;
	opts->writeback_cache_enabled = DEFAULT_WRITEBACK_CACHE;
	opts->max_write = DEFAULT_MAX_WRITE;
	opts->skip_rw = DEFAULT_SKIP_RW;
}

int
spdk_fsdev_aio_create(struct spdk_fsdev **fsdev, const char *name, const char *root_path,
		      const struct spdk_fsdev_aio_opts *opts)
{
	struct aio_fsdev *vfsdev;
	int rc;

	vfsdev = calloc(1, sizeof(*vfsdev));
	if (!vfsdev) {
		SPDK_ERRLOG("Could not allocate aio_fsdev\n");
		return -ENOMEM;
	}

	vfsdev->proc_self_fd = -1;

	vfsdev->fsdev.name = strdup(name);
	if (!vfsdev->fsdev.name) {
		SPDK_ERRLOG("Could not strdup fsdev name: %s\n", name);
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

	vfsdev->root_path = strdup(root_path);
	if (!vfsdev->root_path) {
		SPDK_ERRLOG("Could not strdup root path: %s\n", root_path);
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

	rc = setup_root(vfsdev);
	if (rc) {
		SPDK_ERRLOG("Could not setup root: %s (err=%d)\n", root_path, rc);
		fsdev_aio_free(vfsdev);
		return rc;
	}

	rc = setup_proc_self_fd(vfsdev);
	if (rc) {
		SPDK_ERRLOG("Could not setup proc_self_fd (err=%d)\n", rc);
		fsdev_aio_free(vfsdev);
		return rc;
	}

	if (opts->xattr_enabled) {
		SPDK_ERRLOG("Extended attributes can only be enabled in Linux\n");
		fsdev_aio_free(vfsdev);
		return rc;
	}

	vfsdev->xattr_enabled = opts->xattr_enabled;
	vfsdev->fsdev.ctxt = vfsdev;
	vfsdev->fsdev.fn_table = &aio_fn_table;
	vfsdev->fsdev.module = &aio_fsdev_module;

	pthread_mutex_init(&vfsdev->mutex, NULL);

	rc = spdk_fsdev_register(&vfsdev->fsdev);
	if (rc) {
		fsdev_aio_free(vfsdev);
		return rc;
	}

	vfsdev->mount_opts.writeback_cache_enabled = DEFAULT_WRITEBACK_CACHE;
	vfsdev->mount_opts.max_write = DEFAULT_MAX_WRITE;

	vfsdev->skip_rw = opts->skip_rw;

	*fsdev = &(vfsdev->fsdev);
	TAILQ_INSERT_TAIL(&g_aio_fsdev_head, vfsdev, tailq);
	SPDK_DEBUGLOG(fsdev_aio, "Created aio filesystem %s (xattr_enabled=%" PRIu8 " writeback_cache=%"
		      PRIu8 " max_write=%" PRIu32 " skip_rw=%" PRIu8 ")\n",
		      vfsdev->fsdev.name, vfsdev->xattr_enabled, vfsdev->mount_opts.writeback_cache_enabled,
		      vfsdev->mount_opts.max_write, vfsdev->skip_rw);
	return rc;
}
void
spdk_fsdev_aio_delete(const char *name,
		      spdk_delete_aio_fsdev_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_fsdev_unregister_by_name(name, &aio_fsdev_module, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}

	SPDK_DEBUGLOG(fsdev_aio, "Deleted aio filesystem %s\n", name);
}

SPDK_LOG_REGISTER_COMPONENT(fsdev_aio)
