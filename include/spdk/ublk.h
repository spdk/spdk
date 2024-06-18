/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_UBLK_H_
#define SPDK_UBLK_H_

#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spdk_ublk_fini_cb)(void *arg);

/**
 * Initialize the ublk library.
 */
void spdk_ublk_init(void);

/**
 * Stop the ublk layer and close all running ublk block devices.
 *
 * \param cb_fn Callback to be always called.
 * \param cb_arg Passed to cb_fn.
 * \return 0 on success.
 */
int spdk_ublk_fini(spdk_ublk_fini_cb cb_fn, void *cb_arg);

/**
 * Write UBLK subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_ublk_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_UBLK_H_ */
