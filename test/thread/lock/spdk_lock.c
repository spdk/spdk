/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk_internal/thread.h"
#include "spdk/barrier.h"

#include "thread/thread.c"

/*
 * Used by multiple tests
 */

typedef void (*test_setup_fn)(void);

struct test {
	/* Initialized in g_tests array */
	const char		*name;
	uint32_t		thread_count;
	test_setup_fn		setup_fn;
	spdk_poller_fn		end_fn;
	uint32_t		poller_thread_number;
	/* State set while a test is running */
	struct spdk_poller	*poller;
};

#define ASSERT(cond) do { \
	if (cond) { \
		g_pass++; \
	} else { \
		g_fail++; \
		printf("FAIL: %s:%d %s %s\n", __FILE__, __LINE__, __func__, #cond); \
	} \
} while (0);

#define WORKER_COUNT 2

static uint32_t g_pass;
static uint32_t g_fail;
static struct spdk_thread *g_thread[WORKER_COUNT];
/* Protects g_lock_error_count during updates by spin_abort_fn(). */
pthread_mutex_t g_lock_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_lock_error_count[SPIN_ERR_LAST];

static void launch_next_test(void *arg);

static bool
check_spin_err_count(enum spin_error *expect)
{
	enum spin_error i;
	bool ret = true;

	for (i = SPIN_ERR_NONE; i < SPIN_ERR_LAST; i++) {
		if (g_lock_error_count[i] != expect[i]) {
			printf("FAIL: %s: Error %d expected %u, got %u\n", __func__, i,
			       expect[i], g_lock_error_count[i]);
			ret = false;
		}
	}

	return ret;
}

/* A spin_abort_fn() implementation */
static void
do_not_abort(enum spin_error error)
{
	struct spdk_thread *thread = spdk_get_thread();
	uint32_t i;

	/*
	 * Only count on threads for the current test. Those from a previous test may continue to
	 * rack up errors in their death throes. A real application will abort() or exit() on the
	 * first error.
	 */
	for (i = 0; i < SPDK_COUNTOF(g_thread); i++) {
		if (g_thread[i] != thread) {
			continue;
		}
		ASSERT(error >= SPIN_ERR_NONE && error < SPIN_ERR_LAST);
		if (error >= SPIN_ERR_NONE && error < SPIN_ERR_LAST) {
			pthread_mutex_lock(&g_lock_lock);
			g_lock_error_count[error]++;
			pthread_mutex_unlock(&g_lock_lock);
		}
	}
}

/*
 * contend - make sure that two concurrent threads can take turns at getting the lock
 */

struct contend_worker_data {
	struct spdk_poller *poller;
	uint64_t wait_time;
	uint64_t hold_time;
	uint32_t increments;
	uint32_t delay_us;
	uint32_t bit;
};

static struct spdk_spinlock g_contend_spinlock;
static uint32_t g_contend_remaining;
static uint32_t g_get_lock_times = 50000;
static struct contend_worker_data g_contend_data[WORKER_COUNT] = {
	{ .bit = 0, .delay_us = 3 },
	{ .bit = 1, .delay_us = 5 },
};

static inline uint64_t
timediff(struct timespec *ts0, struct timespec *ts1)
{
	return (ts1->tv_sec - ts0->tv_sec) * SPDK_SEC_TO_NSEC + ts1->tv_nsec - ts0->tv_nsec;
}

static uint32_t g_contend_word;

static int
contend_worker_fn(void *arg)
{
	struct contend_worker_data *data = arg;
	struct timespec ts0, ts1, ts2;
	const uint32_t mask = 1 << data->bit;

	clock_gettime(CLOCK_MONOTONIC, &ts0);
	spdk_spin_lock(&g_contend_spinlock);
	clock_gettime(CLOCK_MONOTONIC, &ts1);
	data->wait_time += timediff(&ts0, &ts1);

	switch (data->increments & 0x1) {
	case 0:
		ASSERT((g_contend_word & mask) == 0);
		g_contend_word |= mask;
		break;
	case 1:
		ASSERT((g_contend_word & mask) == mask);
		g_contend_word ^= mask;
		break;
	default:
		abort();
	}
	data->increments++;
	spdk_delay_us(data->delay_us);

	if (data->increments == g_get_lock_times) {
		g_contend_remaining--;
		spdk_poller_unregister(&data->poller);
		assert(data->poller == NULL);
	}

	spdk_spin_unlock(&g_contend_spinlock);
	clock_gettime(CLOCK_MONOTONIC, &ts2);
	data->hold_time += timediff(&ts1, &ts2);

	return SPDK_POLLER_BUSY;
}

static void
contend_start_worker_poller(void *ctx)
{
	struct contend_worker_data *data = ctx;

	data->poller = SPDK_POLLER_REGISTER(contend_worker_fn, data, 0);
	if (data->poller == NULL) {
		fprintf(stderr, "Failed to start poller\n");
		abort();
	}
}

