/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/fsdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#define TEST_FILENAME "hello_file"
#define DATA_SIZE 512
#define ROOT_NODEID 1

static char *g_fsdev_name = "Fs0";
int g_result = 0;

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
	struct spdk_thread *app_thread;
	struct spdk_fsdev_desc *fsdev_desc;
	struct spdk_io_channel *fsdev_io_channel;
	struct spdk_fsdev_file_object *root_fobject;
	char *fsdev_name;
	int thread_count;
};

struct hello_thread_t {
	struct hello_context_t *hello_context;
	struct spdk_thread *thread;
	struct spdk_io_channel *fsdev_io_channel;
	uint64_t unique;
	uint8_t *buf;
	char *file_name;
	struct spdk_fsdev_file_object *fobject;
	struct spdk_fsdev_file_handle *fhandle;
	struct iovec iov[2];
};

/*
 * Usage function for printing parameters that are specific to this application
 */
static void
hello_fsdev_usage(void)
{
	printf(" -f <fs>                 name of the fsdev to use\n");
}

/*
 * This function is called to parse the parameters that are specific to this application
 */
static int
hello_fsdev_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_fsdev_name = arg;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
hello_app_done(struct hello_context_t *hello_context, int rc)
{
	spdk_put_io_channel(hello_context->fsdev_io_channel);
	spdk_fsdev_close(hello_context->fsdev_desc);
	SPDK_NOTICELOG("Stopping app: rc %d\n", rc);
	spdk_app_stop(rc);
}

static void
root_forget_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct hello_context_t *hello_context = cb_arg;

	SPDK_NOTICELOG("Root forget complete (status=%d)\n", status);
	if (status) {
		SPDK_ERRLOG("Root forget failed: error %d\n", status);
		g_result = EINVAL;
	}

	hello_app_done(hello_context, g_result);
}

static void
hello_root_release(struct hello_context_t *hello_context)
{
	int res;

	SPDK_NOTICELOG("Forget root\n");
	res = spdk_fsdev_forget(hello_context->fsdev_desc, hello_context->fsdev_io_channel, 0,
				hello_context->root_fobject, 1,
				root_forget_complete, hello_context);
	if (res) {
		SPDK_ERRLOG("Failed to forget root (err=%d)\n", res);
		hello_app_done(hello_context, EINVAL);
	}
}

static void
hello_app_notify_thread_done(void *ctx)
{
	struct hello_context_t *hello_context = (struct hello_context_t *)ctx;

	assert(hello_context->thread_count > 0);
	hello_context->thread_count--;
	if (hello_context->thread_count == 0) {
		hello_root_release(hello_context);
	}
}

static void
hello_thread_done(struct hello_thread_t *hello_thread, int rc)
{
	struct hello_context_t *hello_context = hello_thread->hello_context;

	spdk_put_io_channel(hello_thread->fsdev_io_channel);
	free(hello_thread->buf);
	free(hello_thread->file_name);
	SPDK_NOTICELOG("Thread %s done: rc %d\n",
		       spdk_thread_get_name(hello_thread->thread), rc);
	spdk_thread_exit(hello_thread->thread);
	free(hello_thread);
	if (rc) {
		g_result = rc;
	}

	spdk_thread_send_msg(hello_context->app_thread, hello_app_notify_thread_done, hello_context);
}

static bool
hello_check_complete(struct hello_thread_t *hello_thread, int status, const char *op)
{
	hello_thread->unique++;
	if (status) {
		SPDK_ERRLOG("%s failed with %d\n", op, status);
		hello_thread_done(hello_thread, EIO);
		return false;
	}

	return true;
}

static void
unlink_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct hello_thread_t *hello_thread = cb_arg;

	SPDK_NOTICELOG("Unlink complete (status=%d)\n", status);
	if (!hello_check_complete(hello_thread, status, "unlink")) {
		return;
	}

	hello_thread->fobject = NULL;
	hello_thread_done(hello_thread, 0);
}

