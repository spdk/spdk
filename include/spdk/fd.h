/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * OS filesystem utility functions
 */

#ifndef SPDK_FD_H
#define SPDK_FD_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the file size.
 *
 * \param fd  File descriptor.
 *
 * \return    File size.
 */
uint64_t spdk_fd_get_size(int fd);

/**
 * Get the block size of the file.
 *
 * \param fd  File descriptor.
 *
 * \return    Block size.
 */
uint32_t spdk_fd_get_blocklen(int fd);

/**
 * Set O_NONBLOCK file status flag.
 *
 * \param fd  File descriptor.
 *
 * \return    0 on success, negative errno value on failure.
 */
int spdk_fd_set_nonblock(int fd);

/**
 * Clear O_NONBLOCK file status flag.
 *
 * \param fd  File descriptor.
 *
 * \return    0 on success, negative errno value on failure.
 */
int spdk_fd_clear_nonblock(int fd);

#ifdef __cplusplus
}
#endif

#endif
