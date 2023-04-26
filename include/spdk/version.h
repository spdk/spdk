/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/** \file
 * SPDK version number definitions
 */

#ifndef SPDK_VERSION_H
#define SPDK_VERSION_H

/**
 * Major version number (year of original release minus 2000).
 */
#define SPDK_VERSION_MAJOR	23

/**
 * Minor version number (month of original release).
 */
#define SPDK_VERSION_MINOR	1

/**
 * Patch level.
 *
 * Patch level is incremented on maintenance branch releases and reset to 0 for each
 * new major.minor release.
 */
#define SPDK_VERSION_PATCH	1

/**
 * Version string suffix.
 */
#define SPDK_VERSION_SUFFIX	""

/**
 * Single numeric value representing a version number for compile-time comparisons.
 *
 * Example usage:
 *
 * \code
 * #if SPDK_VERSION >= SPDK_VERSION_NUM(17, 7, 0)
 *   Use feature from SPDK v17.07
 * #endif
 * \endcode
 */
#define SPDK_VERSION_NUM(major, minor, patch) \
	(((major) * 100 + (minor)) * 100 + (patch))

/**
 * Current version as a SPDK_VERSION_NUM.
 */
#define SPDK_VERSION	SPDK_VERSION_NUM(SPDK_VERSION_MAJOR, SPDK_VERSION_MINOR, SPDK_VERSION_PATCH)

#define SPDK_VERSION_STRINGIFY_x(x)	#x
#define SPDK_VERSION_STRINGIFY(x)	SPDK_VERSION_STRINGIFY_x(x)

#define SPDK_VERSION_MAJOR_STRING	SPDK_VERSION_STRINGIFY(SPDK_VERSION_MAJOR)

#if SPDK_VERSION_MINOR < 10
#define SPDK_VERSION_MINOR_STRING	".0" SPDK_VERSION_STRINGIFY(SPDK_VERSION_MINOR)
#else
#define SPDK_VERSION_MINOR_STRING	"." SPDK_VERSION_STRINGIFY(SPDK_VERSION_MINOR)
#endif

#if SPDK_VERSION_PATCH != 0
#define SPDK_VERSION_PATCH_STRING	"." SPDK_VERSION_STRINGIFY(SPDK_VERSION_PATCH)
#else
#define SPDK_VERSION_PATCH_STRING	""
#endif

#ifdef SPDK_GIT_COMMIT
#define SPDK_GIT_COMMIT_STRING SPDK_VERSION_STRINGIFY(SPDK_GIT_COMMIT)
#define SPDK_GIT_COMMIT_STRING_SHA1 " git sha1 " SPDK_GIT_COMMIT_STRING
#else
#define SPDK_GIT_COMMIT_STRING ""
#define SPDK_GIT_COMMIT_STRING_SHA1 ""
#endif

/**
 * Human-readable version string.
 */
#define SPDK_VERSION_STRING	\
	"SPDK v" \
	SPDK_VERSION_MAJOR_STRING \
	SPDK_VERSION_MINOR_STRING \
	SPDK_VERSION_PATCH_STRING \
	SPDK_VERSION_SUFFIX \
	SPDK_GIT_COMMIT_STRING_SHA1

#endif