static void
hello_unlink(struct hello_thread_t *hello_thread)
{
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Unlink file %s\n", hello_thread->file_name);

	res = spdk_fsdev_unlink(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
				hello_thread->unique, hello_context->root_fobject, hello_thread->file_name,
				unlink_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("unlink failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
release_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct hello_thread_t *hello_thread = cb_arg;

	SPDK_NOTICELOG("Release complete (status=%d)\n", status);
	if (!hello_check_complete(hello_thread, status, "release")) {
		return;
	}

	hello_thread->fhandle = NULL;
	hello_unlink(hello_thread);
}

static void
hello_release(struct hello_thread_t *hello_thread)
{
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Release file handle %p\n", hello_thread->fhandle);

	res = spdk_fsdev_release(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
				 hello_thread->unique, hello_thread->fobject, hello_thread->fhandle,
				 release_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("release failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
read_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct hello_thread_t *hello_thread = cb_arg;
	uint8_t data = spdk_env_get_current_core();
	uint32_t i;

	SPDK_NOTICELOG("Read complete (status=%d, %" PRIu32 "bytes read)\n", status, data_size);
	if (!hello_check_complete(hello_thread, status, "read")) {
		return;
	}

	assert(data_size == DATA_SIZE);

	for (i = 0; i < DATA_SIZE; ++i) {
		if (hello_thread->buf[i] != data) {
			SPDK_NOTICELOG("Bad read data at offset %d, 0x%02X != 0x%02X\n",
				       i, hello_thread->buf[i], data);
			break;
		}
	}

	hello_release(hello_thread);
}

static void
hello_read(struct hello_thread_t *hello_thread)
{
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Read from file handle %p\n", hello_thread->fhandle);

	memset(hello_thread->buf, 0xFF, DATA_SIZE);

	hello_thread->iov[0].iov_base = hello_thread->buf;
	hello_thread->iov[0].iov_len = DATA_SIZE / 4;
	hello_thread->iov[1].iov_base = hello_thread->buf + hello_thread->iov[0].iov_len;
	hello_thread->iov[1].iov_len = DATA_SIZE - hello_thread->iov[0].iov_len;

	res = spdk_fsdev_read(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
			      hello_thread->unique, hello_thread->fobject, hello_thread->fhandle,
			      DATA_SIZE, 0, 0, hello_thread->iov, 2, NULL,
			      read_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("write failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
write_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct hello_thread_t *hello_thread = cb_arg;

	SPDK_NOTICELOG("Write complete (status=%d, %" PRIu32 "bytes written)\n", status, data_size);
	if (!hello_check_complete(hello_thread, status, "write")) {
		return;
	}

	assert(data_size == DATA_SIZE);
	hello_read(hello_thread);
}

static void
hello_write(struct hello_thread_t *hello_thread)
{
	uint8_t data = spdk_env_get_current_core();
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Write to file handle %p\n", hello_thread->fhandle);

	memset(hello_thread->buf, data, DATA_SIZE);

	hello_thread->iov[0].iov_base = hello_thread->buf;
	hello_thread->iov[0].iov_len = DATA_SIZE / 2;
	hello_thread->iov[1].iov_base = hello_thread->buf + hello_thread->iov[0].iov_len;
	hello_thread->iov[1].iov_len = DATA_SIZE - hello_thread->iov[0].iov_len;

	res = spdk_fsdev_write(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
			       hello_thread->unique, hello_thread->fobject, hello_thread->fhandle,
			       DATA_SIZE, 0, 0, hello_thread->iov, 2, NULL,
			       write_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("write failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
fopen_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
	       struct spdk_fsdev_file_handle *fhandle)
{
	struct hello_thread_t *hello_thread = cb_arg;

	SPDK_NOTICELOG("Open complete (status=%d)\n", status);
	if (!hello_check_complete(hello_thread, status, "open")) {
		return;
	}

	hello_thread->fhandle = fhandle;
	hello_write(hello_thread);
}

static void
hello_open(struct hello_thread_t *hello_thread)
{
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Open fobject %p\n", hello_thread->fobject);

	res = spdk_fsdev_fopen(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
			       hello_thread->unique, hello_thread->fobject, O_RDWR,
			       fopen_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("open failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
lookup_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
		struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct hello_thread_t *hello_thread = cb_arg;

	SPDK_NOTICELOG("Lookup complete (status=%d)\n", status);
	if (!hello_check_complete(hello_thread, status, "lookup")) {
		return;
	}

	assert(hello_thread->fobject == fobject);
	hello_open(hello_thread);
}

static void
hello_lookup(struct hello_thread_t *hello_thread)
{
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Lookup file %s\n", hello_thread->file_name);

	res = spdk_fsdev_lookup(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
				hello_thread->unique, hello_context->root_fobject, hello_thread->file_name,
				lookup_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("lookup failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
mknod_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
	       struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct hello_thread_t *hello_thread = cb_arg;

	SPDK_NOTICELOG("Mknod complete (status=%d)\n", status);
	if (!hello_check_complete(hello_thread, status, "mknod")) {
		return;
	}

	hello_thread->fobject = fobject;
	hello_lookup(hello_thread);
}

static void
hello_mknod(void *ctx)
{
	struct hello_thread_t *hello_thread = (struct hello_thread_t *)ctx;
	struct hello_context_t *hello_context = hello_thread->hello_context;
	int res;

	SPDK_NOTICELOG("Mknod file %s\n", hello_thread->file_name);

	res = spdk_fsdev_mknod(hello_context->fsdev_desc, hello_thread->fsdev_io_channel,
			       hello_thread->unique, hello_context->root_fobject, hello_thread->file_name,
			       S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO, 0, 0, 0, mknod_complete, hello_thread);
	if (res) {
		SPDK_ERRLOG("mknod failed with %d\n", res);
		hello_thread_done(hello_thread, EIO);
	}
}

static void
hello_start_thread(void *ctx)
{
	struct hello_context_t *hello_context = (struct hello_context_t *)ctx;
	struct hello_thread_t *hello_thread;
	/* File name size assumes that core number will fit into 3 characters */
	const int filename_size = strlen(TEST_FILENAME) + 5;

	hello_thread = calloc(1, sizeof(struct hello_thread_t));
	if (!hello_thread) {
		SPDK_ERRLOG("Failed to allocate thread context\n");
		spdk_thread_send_msg(hello_context->app_thread, hello_app_notify_thread_done, hello_context);
		return;
	}

	hello_thread->hello_context = hello_context;
	hello_thread->thread = spdk_get_thread();
	hello_thread->unique = 1;
	hello_thread->buf = (char *)malloc(DATA_SIZE);
	if (!hello_thread->buf) {
		SPDK_ERRLOG("Could not allocate data buffer\n");
		hello_thread_done(hello_thread, ENOMEM);
		return;
	}

	hello_thread->file_name = (char *)malloc(filename_size);
	if (!hello_thread->file_name) {
		SPDK_ERRLOG("Could not allocate file name buffer\n");
		hello_thread_done(hello_thread, ENOMEM);
		return;
	}

	if (snprintf(hello_thread->file_name, filename_size, "%s_%u",
		     TEST_FILENAME, spdk_env_get_current_core()) >= filename_size) {
		SPDK_ERRLOG("File name size doesn't fit into buffer\n");
		hello_thread_done(hello_thread, ENOMEM);
		return;
	}

	hello_thread->fsdev_io_channel = spdk_fsdev_get_io_channel(hello_thread->hello_context->fsdev_desc);
	if (!hello_thread->fsdev_io_channel) {
		SPDK_ERRLOG("Could not create fsdev I/O channel!\n");
		hello_thread_done(hello_thread, ENOMEM);
		return;
	}

	SPDK_NOTICELOG("Started thread %s on core %u\n",
		       spdk_thread_get_name(hello_thread->thread),
		       spdk_env_get_current_core());
	spdk_thread_send_msg(hello_thread->thread, hello_mknod, hello_thread);
}

static void
hello_create_threads(struct hello_context_t *hello_context)
{
	uint32_t cpu;
	char thread_name[32];
	struct spdk_cpuset mask = {};
	struct spdk_thread *thread;

	SPDK_ENV_FOREACH_CORE(cpu) {
		snprintf(thread_name, sizeof(thread_name), "hello_fsdev_%u", cpu);
		spdk_cpuset_zero(&mask);
		spdk_cpuset_set_cpu(&mask, cpu, true);
		thread = spdk_thread_create(thread_name, &mask);
		assert(thread != NULL);
		hello_context->thread_count++;
		spdk_thread_send_msg(thread, hello_start_thread, hello_context);
	}
}


static void
root_lookup_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
		     struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	struct hello_context_t *hello_context = cb_arg;

	SPDK_NOTICELOG("Root lookup complete (status=%d)\n", status);
	if (status) {
		SPDK_ERRLOG("Fuse init failed: error %d\n", status);
		hello_app_done(hello_context, status);
		return;
	}

	hello_context->root_fobject = fobject;

	hello_create_threads(hello_context);
}

static void
root_lookup(struct hello_context_t *hello_context)
{
	int res;

	SPDK_NOTICELOG("Lookup for the root\n");

	res = spdk_fsdev_lookup(hello_context->fsdev_desc, hello_context->fsdev_io_channel, 0,
				NULL /* root */, "" /* will be ignored */, root_lookup_complete, hello_context);
	if (res) {
		SPDK_ERRLOG("Failed to initiate lookup for the root (err=%d)\n", res);
		hello_app_done(hello_context, res);
		return;
	}
}

static void
hello_fsdev_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev, void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported fsdev event: type %d\n", type);
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1)
{
	struct hello_context_t *hello_context = arg1;
	int rc = 0;
	hello_context->fsdev_desc = NULL;

	SPDK_NOTICELOG("Successfully started the application\n");

	hello_context->app_thread = spdk_get_thread();

	/*
	 * There can be many fsdevs configured, but this application will only use
	 * the one input by the user at runtime.
	 *
	 * Open the fs by calling spdk_fsdev_open() with its name.
	 * The function will return a descriptor
	 */
	SPDK_NOTICELOG("Opening the fsdev %s\n", hello_context->fsdev_name);
	rc = spdk_fsdev_open(hello_context->fsdev_name,
			     hello_fsdev_event_cb, NULL,
			     &hello_context->fsdev_desc);
	if (rc) {
		SPDK_ERRLOG("Could not open fsdev: %s\n", hello_context->fsdev_name);
		spdk_app_stop(-1);
		return;
	}

	SPDK_NOTICELOG("Opening io channel\n");
	/* Open I/O channel */
	hello_context->fsdev_io_channel = spdk_fsdev_get_io_channel(hello_context->fsdev_desc);
	if (!hello_context->fsdev_io_channel) {
		SPDK_ERRLOG("Could not create fsdev I/O channel!\n");
		spdk_fsdev_close(hello_context->fsdev_desc);
		spdk_app_stop(-1);
		return;
	}

	root_lookup(hello_context);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context_t hello_context = {};

	/* Set default values in opts structure. */
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "hello_fsdev";

	/*
	 * Parse built-in SPDK command line parameters as well
	 * as our custom one(s).
	 */
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:", NULL, hello_fsdev_parse_arg,
				      hello_fsdev_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}
	hello_context.fsdev_name = g_fsdev_name;

	/*
	 * spdk_app_start() will initialize the SPDK framework, call hello_start(),
	 * and then block until spdk_app_stop() is called (or if an initialization
	 * error occurs, spdk_app_start() will return with rc even without calling
	 * hello_start().
	 */
	rc = spdk_app_start(&opts, hello_start, &hello_context);
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}

	/* At this point either spdk_app_stop() was called, or spdk_app_start()
	 * failed because of internal error.
	 */

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
