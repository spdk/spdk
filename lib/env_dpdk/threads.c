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

#include "spdk/env.h"

#include <rte_config.h>
#include <rte_lcore.h>

uint32_t
spdk_env_get_core_count(void)
{
	return rte_lcore_count();
}

uint32_t
spdk_env_get_current_core(void)
{
	return rte_lcore_id();
}

uint32_t
spdk_env_get_first_core(void)
{
	return rte_get_next_lcore(-1, 0, 0);
}

uint32_t
spdk_env_get_next_core(uint32_t prev_core)
{
	unsigned lcore;

	lcore = rte_get_next_lcore(prev_core, 0, 0);
	if (lcore == SPDK_MAX_LCORE) {
		return UINT32_MAX;
	}
	return lcore;
}

uint32_t
spdk_env_get_socket_id(uint32_t core)
{
	return rte_lcore_to_socket_id(core);
}

/**
 * Get the id of the master lcore
 */
unsigned
spdk_env_get_master_lcore(void)
{
	return rte_get_master_lcore();
}

/**
 * Test if an lcore is enabled.
 */
int
spdk_env_lcore_is_enabled(unsigned lcore_id)
{
	return rte_lcore_is_enabled(lcore_id);
}


/**
 * Wait until a lcore finished its job.
 */
int
spdk_env_wait_lcore(unsigned slave_id)
{
	return rte_eal_wait_lcore(slave_id);
}

/**
 * Wait for all  lcores to finish processing their jobs.
 */
void
spdk_env_mp_wait_lcore(void)
{
	rte_eal_mp_wait_lcore();
}

/**
 * Return the state of the lcore identified by slave_id.
 */
enum spdk_env_lcore_state_t
spdk_env_get_lcore_state(unsigned lcore_id) {
	enum spdk_env_lcore_state_t ret = SPDK_LCORE_STATE_FINISHED;
	switch (rte_eal_get_lcore_state(lcore_id))
	{
	case WAIT:
		ret = SPDK_LCORE_STATE_WAIT;
		break;
	case RUNNING:
		ret = SPDK_LCORE_STATE_RUNNING;
		break;
	case FINISHED:
		ret = SPDK_LCORE_STATE_FINISHED;
		break;
	default:
		break;
	}
	return ret;
}


/**
 * Send a message to a slave lcore identified by slave_id to call a
 * function f with argument arg. Once the execution is done, the
 * remote lcore switch in FINISHED state.
 */
int
spdk_env_remote_launch(int (*f)(void *), void *arg, unsigned slave_id)
{
	return rte_eal_remote_launch(f, arg, slave_id);
}
