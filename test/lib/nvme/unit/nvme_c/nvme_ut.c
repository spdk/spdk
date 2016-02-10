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

#include "spdk_cunit.h"

#include "nvme/nvme.c"

char outbuf[OUTBUF_SIZE];

volatile int sync_start = 0;
volatile int threads_pass = 0;
volatile int threads_fail = 0;

uint64_t nvme_vtophys(void *buf)
{
	return (uintptr_t)buf;
}

int
nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
}

int
nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static void prepare_for_test(uint32_t max_io_queues)
{
	struct nvme_driver *driver = &g_nvme_driver;

	driver->max_io_queues = max_io_queues;
	if (driver->ioq_index_pool != NULL) {
		free(driver->ioq_index_pool);
		driver->ioq_index_pool = NULL;
	}
	driver->ioq_index_pool_next = 0;
	nvme_thread_ioq_index = -1;

	sync_start = 0;
	threads_pass = 0;
	threads_fail = 0;
}

static void *
nvme_thread(void *arg)
{
	int rc;

	/* Try to synchronize the nvme_register_io_thread() calls
	 *  as much as possible to ensure the mutex locking is tested
	 *  correctly.
	 */
	while (sync_start == 0)
		;

	rc = spdk_nvme_register_io_thread();
	if (rc == 0) {
		__sync_fetch_and_add(&threads_pass, 1);
	} else {
		__sync_fetch_and_add(&threads_fail, 1);
	}

	pthread_exit(NULL);
}

static void
test1(void)
{
	struct nvme_driver *driver = &g_nvme_driver;
	int rc;
	int last_index;

	prepare_for_test(1);

	CU_ASSERT(nvme_thread_ioq_index == -1);

	rc = spdk_nvme_register_io_thread();
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_thread_ioq_index >= 0);
	CU_ASSERT(driver->ioq_index_pool_next == 1);

	/* try to register thread again - this should fail */
	last_index = nvme_thread_ioq_index;
	rc = spdk_nvme_register_io_thread();
	CU_ASSERT(rc != 0);
	/* assert that the ioq_index was unchanged */
	CU_ASSERT(nvme_thread_ioq_index == last_index);

	spdk_nvme_unregister_io_thread();
	CU_ASSERT(nvme_thread_ioq_index == -1);
	CU_ASSERT(driver->ioq_index_pool_next == 0);
}

static void
test2(void)
{
	int num_threads = 16;
	int i;
	pthread_t td;

	/*
	 * Start 16 threads, but only simulate a maximum of 12 I/O
	 *  queues.  12 threads should be able to successfully
	 *  register, while the other 4 should fail.
	 */
	prepare_for_test(12);

	for (i = 0; i < num_threads; i++) {
		pthread_create(&td, NULL, nvme_thread, NULL);
	}

	sync_start = 1;

	while ((threads_pass + threads_fail) < num_threads)
		;

	CU_ASSERT(threads_pass == 12);
	CU_ASSERT(threads_fail == 4);
}


int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test1", test1) == NULL
		|| CU_add_test(suite, "test2", test2) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
