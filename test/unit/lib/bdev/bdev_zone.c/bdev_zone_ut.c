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
 *   A PARTICULAR PURPOSE AiRE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
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
#include "spdk/env.h"
#include "spdk_internal/mock.h"

#include "bdev/bdev_zone.c"

DEFINE_STUB_V(spdk_bdev_io_init, (struct spdk_bdev_io *bdev_io,
				  struct spdk_bdev *bdev, void *cb_arg,
				  spdk_bdev_io_completion_cb cb));

DEFINE_STUB_V(spdk_bdev_io_submit, (struct spdk_bdev_io *bdev_io));

/* Construct zone io_operation structure */
struct zone_io_operation {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	union {
		struct {
			uint64_t zone_id;
			size_t num_zones;
			enum spdk_bdev_zone_action zone_action;
			struct spdk_bdev_zone_info *info_;
		} zone_mgmt;
		struct {
			void *md_buf;
			void *buf;
			struct iovec *iovs;
			int iovcnt;
			uint64_t num_blocks;
			uint64_t offset_blocks;
			uint64_t zone_id;
			uint64_t start_lba;
		} bdev;
	};
	spdk_bdev_io_completion_cb cb;
	void *cb_arg;
	enum spdk_bdev_io_type io_type;
};

/* Global variables */
static struct zone_io_operation *g_zone_op = NULL;
static struct spdk_bdev_io  *g_bdev_io = NULL;
static struct spdk_bdev_zone_info g_zone_info = {0};
static enum spdk_bdev_zone_action g_zone_action = SPDK_BDEV_ZONE_CLOSE;
static enum spdk_bdev_io_type g_io_type = SPDK_BDEV_IO_TYPE_GET_ZONE_INFO;

static uint64_t  g_expected_zone_id = 0x1000, g_expected_num_zones = 1024;
static uint64_t g_num_blocks = 4096 * 1024;
static void *g_md_buf = (void *)0x1000;
static void *g_buf = (void *)0xABCD;
static uint64_t g_start_lba = 4096;
static uint64_t g_bdev_blocklen = 4096;
static struct iovec io_buf[2];
static struct spdk_bdev *g_bdev = NULL;

static void
start_operation(void)
{
	g_zone_op = calloc(1, sizeof(g_zone_op));
	CU_ASSERT(g_zone_op != NULL);

	if (g_io_type == SPDK_BDEV_IO_TYPE_ZONE_APPEND) {
		g_buf = calloc(1, sizeof(g_buf));
		g_md_buf = calloc(1, sizeof(g_md_buf));
	}
}

static void
stop_operation(void)
{
	free(g_zone_op);
	free(g_bdev_io);
	free(g_bdev);

	if (g_io_type == SPDK_BDEV_IO_TYPE_ZONE_APPEND) {
		free(g_buf);
		free(g_md_buf);
	}
}

struct spdk_bdev_io *
spdk_bdev_get_io(struct spdk_bdev_channel *channel)
{
	struct spdk_bdev_io *bdev_io;
	struct iovec iov[2];

	bdev_io = (struct spdk_bdev_io *) 0x1;
	bdev_io = calloc(1, sizeof(struct spdk_bdev_io));

	memset(io_buf, 0x5a, sizeof(io_buf));
	iov[0] = io_buf[0];
	bdev_io->iov = iov[0];
	iov[0].iov_base = g_buf;
	iov[0].iov_len = g_num_blocks * g_bdev_blocklen;

	bdev_io->internal.ch = channel;
	bdev_io->type = g_io_type;

	switch (g_io_type) {
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		bdev_io->u.zone_mgmt.zone_id  = g_expected_zone_id;
		bdev_io->u.zone_mgmt.num_zones = g_expected_num_zones;
		bdev_io->u.zone_mgmt.zone_action = g_zone_action;
		break;
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		bdev_io->u.bdev.iovs = &bdev_io->iov;
		bdev_io->u.bdev.iovs[0].iov_base = g_buf;
		bdev_io->u.bdev.iovs[0].iov_len = g_num_blocks * g_bdev_blocklen;
		bdev_io->u.bdev.iovcnt = 1;
		bdev_io->u.bdev.md_buf = g_md_buf;
		bdev_io->u.bdev.num_blocks = g_num_blocks;
		bdev_io->u.bdev.offset_blocks = g_expected_zone_id;
		break;
	default:
		CU_ASSERT(0);
	}

	g_bdev_io = bdev_io;

	return bdev_io;
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
	*_desc = (void *)0x1;
	return 0;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return (struct spdk_io_channel *)0x1;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	CU_ASSERT(ch == (void *)1);
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev;

	bdev = (struct spdk_bdev *)0x1;
	bdev = calloc(1, sizeof(struct spdk_bdev_io));

	CU_ASSERT(bdev != NULL);

	if (g_io_type == SPDK_BDEV_IO_TYPE_ZONE_APPEND) {
		bdev->blocklen =  g_bdev_blocklen;
	}

	g_bdev = bdev;

	return	 bdev;
}

