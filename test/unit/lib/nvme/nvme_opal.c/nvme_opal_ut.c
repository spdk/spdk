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
#include "spdk/stdinc.h"
#include "spdk/opal.h"

#include "common/lib/test_env.c"

#include "nvme/nvme_internal.h"
#include "nvme/nvme_opal.c"

struct spdk_nvme_ctrlr g_nvme_ctrlr;

int
spdk_nvme_ctrlr_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				 uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	return 0;
}

int
spdk_nvme_ctrlr_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
			      uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	return 0;
}

static void
opal_revert_cb(struct spdk_opal_dev *dev, void *ctx, int rc)
{
	return;
}

static void
test_nvme_opal_state_transition(void)
{
	struct spdk_opal_dev *opal_dev;
	int rc;
	struct spdk_opal_header *header;

	opal_dev = spdk_opal_init_dev(&g_nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(opal_dev != NULL);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_DEFAULT);
	CU_ASSERT(opal_dev->supported == false);

	opal_dev->supported = true;
	rc = spdk_opal_cmd_take_ownership(opal_dev, "test");
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_DEFAULT);   /* take ownership failure */

	opal_dev->state = OPAL_DEV_STATE_ENABLED;   /* if take ownership success */
	rc = spdk_opal_cmd_take_ownership(opal_dev, "test");    /* other user want to take ownership */
	CU_ASSERT(rc == -EACCES);

	rc = spdk_opal_cmd_revert_tper_async(opal_dev, "test", opal_revert_cb, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_ENABLED);   /* revert failure */

	opal_dev->state = OPAL_DEV_STATE_BUSY;  /* if async command is set */
	header = (void *)opal_dev->resp;
	header->com_packet.outstanding_data = 1;
	rc = spdk_opal_revert_poll(opal_dev);   /* poll the response one time */
	CU_ASSERT(rc == -EAGAIN);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_BUSY);

	header->com_packet.outstanding_data = 0;
	rc = spdk_opal_revert_poll(opal_dev);   /* poll the response one time */
	CU_ASSERT(rc == 0);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_DEFAULT);   /* revert success */

	rc = spdk_opal_cmd_revert_tper_async(opal_dev, "test", opal_revert_cb,
					     NULL);   /* if revert command is issued again */
	CU_ASSERT(rc == 0);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_DEFAULT);

	rc = spdk_opal_cmd_revert_tper(opal_dev, "test");
	CU_ASSERT(rc == 0);
	CU_ASSERT(opal_dev->state == OPAL_DEV_STATE_DEFAULT);

	spdk_opal_close(opal_dev);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_opal", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "opal_state_transition_test", test_nvme_opal_state_transition) == NULL
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
