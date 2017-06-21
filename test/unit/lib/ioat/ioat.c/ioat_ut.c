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

#include "CUnit/Basic.h"

#include "ioat/ioat.c"

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	return calloc(1, size);
}

void spdk_dma_free(void *buf)
{
	free(buf);
}

uint64_t spdk_vtophys(void *buf)
{
	return (uint64_t)buf;
}

void spdk_delay_us(unsigned int us)
{

}

int
spdk_pci_ioat_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	return -1;
}

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	*mapped_addr = NULL;
	*phys_addr = 0;
	*size = 0;
	return 0;
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
	return 0;
}

int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value,
			   uint32_t offset)
{
	*value = 0xFFFFFFFFu;
	return 0;
}

int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value,
			    uint32_t offset)
{
	return 0;
}

static void ioat_state_check(void)
{
	/*
	 * CHANSTS's STATUS field is 3 bits (8 possible values), but only has 5 valid states:
	 * ACTIVE	0x0
	 * IDLE		0x1
	 * SUSPENDED	0x2
	 * HALTED	0x3
	 * ARMED	0x4
	 */

	CU_ASSERT(is_ioat_active(0) == 1); /* ACTIVE */
	CU_ASSERT(is_ioat_active(1) == 0); /* IDLE */
	CU_ASSERT(is_ioat_active(2) == 0); /* SUSPENDED */
	CU_ASSERT(is_ioat_active(3) == 0); /* HALTED */
	CU_ASSERT(is_ioat_active(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_active(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_active(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_active(7) == 0); /* reserved */

	CU_ASSERT(is_ioat_idle(0) == 0); /* ACTIVE */
	CU_ASSERT(is_ioat_idle(1) == 1); /* IDLE */
	CU_ASSERT(is_ioat_idle(2) == 0); /* SUSPENDED */
	CU_ASSERT(is_ioat_idle(3) == 0); /* HALTED */
	CU_ASSERT(is_ioat_idle(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_idle(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_idle(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_idle(7) == 0); /* reserved */

	CU_ASSERT(is_ioat_suspended(0) == 0); /* ACTIVE */
	CU_ASSERT(is_ioat_suspended(1) == 0); /* IDLE */
	CU_ASSERT(is_ioat_suspended(2) == 1); /* SUSPENDED */
	CU_ASSERT(is_ioat_suspended(3) == 0); /* HALTED */
	CU_ASSERT(is_ioat_suspended(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_suspended(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_suspended(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_suspended(7) == 0); /* reserved */

	CU_ASSERT(is_ioat_halted(0) == 0); /* ACTIVE */
	CU_ASSERT(is_ioat_halted(1) == 0); /* IDLE */
	CU_ASSERT(is_ioat_halted(2) == 0); /* SUSPENDED */
	CU_ASSERT(is_ioat_halted(3) == 1); /* HALTED */
	CU_ASSERT(is_ioat_halted(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_halted(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_halted(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_halted(7) == 0); /* reserved */
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ioat", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "ioat_state_check", ioat_state_check) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
