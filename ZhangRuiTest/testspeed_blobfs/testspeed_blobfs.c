
#include "spdk/event.h"
#include "spdk/blob.h"
#include "spdk/blobfs.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include<sys/time.h>
#include "spdk_internal/thread.h"

#define BUFFERSIZE 250000
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
		printf("load failed, error code num is %s\n", strerror(errno));
	}
	g_spdk_ready = true;
}



static void
set_channel(void)
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
deletefile(void)
{

	int rc;
	set_channel();
	rc = spdk_fs_delete_file(g_fs,  g_sync_args.channel, "bigfile500M");
	if (rc < 0) {
		printf("deletefile error\n");
	} else {
		printf("delete successfuly\n");
	}

}
static void
readfromblobfs(char *filename)
{
	char newfilename[100];
	int rc;
	uint64_t filelength, remain, now = 0;
	FILE *fd;
	int eachtime = BUFFERSIZE;
	struct timeval starttime, endtime;
	double timeuse, writespeed, readspeed;
	void *block = malloc(4L * 1024 * 1024 * 1024);
	rc = spdk_fs_open_file(g_fs, g_sync_args.channel, filename, SPDK_BLOBFS_OPEN_CREATE, &g_file);
	sprintf(newfilename, "%s%s", "blobfs_", filename);
	fd = fopen(newfilename, "wb");
	filelength = spdk_file_get_length(g_file);
	printf("begin read from blobfs, newfilename is %s, file length is %lu\n", newfilename, filelength);
	remain = filelength;
	gettimeofday(&starttime, NULL);
	while (remain > 0) {
		eachtime = remain > BUFFERSIZE ? BUFFERSIZE : remain;
		// printf("eachtime is %d\n", eachtime);
		rc = spdk_file_read(g_file, g_sync_args.channel, block + now, now, eachtime);
		// printf("rc is %d\n", rc);
		if (rc < 0) {
			printf("read error %s\n", strerror(rc));

		}
		remain -= eachtime;
		now += eachtime;
		//write(fd, r_buf, eachtime);
		// printf("write complete. remain is %d\n", remain);


	}
	gettimeofday(&endtime, NULL);
	spdk_file_close(g_file, g_sync_args.channel);
	timeuse = 1000000 * (endtime.tv_sec - starttime.tv_sec) + endtime.tv_usec - starttime.tv_usec;
	timeuse /= 1000000; /*转换成毫秒输出*/
	readspeed = filelength / 1000 / 1000 / timeuse;
	printf("读取完成, read time is %f, read speed is %fMB/s\n", timeuse, readspeed);
	printf("begin write\n");
	remain = filelength;
	now = 0;
	gettimeofday(&starttime, NULL);
	while (remain > 0) {
		eachtime = remain > BUFFERSIZE ? BUFFERSIZE : remain;
		fwrite((char *)block + now, sizeof(char), eachtime, fd);
		remain -= eachtime;
		now += eachtime;
	}
	gettimeofday(&endtime, NULL);
	fclose(fd);
	timeuse = 1000000 * (endtime.tv_sec - starttime.tv_sec) + endtime.tv_usec - starttime.tv_usec;
	timeuse /= 1000000; /*转换成毫秒输出*/
	writespeed = filelength / 1000 / 1000 / timeuse;
	printf("write to kernel complete, write time is %f, write speed is %fMB/s\n", timeuse, writespeed);
	if (block) {
		free(block);
		printf("free successfully\n");
	}
	printf("end close\n");

}
static void
testwritespeed(void)
{
	FILE *fp;
	int rc;
	void *block;
	int nbytes, writesize;
	long long filesize, index = 0;
	uint64_t filelength;
	char filename[] = "bigfile1G";
	struct timeval starttime, endtime;
	double timeuse, writespeed, readspeed;
	block = malloc(4L * 1024 * 1024 * 1024);
	if (!block) {
		printf("malloc error\n");
		free(block);
		spdk_app_stop(-1);
	}
	fp = fopen(filename, "rb");
	if (!fp) {
		printf("open file error\n");
		free(block);
		spdk_app_stop(-1);
	}
	gettimeofday(&starttime, NULL);
	while ((nbytes = fread(block + index, sizeof(char), BUFFERSIZE, fp)) > 0) {
		index += nbytes;
	}
	gettimeofday(&endtime, NULL);
	fclose(fp);
	filesize = index;
	index = 0;
	timeuse = 1000000 * (endtime.tv_sec - starttime.tv_sec) + endtime.tv_usec - starttime.tv_usec;
	timeuse /= 1000000; /*转换成毫秒输出*/
	readspeed = filesize / 1000 / 1000 / timeuse;
	printf("read from kernel complete, filesize is %lld, read time is %f, read speed is %fMB/s\n",
	       filesize, timeuse, readspeed);

	set_channel();
	rc = spdk_fs_delete_file(g_fs,  g_sync_args.channel, filename);
	if (rc < 0) {
		printf("deletefile error\n");
	} else {
		printf("delete successfuly\n");
	}
	rc = spdk_fs_open_file(g_fs, g_sync_args.channel, filename, SPDK_BLOBFS_OPEN_CREATE, &g_file);
	if (rc < 0) {
		printf("open file error, %s\n", strerror(rc));
		free(block);
		spdk_app_stop(-1);
	}
	filelength = spdk_file_get_length(g_file);
	printf("before write, spdk file length is %ld\n", filelength);

	writesize = BUFFERSIZE > filesize ? filesize : BUFFERSIZE;
	printf("begin write to blobfs\n");
	gettimeofday(&starttime, NULL);
	while (filesize > 0) {
		rc = spdk_file_write(g_file, g_sync_args.channel, block + index, index, writesize);
		if (rc < 0) {
			printf("write file error, %s\n", strerror(rc));
			free(block);
			block = NULL;
			spdk_app_stop(-1);
		}
		index += writesize;
		filesize -= writesize;
		writesize = BUFFERSIZE > filesize ? filesize : BUFFERSIZE;
	}
	gettimeofday(&endtime, NULL);
	filelength = spdk_file_get_length(g_file);
	printf("spdk file length is %ld\n", filelength);
	spdk_file_close(g_file, g_sync_args.channel);

	timeuse = 1000000 * (endtime.tv_sec - starttime.tv_sec) + endtime.tv_usec - starttime.tv_usec;
	timeuse /= 1000000; /*转换成毫秒输出*/
	writespeed = filelength / 1000 / 1000 / timeuse;
	printf("写入完成, filesize is %ld, write time is %f, write speed is %fMB/s\n", filelength,
	       timeuse, writespeed);


	if (block) {
		free(block);
	}
	readfromblobfs(filename);

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
	char mConfig[100] = "./ruiblobfs.conf";
	pthread_t mSpdkTid;
	//uint64_t cache_size_in_mb = 100;
	char mBdev[100] = "Nvme0n1";
	struct spdk_app_opts opts = {};
	spdk_app_opts_init(&opts);
	opts.name = "ruiblobfs";
	opts.config_file = mConfig;
	opts.shutdown_cb = spdk_ruiblobfs_shutdown;


	//spdk_fs_set_cache_size(cache_size_in_mb);
	strcpy(g_bdev_name, mBdev);
	//printf("g_bdev_name is %s\n", g_bdev_name);
	pthread_create(&mSpdkTid, NULL, &initialize_spdk, &opts);
	while (!g_spdk_ready && !g_spdk_start_failure)
		;
	if (g_spdk_start_failure) {

		printf("spdk_app_start() unable to start spdk_rocksdb_run()");
	}
	testwritespeed();
	// deletefile();
	pthread_join(mSpdkTid, NULL);


	return 0;


}