static void
contend_setup(void)
{
	uint32_t i;

	memset(&g_contend_spinlock, 0, sizeof(g_contend_spinlock));
	spdk_spin_init(&g_contend_spinlock);
	g_contend_remaining = SPDK_COUNTOF(g_contend_data);

	/* Add a poller to each thread */
	for (i = 0; i < SPDK_COUNTOF(g_contend_data); i++) {
		spdk_thread_send_msg(g_thread[i], contend_start_worker_poller, &g_contend_data[i]);
	}
}

static int
contend_end(void *arg)
{
	struct test *test = arg;
	enum spin_error expect[SPIN_ERR_LAST] = { 0 };
	uint32_t i;

	if (g_contend_remaining != 0) {
		return SPDK_POLLER_IDLE;
	}

	ASSERT(check_spin_err_count(expect));
	ASSERT(g_get_lock_times == g_contend_data[0].increments);
	ASSERT(g_get_lock_times == g_contend_data[1].increments);

	printf("%8s %8s %8s %8s %8s\n", "Worker", "Delay", "Wait us", "Hold us", "Total us");
	for (i = 0; i < SPDK_COUNTOF(g_contend_data); i++) {
		printf("%8" PRIu32 " %8" PRIu32 " %8" PRIu64 " %8" PRIu64 " %8" PRIu64 "\n",
		       i, g_contend_data[i].delay_us,
		       g_contend_data[i].wait_time / 1000, g_contend_data[i].hold_time / 1000,
		       (g_contend_data[i].wait_time + g_contend_data[i].hold_time) / 1000);
	}

	spdk_poller_unregister(&test->poller);
	spdk_thread_send_msg(spdk_thread_get_app_thread(), launch_next_test, NULL);
	return SPDK_POLLER_BUSY;
}

/*
 * hold_by_poller - a lock held by a poller when it returns trips an assert
 */

static struct spdk_spinlock g_hold_by_poller_spinlock;
struct spdk_poller *g_hold_by_poller_poller;
static bool g_hold_by_poller_done;

static int
hold_by_poller(void *arg)
{
	static int times_called = 0;
	enum spin_error expect[SPIN_ERR_LAST] = { 0 };

	/* This polller will be called three times, trying to take the lock the first two times. */
	switch (times_called) {
	case 0:
		ASSERT(check_spin_err_count(expect));
		break;
	case 1:
		expect[SPIN_ERR_HOLD_DURING_SWITCH] = 1;
		ASSERT(check_spin_err_count(expect));
		break;
	default:
		abort();
	}

	spdk_spin_lock(&g_hold_by_poller_spinlock);

	memset(expect, 0, sizeof(expect));
	switch (times_called) {
	case 0:
		ASSERT(check_spin_err_count(expect));
		break;
	case 1:
		expect[SPIN_ERR_DEADLOCK] = 1;
		expect[SPIN_ERR_HOLD_DURING_SWITCH] = 1;
		ASSERT(check_spin_err_count(expect));
		/*
		 * Unlock so that future polls don't continue to increase the "hold during switch"
		 * count. Without this, the SPIN_ERR_HOLD_DURING_SWITCH is indeterminant.
		 */
		spdk_spin_unlock(&g_hold_by_poller_spinlock);
		ASSERT(check_spin_err_count(expect));
		spdk_poller_unregister(&g_hold_by_poller_poller);
		g_hold_by_poller_done = true;
		break;
	default:
		abort();
	}

	times_called++;

	return SPDK_POLLER_BUSY;
}

static void
hold_by_poller_start(void *arg)
{
	memset(g_lock_error_count, 0, sizeof(g_lock_error_count));
	spdk_spin_init(&g_hold_by_poller_spinlock);

	g_hold_by_poller_poller = spdk_poller_register(hold_by_poller, NULL, 0);
}

static void
hold_by_poller_setup(void)
{
	spdk_thread_send_msg(g_thread[0], hold_by_poller_start, NULL);
}

static int
hold_by_poller_end(void *arg)
{
	struct test *test = arg;
	enum spin_error expect[SPIN_ERR_LAST] = { 0 };

	/* Wait for hold_by_poller() to complete its work. */
	if (!g_hold_by_poller_done) {
		return SPDK_POLLER_IDLE;
	}

	/* Some final checks to be sure all the expected errors were seen */
	expect[SPIN_ERR_DEADLOCK] = 1;
	expect[SPIN_ERR_HOLD_DURING_SWITCH] = 1;
	ASSERT(check_spin_err_count(expect));

	/* All done, move on to next test */
	spdk_poller_unregister(&test->poller);
	spdk_thread_send_msg(spdk_thread_get_app_thread(), launch_next_test, NULL);

	return SPDK_POLLER_BUSY;
}

/*
 * hold_by_message - A message sent to a thread retains the lock when it returns.
 */

static struct spdk_spinlock g_hold_by_message_spinlock;
static bool g_hold_by_message_done;

static void
hold_by_message(void *ctx)
{
	spdk_spin_lock(&g_hold_by_message_spinlock);

	g_hold_by_message_done = true;
}

