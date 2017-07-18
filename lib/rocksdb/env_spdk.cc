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

#include "rocksdb/env.h"

extern "C" {
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob.h"
#include "spdk/blobfs.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"
#include "spdk/bdev.h"
}

namespace rocksdb
{

struct spdk_filesystem *g_fs = NULL;
struct spdk_bs_dev *g_bs_dev;
std::string g_bdev_name;
volatile bool g_spdk_ready = false;
struct sync_args {
	struct spdk_io_channel *channel;
};

__thread struct sync_args g_sync_args;

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

	event = spdk_event_allocate(0, __call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

class SpdkSequentialFile : public SequentialFile
{
	struct spdk_file *mFile;
	uint64_t mOffset;
public:
	SpdkSequentialFile(struct spdk_file *file) : mFile(file), mOffset(0) {}
	virtual ~SpdkSequentialFile();

	virtual Status Read(size_t n, Slice *result, char *scratch) override;
	virtual Status Skip(uint64_t n) override;
	virtual Status InvalidateCache(size_t offset, size_t length) override;
};

static std::string
basename(std::string full)
{
	return full.substr(full.rfind("/") + 1);
}

SpdkSequentialFile::~SpdkSequentialFile(void)
{
	spdk_file_close(mFile, g_sync_args.channel);
}

Status
SpdkSequentialFile::Read(size_t n, Slice *result, char *scratch)
{
	uint64_t ret;

	ret = spdk_file_read(mFile, g_sync_args.channel, scratch, mOffset, n);
	mOffset += ret;
	*result = Slice(scratch, ret);
	return Status::OK();
}

Status
SpdkSequentialFile::Skip(uint64_t n)
{
	mOffset += n;
	return Status::OK();
}

Status
SpdkSequentialFile::InvalidateCache(size_t offset, size_t length)
{
	return Status::OK();
}

class SpdkRandomAccessFile : public RandomAccessFile
{
	struct spdk_file *mFile;
public:
	SpdkRandomAccessFile(const std::string &fname, const EnvOptions &options);
	virtual ~SpdkRandomAccessFile();

	virtual Status Read(uint64_t offset, size_t n, Slice *result, char *scratch) const override;
	virtual Status InvalidateCache(size_t offset, size_t length) override;
};

SpdkRandomAccessFile::SpdkRandomAccessFile(const std::string &fname, const EnvOptions &options)
{
	spdk_fs_open_file(g_fs, g_sync_args.channel, fname.c_str(), SPDK_BLOBFS_OPEN_CREATE, &mFile);
}

SpdkRandomAccessFile::~SpdkRandomAccessFile(void)
{
	spdk_file_close(mFile, g_sync_args.channel);
}

Status
SpdkRandomAccessFile::Read(uint64_t offset, size_t n, Slice *result, char *scratch) const
{
	spdk_file_read(mFile, g_sync_args.channel, scratch, offset, n);
	*result = Slice(scratch, n);
	return Status::OK();
}

Status
SpdkRandomAccessFile::InvalidateCache(size_t offset, size_t length)
{
	return Status::OK();
}

class SpdkWritableFile : public WritableFile
{
	struct spdk_file *mFile;
	uint32_t mSize;

public:
	SpdkWritableFile(const std::string &fname, const EnvOptions &options);
	~SpdkWritableFile()
	{
		if (mFile != NULL) {
			Close();
		}
	}

	virtual void SetIOPriority(Env::IOPriority pri)
	{
		if (pri == Env::IO_HIGH) {
			spdk_file_set_priority(mFile, SPDK_FILE_PRIORITY_HIGH);
		}
	}

	virtual Status Truncate(uint64_t size) override
	{
		spdk_file_truncate(mFile, g_sync_args.channel, size);
		mSize = size;
		return Status::OK();
	}
	virtual Status Close() override
	{
		spdk_file_close(mFile, g_sync_args.channel);
		mFile = NULL;
		return Status::OK();
	}
	virtual Status Append(const Slice &data) override;
	virtual Status Flush() override
	{
		return Status::OK();
	}
	virtual Status Sync() override
	{
		spdk_file_sync(mFile, g_sync_args.channel);
		return Status::OK();
	}
	virtual Status Fsync() override
	{
		spdk_file_sync(mFile, g_sync_args.channel);
		return Status::OK();
	}
	virtual bool IsSyncThreadSafe() const override
	{
		return true;
	}
	virtual uint64_t GetFileSize() override
	{
		return mSize;
	}
	virtual Status InvalidateCache(size_t offset, size_t length) override
	{
		return Status::OK();
	}
#ifdef ROCKSDB_FALLOCATE_PRESENT
	virtual Status Allocate(uint64_t offset, uint64_t len) override
	{
		spdk_file_truncate(mFile, g_sync_args.channel, offset + len);
		return Status::OK();
	}
	virtual Status RangeSync(uint64_t offset, uint64_t nbytes) override
	{
		/*
		 * SPDK BlobFS does not have a range sync operation yet, so just sync
		 *  the whole file.
		 */
		spdk_file_sync(mFile, g_sync_args.channel);
		return Status::OK();
	}
	virtual size_t GetUniqueId(char *id, size_t max_size) const override
	{
		return 0;
	}
#endif
};

SpdkWritableFile::SpdkWritableFile(const std::string &fname, const EnvOptions &options) : mSize(0)
{
	spdk_fs_open_file(g_fs, g_sync_args.channel, fname.c_str(), SPDK_BLOBFS_OPEN_CREATE, &mFile);
}

Status
SpdkWritableFile::Append(const Slice &data)
{
	spdk_file_write(mFile, g_sync_args.channel, (void *)data.data(), mSize, data.size());
	mSize += data.size();

	return Status::OK();
}

class SpdkDirectory : public Directory
{
public:
	SpdkDirectory() {}
	~SpdkDirectory() {}
	Status Fsync() override
	{
		return Status::OK();
	}
};

class SpdkEnv : public EnvWrapper
{
private:
	pthread_t mSpdkTid;
	std::string mDirectory;
	std::string mConfig;
	std::string mBdev;

public:
	SpdkEnv(Env* base_env, const std::string &dir, const std::string &conf,
		const std::string &bdev, uint64_t cache_size_in_mb);

	virtual ~SpdkEnv();

	virtual Status NewSequentialFile(const std::string &fname,
					 unique_ptr<SequentialFile> *result,
					 const EnvOptions &options) override
	{
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			struct spdk_file *file;
			int rc;

			rc = spdk_fs_open_file(g_fs, g_sync_args.channel,
					       basename(fname).c_str(), 0, &file);
			if (rc == 0) {
				result->reset(new SpdkSequentialFile(file));
				return Status::OK();
			} else {
				/* Myrocks engine uses errno(ENOENT) as one
				 * special condition, for the purpose to
				 * support MySQL, set the errno to right value.
				 */
				errno = -rc;
				return Status::IOError(fname, strerror(errno));
			}
		} else {
			return EnvWrapper::NewSequentialFile(fname, result, options);
		}
	}

	virtual Status NewRandomAccessFile(const std::string &fname,
					   unique_ptr<RandomAccessFile> *result,
					   const EnvOptions &options) override
	{
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			result->reset(new SpdkRandomAccessFile(basename(fname), options));
			return Status::OK();
		} else {
			return EnvWrapper::NewRandomAccessFile(fname, result, options);
		}
	}

