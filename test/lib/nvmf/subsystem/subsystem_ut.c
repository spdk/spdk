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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#include "spdk_cunit.h"
#include "subsystem.h"

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_direct_ctrlr_ops;

#include "subsystem.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

struct spdk_nvmf_globals g_nvmf_tgt;

uint32_t
spdk_app_get_current_core(void)
{
	return 0;
}

struct spdk_event *spdk_event_allocate(uint32_t lcore, spdk_event_fn fn,
				       void *arg1, void *arg2,
				       spdk_event_t next)
{
	return NULL;
}

void
spdk_poller_register(struct spdk_poller **ppoller, spdk_poller_fn fn, void *arg, uint32_t lcore,
		     struct spdk_event *complete, uint64_t period_microseconds)
{
}

void spdk_poller_unregister(struct spdk_poller **ppoller,
			    struct spdk_event *complete)
{
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr, enum spdk_nvme_qprio qprio)
{
	return NULL;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

void
spdk_nvmf_session_destruct(struct spdk_nvmf_session *session)
{
}

int
spdk_nvmf_session_poll(struct spdk_nvmf_session *session)
{
	return -1;
}

static void
nvmf_test_create_subsystem(void)
{
	char nqn[256];
	struct spdk_nvmf_subsystem *subsystem;

	strncpy(nqn, "nqn.2016-06.io.spdk:subsystem1", sizeof(nqn));
	subsystem = spdk_nvmf_create_subsystem(1, nqn, SPDK_NVMF_SUBTYPE_NVME, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_EQUAL(subsystem->num, 1);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Longest valid name */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 222 - strlen(nqn));
	nqn[222] = '\0';
	CU_ASSERT(strlen(nqn) == 222);
	subsystem = spdk_nvmf_create_subsystem(2, nqn, SPDK_NVMF_SUBTYPE_NVME, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Name that is one byte longer than allowed */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 223 - strlen(nqn));
	nqn[223] = '\0';
	CU_ASSERT(strlen(nqn) == 223);
	subsystem = spdk_nvmf_create_subsystem(2, nqn, SPDK_NVMF_SUBTYPE_NVME, NULL, NULL, NULL);
	CU_ASSERT(subsystem == NULL);
}

static void
nvmf_test_find_subsystem(void)
{
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem(NULL, NULL));
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem("fake", NULL));
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem(NULL, "fake"));
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem("fake", "fake"));
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "create_subsystem", nvmf_test_create_subsystem) == NULL ||
		CU_add_test(suite, "find_subsystem", nvmf_test_find_subsystem) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