static void
hold_by_message_setup(void)
{
	memset(g_lock_error_count, 0, sizeof(g_lock_error_count));
	spdk_spin_init(&g_hold_by_message_spinlock);

	spdk_thread_send_msg(g_thread[0], hold_by_message, NULL);
}

static int
hold_by_message_end(void *arg)
{
	struct test *test = arg;
	enum spin_error expect[SPIN_ERR_LAST] = { 0 };

	/* Wait for the message to be processed */
	if (!g_hold_by_message_done) {
		return SPDK_POLLER_IDLE;
	}

	/* Verify an error was seen */
	expect[SPIN_ERR_HOLD_DURING_SWITCH] = 1;
	ASSERT(check_spin_err_count(expect));

	/* All done, move on to next test */
	spdk_poller_unregister(&test->poller);
	spdk_thread_send_msg(spdk_thread_get_app_thread(), launch_next_test, NULL);

	return SPDK_POLLER_BUSY;
}

/*
 * Test definitions
 */

static void
start_threads(uint32_t count)
{
	struct spdk_cpuset *cpuset;
	uint32_t i;

	cpuset = spdk_cpuset_alloc();
	if (cpuset == NULL) {
		fprintf(stderr, "failed to allocate cpuset\n");
		abort();
	}

	assert(count <= SPDK_COUNTOF(g_thread));

	for (i = 0; i < count; i++) {
		spdk_cpuset_zero(cpuset);
		spdk_cpuset_set_cpu(cpuset, i, true);
		g_thread[i] = spdk_thread_create("worker", cpuset);
		if (g_thread[i] == NULL) {
			fprintf(stderr, "failed to create thread\n");
			abort();
		}
	}
	spdk_cpuset_free(cpuset);
}

static void
stop_thread(void *arg)
{
	struct spdk_thread *thread = arg;

	spdk_thread_exit(thread);
}

static void
stop_threads(void)
{
	uint32_t i;

	for (i = 0; i < SPDK_COUNTOF(g_thread); i++) {
		if (g_thread[i] == NULL) {
			break;
		}
		spdk_thread_send_msg(g_thread[i], stop_thread, g_thread[i]);
		g_thread[i] = NULL;
	}
}

static struct test g_tests[] = {
	{"contend", 2, contend_setup, contend_end, 0},
	{"hold_by_poller", 1, hold_by_poller_setup, hold_by_poller_end, 0},
	{"hold_by_message", 1, hold_by_message_setup, hold_by_message_end, 1},
};

static void
launch_end_poller(void *arg)
{
	struct test *test = arg;

	test->poller = SPDK_POLLER_REGISTER(test->end_fn, test, 100);
}

static void
launch_next_test(void *arg)
{
	struct test *test;
	static uint32_t last_fail_count = 0;
	static uint32_t current_test = 0;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (current_test != 0) {
		const char *name = g_tests[current_test - 1].name;
		if (g_fail == last_fail_count) {
			printf("PASS test %s\n", name);
		} else {
			printf("FAIL test %s (%u failed assertions)\n", name,
			       g_fail - last_fail_count);
		}
		stop_threads();
	}

	if (current_test == SPDK_COUNTOF(g_tests)) {
		spdk_app_stop(g_fail);

		return;
	}

	test = &g_tests[current_test];

	printf("Starting test %s\n", test->name);
	start_threads(test->thread_count);

	if (test->poller_thread_number == 0) {
		launch_end_poller(test);
	} else {
		/*
		 * A test may set a done flag then return, expecting the error to be generated
		 * when the poller or message goes off CPU. To ensure that we don't check for the
		 * error between the time that "done" is set and the time the error is registered,
		 * check for the error on the thread that runs the poller or handles the message.
		 */
		spdk_thread_send_msg(g_thread[test->poller_thread_number - 1],
				     launch_end_poller, test);
	}

	/*
	 * The setup function starts after the end poller. If it's not done this way, the start
	 * function may trigger an error condition (thread->lock_count != 0) that would cause
	 * extraneous calls to spin_abort_fn() as the end poller is registered.
	 */
	test->setup_fn();

	current_test++;
}

static void
start_tests(void *arg)
{
	g_spin_abort_fn = do_not_abort;
	spdk_thread_send_msg(spdk_thread_get_app_thread(), launch_next_test, NULL);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	char *me = argv[0];
	int ret;
	char mask[8];

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_lock_test";
	snprintf(mask, sizeof(mask), "0x%x", (1 << SPDK_COUNTOF(g_thread)) - 1);
	opts.reactor_mask = mask;

	spdk_app_start(&opts, start_tests, NULL);

	spdk_app_fini();

	printf("%s summary:\n", me);
	printf(" %8u assertions passed\n", g_pass);
	printf(" %8u assertions failed\n", g_fail);

	if (g_pass + g_fail == 0) {
		ret = 1;
	} else {
		ret = spdk_min(g_fail, 127);
	}
	return ret;
}
