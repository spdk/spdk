/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef __TRACE_INTERNAL_H__
#define __TRACE_INTERNAL_H__

#include "spdk/trace.h"

/* Get shared memory file name. */
const char *trace_get_shm_name(void);

#endif
