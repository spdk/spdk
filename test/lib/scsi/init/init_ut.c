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

#include "spdk/event.h"
#include "spdk/scsi.h"

#include "spdk_cunit.h"

#include "scsi.c"

/* Unit test stubbed bdev subsystem dependency */
SPDK_SUBSYSTEM_REGISTER(bdev, NULL, NULL, NULL)

static int
null_init(void)
{
	return 0;
}

static int
null_clean(void)
{
	return 0;
}

void
spdk_add_subsystem(struct spdk_subsystem *subsystem)
{
}

void
spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend)
{
}

static struct spdk_conf *
spdk_config_init_scsi_params(char *key, char *value)
{
	struct spdk_conf *spdk_config;
	FILE *f;
	int fd, rc;
	char filename[] = "/tmp/scsi_init_ut.XXXXXX";

	/* Create temporary file to hold config */
	fd = mkstemp(filename);
	SPDK_CU_ASSERT_FATAL(fd != -1);

	f = fdopen(fd, "wb+");
	SPDK_CU_ASSERT_FATAL(f != NULL);

	fprintf(f, "[Scsi]\n");
	fprintf(f, "%s %s\n", key, value);

	fclose(f);

	spdk_config = spdk_conf_allocate();
	SPDK_CU_ASSERT_FATAL(spdk_config != NULL);

	rc = spdk_conf_read(spdk_config, filename);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	spdk_conf_set_as_default(spdk_config);

	remove(filename);

	return spdk_config;
}

static void
set_default_scsi_params(struct spdk_scsi_parameters *params)
{
	memset(params, 0, sizeof(*params));
	params->max_unmap_lba_count = DEFAULT_MAX_UNMAP_LBA_COUNT;
	params->max_unmap_block_descriptor_count = DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT;
	params->optimal_unmap_granularity = DEFAULT_OPTIMAL_UNMAP_GRANULARITY;
	params->unmap_granularity_alignment = DEFAULT_UNMAP_GRANULARITY_ALIGNMENT;
	params->ugavalid = DEFAULT_UGAVALID;
	params->max_write_same_length = DEFAULT_MAX_WRITE_SAME_LENGTH;
}

static void
scsi_init_sp_null(void)
{
	struct spdk_conf *config;
	int rc;

	config = spdk_conf_allocate();
	SPDK_CU_ASSERT_FATAL(config != NULL);

	spdk_conf_set_as_default(config);

	rc = spdk_scsi_subsystem_init();

	/* sp = null; set default scsi params */
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_set_as_default(NULL);

	spdk_conf_free(config);
}

static void
scsi_init_set_max_unmap_lba_count_config_param(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.max_unmap_lba_count = 65536 of Scsi section */
	config = spdk_config_init_scsi_params("MaxUnmapLbaCount", "65536");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.max_unmap_lba_count == 65536 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.max_unmap_lba_count = 65536;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_set_max_unmap_block_descriptor_count_config_param(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.max_unmap_block_descriptor_count = 1
	 * of Scsi section */
	config = spdk_config_init_scsi_params("MaxUnmapBlockDescriptorCount", "1");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.max_unmap_block_descriptor_count == 1 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.max_unmap_block_descriptor_count = 1;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_set_optimal_unmap_granularity_config_param(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.optimal_unmap_granularity = 0
	 * of Scsi section */
	config = spdk_config_init_scsi_params("OptimalUnmapGranularity", "0");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.optimal_unmap_granularity == 0 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.optimal_unmap_granularity = 0;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_set_unmap_granularity_alignment_config_param(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.unmap_granularity_alignment = 0
	 * of Scsi section */
	config = spdk_config_init_scsi_params("UnmapGranularityAlignment", "0");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.unmap_granularity_alignment == 0 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.unmap_granularity_alignment = 0;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_ugavalid_yes(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.ugavalid = Yes
	 * of Scsi section */
	config = spdk_config_init_scsi_params("Ugavalid", "Yes");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.ugavalid == 1 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.ugavalid = 1;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_ugavalid_no(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.ugavalid = No
	 * of Scsi section */
	config = spdk_config_init_scsi_params("Ugavalid", "No");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.ugavalid == 0 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.ugavalid = 0;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_ugavalid_unknown_value_failure(void)
{
	struct spdk_scsi_parameters params;
	int rc;
	struct spdk_conf *config;

	/* set scsi_params.ugavalid = unknown value
	 * of Scsi section */
	config = spdk_config_init_scsi_params("Ugavalid", "unknown value");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();
	CU_ASSERT_EQUAL(rc, 0);

	/* Assert the scsi_params.ugavalid == DEFAULT_UGAVALID and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.ugavalid = DEFAULT_UGAVALID;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);

	spdk_conf_free(config);
}

static void
scsi_init_max_write_same_length(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* set scsi_params.max_write_same_length = 512
	 * of Scsi section */
	config = spdk_config_init_scsi_params("MaxWriteSameLength", "512");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Assert the scsi_params.max_write_same_length == 512 and
	 * assert the rest of the params are set to their default values */
	set_default_scsi_params(&params);
	params.max_write_same_length = 512;
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_read_config_scsi_params(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* Set null for item's key and value;
	 * set default scsi parameters */
	config = spdk_config_init_scsi_params("", "");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Sets the default values for all the parameters
	 * of the Scsi section and returns success */
	set_default_scsi_params(&params);
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

static void
scsi_init_success(void)
{
	struct spdk_scsi_parameters params;
	struct spdk_conf *config;
	int rc;

	/* Set null for item's key and value;
	 * set default scsi parameters */
	config = spdk_config_init_scsi_params("", "");
	spdk_conf_set_as_default(config);
	rc = spdk_scsi_subsystem_init();

	/* Sets the default values for all the parameters
	 * of the Scsi section, initialize th device
	 *  and returns success */
	set_default_scsi_params(&params);
	CU_ASSERT(memcmp(&g_spdk_scsi.scsi_params, &params, sizeof(params)) == 0);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_conf_free(config);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int 	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("scsi_suite", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "scsi init - set default scsi params", \
			    scsi_init_sp_null) == NULL
		|| CU_add_test(suite, "scsi init - set max_unmap_lba_count", \
			       scsi_init_set_max_unmap_lba_count_config_param) == NULL
		|| CU_add_test(suite, "scsi init - set max_unmap_block_descriptor_count", \
			       scsi_init_set_max_unmap_block_descriptor_count_config_param) == NULL
		|| CU_add_test(suite, "scsi init - set optimal_unmap_granularity", \
			       scsi_init_set_optimal_unmap_granularity_config_param) == NULL
		|| CU_add_test(suite, "scsi init - set unmap_granularity_alignment", \
			       scsi_init_set_unmap_granularity_alignment_config_param) == NULL
		|| CU_add_test(suite, "scsi init - ugavalid value yes", \
			       scsi_init_ugavalid_yes) == NULL
		|| CU_add_test(suite, "scsi init - ugavalid value no", \
			       scsi_init_ugavalid_no) == NULL
		|| CU_add_test(suite, "scsi init - ugavalid unknown value", \
			       scsi_init_ugavalid_unknown_value_failure) == NULL
		|| CU_add_test(suite, "scsi init - set max_write_same_length", \
			       scsi_init_max_write_same_length) == NULL
		|| CU_add_test(suite, "scsi init - read config scsi parameters", \
			       scsi_init_read_config_scsi_params) == NULL
		|| CU_add_test(suite, "scsi init - success", scsi_init_success) == NULL
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
