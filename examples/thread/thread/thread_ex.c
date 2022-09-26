/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/init.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/rpc.h"
#include "spdk/likely.h"

#include "spdk_internal/event.h"
#include "spdk_internal/thread.h"

#define NAME_MAX_LENGTH 256
#define TIMED_POLLER_PERIOD 1000000
#define POLLING_TIME 6
#define MAX_POLLER_TYPE_STR_LEN 100

#define POLLER_TYPE_ACTIVE "active"
#define POLLER_TYPE_TIMED "timed"

struct lw_thread {
	TAILQ_ENTRY(lw_thread) link;
	bool resched;
};

struct reactor {
	uint32_t core;

	struct spdk_ring	*threads;
	TAILQ_ENTRY(reactor)	link;
};

struct poller_ctx {
	char *poller_type;
	uint64_t *run_count;
};

static struct reactor g_main_reactor;
static struct spdk_thread *g_init_thread = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_time_start;
static uint64_t g_counting_poller_counter;
static uint64_t g_printing_poller_counter;
static uint64_t g_for_each_thread_poller_counter;
static uint64_t g_for_each_channel_poller_counter;
static uint64_t g_thread_poll_cnt;
static uint64_t g_io_channel_cnt;
static struct spdk_poller *g_active_poller = NULL, *g_timed_poller = NULL;
static struct spdk_poller *g_timed_for_each_thread = NULL, *g_timed_for_each_channel = NULL;

static int schedule_spdk_thread(struct spdk_thread *thread);

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-h show this usage message]\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *opts)
{
	int op;

	while ((op = getopt(argc, argv, "h")) != -1) {
		switch (op) {
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

static void
reactor_run(void)
{
	struct reactor *reactor = &g_main_reactor;
	struct lw_thread *lw_thread;
	struct spdk_thread *thread = NULL;

	/* Run all the SPDK threads in this reactor by FIFO. */
	if (spdk_ring_dequeue(reactor->threads, (void **)&lw_thread, 1)) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		assert(thread != NULL);

		spdk_thread_poll(thread, 0, 0);

		/* spdk_unlikely() is a branch prediction macro. Here it means the
		 * thread should not be exited and idle, but it is still possible. */
		if (spdk_unlikely(spdk_thread_is_exited(thread) &&
				  spdk_thread_is_idle(thread))) {
			spdk_thread_destroy(thread);
		} else {
			spdk_ring_enqueue(reactor->threads, (void **)&lw_thread, 1, NULL);
		}
	}
}

static void
reactor_run_fini(void)
{
	struct reactor *reactor = &g_main_reactor;
	struct lw_thread *lw_thread;
	struct spdk_thread *thread = NULL;

	/* Free all the lightweight threads. */
	while (spdk_ring_dequeue(reactor->threads, (void **)&lw_thread, 1)) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		assert(thread != NULL);
		spdk_set_thread(thread);

		if (spdk_thread_is_exited(thread)) {
			spdk_thread_destroy(thread);
		} else {
			/* This thread is not exited yet, and may need to communicate
			 * with other threads to be exited. So mark it as exiting,
			 * and check again after traversing other threads. */
			spdk_thread_exit(thread);
			spdk_thread_poll(thread, 0, 0);
			spdk_ring_enqueue(reactor->threads, (void **)&lw_thread, 1, NULL);
		}
	}
}

static int
schedule_spdk_thread(struct spdk_thread *thread)
{
	struct reactor *reactor;
	struct lw_thread *lw_thread;

	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);
	memset(lw_thread, 0, sizeof(*lw_thread));

	/* Assign lightweight threads to reactor(core). Here we use a mutex.
	 * The way the actual SPDK event framework solves this is by using
	 * internal rings for messages between reactors. */
	pthread_mutex_lock(&g_mutex);
	reactor = &g_main_reactor;

	spdk_ring_enqueue(reactor->threads, (void **)&lw_thread, 1, NULL);
	pthread_mutex_unlock(&g_mutex);

	return 0;
}

static int
reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
		return schedule_spdk_thread(thread);
	default:
		return -ENOTSUP;
	}
}

static bool
reactor_thread_op_supported(enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
		return true;
	default:
		return false;
	}
}

