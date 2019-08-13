
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob.h"
#include "spdk/blobfs.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#include "spdk_internal/thread.h"

struct spdk_filesystem *g_fs = NULL;
struct spdk_bs_dev *g_bs_dev;
uint32_t g_lcore = 0;
char g_bdev_name[100];
volatile bool g_spdk_ready = false;
volatile bool g_spdk_start_failure = false;
struct spdk_file *g_file;


static void
fs_unload_cb(__attribute__((unused)) void *ctx,
	     __attribute__((unused)) int fserrno)
{
	assert(fserrno == 0);

	spdk_app_stop(0);
}

struct SpdkThreadCtx {
	struct spdk_fs_thread_ctx *channel;
};

struct SpdkThreadCtx g_sync_args = {NULL};


static void
spdk_ruiblobfs_shutdown(void)
{
	printf("spdk_ruiblobfs_shutdown called\n");
	if (g_sync_args.channel) {
		spdk_fs_free_thread_ctx(g_sync_args.channel);
		g_sync_args.channel = NULL;
	}
	if (g_fs != NULL) {
		spdk_fs_unload(g_fs, fs_unload_cb, NULL);
	} else {
		fs_unload_cb(NULL, 0);
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
		printf("load failed, error code num is %d\n", fserrno);
	}
	g_spdk_ready = true;
}

static void
init()
{
	struct spdk_thread *thread;
	if (g_fs != NULL) {
		thread = spdk_thread_create("rui_blobfs", NULL);
		spdk_set_thread(thread);
		g_sync_args.channel = spdk_fs_alloc_thread_ctx(g_fs);
	}
}

static void
set_channel()
{
	struct spdk_thread *thread;

	if (g_fs != NULL && g_sync_args.channel == NULL) {
		thread = spdk_thread_create("rui_blobfs", NULL);
		spdk_set_thread(thread);
		g_sync_args.channel = spdk_fs_alloc_thread_ctx(g_fs);
		printf("channel seted\n");
	}
}





static void
cache_read_after_write(void)
{
	uint64_t length;
	int rc;
	char w_buf[100] = "hello world", r_buf[100];

	set_channel();
	rc = spdk_fs_open_file(g_fs, g_sync_args.channel, "testfile.txt", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	printf("spdk_fs_open_file %d\n", rc);

	/*length = (4 * 1024 * 1024);
	rc = spdk_file_truncate(g_file, g_sync_args.channel, length);
	printf("spdk_file_truncate %d\n", rc);
	if(rc<0)
	{
		printf("error info is %s\n",strerror(errno));
	}

	rc = spdk_file_write(g_file, g_sync_args.channel, w_buf, 0, sizeof(w_buf));
	printf("spdk_file_write %d\n", rc);
	if(rc<0)
	{
	printf("error info is %s\n",strerror(errno));
	}*/

	rc = spdk_file_read(g_file, g_sync_args.channel, r_buf, 0, sizeof(r_buf));
	printf("%s\n", r_buf);
	printf("spdk_file_read %d\n", rc);

	spdk_file_close(g_file, g_sync_args.channel);





}
static void
spdk_ruiblobfs_run(__attribute__((unused)) void *arg1)
{
	struct spdk_bdev *bdev;
	printf("start get bdev\n");
	bdev = spdk_bdev_get_by_name(g_bdev_name);

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev_name);
		exit(1);
	}

	g_lcore = spdk_env_get_first_core();

	g_bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	printf("using bdev %s\n", g_bdev_name);
	spdk_fs_load(g_bs_dev, __send_request, fs_load_cb, NULL);


}

static void *
initialize_spdk(void *arg)
{
	struct spdk_app_opts *opts = (struct spdk_app_opts *)arg;
	int rc;
	printf("init start\n");
	rc = spdk_app_start(opts, spdk_ruiblobfs_run, NULL);
	/*
	 * TODO:  Revisit for case of internal failure of
	 * spdk_app_start(), itself.  At this time, it's known
	 * the only application's use of spdk_app_stop() passes
	 * a zero; i.e. no fail (non-zero) cases so here we
	 * assume there was an internal failure and flag it
	 * so we can throw an exception.
	 */
	printf("success\n");
	if (rc) {
		printf("fail\n");
		g_spdk_start_failure = true;
	} else {
		spdk_app_fini();

	}
	printf("exit");
	pthread_exit(NULL);

}


int main(void)
{
	char mConfig[100] = "/root/spdk/ZhangRuiTest/try_blobfs/ruiblobfs.conf";
	pthread_t mSpdkTid;
	uint64_t cache_size_in_mb = 100;
	char mBdev[100] = "Nvme0n1";
	struct spdk_app_opts opts = {};
	spdk_app_opts_init(&opts);
	opts.name = "ruiblobfs";
	opts.config_file = mConfig;
	opts.shutdown_cb = spdk_ruiblobfs_shutdown;


	spdk_fs_set_cache_size(cache_size_in_mb);
	strcpy(g_bdev_name, mBdev);
	//printf("g_bdev_name is %s\n", g_bdev_name);
	pthread_create(&mSpdkTid, NULL, &initialize_spdk, &opts);
	while (!g_spdk_ready && !g_spdk_start_failure)
		;
	if (g_spdk_start_failure) {

		printf("spdk_app_start() unable to start spdk_rocksdb_run()");
	}
	cache_read_after_write();
	pthread_join(mSpdkTid, NULL);


	return 0;


}
