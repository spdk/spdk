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

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/nvme.h"
#include "nvme/nvme_io_msg.c"
#include "common/lib/nvme/common_stubs.h"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(spdk_nvme_ctrlr_free_io_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

DEFINE_RETURN_MOCK(spdk_nvme_ctrlr_alloc_io_qpair, struct spdk_nvme_qpair *);
struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *user_opts,
			       size_t opts_size)
{
	HANDLE_RETURN_MOCK(spdk_nvme_ctrlr_alloc_io_qpair);
	return NULL;
}

static void
ut_io_msg_fn(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	static uint32_t i = 0;

	CU_ASSERT(arg == (void *)(0xDEADBEEF + sizeof(int *) * i));
	CU_ASSERT(nsid == i);
	CU_ASSERT(ctrlr->external_io_msgs_qpair == (struct spdk_nvme_qpair *)0xDBADBEEF);
	i++;
}

static void
test_nvme_io_msg_process(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_ring external_io_msgs = {};
	int rc, i;

	ctrlr.external_io_msgs = &external_io_msgs;
	ctrlr.external_io_msgs_qpair = (struct spdk_nvme_qpair *)0xDBADBEEF;
	TAILQ_INIT(&external_io_msgs.elements);
	pthread_mutex_init(&ctrlr.external_io_msgs_lock, NULL);
	pthread_mutex_init(&ctrlr.external_io_msgs->lock, NULL);

	/* Send IO processing size requests */
	for (i = 0; i < SPDK_NVME_MSG_IO_PROCESS_SIZE; i ++) {
		nvme_io_msg_send(&ctrlr, i, ut_io_msg_fn,
				 (void *)(0xDEADBEEF + sizeof(int *) * i));
	}

	rc = nvme_io_msg_process(&ctrlr);
	CU_ASSERT(rc == SPDK_NVME_MSG_IO_PROCESS_SIZE);
	CU_ASSERT(TAILQ_EMPTY(&external_io_msgs.elements));

	/* Unavailable external_io_msgs and external_io_msgs_qpair */
	ctrlr.external_io_msgs = NULL;
	ctrlr.external_io_msgs_qpair = NULL;

	rc = nvme_io_msg_process(&ctrlr);
	SPDK_CU_ASSERT_FATAL(rc == 0);
}

static void
test_nvme_io_msg_send(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_io_msg *request = NULL;
	struct spdk_ring external_io_msgs = {};
	int rc;
	uint32_t nsid = 1;
	void *arg = (void *)0xDEADBEEF;

	ctrlr.external_io_msgs = &external_io_msgs;
	TAILQ_INIT(&external_io_msgs.elements);
	pthread_mutex_init(&ctrlr.external_io_msgs_lock, NULL);
	pthread_mutex_init(&ctrlr.external_io_msgs->lock, NULL);

	/* Dequeue the request after sending io message */
	rc = nvme_io_msg_send(&ctrlr, nsid, ut_io_msg_fn, arg);
	spdk_ring_dequeue(ctrlr.external_io_msgs, (void **)&request, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(request != NULL);
	CU_ASSERT(request->ctrlr == &ctrlr);
	CU_ASSERT(request->nsid == nsid);
	CU_ASSERT(request->fn == ut_io_msg_fn);
	CU_ASSERT(request->arg == arg);
	CU_ASSERT(TAILQ_EMPTY(&external_io_msgs.elements));

	free(request);
}

static void
ut_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

static void
ut_update(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

static struct nvme_io_msg_producer ut_nvme_io_msg_producer[2] = {
	{
		.name = "ut_test1",
		.stop = ut_stop,
		.update = ut_update,
	}, {
		.name = "ut_test2",
		.stop = ut_stop,
		.update = ut_update,
	}
};

static void
test_nvme_io_msg_ctrlr_register_unregister(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	STAILQ_INIT(&ctrlr.io_producers);
	MOCK_SET(spdk_nvme_ctrlr_alloc_io_qpair, (void *)0xDEADBEEF);

	rc = nvme_io_msg_ctrlr_register(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctrlr.external_io_msgs != NULL);
	CU_ASSERT(!STAILQ_EMPTY(&ctrlr.io_producers));
	CU_ASSERT(ctrlr.external_io_msgs_qpair == (void *)0xDEADBEEF);

	nvme_io_msg_ctrlr_unregister(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(ctrlr.external_io_msgs == NULL);
	CU_ASSERT(ctrlr.external_io_msgs_qpair == NULL);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.io_producers));

	/* Multiple producer */
	rc = nvme_io_msg_ctrlr_register(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(rc == 0);

	rc = nvme_io_msg_ctrlr_register(&ctrlr, &ut_nvme_io_msg_producer[1]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctrlr.external_io_msgs != NULL);
	CU_ASSERT(ctrlr.external_io_msgs_qpair == (void *)0xDEADBEEF);
	nvme_io_msg_ctrlr_unregister(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(!STAILQ_EMPTY(&ctrlr.io_producers));
	nvme_io_msg_ctrlr_unregister(&ctrlr, &ut_nvme_io_msg_producer[1]);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.io_producers));
	CU_ASSERT(ctrlr.external_io_msgs == NULL);
	CU_ASSERT(ctrlr.external_io_msgs_qpair == NULL);

	/* The same producer exist */
	rc = nvme_io_msg_ctrlr_register(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctrlr.external_io_msgs != NULL);
	CU_ASSERT(ctrlr.external_io_msgs_qpair == (void *)0xDEADBEEF);

	rc = nvme_io_msg_ctrlr_register(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(rc == -EEXIST);
	nvme_io_msg_ctrlr_unregister(&ctrlr, &ut_nvme_io_msg_producer[0]);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.io_producers));
	CU_ASSERT(ctrlr.external_io_msgs == NULL);
	CU_ASSERT(ctrlr.external_io_msgs_qpair == NULL);
	MOCK_CLEAR(spdk_nvme_ctrlr_alloc_io_qpair);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_io_msg", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_io_msg_send);
	CU_ADD_TEST(suite, test_nvme_io_msg_process);
	CU_ADD_TEST(suite, test_nvme_io_msg_ctrlr_register_unregister);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
