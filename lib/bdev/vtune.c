/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/config.h"
#if SPDK_CONFIG_VTUNE

/* Disable warnings triggered by the VTune code */
#if defined(__GNUC__) && \
	__GNUC__ > 4 || \
	(__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic ignored "-Wsign-compare"
#if __GNUC__ >= 7
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#endif

#include "ittnotify_static.c"

#endif
