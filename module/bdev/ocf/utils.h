/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_UTILS_H
#define VBDEV_OCF_UTILS_H

#include <ocf/ocf.h>
#include "vbdev_ocf.h"

ocf_cache_mode_t ocf_get_cache_mode(const char *cache_mode);
const char *ocf_get_cache_modename(ocf_cache_mode_t mode);

/* Get cache line size in KiB units */
int ocf_get_cache_line_size(ocf_cache_t cache);

/* Get sequential cutoff policy by name */
ocf_seq_cutoff_policy ocf_get_seqcutoff_policy(const char *policy_name);

/* Initiate management operation
 * Receives NULL terminated array of functions (path)
 * and callback (cb)
 * and callback argument (cb_arg)
 * This function may fail with ENOMEM or EBUSY */
int vbdev_ocf_mngt_start(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn *path,
			 vbdev_ocf_mngt_callback cb, void *cb_arg);

/* Continue execution with polling operation (fn)
 * fn must invoke vbdev_ocf_mngt_continue() to stop polling
 * Poller has default timeout of 5 seconds */
void vbdev_ocf_mngt_poll(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn fn);

/* Continue execution with next function that is on path
 * If next function is NULL, finish management operation and invoke callback */
void vbdev_ocf_mngt_continue(struct vbdev_ocf *vbdev, int status);

/* Stop the execution, if status is non zero set it,
 * if rollback function is not null invoke rollback
 * else invoke callback with last status returned */
void vbdev_ocf_mngt_stop(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn *rollback_path, int status);

/* Get status */
int vbdev_ocf_mngt_get_status(struct vbdev_ocf *vbdev);
#endif
