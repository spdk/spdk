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

/**
 * Get the string value for a given sysfs attribute path
 *
 * When successful, the returned string will be null-terminated, without
 * a trailing newline.
 *
 * \param attribute output parameter for contents of the attribute; caller must
 *		    free() the buffer pointed to by attribute at some
 *		    point after a successful call
 * \param path_format format string for constructing patch to sysfs file
 *
 * \return 0 on success
 *         negative errno if unable to read the attribute
 */
int spdk_read_sysfs_attribute(char **attribute, const char *path_format, ...)
__attribute__((format(printf, 2, 3)));

/**
 * Get the uint32 value for a given sysfs attribute path
 *
 * \param attribute output parameter for contents of the attribute
 * \param path_format format string for constructing patch to sysfs file
 *
 * \return 0 on success
 *         negative errno if unable to read the attribute or it is not a uint32
 */
int spdk_read_sysfs_attribute_uint32(uint32_t *attribute, const char *path_format, ...)
__attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif
