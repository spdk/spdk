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

#include <CUnit/Basic.h>

#include "subsystem.c"

static struct spdk_subsystem g_ut_subsystems[8];
static struct spdk_subsystem_depend g_ut_subsystem_deps[8];

static void
set_up_subsystem(struct spdk_subsystem *subsystem, const char *name)
{
	subsystem->init = NULL;
	subsystem->fini = NULL;
	subsystem->config = NULL;
	subsystem->name = name;
}

static void
set_up_depends(struct spdk_subsystem_depend *depend, const char *subsystem_name,
	       const char *dpends_on_name)
{
	depend->name = subsystem_name;
	depend->depends_on = dpends_on_name;
}

static void
subsystem_clear(void)
{
	struct spdk_subsystem *subsystem, *subsystem_tmp;
	struct spdk_subsystem_depend *subsystem_dep, *subsystem_dep_tmp;

	TAILQ_FOREACH_SAFE(subsystem, &g_subsystems, tailq, subsystem_tmp) {
		TAILQ_REMOVE(&g_subsystems, subsystem, tailq);
	}

	TAILQ_FOREACH_SAFE(subsystem_dep, &g_depends, tailq, subsystem_dep_tmp) {
		TAILQ_REMOVE(&g_depends, subsystem_dep, tailq);
	}
}

static void
subsystem_sort_test_depends_on_single(void)
{
	struct spdk_subsystem *subsystem;
	int i;
	char subsystem_name[16];

	spdk_subsystem_init();

	i = 4;
	TAILQ_FOREACH(subsystem, &g_subsystems, tailq) {
		snprintf(subsystem_name, sizeof(subsystem_name), "subsystem%d", i);
		i--;
		CU_ASSERT(strcmp(subsystem_name, subsystem->name) == 0);
	}
}

static void
subsystem_sort_test_depends_on_multiple(void)
{
	int i;
	struct spdk_subsystem *subsystem;

	subsystem_clear();
	set_up_subsystem(&g_ut_subsystems[0], "iscsi");
	set_up_subsystem(&g_ut_subsystems[1], "nvmf");
	set_up_subsystem(&g_ut_subsystems[2], "sock");
	set_up_subsystem(&g_ut_subsystems[3], "bdev");
	set_up_subsystem(&g_ut_subsystems[4], "rpc");
	set_up_subsystem(&g_ut_subsystems[5], "scsi");
	set_up_subsystem(&g_ut_subsystems[6], "interface");
	set_up_subsystem(&g_ut_subsystems[7], "copy");

	for (i = 0; i < 8; i++) {
		spdk_add_subsystem(&g_ut_subsystems[i]);
	}

	set_up_depends(&g_ut_subsystem_deps[0], "bdev", "copy");
	set_up_depends(&g_ut_subsystem_deps[1], "scsi", "bdev");
	set_up_depends(&g_ut_subsystem_deps[2], "rpc", "interface");
	set_up_depends(&g_ut_subsystem_deps[3], "sock", "interface");
	set_up_depends(&g_ut_subsystem_deps[4], "nvmf", "interface");
	set_up_depends(&g_ut_subsystem_deps[5], "iscsi", "scsi");
	set_up_depends(&g_ut_subsystem_deps[6], "iscsi", "sock");
	set_up_depends(&g_ut_subsystem_deps[7], "iscsi", "rpc");

	for (i = 0; i < 8; i++) {
		spdk_add_subsystem_depend(&g_ut_subsystem_deps[i]);
	}

	spdk_subsystem_init();

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "interface") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "copy") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "nvmf") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "sock") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "bdev") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "rpc") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "scsi") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);

	subsystem = TAILQ_FIRST(&g_subsystems);
	CU_ASSERT(strcmp(subsystem->name, "iscsi") == 0);
	TAILQ_REMOVE(&g_subsystems, subsystem, tailq);
}

SPDK_SUBSYSTEM_REGISTER(subsystem1, NULL, NULL, NULL)
SPDK_SUBSYSTEM_REGISTER(subsystem2, NULL, NULL, NULL)
SPDK_SUBSYSTEM_REGISTER(subsystem3, NULL, NULL, NULL)
SPDK_SUBSYSTEM_REGISTER(subsystem4, NULL, NULL, NULL)
SPDK_SUBSYSTEM_DEPEND(subsystem1, subsystem2)
SPDK_SUBSYSTEM_DEPEND(subsystem2, subsystem3)
SPDK_SUBSYSTEM_DEPEND(subsystem3, subsystem4)


static void
subsystem_sort_test_missing_dependency(void)
{
	/*
	 * A depends on B, but B is missing
	 */

	subsystem_clear();
	set_up_subsystem(&g_ut_subsystems[0], "A");
	spdk_add_subsystem(&g_ut_subsystems[0]);

	set_up_depends(&g_ut_subsystem_deps[0], "A", "B");
	spdk_add_subsystem_depend(&g_ut_subsystem_deps[0]);

	CU_ASSERT(spdk_subsystem_init() != 0);

	/*
	 * Dependency from C to A is defined, but C is missing
	 */

	subsystem_clear();
	set_up_subsystem(&g_ut_subsystems[0], "A");
	spdk_add_subsystem(&g_ut_subsystems[0]);

	set_up_depends(&g_ut_subsystem_deps[0], "C", "A");
	spdk_add_subsystem_depend(&g_ut_subsystem_deps[0]);

	CU_ASSERT(spdk_subsystem_init() != 0);

}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int 	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("subsystem_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "subsystem_sort_test_depends_on_single",
			    subsystem_sort_test_depends_on_single) == NULL
		|| CU_add_test(suite, "subsystem_sort_test_depends_on_multiple",
			       subsystem_sort_test_depends_on_multiple) == NULL
		|| CU_add_test(suite, "subsystem_sort_test_missing_dependency",
			       subsystem_sort_test_missing_dependency) == NULL
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