	virtual Status NewWritableFile(const std::string &fname,
				       unique_ptr<WritableFile> *result,
				       const EnvOptions &options) override
	{
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			result->reset(new SpdkWritableFile(basename(fname), options));
			return Status::OK();
		} else {
			return EnvWrapper::NewWritableFile(fname, result, options);
		}
	}

	virtual Status ReuseWritableFile(const std::string &fname,
					 const std::string &old_fname,
					 unique_ptr<WritableFile> *result,
					 const EnvOptions &options) override
	{
		return EnvWrapper::ReuseWritableFile(fname, old_fname, result, options);
	}

	virtual Status NewDirectory(const std::string &name,
				    unique_ptr<Directory> *result) override
	{
		result->reset(new SpdkDirectory());
		return Status::OK();
	}
	virtual Status FileExists(const std::string &fname) override
	{
		struct spdk_file_stat stat;
		std::string fname_base = basename(fname);
		int rc;

		rc = spdk_fs_file_stat(g_fs, g_sync_args.channel, fname_base.c_str(), &stat);
		if (rc == 0) {
			return Status::OK();
		}
		return EnvWrapper::FileExists(fname);
	}
	virtual Status RenameFile(const std::string &src, const std::string &target) override
	{
		std::string target_base = basename(target);
		std::string src_base = basename(src);
		int rc;

		rc = spdk_fs_rename_file(g_fs, g_sync_args.channel,
					 src_base.c_str(), target_base.c_str());
		if (rc == -ENOENT) {
			return EnvWrapper::RenameFile(src, target);
		}
		return Status::OK();
	}
	virtual Status LinkFile(const std::string &src, const std::string &target) override
	{
		return Status::NotSupported("SpdkEnv does not support LinkFile");
	}
	virtual Status GetFileSize(const std::string &fname, uint64_t *size) override
	{
		struct spdk_file_stat stat;
		std::string fname_base = basename(fname);
		int rc;

		rc = spdk_fs_file_stat(g_fs, g_sync_args.channel, fname_base.c_str(), &stat);
		if (rc == -ENOENT) {
			return EnvWrapper::GetFileSize(fname, size);
		}
		*size = stat.size;
		return Status::OK();
	}
	virtual Status DeleteFile(const std::string &fname) override
	{
		int rc;
		std::string fname_base = basename(fname);
		rc = spdk_fs_delete_file(g_fs, g_sync_args.channel, fname_base.c_str());
		if (rc == -ENOENT) {
			return EnvWrapper::DeleteFile(fname);
		}
		return Status::OK();
	}
	virtual void StartThread(void (*function)(void *arg), void *arg) override;
	virtual Status LockFile(const std::string &fname, FileLock **lock) override
	{
		spdk_fs_open_file(g_fs, g_sync_args.channel, basename(fname).c_str(),
				  SPDK_BLOBFS_OPEN_CREATE, (struct spdk_file **)lock);
		return Status::OK();
	}
	virtual Status UnlockFile(FileLock *lock) override
	{
		spdk_file_close((struct spdk_file *)lock, g_sync_args.channel);
		return Status::OK();
	}
	virtual Status GetChildren(const std::string &dir,
				   std::vector<std::string> *result) override
	{
		if (dir.find("archive") != std::string::npos) {
			return Status::OK();
		}
		if (dir.compare(0, mDirectory.length(), mDirectory) == 0) {
			spdk_fs_iter iter;
			struct spdk_file *file;

			iter = spdk_fs_iter_first(g_fs);
			while (iter != NULL) {
				file = spdk_fs_iter_get_file(iter);
				result->push_back(std::string(spdk_file_get_name(file)));
				iter = spdk_fs_iter_next(iter);
			}
			return Status::OK();
		}
		return EnvWrapper::GetChildren(dir, result);
	}
};

static void
_spdk_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	/* Not supported */
	assert(false);
}

void SpdkInitializeThread(void)
{
	if (g_fs != NULL) {
		spdk_allocate_thread(_spdk_send_msg, NULL);
		g_sync_args.channel = spdk_fs_alloc_io_channel_sync(g_fs);
	}
}

struct SpdkThreadState {
	void (*user_function)(void*);
	void* arg;
};

static void SpdkStartThreadWrapper(void *arg)
{
	SpdkThreadState *state = reinterpret_cast<SpdkThreadState *>(arg);

	SpdkInitializeThread();
	state->user_function(state->arg);
	delete state;
}

void SpdkEnv::StartThread(void (*function)(void *arg), void *arg)
{
	SpdkThreadState *state = new SpdkThreadState;
	state->user_function = function;
	state->arg = arg;
	EnvWrapper::StartThread(SpdkStartThreadWrapper, state);
}

static void
fs_load_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	if (fserrno == 0) {
		g_fs = fs;
	}
	g_spdk_ready = true;
}

static void
spdk_rocksdb_run(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;

	pthread_setname_np(pthread_self(), "spdk");
	bdev = spdk_bdev_get_by_name(g_bdev_name.c_str());

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev_name.c_str());
		exit(1);
	}

	g_bs_dev = spdk_bdev_create_bs_dev(bdev);
	printf("using bdev %s\n", g_bdev_name.c_str());
	spdk_fs_load(g_bs_dev, __send_request, fs_load_cb, NULL);
}

static void
fs_unload_cb(void *ctx, int fserrno)
{
	assert(fserrno == 0);

	spdk_app_stop(0);
}

static void
spdk_rocksdb_shutdown(void)
{
	if (g_fs != NULL) {
		spdk_fs_unload(g_fs, fs_unload_cb, NULL);
	} else {
		fs_unload_cb(NULL, 0);
	}
}

static void *
initialize_spdk(void *arg)
{
	struct spdk_app_opts *opts = (struct spdk_app_opts *)arg;

	spdk_app_start(opts, spdk_rocksdb_run, NULL, NULL);
	spdk_app_fini();

	delete opts;
	pthread_exit(NULL);
}

SpdkEnv::SpdkEnv(Env *base_env, const std::string &dir, const std::string &conf,
		 const std::string &bdev, uint64_t cache_size_in_mb)
	: EnvWrapper(base_env), mDirectory(dir), mConfig(conf), mBdev(bdev)
{
	struct spdk_app_opts *opts = new struct spdk_app_opts;

	spdk_app_opts_init(opts);
	opts->name = "rocksdb";
	opts->config_file = mConfig.c_str();
	opts->reactor_mask = "0x1";
	opts->mem_size = 1024 + cache_size_in_mb;
	opts->shutdown_cb = spdk_rocksdb_shutdown;

	spdk_fs_set_cache_size(cache_size_in_mb);
	g_bdev_name = mBdev;

	pthread_create(&mSpdkTid, NULL, &initialize_spdk, opts);
	while (!g_spdk_ready)
		;

	SpdkInitializeThread();
}

SpdkEnv::~SpdkEnv()
{
	spdk_app_start_shutdown();
	pthread_join(mSpdkTid, NULL);
}

Env* NewSpdkEnv(Env *base_env, const std::string &dir, const std::string &conf,
		const std::string &bdev, uint64_t cache_size_in_mb)
{
	SpdkEnv *spdk_env = new SpdkEnv(base_env, dir, conf, bdev, cache_size_in_mb);

	if (g_fs != NULL) {
		return spdk_env;
	} else {
		delete spdk_env;
		return NULL;
	}
}

} // namespace rocksdb
