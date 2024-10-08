/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_DOBJ_H
#define VBDEV_OCF_DOBJ_H

#include <ocf/ocf.h>

#include "ctx.h"
#include "data.h"

int vbdev_ocf_volume_init(void);
void vbdev_ocf_volume_cleanup(void);

#endif