static int
init_reactor(void)
{
	int rc;
	char thread_name[32];
	struct spdk_cpuset cpumask;
	uint32_t main_core = spdk_env_get_current_core();

	printf("Initializing thread library.\n");

	/* Whenever SPDK creates a new lightweight thread it will call
	 * schedule_spdk_thread() asking for the application to begin
	 * polling it via spdk_thread_poll(). Each lightweight thread in
	 * SPDK optionally allocates extra memory to be used by the application
	 * framework. The size of the extra memory allocated is the third parameter. */
	spdk_thread_lib_init_ext(reactor_thread_op, reactor_thread_op_supported,
				 sizeof(struct lw_thread), SPDK_DEFAULT_MSG_MEMPOOL_SIZE);

	g_main_reactor.core = main_core;

	g_main_reactor.threads = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 1024, SPDK_ENV_SOCKET_ID_ANY);
	if (!g_main_reactor.threads) {
		fprintf(stderr, "ERROR: Failed to alloc thread ring!\n");
		rc = -ENOMEM;
		goto err_exit;
	}

	/* Spawn an spdk_thread thread on the current core to manage this application. */
	spdk_cpuset_zero(&cpumask);
	spdk_cpuset_set_cpu(&cpumask, main_core, true);
	snprintf(thread_name, sizeof(thread_name), "example_main_thread");
	g_init_thread = spdk_thread_create(thread_name, &cpumask);
	if (!g_init_thread) {
		fprintf(stderr, "ERROR: Failed to create SPDK thread!\n");
		return -1;
	}

	fprintf(stdout, "SPDK threads initialized successfully.\n");
	return 0;

err_exit:
	return rc;
}

static void
destroy_threads(void)
{
	struct reactor *reactor = &g_main_reactor;

	spdk_ring_free(reactor->threads);

	pthread_mutex_destroy(&g_mutex);
	spdk_thread_lib_fini();
	printf("Threads destroyed successfully\n");
}

static void
thread_fn(void *ctx)
{
	struct spdk_thread *thread = ctx;

	printf("Hello from new SPDK thread! Thread name: %s\n", spdk_thread_get_name(thread));
}

static struct spdk_thread *
register_thread(char *thread_num)
{
	struct spdk_thread *thread = NULL;
	char thread_name[16] = "example_thread";
	struct spdk_cpuset tmp_cpumask = {};

	strncat(thread_name, thread_num, 1);

	printf("Initializing new SPDK thread: %s\n", thread_name);

	spdk_cpuset_zero(&tmp_cpumask);
	spdk_cpuset_set_cpu(&tmp_cpumask, spdk_env_get_first_core(), true);

	thread = spdk_thread_create(thread_name, &tmp_cpumask);
	assert(thread != NULL);

	spdk_thread_send_msg(thread, thread_fn, thread);

	return thread;
}

static int
create_cb(void *io_device, void *ctx_buf)
{
	int *ch_count = io_device;

	(*ch_count)++;

	printf("Hello from IO device register callback!\n");

	return 0;
}

static void
destroy_cb(void *io_device, void *ctx_buf)
{
	int *ch_count = io_device;

	(*ch_count)--;

	printf("Hello from IO device destroy callback!\n");
}

static void
app_thread_register_io_device(void *arg)
{
	struct spdk_io_channel *ch0 = NULL;

	printf("Registering a new IO device.\n");
	spdk_io_device_register(&g_io_channel_cnt, create_cb, destroy_cb,
				sizeof(int), NULL);

	/* Get a reference pointer to IO channel. */
	ch0 = spdk_get_io_channel(&g_io_channel_cnt);
	assert(ch0 != NULL);
	/* Put (away) the reference pointer. */
	spdk_put_io_channel(ch0);
}

static void
unregister_cb(void *io_device)
{
	int *ch_count __attribute__((unused));

	ch_count = io_device;
	assert(*ch_count == 0);

	printf("Hello from IO device unregister callback!\n");
}

static void
app_thread_unregister_io_device(void *arg)
{
	printf("Unregistering IO device...\n");

	spdk_io_device_unregister(&g_io_channel_cnt, unregister_cb);
}

static int
poller_count(void *arg)
{
	struct poller_ctx *ctx = arg;
	uint64_t time_diff;

	time_diff = (spdk_get_ticks() - g_time_start) / spdk_get_ticks_hz();

	(*ctx->run_count)++;

	/* After POLLING_TIME seconds pass, let the poller unregister itself. */
	if (time_diff >= POLLING_TIME) {
		spdk_poller_unregister(&g_active_poller);
	}

	return 0;
}