static void
test_get_zone_size(void)
{
	struct spdk_bdev bdev;
	uint64_t get_zone_size;

	memset(&bdev, 0, sizeof(bdev));
	bdev.zone_size = 1024 * 4096;

	get_zone_size = spdk_bdev_get_zone_size(&bdev);
	CU_ASSERT(get_zone_size == 1024 * 4096);
}

static void
test_get_max_open_zones(void)
{
	struct spdk_bdev bdev;
	uint32_t get_max_open_zones;

	memset(&bdev, 0, sizeof(bdev));
	bdev.max_open_zones = 8192;

	get_max_open_zones = spdk_bdev_get_max_open_zones(&bdev);
	CU_ASSERT(get_max_open_zones == 8192);
}

static void
test_get_optimal_open_zones(void)
{

	struct spdk_bdev bdev;
	uint32_t get_optimal_open_zones;

	memset(&bdev, 0, sizeof(bdev));
	bdev.optimal_open_zones = 4096;

	get_optimal_open_zones = spdk_bdev_get_optimal_open_zones(&bdev);
	CU_ASSERT(get_optimal_open_zones == 4096);
}

static void
test_bdev_io_get_append_location(void)
{
	struct spdk_bdev_io bdev_io;
	uint64_t get_offset_blocks;

	memset(&bdev_io, 0, sizeof(bdev_io));
	bdev_io.u.bdev.offset_blocks = 1024 * 10;

	get_offset_blocks = spdk_bdev_io_get_append_location(&bdev_io);
	CU_ASSERT(get_offset_blocks == 1024 * 10);
}

static void
test_zone_get_operation(void)
{
	test_get_zone_size();
	test_get_max_open_zones();
	test_get_optimal_open_zones();
}

