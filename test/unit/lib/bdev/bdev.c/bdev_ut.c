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

#include "lib/test_env.c"
#include "bdev.c"

void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	return NULL;
}

void
spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size)
{
}

void
spdk_io_device_unregister(void *io_device)
{
}

void
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx)
{
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	return NULL;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
}

void
spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
		      spdk_channel_for_each_cpl cpl)
{
}

struct spdk_thread *
spdk_io_channel_get_thread(struct spdk_io_channel *ch)
{
	return NULL;
}

void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			 int *sc, int *sk, int *asc, int *ascq)
{
}

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

static void
bdev_test(void)
{
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "bdev", bdev_test) == NULL
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
