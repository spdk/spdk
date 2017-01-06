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
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops;

#include "subsystem.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

struct spdk_nvmf_globals g_nvmf_tgt;

static int
test_transport1_listen_addr_add(struct spdk_nvmf_listen_addr *listen_addr)
{
	return 0;
}

static void
test_transport1_listen_addr_discover(struct spdk_nvmf_listen_addr *listen_addr,
				     struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

static const struct spdk_nvmf_transport test_transport1 = {
	.listen_addr_add = test_transport1_listen_addr_add,
	.listen_addr_discover = test_transport1_listen_addr_discover,
};

const struct spdk_nvmf_transport *
spdk_nvmf_transport_get(const char *trname)
{
	if (!strcasecmp(trname, "test_transport1")) {
		return &test_transport1;
	}

	return NULL;
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
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Longest valid name */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 222 - strlen(nqn));
	nqn[222] = '\0';
	CU_ASSERT(strlen(nqn) == 222);
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Name that is one byte longer than allowed */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 223 - strlen(nqn));
	nqn[223] = '\0';
	CU_ASSERT(strlen(nqn) == 223);
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	CU_ASSERT(subsystem == NULL);
}

static void
nvmf_test_find_subsystem(void)
{
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem(NULL));
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem("fake"));
}

static bool
all_zero(const void *buf, size_t size)
{
	const uint8_t *b = buf;

	while (size--) {
		if (*b != 0) {
			return false;
		}
		b++;
	}

	return true;
}

static void
test_discovery_log(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	uint8_t buffer[8192];
	struct spdk_nvmf_discovery_log_page *disc_log;
	struct spdk_nvmf_discovery_log_page_entry *entry;

	/* Reset discovery-related globals */
	g_discovery_genctr = 0;
	free(g_discovery_log_page);
	g_discovery_log_page = NULL;
	g_discovery_log_page_size = 0;

	/* Add one subsystem and verify that the discovery log contains it */
	subsystem = spdk_nvmf_create_subsystem("nqn.2016-06.io.spdk:subsystem1", SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	SPDK_CU_ASSERT_FATAL(spdk_nvmf_subsystem_add_listener(subsystem, "test_transport1",
			     "1234", "5678") == 0);

	/* Get only genctr (first field in the header) */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(disc_log->genctr));
	CU_ASSERT(disc_log->genctr == 2); /* one added subsystem + one added listen address */

	/* Get only the header, no entries */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(*disc_log));
	CU_ASSERT(disc_log->genctr == 2);
	CU_ASSERT(disc_log->numrec == 1);

	/* Offset 0, exact size match */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(*disc_log) + sizeof(disc_log->entries[0]));
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);

	/* Offset 0, oversize buffer */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(buffer));
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);
	CU_ASSERT(all_zero(buffer + sizeof(*disc_log) + sizeof(disc_log->entries[0]),
			   sizeof(buffer) - (sizeof(*disc_log) + sizeof(disc_log->entries[0]))));

	/* Get just the first entry, no header */
	memset(buffer, 0xCC, sizeof(buffer));
	entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer,
					 offsetof(struct spdk_nvmf_discovery_log_page, entries[0]),
					 sizeof(*entry));
	CU_ASSERT(entry->trtype == 42);
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
		CU_add_test(suite, "find_subsystem", nvmf_test_find_subsystem) == NULL ||
		CU_add_test(suite, "discovery_log", test_discovery_log) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
