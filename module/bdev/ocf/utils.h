/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VBDEV_OCF_UTILS_H
#define VBDEV_OCF_UTILS_H

#include <ocf/ocf.h>
#include "vbdev_ocf.h"

ocf_cache_mode_t ocf_get_cache_mode(const char *cache_mode);
const char *ocf_get_cache_modename(ocf_cache_mode_t mode);

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
