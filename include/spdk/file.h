/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * file operation functions
 */

#ifndef SPDK_FILE_H
#define SPDK_FILE_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load the input file content into a data buffer.
 *
 * \param file File handle.
 * \param size Size of bytes read from the file.
 *
 * \return data contains the content on success, NULL on failure.
 */
void *spdk_posix_file_load(FILE *file, size_t *size);

/**
 * Load content of a given file name into a data buffer.
 *
 * \param file_name File name.
 * \param size Size of bytes read from the file.
 *
 * \return data containing the content on success, NULL on failure.
 */
void *spdk_posix_file_load_from_name(const char *file_name, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
