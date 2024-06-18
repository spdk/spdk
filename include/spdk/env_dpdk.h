/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Encapsulated DPDK specific dependencies
 */

#include "spdk/stdinc.h"

#ifndef SPDK_ENV_DPDK_H
#define SPDK_ENV_DPDK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the environment library after DPDK env is already initialized.
 * If DPDK's rte_eal_init is already called, this function must be called
 * instead of spdk_env_init, prior to using any other functions in SPDK
 * env library.
 *
 * \param legacy_mem Indicates whether DPDK was initialized with --legacy-mem
 *                   eal parameter.
 * \return 0 on success, or negative errno on failure.
 */
int spdk_env_dpdk_post_init(bool legacy_mem);

/**
 * Release any resources of the environment library that were allocated with
 * spdk_env_dpdk_post_init(). After this call, no DPDK function calls may
 * be made. It is expected that common usage of this function is to call it
 * just before terminating the process.
 */
void spdk_env_dpdk_post_fini(void);

/**
 * Check if DPDK was initialized external to the SPDK env_dpdk library.
 *
 * \return true if DPDK was initialized external to the SPDK env_dpdk library.
 * \return false otherwise
 */
bool spdk_env_dpdk_external_init(void);

/**
 * Dump the env allocated memory to the given file.
 *
 * \param file The file object to write to.
 */
void spdk_env_dpdk_dump_mem_stats(FILE *file);

#ifdef __cplusplus
}
#endif

#endif
