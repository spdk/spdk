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
#include "spdk/log.h"
#include "spdk/event.h"

#include "spdk_internal/event.h"

#include <rte_power.h>


static uint32_t
_get_core_freqs(uint32_t lcore_id, uint32_t *freqs, uint32_t num)
{
	return rte_power_freqs(lcore_id, freqs,  num);
}

static uint32_t
_get_core_curr_freq(uint32_t lcore_id)
{
	return rte_power_get_freq(lcore_id);
}

static int
_set_core_freq(uint32_t lcore_id, uint32_t freq_index)
{
	return rte_power_set_freq(lcore_id, freq_index);
}

static int
_core_freq_up(uint32_t lcore_id)
{
	return rte_power_freq_up(lcore_id);
}

static int
_core_freq_down(uint32_t lcore_id)
{
	return rte_power_freq_down(lcore_id);
}

static int
_set_core_freq_max(uint32_t lcore_id)
{
	return rte_power_freq_max(lcore_id);
}

static int
_set_core_freq_min(uint32_t lcore_id)
{
	return rte_power_freq_min(lcore_id);
}

static int
_get_core_turbo_status(uint32_t lcore_id)
{
	return rte_power_turbo_status(lcore_id);
}

static int
_enable_core_turbo(uint32_t lcore_id)
{
	return rte_power_freq_enable_turbo(lcore_id);
}

static int
_disable_core_turbo(uint32_t lcore_id)
{
	return rte_power_freq_disable_turbo(lcore_id);
}

static int
_get_core_capabilities(uint32_t lcore_id, struct spdk_governor_capabilities *capabilities)
{
	struct rte_power_core_capabilities caps;
	int rc;

	rc = rte_power_get_capabilities(lcore_id, &caps);
	if (rc != 0) {
		return rc;
	}

	capabilities->turbo_available = caps.turbo == 0 ? false : true;
	capabilities->priority = caps.priority == 0 ? false : true;
	capabilities->freq_change = true;
	capabilities->freq_getset = true;
	capabilities->freq_up = true;
	capabilities->freq_down = true;
	capabilities->freq_max = true;
	capabilities->freq_min = true;
	capabilities->turbo_set = true;

	return 0;
}

static int
_init_core(uint32_t lcore_id)
{
	int rc;

	rc = rte_power_init(lcore_id);
	if (rc) {
		SPDK_ERRLOG("DPDK Power management library initialization failed on core%d\n", lcore_id);
	}

	return rc;
}

static int
_deinit_core(uint32_t lcore_id)
{
	int rc;

	rc = rte_power_exit(lcore_id);
	if (rc) {
		SPDK_ERRLOG("DPDK Power management library deinitialization failed on core%d\n", lcore_id);
	}

	return rc;
}

static struct spdk_governor dpdk_governor = {
	.name = "dpdk_governor",
	.get_core_freqs = _get_core_freqs,
	.get_core_curr_freq = _get_core_curr_freq,
	.set_core_freq = _set_core_freq,
	.core_freq_up = _core_freq_up,
	.core_freq_down = _core_freq_down,
	.set_core_freq_max = _set_core_freq_max,
	.set_core_freq_min = _set_core_freq_min,
	.get_core_turbo_status = _get_core_turbo_status,
	.enable_core_turbo = _enable_core_turbo,
	.disable_core_turbo = _disable_core_turbo,
	.get_core_capabilities = _get_core_capabilities,
	.init_core = _init_core,
	.deinit_core = _deinit_core,
	.init = NULL,
	.deinit = NULL,
};

SPDK_GOVERNOR_REGISTER(&dpdk_governor);