static void
thread1_counting_poller(void *arg)
{
	struct poller_ctx *ctx = arg;

	printf("Registering new active poller...\n");
	/* Register an ACTIVE poller for this SPDK thread.
	 * Active poller runs continuously, in other words:
	 * it's execution period is set to 0. */
	g_active_poller = SPDK_POLLER_REGISTER(poller_count, ctx, 0);
	assert(g_active_poller != NULL);
}

static int
poller_print_msg(void *arg)
{
	struct poller_ctx *ctx = arg;
	uint64_t time_diff;

	time_diff = (spdk_get_ticks() - g_time_start) / spdk_get_ticks_hz();
	(*ctx->run_count)++;

	printf("Hello from %s poller! Time elapsed: %ld, Current run count: %ld\n", ctx->poller_type,
	       time_diff, *ctx->run_count);

	/* After POLLING_TIME seconds pass, let the poller unregister itself. */
	if (time_diff >= POLLING_TIME) {
		spdk_poller_unregister(&g_timed_poller);
	}

	return 0;
}

static void
thread2_printing_poller(void *arg)
{
	struct poller_ctx *ctx = arg;

	printf("Registering new timed poller...\n");
	/* Timed pollers run every set time period defined in microseconds.
	 * This one is set to execute every "TIMED_POLLER_PERIOD". */
	g_timed_poller = SPDK_POLLER_REGISTER(poller_print_msg, ctx, TIMED_POLLER_PERIOD);
	assert(g_timed_poller != NULL);
}

static void
thread_msg_fn(void *arg)
{
	uint64_t *thread_poll_cnt = arg;
	struct spdk_thread *thread = spdk_get_thread();

	(*thread_poll_cnt)++;

	printf("Message received by thread: %s, current thread poll count: %ld\n",
	       spdk_thread_get_name(thread), *thread_poll_cnt);
}

static void
thread_msg_cpl_fn(void *arg)
{
	printf("Finished iterating over SPDK threads!\n");
}

static int
poller_for_each_thread(void *arg)
{
	struct poller_ctx *ctx = arg;
	uint64_t time_diff;

	time_diff = (spdk_get_ticks() - g_time_start) / spdk_get_ticks_hz();
	(*ctx->run_count)++;

	printf("Calling all threads from %s poller! Time elapsed: %ld, Current run count: %ld\n",
	       ctx->poller_type, time_diff, *ctx->run_count);

	/* Send a message to each thread. */
	spdk_for_each_thread(thread_msg_fn, &g_thread_poll_cnt, thread_msg_cpl_fn);

	/* After POLLING_TIME seconds pass, let the poller unregister itself. */
	if (time_diff >= POLLING_TIME) {
		spdk_poller_unregister(&g_timed_for_each_thread);
	}

	return 0;
}

static void
thread2_for_each_thread_poller(void *arg)
{
	struct poller_ctx *ctx = arg;

	printf("Registering new timed poller...\n");
	/* Register a poller to send a message to all available threads via
	 * spdk_for_each_thread(). */
	g_timed_for_each_thread = SPDK_POLLER_REGISTER(poller_for_each_thread, ctx, TIMED_POLLER_PERIOD);
	assert(g_timed_for_each_thread != NULL);
}

static void
io_device_send_msg_fn(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_thread *thread = spdk_io_channel_get_thread(ch);

	printf("Iterating over IO channels. Currently on thread: %s and IO device: %s\n",
	       spdk_thread_get_name(thread), spdk_io_channel_get_io_device_name(ch));
	spdk_for_each_channel_continue(i, 0);
}

static void
io_device_msg_cpl_fn(struct spdk_io_channel_iter *i, int status)
{
	printf("Completed iterating over IO channels with status: %d.\n", status);
}