static void
test_bdev_zone_get_info(void)
{
	struct spdk_bdev bdev;
	struct spdk_io_channel *ch;
	struct spdk_bdev_desc *desc = NULL;
	int rc;

	memset(&bdev, 0, sizeof(bdev));

	bdev.name = "bdev_zone_ut";
	rc = spdk_bdev_open(&bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(ch != NULL);
	g_zone_info.zone_id = g_expected_zone_id;
	g_io_type = SPDK_BDEV_IO_TYPE_GET_ZONE_INFO;

	start_operation();

	rc = spdk_bdev_get_zone_info(desc, ch, g_expected_zone_id, g_expected_num_zones, &g_zone_info, NULL,
				     NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->type == SPDK_BDEV_IO_TYPE_GET_ZONE_INFO);
	CU_ASSERT(g_bdev_io->u.zone_mgmt.zone_id  == g_expected_zone_id);
	CU_ASSERT(g_bdev_io->u.zone_mgmt.num_zones  == g_expected_num_zones);

	stop_operation();
}

static void
test_bdev_zone_management(void)
{
	struct spdk_bdev bdev;
	struct spdk_io_channel *ch;
	struct spdk_bdev_desc *desc = NULL;
	int rc;

	memset(&bdev, 0, sizeof(bdev));

	bdev.name = "bdev_zone_ut";
	rc = spdk_bdev_open(&bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(ch != NULL);

	g_zone_info.zone_id = g_expected_zone_id;
	g_io_type = SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT;

	start_operation();

	rc = spdk_bdev_zone_management(desc, ch, g_expected_zone_id, SPDK_BDEV_ZONE_OPEN, NULL,
				       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->type == SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT);
	CU_ASSERT(g_bdev_io->u.zone_mgmt.zone_id  == g_expected_zone_id);
	CU_ASSERT(g_bdev_io->u.zone_mgmt.zone_action  == SPDK_BDEV_ZONE_OPEN);
	CU_ASSERT(g_bdev_io->u.zone_mgmt.num_zones  == 1);

	stop_operation();
}

static void
test_bdev_zone_append(void)
{
	struct spdk_bdev bdev;
	struct spdk_io_channel *ch;
	struct spdk_bdev_desc *desc = NULL;
	int rc;

	memset(&bdev, 0, sizeof(bdev));

	bdev.name = "bdev_zone_ut";
	bdev.blocklen = g_bdev_blocklen;
	rc = spdk_bdev_open(&bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(ch != NULL);

	g_zone_info.zone_id = g_expected_zone_id;
	g_io_type = SPDK_BDEV_IO_TYPE_ZONE_APPEND;

	start_operation();

	rc = spdk_bdev_zone_append(desc, ch, g_buf, g_start_lba, g_num_blocks, NULL, NULL);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.desc == desc);
	CU_ASSERT(g_bdev_io->type == SPDK_BDEV_IO_TYPE_ZONE_APPEND);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == g_buf);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_len == g_num_blocks * g_bdev_blocklen);
	CU_ASSERT(g_bdev_io->u.bdev.iovcnt == 1);
	CU_ASSERT(g_bdev_io->u.bdev.md_buf == NULL);
	CU_ASSERT(g_bdev_io->u.bdev.num_blocks == g_num_blocks);
	CU_ASSERT(g_bdev_io->u.bdev.offset_blocks == g_expected_zone_id);

	stop_operation();
}

static void
test_bdev_zone_append_with_md(void)
{
	struct spdk_bdev bdev;
	struct spdk_io_channel *ch;
	struct spdk_bdev_desc *desc = NULL;
	int rc;

	memset(&bdev, 0, sizeof(bdev));

	bdev.name = "bdev_zone_ut";
	bdev.blocklen = g_bdev_blocklen;
	rc = spdk_bdev_open(&bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(ch != NULL);

	g_zone_info.zone_id = g_expected_zone_id;
	g_io_type = SPDK_BDEV_IO_TYPE_ZONE_APPEND;

	start_operation();

	rc = spdk_bdev_zone_append_with_md(desc, ch, g_buf, g_md_buf, g_start_lba, g_num_blocks, NULL,
					   NULL);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_io->internal.desc == desc);
	CU_ASSERT(g_bdev_io->type == SPDK_BDEV_IO_TYPE_ZONE_APPEND);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_base == g_buf);
	CU_ASSERT(g_bdev_io->u.bdev.iovs[0].iov_len == g_num_blocks * g_bdev_blocklen);
	CU_ASSERT(g_bdev_io->u.bdev.iovcnt == 1);
	CU_ASSERT(g_bdev_io->u.bdev.md_buf == g_md_buf);
	CU_ASSERT(g_bdev_io->u.bdev.num_blocks == g_num_blocks);
	CU_ASSERT(g_bdev_io->u.bdev.offset_blocks == g_expected_zone_id);

	stop_operation();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("zone", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "test_zone_get_operation", test_zone_get_operation) == NULL
	    ||  CU_add_test(suite, "test_bdev_zone_get_info", test_bdev_zone_get_info) == NULL
	    ||  CU_add_test(suite, "test_bdev_zone_management", test_bdev_zone_management) == NULL
	    ||  CU_add_test(suite, "test_bdev_zone_append", test_bdev_zone_append) == NULL
	    ||  CU_add_test(suite, "test_bdev_zone_append_with_md", test_bdev_zone_append_with_md) == NULL
	    ||  CU_add_test(suite, "test_bdev_io_get_append_location", test_bdev_io_get_append_location) == NULL
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