static int
poller_for_each_channel(void *arg)
{
	struct poller_ctx *ctx = arg;
	uint64_t time_diff;

	time_diff = (spdk_get_ticks() - g_time_start) / spdk_get_ticks_hz();
	(*ctx->run_count)++;

	printf("Calling all IO channels from %s poller! Time elapsed: %ld, Current run count: %ld\n",
	       ctx->poller_type, time_diff, *ctx->run_count);

	/* Send a message to all io devices. */
	spdk_for_each_channel(&g_io_channel_cnt, io_device_send_msg_fn, NULL, io_device_msg_cpl_fn);

	/* After POLLING_TIME seconds pass, let the poller unregister itself. */
	if (time_diff >= POLLING_TIME) {
		spdk_poller_unregister(&g_timed_for_each_channel);
	}

	return 0;
}

static void
thread2_for_each_channel_poller(void *arg)
{
	struct poller_ctx *ctx = arg;

	printf("Registering new timed poller...\n");
	/* Register a poller to send a message to all available IO channels via
	 * spdk_for_each_channel(). */
	g_timed_for_each_channel = SPDK_POLLER_REGISTER(poller_for_each_channel, ctx, TIMED_POLLER_PERIOD);
	assert(g_timed_for_each_channel != NULL);
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct spdk_thread *example_thread1, *example_thread2;
	uint64_t time_diff = 0;
	struct poller_ctx ctx_counting, ctx_printing, ctx_for_each_thread, ctx_for_each_channel;

	spdk_env_opts_init(&opts);
	opts.name = "thread-example";
	opts.core_mask = "0x1";

	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		fprintf(stderr, "ERROR: Unable to parse program args! Code: %d\n", rc);
		return rc;
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "ERROR: Unable to initialize SPDK env!\n");
		return -EINVAL;
	}

	/* Initialize a reactor and an SPDK thread to manage the application. */
	rc = init_reactor();
	if (rc != 0) {
		fprintf(stderr, "ERROR: Unable to initialize reactor! Code: %d\n", rc);
		return rc;
	}

	/* Get a time reference to print elapsed time in poller functions. */
	g_time_start = spdk_get_ticks();

	/* Register a mock IO device on app_thread (main application thread). */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), app_thread_register_io_device, NULL);

	/* Register two new SPDK threads. */
	example_thread1 = register_thread("1");
	example_thread2 = register_thread("2");

	/* Signal the first thread to register and execute an ACTIVE poller, which will run as often as possible. */
	ctx_counting.poller_type = POLLER_TYPE_ACTIVE;
	ctx_counting.run_count = &g_counting_poller_counter;
	spdk_thread_send_msg(example_thread1, thread1_counting_poller, &ctx_counting);

	/* Signal the second thread to register and execute TIMED pollers, which will run periodically. */
	ctx_printing.poller_type = POLLER_TYPE_TIMED;
	ctx_printing.run_count = &g_printing_poller_counter;
	spdk_thread_send_msg(example_thread2, thread2_printing_poller, &ctx_printing);

	ctx_for_each_thread.poller_type = POLLER_TYPE_TIMED;
	ctx_for_each_thread.run_count = &g_for_each_thread_poller_counter;
	spdk_thread_send_msg(example_thread2, thread2_for_each_thread_poller, &ctx_for_each_thread);

	ctx_for_each_channel.poller_type = POLLER_TYPE_TIMED;
	ctx_for_each_channel.run_count = &g_for_each_channel_poller_counter;
	spdk_thread_send_msg(example_thread2, thread2_for_each_channel_poller, &ctx_for_each_channel);

	/* Poll SPDK threads and IO devices for POLLING_TIME + 1 seconds - to avoid a race
	 * between all the pollers and IO device unregistering, let below while loop
	 * poll for one second longer than all the pollers.  */
	while (time_diff < POLLING_TIME + 1) {
		time_diff = (spdk_get_ticks() - g_time_start) / spdk_get_ticks_hz();
		reactor_run();
	}

	printf("ACTIVE (counting) poller ran %lu times.\n", g_counting_poller_counter);
	printf("TIMED (printing) poller ran %lu times.\n", g_printing_poller_counter);
	printf("TIMED (for each thread) poller ran %lu times.\n", g_for_each_thread_poller_counter);
	printf("TIMED (for each channel) poller ran %lu times.\n", g_for_each_channel_poller_counter);

	/* Unregister the mock IO device. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), app_thread_unregister_io_device, NULL);

	/* Disable the reactor and free all SPDK threads. */
	reactor_run_fini();
	destroy_threads();

	/* Stop SPDK environment. */
	spdk_env_fini();

	return 0;
}
